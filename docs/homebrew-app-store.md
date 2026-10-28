# Homebrew App Store

This is how Cobalt gets onto the Wii U Homebrew App Store (`fortheusers/hb-appstore`), what I could and could not check about the process, and the one command that does the submission. Cobalt is listed there now: the package `packages/Cobalt` in [`fortheusers/wiiu-hbas-repo`](https://github.com/fortheusers/wiiu-hbas-repo) carries version 0.12.0 and points its `update` asset at the `cobalt-0.12.0.wuhb` release download. Getting it there was a pull request on someone else's repository in my name, so it was mine to send. Routine releases need nothing from here: the store keeps the package in step with them itself (see [How a new release reaches the store](#how-a-new-release-reaches-the-store)). `tools/submit-hbas.sh` is for the first listing and for changing what the store shows.

What has not been checked is the store client itself: nothing here has installed the package from `hb-appstore` on a console, so the listing is confirmed from the repository that the store serves, not from a Wii U.

## What the store is

The store is a client (`hb-appstore`) over a package manager (`libget`). Its Wii U repository is [`fortheusers/wiiu-hbas-repo`](https://github.com/fortheusers/wiiu-hbas-repo), which says: "If you want to add a new, or update an existing app, please feel free to open a Pull request". Each app is a folder under `packages/` holding a `pkgbuild.json` and its images. Their tool (spinarak) downloads the assets named in `pkgbuild.json` and zips the package; libget serves "repo JSON data and package zips ... designed to be statically hosted as files, with no explicit backend logic", and a client detects an update from the package's `version`.

## What I read, and what I could not

Read: the `wiiu-hbas-repo` README, the libget wiki page on package structure (the `repo.json` fields `name`, `version`, `category`, `title`, `author`, `url`, `license`, `description`, `details`, `changelog`), the `repogen.py` page, and a live package (`packages/CafXplorer`: `pkgbuild.json`, `icon.png`, `screen.png`, `screen-0.png` to `screen-6.png`). The generated folder copies that package's shape: an `info` block, an `assets` list with one `update` asset (`url` and `dest`), an `icon`, a `banner` and `screenshot` assets, and a `changelog` string.

Not verifiable from here, so not claimed: the format is documented as a TODO upstream ("describe this format"); the full list of categories (I use `tool`, which that package uses); the required image sizes (I use a 128x128 icon, the same as the Wii U menu icon, and 1280x720 images, and the check refuses anything under 640x360); and the store's own review rules. The Aroma documentation site and `hb-app.store/api-info` were blocked from the build environment and I did not retry them.

One thing I had recorded here as unverifiable — whether the store rebuilds or auto-updates a package from a GitHub release — has since been answered by the repository itself, below.

## What is generated

`tools/make-hbas-listing.py` writes `dist/hbas/packages/Cobalt/` from what the repository already has: the version from `src/util/version.h`, the changelog from `CHANGELOG.md`, the icon from `assets/icon.png`, the banner from `assets/tv_splash.png`, the screenshots from the host snapshot renderer, and the `.wuhb` URL from the release layout `tools/publish.sh` produces (`https://github.com/ewanc26/cobalt/releases/download/v<version>/cobalt-<version>.wuhb`, installed to `/wiiu/apps/cobalt.wuhb`, the path the `.wuhb` has to land in).

`tools/check-hbas-listing.py` validates it and `tools/check-hbas-listing-selftest.sh` breaks each rule on purpose. CI runs all three on every push and uploads the folder as the `hbas-listing` artifact.

The screenshots are rendered on a PC from fixture data (the "Ewan Croft" account in the mock server), not captured on a console. The pull request says so.

## How a new release reaches the store

No step of the release process touches the store. [`fortheusers/wiiu-hbas-repo`](https://github.com/fortheusers/wiiu-hbas-repo) is changed by the store's own automation, which commits as the account `Dragonite <fight@fortheusers.org>` (`[auto] Update <package> to <version>`, committed by GitHub, and it does this for many packages besides Cobalt). What it did to Cobalt on 2026-10-23, read from the commit and the two versions of the file:

| field | 0.11.1 → 0.12.0 |
|---|---|
| `info.version` | bumped |
| the `update` asset's `url` | `.../v0.11.1/cobalt-0.11.1.wuhb` → `.../v0.12.0/cobalt-0.12.0.wuhb` |
| `changelog` | filled from the release notes, which `tools/publish.sh` takes from `CHANGELOG.md` |
| `info.details` | copied forward unchanged |
| icon, banner, screenshots | left as they were |

So a release published with `tools/release.sh` reaches the store on its own, and the one thing that cannot change that way is the written metadata: `details` is carried over, not regenerated from this repository. The store therefore still says Cobalt "can check this GitHub project for updates from its Updates screen", which stopped being true when the updater was removed. A re-submission is what changes it.

What is still inference rather than observed: how the automation picks the version and the asset (it named the release's own `.wuhb`, `cobalt-<version>.wuhb`, from the tag), whether it ever refreshes `details`, and how quickly it runs. Nothing in the upstream repository references this one's `update.json` manifest, which is retired in any case.

Two consequences for this repository:

- The `.wuhb` attached to a release must stay named `cobalt-<version>.wuhb`, because the URL the store downloads is built from the tag and that name. `tools/check-release-layout.py` (CI job `host-tests`, with its own self-test) fails when the tools that name it stop agreeing.
- A metadata change means a re-submission, and the artifact is the one CI built on the tag, so it takes effect from the next release rather than the current one.

## Submitting

For the first listing (already done), or to change what the store shows. With the GitHub CLI logged in as me, after a release has been cut with `tools/release.sh`:

```sh
tools/submit-hbas.sh            # the version in src/util/version.h
tools/submit-hbas.sh 0.6.0      # or name it
tools/submit-hbas.sh --dry-run  # stop before anything is pushed
```

It checks the release has `cobalt-<version>.wuhb`, downloads the artifact CI built on the tagged commit, validates it, forks `fortheusers/wiiu-hbas-repo`, adds or updates `packages/Cobalt` on a branch and opens the pull request with the details filled in.

Two things to know before using it. The artifact is built by CI on the tag, so running it against an old release re-submits that release's text rather than the current repository's — the wording on this page, the store's `details`, only moves when the release it names carries the new text. And it is mine to run, never an agent's: it opens a pull request on someone else's repository in my name.

## The seed

A store install is only the `.wuhb`. Cobalt needs a per-console entropy seed before it will go online, and a seed shipped inside the package would be the same on every console, so the package never carries one. On first run Cobalt asks you to scribble on the GamePad to make one (see the README and AGENTS.md section 13). That path has only been tested on the host.
