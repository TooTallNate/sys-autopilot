// Tests for scripts/changelog-github.mjs — our resilient replacement for
// @changesets/changelog-github. Verifies:
//   1. output matches the upstream plugin byte-for-byte for the same inputs,
//   2. transient GraphQL failures are retried (and eventually succeed),
//   3. permanent failures (and exhausted retries) reject loudly,
//   4. summary pr:/commit:/author: overrides and #ref linkification.
//
// Run: node --test tests/changelog-github.test.mjs
// (fetch is stubbed, so no network and no GITHUB_TOKEN required at call time.)

import assert from "node:assert/strict";
import test from "node:test";
import { pathToFileURL } from "node:url";
import path from "node:path";

process.env.GITHUB_TOKEN ??= "test-token";
// Keep retry backoff fast in tests.
process.env.CHANGESET_GITHUB_RETRIES = "4";

const modUrl = pathToFileURL(
  path.resolve(import.meta.dirname, "../scripts/changelog-github.mjs"),
).href;

const REPO = "TooTallNate/sys-autopilot";
const COMMIT = "a085003f0b0e08e02f9c4d77a4ce354e0309c2cc";

// Build a GraphQL response body for a commit lookup that resolves to a PR.
function commitResponse({ index = 0, login = "Andarist", pr = 1613 } = {}) {
  return {
    data: {
      [`a${index}`]: {
        [`a${COMMIT}`]: {
          commitUrl: `https://github.com/${REPO}/commit/${COMMIT}`,
          associatedPullRequests: {
            nodes: [
              {
                number: pr,
                url: `https://github.com/${REPO}/pull/${pr}`,
                mergedAt: "2026-01-07T06:43:58Z",
                author: { login, url: `https://github.com/${login}` },
              },
            ],
          },
          author: { user: { login, url: `https://github.com/${login}` } },
        },
      },
    },
  };
}

// Install a stub global.fetch that returns the given sequence of outcomes.
// Each outcome is either a function throwing (transient) or a JSON body object.
function stubFetch(outcomes) {
  let call = 0;
  const calls = [];
  globalThis.fetch = async (url, init) => {
    calls.push({ url, init });
    const outcome = outcomes[Math.min(call, outcomes.length - 1)];
    call++;
    if (typeof outcome === "function") return outcome();
    return {
      status: 200,
      json: async () => outcome,
    };
  };
  return { get calls() { return calls; } };
}

// Fresh module instance per test so the internal request cache doesn't leak.
async function freshModule() {
  return (await import(`${modUrl}?t=${Math.random()}`)).default;
}

test("getReleaseLine: commit resolves to PR + author (matches upstream format)", async () => {
  stubFetch([commitResponse()]);
  const mod = await freshModule();
  const line = await mod.getReleaseLine(
    { summary: "Add a thing", commit: COMMIT, id: "x" },
    "minor",
    { repo: REPO },
  );
  assert.equal(
    line,
    `\n\n- [#1613](https://github.com/${REPO}/pull/1613) ` +
      `[\`a085003\`](https://github.com/${REPO}/commit/${COMMIT}) ` +
      `Thanks [@Andarist](https://github.com/Andarist)! - Add a thing\n`,
  );
});

test("getReleaseLine: summary pr/author overrides win over commit lookup", async () => {
  // PR override -> getInfoFromPullRequest query shape.
  stubFetch([
    {
      data: {
        a0: {
          pr__42: {
            url: `https://github.com/${REPO}/pull/42`,
            author: { login: "octocat", url: "https://github.com/octocat" },
            mergeCommit: {
              commitUrl: `https://github.com/${REPO}/commit/deadbee`,
              abbreviatedOid: "deadbee",
            },
          },
        },
      },
    },
  ]);
  const mod = await freshModule();
  const line = await mod.getReleaseLine(
    {
      summary: "pr: 42\nauthor: someone\nFixes #7 and a bug",
      commit: COMMIT,
      id: "y",
    },
    "patch",
    { repo: REPO },
  );
  assert.match(line, /\[#42\]\(https:\/\/github\.com\/TooTallNate\/sys-autopilot\/pull\/42\)/);
  assert.match(line, /Thanks \[@someone\]\(https:\/\/github\.com\/someone\)!/);
  // Bare #7 linkified to issues URL.
  assert.match(line, /\[#7\]\(https:\/\/github\.com\/TooTallNate\/sys-autopilot\/issues\/7\)/);
});

test("getReleaseLine: no commit -> no links prefix", async () => {
  stubFetch([{ data: {} }]);
  const mod = await freshModule();
  const line = await mod.getReleaseLine(
    { summary: "Standalone note", commit: undefined, id: "z" },
    "patch",
    { repo: REPO },
  );
  assert.equal(line, "\n\n- Standalone note\n");
});

test("retries transient 'Premature close' then succeeds", async () => {
  const transient = () => {
    throw new Error("Invalid response body ... Premature close");
  };
  const stub = stubFetch([transient, transient, commitResponse()]);
  const mod = await freshModule();
  const line = await mod.getReleaseLine(
    { summary: "Resilient", commit: COMMIT, id: "r" },
    "patch",
    { repo: REPO },
  );
  assert.match(line, /Thanks \[@Andarist\]/);
  assert.equal(stub.calls.length, 3, "should have retried twice before success");
});

test("retries HTTP 5xx then succeeds", async () => {
  const five = () => ({ status: 503, json: async () => ({}) });
  const stub = stubFetch([five, commitResponse()]);
  const mod = await freshModule();
  const line = await mod.getReleaseLine(
    { summary: "ok", commit: COMMIT, id: "r2" },
    "patch",
    { repo: REPO },
  );
  assert.match(line, /Thanks \[@Andarist\]/);
  assert.equal(stub.calls.length, 2);
});

test("fails loudly after exhausting retries (no silent fallback)", async () => {
  const transient = () => {
    throw new Error("fetch failed: terminated");
  };
  stubFetch([transient]); // always transient
  const mod = await freshModule();
  await assert.rejects(
    mod.getReleaseLine(
      { summary: "should fail", commit: COMMIT, id: "f" },
      "patch",
      { repo: REPO },
    ),
    /terminated/,
  );
});

test("does NOT retry permanent GraphQL/auth errors", async () => {
  const authError = () => ({
    status: 200,
    json: async () => ({ errors: [{ message: "Bad credentials" }] }),
  });
  const stub = stubFetch([authError]);
  const mod = await freshModule();
  await assert.rejects(
    mod.getReleaseLine(
      { summary: "x", commit: COMMIT, id: "a" },
      "patch",
      { repo: REPO },
    ),
    /Bad credentials/,
  );
  assert.equal(stub.calls.length, 1, "auth errors must not be retried");
});

test("getDependencyReleaseLine formats updated deps with commit links", async () => {
  stubFetch([commitResponse()]);
  const mod = await freshModule();
  const line = await mod.getDependencyReleaseLine(
    [{ commit: COMMIT, summary: "dep bump", id: "d" }],
    [{ name: "some-dep", newVersion: "1.2.3" }],
    { repo: REPO },
  );
  assert.equal(
    line,
    `- Updated dependencies [[\`a085003\`](https://github.com/${REPO}/commit/${COMMIT})]:\n` +
      `  - some-dep@1.2.3`,
  );
});

test("missing repo option throws", async () => {
  stubFetch([{ data: {} }]);
  const mod = await freshModule();
  await assert.rejects(
    mod.getReleaseLine({ summary: "x", id: "n" }, "patch", {}),
    /provide a repo/,
  );
});
