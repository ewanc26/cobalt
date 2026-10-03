# Contributing to cobalt

a Wii U Bluesky client

## Project context

- Primary language: C
- Default branch: main

## Before submitting changes

- Read the README, manifests, and CI workflows before choosing commands.
- Run the documented formatter, linter, build, and test checks relevant to your change.
- Keep commits focused and explain compatibility or operational impact.
- Open changes through a pull request with verification results.

## Recent direction

Recent commits: Sync AGENTS.md from zincfox; Sync AGENTS.md from zincfox; Sync CONTRIBUTING.md from zincfox; Sync CONTRIBUTING.md from zincfox; Update issue template: config.yml
## Releasing

Releases are cut from `main` with `tools/release.sh <patch|minor|major|x.y.z>`.

1. Write the notes under `## [Unreleased]` in `CHANGELOG.md` as part of the PRs that make the changes.
2. Run `tools/release.sh patch` (add `--merge` to merge the release PR automatically, `--dry-run` to only check). It bumps `src/util/version.h` and the README, moves the Unreleased notes under the new version, runs `make test` and opens a PR.
3. After the PR is merged, `tools/publish.sh <version>` tags the merge commit, builds the `.wuhb` and publishes a GitHub release with the changelog notes and the `.wuhb` attached (`--merge` does this for you).
4. The `release-check` workflow fails a `v*` tag whose version does not match `COBALT_VERSION` or the changelog.

Use patch for fixes and polish, minor for features (Cobalt is pre-1.0).
