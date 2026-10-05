# How changes get made

This is the flow for Cobalt, and it is the same flow Wolfram, Metalbear, Indigo and Platinum use. The rules that can be checked by a machine are checked by one. Where they are not, the table at the bottom says so.

## The short version

1. Open or pick an issue. Anything that needs the console, a credential or money gets the `needs-owner` label.
2. Branch from `main`: `<type>/<kebab-description>`, for example `fix/scroll-restore`. The types are the commit types below, plus `release`.
3. Commit as `type(scope): description`. Lowercase, imperative, no full stop, 72 characters at most. One concern per commit. Types: `feat`, `fix`, `docs`, `test`, `refactor`, `style`, `build`, `chore`, `ci`, `perf`. The scope is the module (`atproto`, `ui`, `app`, `net`, `cache`, `util`, `input`, `build`, `tests`) and is dropped when a change spans the repository.
4. Open a pull request using the template. Its title follows the commit format. `Summary` says what and why; `Verification` says what actually ran and where: host, emulator or hardware. I do not claim hardware I did not use.
5. Change the code, the tests, `CHANGELOG.md` and the docs in the same pull request. If a change genuinely needs no changelog line or no doc change, the description says so with `Changelog: none (reason)` or `Docs: none (reason)`.
6. Wait for CI to go green. Red CI is not a place to stop: read the job log, reproduce it, fix the cause.
7. Merge only a green pull request, through the pull request, never by pushing to `main`.

Wolfram changes land first. Cobalt adopts them afterwards, by pointing at a Wolfram that has them, not by carrying a copy.

## What checks what

| Rule | Where it is enforced |
|---|---|
| Branch name, PR title, PR description sections, commit subjects, changelog and docs updated | `pr-flow` workflow, which calls the reusable `flow.yml`. The logic is in `.github/flow/flow-check.sh`. |
| Each of those checks can actually fail | `.github/flow/flow-selftest.sh` breaks each rule on purpose and asserts the check goes red. It runs in the `drift` job. |
| The written flow, its enforcement and the hosting repository agree | `.github/flow/check-drift.sh`, in the `drift` job. |
| Host tests, end-to-end, snapshots, and a Wii U build with Wolfram linked | `host-tests` and `wuhb` in `ci.yml`. |
| Nothing reaches `main` without a pull request | Branch protection, which I cannot set from here (see below). The `direct-push-guard` job turns a direct push into a red `main` in the meantime. |
| A release is cut from `main`, with green CI, and matches `version.h` and the changelog | `tools/release.sh` and `tools/publish.sh` refuse otherwise; the `release-check` workflow refuses a `v*` tag that is off `main`, not green, or mismatched. |

## Branch protection

The settings that make the checks binding are in the repository settings, and only the owner can change them. `main` should require a pull request and these status checks: `host-tests`, `wuhb`, `drift` and `flow / conventions`. That is tracked in an issue labelled `needs-owner`. Until it is done, the checks above still run on every pull request; they are advisory rather than blocking.

## Hosting

`.github/workflows/flow.yml` is a reusable workflow and fetches its script from a repository you name, so another repository in the stack uses it with a few lines:

```yaml
jobs:
  flow:
    uses: ewanc26/cobalt/.github/workflows/flow.yml@main
```

It lives in Cobalt because Wolfram did not host it when it was written. When Wolfram does, `flow_repo` changes and the files in `.github/flow/` become copies that `check-drift.sh` compares byte for byte against Wolfram's. The shared files are `flow-check.sh`, `check-drift.sh`, `flow-selftest.sh`, `flow.yml` and `pull_request_template.md`.

## Releases

Notes go under `## [Unreleased]` in `CHANGELOG.md` as part of the pull requests that make the changes. `tools/release.sh` opens the release pull request, and `tools/publish.sh` tags the merge commit and builds the `.wuhb`. See [CONTRIBUTING.md](../CONTRIBUTING.md#releasing).
