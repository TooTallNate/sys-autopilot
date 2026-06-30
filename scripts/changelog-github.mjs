// Local changelog generator for changesets, replacing
// @changesets/changelog-github (+ @changesets/get-github-info).
//
// Why this exists: the upstream packages issue a single GitHub GraphQL request
// with no retry and no batching. On CI that request intermittently dies with
// "Invalid response body ... Premature close" (the connection to
// api.github.com/graphql is cut mid-response), which aborts `changeset version`
// and fails the whole Release job. Those packages are effectively unmaintained
// (see changesets/changesets#1925), so we own this instead.
//
// Output format is byte-for-byte compatible with @changesets/changelog-github
// @0.7.0, so CHANGELOG.md stays identical. On top of that we add:
//   - retry with exponential backoff (+ jitter) on transient GraphQL failures
//   - request batching + correct cache-key dedup (changesets/changesets#1925)
//
// We deliberately do NOT fall back to a degraded changelog entry: if GitHub is
// genuinely unreachable after exhausting retries, the release fails loudly so a
// human investigates, rather than silently shipping an inconsistent changelog.
// The point is to be far more resilient than the upstream package (which has
// zero retries), not to paper over real outages.

const GITHUB_GRAPHQL_URL =
  process.env.GITHUB_GRAPHQL_URL || "https://api.github.com/graphql";
const GITHUB_SERVER_URL = process.env.GITHUB_SERVER_URL || "https://github.com";

// Batch size for GraphQL lookups; large batches can trip "Timeout on
// validation of query" on the GitHub side. Overridable via env.
const DEFAULT_BATCH_SIZE = 100;
const getBatchSize = () => {
  const v = Number.parseInt(process.env.CHANGESET_GITHUB_BATCH_SIZE ?? "", 10);
  return Number.isInteger(v) && v > 0 ? v : DEFAULT_BATCH_SIZE;
};

// Retry tuning. Transient network/5xx failures are retried aggressively with
// exponential backoff + jitter; auth/permanent errors are not retried. Defaults
// (8 attempts, backoff capped at 30s) give GitHub blips ample time to clear
// before the release is allowed to fail.
const MAX_ATTEMPTS = Number(process.env.CHANGESET_GITHUB_RETRIES ?? 8);
const BASE_DELAY_MS = 500;
const MAX_DELAY_MS = 30_000;

const validRepoNameRegex = /^[\w.-]+\/[\w.-]+$/;

const readToken = () => {
  const token = process.env.GITHUB_TOKEN;
  if (!token) {
    throw new Error(
      "Please add a GITHUB_TOKEN environment variable with `read:user` and " +
        "`repo:status` permissions for the changelog generator",
    );
  }
  return token;
};

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// True for errors worth retrying: aborted/closed sockets, resets, timeouts,
// and 5xx / rate-limit responses. Auth and query errors are not retried.
const isTransient = (err) => {
  const msg = String(err?.message ?? err).toLowerCase();
  return (
    msg.includes("premature close") ||
    msg.includes("econnreset") ||
    msg.includes("etimedout") ||
    msg.includes("socket hang up") ||
    msg.includes("network") ||
    msg.includes("fetch failed") ||
    msg.includes("aborted") ||
    msg.includes("terminated") ||
    msg.includes("http 5") ||
    msg.includes("http 429")
  );
};

const makeQuery = (repos) => `
      query {
        ${Object.keys(repos)
          .map(
            (repo, i) => `a${i}: repository(
            owner: ${JSON.stringify(repo.split("/")[0])}
            name: ${JSON.stringify(repo.split("/")[1])}
          ) {
            ${repos[repo]
              .map((data) =>
                data.kind === "commit"
                  ? `a${data.commit}: object(expression: ${JSON.stringify(
                      data.commit,
                    )}) {
            ... on Commit {
            commitUrl
            associatedPullRequests(first: 50) {
              nodes {
                number
                url
                mergedAt
                author { login url }
              }
            }
            author { user { login url } }
          }}`
                  : `pr__${data.pull}: pullRequest(number: ${data.pull}) {
                    url
                    author { login url }
                    mergeCommit { commitUrl abbreviatedOid }
                  }`,
              )
              .join("\n")}
          }`,
          )
          .join("\n")}
        }
    `;

// One GraphQL POST with retry + backoff. Returns the parsed `data.data`.
const fetchGraphQL = async (query, token) => {
  let lastErr;
  for (let attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    try {
      const res = await fetch(GITHUB_GRAPHQL_URL, {
        method: "POST",
        headers: {
          Authorization: `Token ${token}`,
          "Content-Type": "application/json",
        },
        body: JSON.stringify({ query }),
      });
      if (res.status >= 500 || res.status === 429) {
        throw new Error(`GitHub responded with HTTP ${res.status}`);
      }
      const json = await res.json();
      if (json.errors) {
        throw new Error(
          `GitHub GraphQL errors:\n${JSON.stringify(json.errors, null, 2)}`,
        );
      }
      if (!json.data) {
        // Usually an auth problem; not worth retrying.
        throw new Error(`Unexpected GitHub response:\n${JSON.stringify(json)}`);
      }
      return json.data;
    } catch (err) {
      lastErr = err;
      if (attempt === MAX_ATTEMPTS || !isTransient(err)) break;
      // Exponential backoff capped at MAX_DELAY_MS, plus jitter to avoid
      // synchronized retries hammering the API.
      const backoff = Math.min(BASE_DELAY_MS * 2 ** (attempt - 1), MAX_DELAY_MS);
      const delay = backoff + Math.floor(Math.random() * BASE_DELAY_MS);
      console.error(
        `changelog: GitHub GraphQL attempt ${attempt}/${MAX_ATTEMPTS} failed ` +
          `(${err?.message ?? err}); retrying in ${delay}ms`,
      );
      await sleep(delay);
    }
  }
  throw lastErr;
};

// --- batched, cached loader ---------------------------------------------------

// Cache keyed by a stable string (not object identity, which was the upstream
// dedup bug). Stores a promise for the cleaned per-request node.
const cache = new Map();

const cacheKey = (req) =>
  req.kind === "commit"
    ? `commit:${req.repo}:${req.commit}`
    : `pull:${req.repo}:${req.pull}`;

let queue = [];
let scheduled = null;

const flush = () => {
  const batch = queue;
  queue = [];
  scheduled = null;

  const token = readToken();
  const batchSize = getBatchSize();

  void (async () => {
    try {
      for (let i = 0; i < batch.length; i += batchSize) {
        const chunk = batch.slice(i, i + batchSize);
        const repos = {};
        for (const { req } of chunk) {
          (repos[req.repo] ??= []).push(req);
        }

        const data = await fetchGraphQL(makeQuery(repos), token);

        const cleaned = {};
        Object.keys(repos).forEach((repo, index) => {
          const out = { commit: {}, pull: {} };
          cleaned[repo] = out;
          for (const [field, value] of Object.entries(data[`a${index}`] ?? {})) {
            if (field[0] === "a") out.commit[field.slice(1)] = value;
            else out.pull[field.replace("pr__", "")] = value;
          }
        });

        for (const { req, resolve } of chunk) {
          resolve(
            cleaned[req.repo]?.[req.kind][
              req.kind === "pull" ? req.pull : req.commit
            ],
          );
        }
      }
    } catch (err) {
      for (const { reject } of batch) reject(err);
    }
  })();
};

const load = (req) => {
  const key = cacheKey(req);
  if (cache.has(key)) return cache.get(key);
  const promise = new Promise((resolve, reject) => {
    queue.push({ req, resolve, reject });
    scheduled ??= setTimeout(flush, 0);
  });
  cache.set(key, promise);
  return promise;
};

// --- public info helpers (mirror @changesets/get-github-info) -----------------

const getInfo = async (request) => {
  if (!request.commit) throw new Error("Please pass a commit SHA to getInfo");
  if (!validRepoNameRegex.test(request.repo ?? "")) {
    throw new Error(
      "Please pass a valid GitHub repository in the form of userOrOrg/repoName",
    );
  }
  const data = await load({ kind: "commit", ...request });
  let user = data?.author?.user ?? null;
  const nodes = data?.associatedPullRequests?.nodes?.length
    ? data.associatedPullRequests.nodes
    : null;
  const associatedPullRequest = nodes
    ? [...nodes].sort((a, b) => {
        if (a.mergedAt === null && b.mergedAt === null) return 0;
        if (a.mergedAt === null) return 1;
        if (b.mergedAt === null) return -1;
        const da = new Date(a.mergedAt);
        const db = new Date(b.mergedAt);
        return da > db ? 1 : da < db ? -1 : 0;
      })[0]
    : null;
  if (associatedPullRequest) user = associatedPullRequest.author;
  return {
    user: user ? user.login : null,
    pull: associatedPullRequest ? associatedPullRequest.number : null,
    links: {
      commit: `[\`${request.commit.slice(0, 7)}\`](${data.commitUrl})`,
      pull: associatedPullRequest
        ? `[#${associatedPullRequest.number}](${associatedPullRequest.url})`
        : null,
      user: user ? `[@${user.login}](${user.url})` : null,
    },
  };
};

const getInfoFromPullRequest = async (request) => {
  if (request.pull === undefined) {
    throw new Error("Please pass a pull request number");
  }
  if (!validRepoNameRegex.test(request.repo ?? "")) {
    throw new Error(
      "Please pass a valid GitHub repository in the form of userOrOrg/repoName",
    );
  }
  const data = await load({ kind: "pull", ...request });
  const user = data?.author ?? null;
  const commit = data?.mergeCommit ?? null;
  return {
    user: user ? user.login : null,
    commit: commit ? commit.abbreviatedOid : null,
    links: {
      commit: commit
        ? `[\`${commit.abbreviatedOid.slice(0, 7)}\`](${commit.commitUrl})`
        : null,
      pull: `[#${request.pull}](${data.url})`,
      user: user ? `[@${user.login}](${user.url})` : null,
    },
  };
};

// --- changelog formatting (mirrors @changesets/changelog-github) --------------

// Linkify bare #123 issue/PR refs, skipping ones already inside markdown links.
const linkifyIssueRefs = (line, repo) =>
  line.replace(/\[.*?\]\(.*?\)|\B#([1-9]\d*)\b/g, (match, issue) =>
    issue ? `[#${issue}](${GITHUB_SERVER_URL}/${repo}/issues/${issue})` : match,
  );

const repoError = new Error(
  'Please provide a repo to this changelog generator like this:\n"changelog": ["./scripts/changelog-github.mjs", { "repo": "org/repo" }]',
);

const changelogFunctions = {
  getDependencyReleaseLine: async (changesets, dependenciesUpdated, options) => {
    if (!options.repo) throw repoError;
    if (dependenciesUpdated.length === 0) return "";

    const commitLinks = (
      await Promise.all(
        changesets.map(async (cs) => {
          if (!cs.commit) return undefined;
          const { links } = await getInfo({
            repo: options.repo,
            commit: cs.commit,
          });
          return links.commit;
        }),
      )
    ).filter(Boolean);

    const changesetLink = `- Updated dependencies [${commitLinks.join(", ")}]:`;
    const updated = dependenciesUpdated.map(
      (d) => `  - ${d.name}@${d.newVersion}`,
    );
    return [changesetLink, ...updated].join("\n");
  },

  getReleaseLine: async (changeset, _type, options) => {
    if (!options?.repo) throw repoError;

    let prFromSummary;
    let commitFromSummary;
    const usersFromSummary = [];
    const replacedChangelog = changeset.summary
      .replace(/^\s*(?:pr|pull|pull\s+request):\s*#?(\d+)/im, (_, pr) => {
        const num = Number(pr);
        if (!Number.isNaN(num)) prFromSummary = num;
        return "";
      })
      .replace(/^\s*commit:\s*([^\s]+)/im, (_, commit) => {
        commitFromSummary = commit;
        return "";
      })
      .replace(/^\s*(?:author|user):\s*@?([^\s]+)/gim, (_, user) => {
        usersFromSummary.push(user);
        return "";
      })
      .trim();

    const [firstLine, ...futureLines] = replacedChangelog
      .split("\n")
      .map((l) => l.trimEnd());

    const links = await (async () => {
      if (prFromSummary !== undefined) {
        const { links } = await getInfoFromPullRequest({
          repo: options.repo,
          pull: prFromSummary,
        });
        if (commitFromSummary) {
          const short = commitFromSummary.slice(0, 7);
          return {
            ...links,
            commit: `[\`${short}\`](${GITHUB_SERVER_URL}/${options.repo}/commit/${commitFromSummary})`,
          };
        }
        return links;
      }
      const commitToFetchFrom = commitFromSummary || changeset.commit;
      if (commitToFetchFrom) {
        const { links } = await getInfo({
          repo: options.repo,
          commit: commitToFetchFrom,
        });
        return links;
      }
      return { commit: null, pull: null, user: null };
    })();

    const users = options.disableThanks
      ? null
      : usersFromSummary.length
        ? usersFromSummary
            .map((u) => `[@${u}](${GITHUB_SERVER_URL}/${u})`)
            .join(", ")
        : links.user;

    const prefix = [
      links.pull === null ? "" : ` ${links.pull}`,
      links.commit === null ? "" : ` ${links.commit}`,
      users === null ? "" : ` Thanks ${users}!`,
    ].join("");

    return `\n\n-${prefix ? `${prefix} -` : ""} ${linkifyIssueRefs(
      firstLine,
      options.repo,
    )}\n${futureLines
      .map((l) => `  ${linkifyIssueRefs(l, options.repo)}`)
      .join("\n")}`;
  },
};

export default changelogFunctions;
