# Contributing to Cobalt

Cobalt is a Wii U Bluesky client written in C. How changes get made is the stack's shared flow, with the few things Cobalt adds in [docs/workflow.md](docs/workflow.md). Read that first.

## Before opening a pull request

- Branch from `main` as `<type>/<slug>`, and commit as `type(scope): description`.
- Run `make test`. It runs on any machine and needs no devkitPro.
- Fill in the pull request template, including what you ran and where. Hardware results come from a console and nothing else.
- Update `CHANGELOG.md` and the docs in the same pull request.
- Merging is a rebase merge, so each commit has to build and pass on its own.

Build instructions are in the [README](README.md#building). Rules for agents working in this repository are in [AGENTS.md](AGENTS.md).

## Releasing

Releases are cut from `main` with `tools/release.sh <patch|minor|major|x.y.z>`.

1. Write the notes under `## [Unreleased]` in `CHANGELOG.md` as part of the PRs that make the changes.
2. Run `tools/release.sh patch` (add `--merge` to merge the release PR automatically, `--dry-run` to only check). It bumps `src/util/version.h` and the README, moves the Unreleased notes under the new version, runs `make test` and opens a PR.
3. After the PR is merged, `tools/publish.sh <version>` tags the merge commit, builds the `.wuhb` and publishes a GitHub release with the changelog notes and the `.wuhb` attached (`--merge` does this for you).
4. The `release-check` workflow fails a `v*` tag whose version does not match `COBALT_VERSION` or the changelog.

Use patch for fixes and polish, minor for features (Cobalt is pre-1.0).
