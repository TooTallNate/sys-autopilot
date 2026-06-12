# Changesets

This folder is used by [changesets](https://github.com/changesets/changesets)
to manage versioning and the release process.

To record a change for the next release:

```sh
pnpm changeset
```

When changesets land on `main`, the release workflow opens/updates a
"Version Packages" PR. Merging that PR bumps the version, updates
`CHANGELOG.md`, and cuts a GitHub Release with the built sysmodule zip
attached.
