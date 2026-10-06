# Homebrew App Store

This is how Cobalt gets onto the Wii U Homebrew App Store (`fortheusers/hb-appstore`), what I could and could not check about the process, and the one command that does the submission. I have not submitted anything: it is a pull request on someone else's repository in my name, so it is mine to send.

## What the store is

The store is a client (`hb-appstore`) over a package manager (`libget`). Its Wii U repository is [`fortheusers/wiiu-hbas-repo`](https://github.com/fortheusers/wiiu-hbas-repo), which says: "If you want to add a new, or update an existing app, please feel free to open a Pull request". Each app is a folder under `packages/` holding a `pkgbuild.json` and its images. Their tool (spinarak) downloads the assets named in `pkgbuild.json` and zips the package; libget serves "repo JSON data and package zips ... designed to be statically hosted as files, with no explicit backend logic", and a client detects an update from the package's `version`.

## What I read, and what I could not

Read: the `wiiu-hbas-repo` README, the libget wiki page on package structure (the `repo.json` fields `name`, `version`, `category`, `title`, `author`, `url`, `license`, `description`, `details`, `changelog`), the `repogen.py` page, and a live package (`packages/CafXplorer`: `pkgbuild.json`, `icon.png`, `screen.png`, `screen-0.png` to `screen-6.png`). The generated folder copies that package's shape: an `info` block, an `assets` list with one `update` asset (`url` and `dest`), an `icon`, a `banner` and `screenshot` assets, and a `changelog` string.

Not verifiable from here, so not claimed: the format is documented as a TODO upstream ("describe this format"); the full list of categories (I use `tool`, which that package uses); the required image sizes (I use a 128x128 icon, the same as the Wii U menu icon, and 1280x720 images, and the check refuses anything under 640x360); whether the store's maintainers rebuild or auto-update a package from a GitHub release (nothing I read says they do, so assume every release needs a pull request); and the store's own review rules. The Aroma documentation site and `hb-app.store/api-info` were blocked from the build environment and I did not retry them.

## What is generated

`tools/make-hbas-listing.py` writes `dist/hbas/packages/Cobalt/` from what the repository already has: the version from `src/util/version.h`, the changelog from `CHANGELOG.md`, the icon from `assets/icon.png`, the banner from `assets/tv_splash.png`, the screenshots from the host snapshot renderer, and the `.wuhb` URL from the release layout `tools/publish.sh` produces (`https://github.com/ewanc26/cobalt/releases/download/v<version>/cobalt-<version>.wuhb`, installed to `/wiiu/apps/cobalt.wuhb`, which is where the in-app updater expects it).

`tools/check-hbas-listing.py` validates it and `tools/check-hbas-listing-selftest.sh` breaks each rule on purpose. CI runs all three on every push and uploads the folder as the `hbas-listing` artifact.

The screenshots are rendered on a PC from fixture data (the "Alice Example" account in the mock server), not captured on a console. The pull request says so.

## Submitting

With the GitHub CLI logged in as me, after a release has been cut with `tools/release.sh`:

```sh
tools/submit-hbas.sh            # the version in src/util/version.h
tools/submit-hbas.sh 0.6.0      # or name it
tools/submit-hbas.sh --dry-run  # stop before anything is pushed
```

It checks the release has `cobalt-<version>.wuhb`, downloads the artifact CI built on the tagged commit, validates it, forks `fortheusers/wiiu-hbas-repo`, adds or updates `packages/Cobalt` on a branch and opens the pull request with the details filled in. Later releases are the same command.

## The seed

A store install is only the `.wuhb`. Cobalt needs a per-console entropy seed before it will go online, and a seed shipped inside the package would be the same on every console, so the package never carries one. On first run Cobalt asks you to scribble on the GamePad to make one (see the README and AGENTS.md section 13). That path has only been tested on the host.
