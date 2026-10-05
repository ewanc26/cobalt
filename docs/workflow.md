# How changes get made

Cobalt follows the flow the whole stack shares. Wolfram holds the canonical write-up, [docs/flow.md](https://github.com/ewanc26/wolfram/blob/main/docs/flow.md), and the agent-facing version is the `flow` block in [AGENTS.md](../AGENTS.md), which is copied from Wolfram byte for byte. I do not repeat them here. This page is only what Cobalt adds.

## What Cobalt adds

| Rule | Where it is enforced |
|---|---|
| A change to `src/` carries a `CHANGELOG.md` line under `[Unreleased]` and a change to the README, `AGENTS.md` or `docs/`. Opt out only with `Changelog: none (reason)` or `Docs: none (reason)` in the description. | `flow / extras`, from [`tools/flow-extra.sh`](../tools/flow-extra.sh). [`tools/flow-extra-selftest.sh`](../tools/flow-extra-selftest.sh) breaks each rule on purpose and runs in the same job. |
| The README, `docs/PARITY.md` and `AGENTS.md` agree about what Cobalt does. | `parity` job, [`tools/check-parity.sh`](../tools/check-parity.sh) and its self-test. |
| Nothing reaches `main` without a pull request. | Branch protection, which only I can set ([#134](https://github.com/ewanc26/cobalt/issues/134)). Until then `direct-push-guard` turns a direct push into a red `main`. |
| A release is cut from `main`, with green CI, and matches `version.h` and the changelog. | `tools/release.sh` and `tools/publish.sh` refuse otherwise (publish also refuses a commit whose `host-tests` or `wuhb` are not green); the `release-check` workflow refuses a `v*` tag that is off `main`, not green, or mismatched. |

`CI gate` in `ci.yml` aggregates `host-tests`, `wuhb` and `parity`, so a new gating job is added to its `needs` list and nothing else.

## Merging

Rebase merge only. Every commit lands on `main` as I wrote it, so each one builds and passes tests on its own, and review fixes are real `fix(scope): ...` commits. Cobalt's earlier history used merge commits; that stopped when the stack settled on rebase merging, which keeps one convention across the five repositories and keeps `main` linear.

## Releases

Notes go under `## [Unreleased]` in `CHANGELOG.md` as part of the pull requests that make the changes. See [CONTRIBUTING.md](../CONTRIBUTING.md#releasing).
