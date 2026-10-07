<p align="center">
  <img src="docs/logo.svg" alt="Cobalt" width="420">
</p>

<p align="center">
  <a href="https://github.com/ewanc26/cobalt/actions/workflows/ci.yml"><img src="https://github.com/ewanc26/cobalt/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="https://github.com/ewanc26/cobalt/releases/latest"><img src="https://img.shields.io/github/v/release/ewanc26/cobalt?sort=semver" alt="Latest release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/ewanc26/cobalt?label=licence" alt="AGPL-3.0"></a>
  <a href="https://github.com/sponsors/ewanc26"><img src="https://img.shields.io/github/sponsors/ewanc26?logo=githubsponsors&logoColor=white&label=sponsors" alt="Sponsor"></a>
</p>

# Cobalt

A native AT Protocol / Bluesky client for the Nintendo Wii U, built as Aroma
homebrew with devkitPro/WUT and SDL2.

**Version 0.9.0**

Cobalt exists because I looked at a Wii U's weak little PowerPC tri-core and
decided that, somehow, it was going to post to Bluesky.

That is not a metaphor for the project. I deliberately wanted to make AT
Protocol work on hardware that was never designed for it, and I wanted to do
it properly rather than turning the Wii U into a thin browser target.

Cobalt treats the Wii U as the platform it is rather than as a browser target:
the TV and GamePad are both first-class displays, with support for normal
two-screen use and Off-TV Play.

Cobalt uses [Wolfram](https://github.com/ewanc26/wolfram), my C AT Protocol
SDK, for its AT Protocol implementation. The project sits alongside the rest
of my mineral and material-named projects and deliberately echoes
[Channel Blue](https://github.com/ewanc26), the Wii counterpart.

## Status

**Usable on real hardware.**

Cobalt is installed and running on a Wii U, and the core Bluesky workflow is
usable end-to-end. Host-side compilation, unit tests, link checks, mock-PDS
tests and the desktop simulator provide additional coverage, but they do not
replace testing on the console.

### Implemented

- TV + GamePad and Off-TV Play
- Diagnostics covering paths, networking, TLS, Wolfram and session state
- App-password sign-in with an on-screen keyboard
- Browser-based OAuth sign-in through a hosted Wolfram OAuth node: Cobalt shows a pairing link, you open it on a phone or computer, and the PDS handles your password and MFA
- Persistent encrypted sessions and sign-out
- Updates from this repository's releases, only when you ask and confirm
- Home timeline with paging, reposts and threads
- Posting, replies, quote posts and reply gates
- Threads: write up to eight posts in a row and publish them as one thread
- Likes and reposts, including undo
- Notifications and mark-as-seen
- Profiles, follows and unfollows
- Followers, following and profile tabs
- Pinned posts
- Links shown as a QR code (Wolfram's encoder), so a phone can open what the console cannot
- Avatars
- Post, reply and quote images with alt text
- A full-size image viewer, from the post menu, on both screens
- Who liked or reposted a post, from the post menu
- Link-card previews
- Actor search
- Custom feed browsing
- Read-only lists and list members
- Mute and block, including browsable lists
- Post search

### Deliberately not planned

Some Bluesky features do not currently fit Cobalt's Wii U target:

- **Video and GIFs** — there is no suitable decoder in the current dependency
  and performance budget.
- **Push notifications** — there is no modern push service available to a Wii U
  homebrew application.

These are platform constraints rather than features merely waiting in the
backlog. What is done and what is open, next to Indigo and Platinum, is in
[docs/PARITY.md](docs/PARITY.md).

## Requirements

Cobalt requires a Wii U with [Aroma](https://aroma.foryour.cafe/) installed.
It does not install or facilitate the exploit required to run Aroma.

For networked AT Protocol features, you need one of two ways to sign in:

- A Bluesky **app password**. Cobalt does not use your normal account
  password.
- An OAuth sign-in through a hosted
  [Wolfram OAuth node](https://github.com/ewanc26/wolfram/blob/main/docs/oauth-node.md).
  Put the node's URL in the service field, leave the app password empty, and
  open the pairing link Cobalt shows on another device. Someone has to run the
  node; Cobalt does not do OAuth on the console itself.

## Installing

The normal WUHB installation is:

```
sd:/wiiu/apps/cobalt.wuhb
```

For a complete bundle, `make bundle` creates:

```
dist/wiiu/
└── apps/
    ├── cobalt.wuhb
    └── cobalt/
        └── entropy.bin
```

The entropy seed is generated for that installation and must not be shared
between consoles.

If you install only the `.wuhb` (from a release, or from the Homebrew App Store
once Cobalt is listed there) there is no seed, and Cobalt will not go online
without one. The first time it starts it asks you to scribble on the GamePad
screen until a bar fills, builds a seed from that, saves it, and asks you to
start it again. I have not measured how much randomness a scribble carries, so
treat that as better than nothing rather than as a proof; `make bundle` is still
the better way. I have only run this on the host, not on a console.

I have prepared a [Homebrew App Store listing](docs/homebrew-app-store.md) but it is not submitted yet, so Cobalt is not in the store at the moment.

Cobalt can also be loaded in Cemu for development. Cemu is useful for broad
application and UI checks, but it does not reproduce every GamePad, networking,
TLS or hardware condition of a real Wii U.

## Updating

Home, then Updates, checks this repository's latest GitHub release. If there is
a newer one it shows the version and notes and does nothing until you press A.
The download is checked against the SHA-256 published in the release before it
is used, and it replaces `cobalt.wuhb` when you quit Cobalt; start it again from
the Wii U Menu. The previous build is kept as
`sd:/wiiu/apps/cobalt/update/cobalt.wuhb.old` until the new one has started, and
if an update is interrupted the next launch puts it back.

Each release's `update.json` is signed. A GitHub Actions job signs it with an
Ed25519 key that exists only as a repository secret and attaches
`update.json.sig`; Cobalt checks that signature against the public key built in
(`src/update/update_key.h`) before it reads the manifest, and refuses a release
that has no signature or a wrong one. The manifest carries the file's SHA-256,
so the signature covers the download too. Releases up to 0.5.0 are not signed,
so an older Cobalt that predates this check still takes them on the SHA-256
alone. I have only run this on the host, not on a console. Installing a `.wuhb`
by hand still works exactly as above.

## Using

Cobalt starts on the Home menu. Sign in from there; [Requirements](#requirements)
covers the two ways in. Every screen names its own controls in the footer, and on
the GamePad each of those prompts is also a button you can tap. The ones that mean
the same thing everywhere:

| Control | Does |
|---|---|
| D-pad up and down | move the selection |
| D-pad left | like the selected post |
| D-pad right | repost the selected post |
| A | open or confirm |
| B | back |
| Y | the selected post's menu: the author's profile, its images, quote, who liked or reposted it |
| X | write a new post from the timeline |
| + | reload the timeline |
| Touch | select a row, or press a footer prompt; drag a finger up or down a list to scroll it, a row for every row's height dragged |

Aroma keeps the HOME button for itself, so Cobalt never sees it. Quit from
Cobalt's own Home menu instead. The same menu switches between TV + GamePad and
Off-TV Play.

The [user guide](docs/guide.md) goes through each screen with screenshots.

## Authentication and storage

Cobalt signs in with a Bluesky app password, through
`com.atproto.server.createSession`, or through OAuth on a Wolfram OAuth node,
as described under [Requirements](#requirements). Either way, the session it
gets back is stored the same way.

Persistent state is kept under:

| File | Contents |
|---|---|
| `sd:/wiiu/apps/cobalt/session.dat` | Encrypted PDS session |
| `sd:/wiiu/apps/cobalt/device.key` | Per-installation encryption key |
| `sd:/wiiu/apps/cobalt/entropy.bin` | Per-installation entropy seed |
| `sd:/wiiu/apps/cobalt/cobalt.log` | Debug log |

Signing out overwrites the session and key before removing them.

The session encryption is deliberately limited by the Wii U's security model:
the homebrew environment provides no application-accessible keystore, so the
key lives alongside the encrypted session. It protects against incidental
exposure of the session file, not someone who has the whole SD card.

### Entropy

The Wii U's available mbedTLS entropy source is not suitable for Cobalt's
cryptographic needs. Cobalt therefore provisions 64 bytes of entropy per
installation and uses its own deterministic generator for subsequent draws,
while also providing the required entropy to Wolfram and its TLS transport.

The seed is rotated on boot. A missing seed prevents network authentication
rather than silently falling back to weaker randomness.

## Building

### Requirements

- devkitPro with devkitPPC and the WUT SDK
- the Wii U SDL2, SDL2_ttf and SDL2_image portlibs
- Wii U curl and mbedTLS
- a sibling checkout of [Wolfram](https://github.com/ewanc26/wolfram)
- OpenSSL for generating the per-installation entropy seed

### Building Wolfram and Cobalt

Build Wolfram for Wii U first, then build Cobalt:

```sh
git clone https://github.com/ewanc26/wolfram ../wolfram

cd ../wolfram
cmake -S . -B build-wiiu \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/.devdeps/wiiu.cmake \
  -DWOLFRAM_BUILD_WIIU=ON \
  -DWOLFRAM_BUILD_TESTS=OFF \
  -DWOLFRAM_BUILD_EXAMPLES=OFF
cmake --build build-wiiu -j8 --target wolfram

cd ../cobalt
make
```

Cobalt looks for Wolfram at `../wolfram/build-wiiu` by default. Set
`WOLFRAM_ROOT` and `WOLFRAM_BUILD` to override those paths.

A Cobalt build can technically be produced without Wolfram, but the
AT Protocol functionality is then omitted. A normal networked build should
therefore have Wolfram available.

### Build targets

| Command | Purpose |
|---|---|
| `make` | Build the Wii U application |
| `make bundle` | Build the application and create a per-installation entropy seed |
| `make test` | Run the host-side test suite |
| `make cacert` | Refresh the bundled TLS trust store |
| `make run` | Print the expected installation/push location |
| `make clean` | Remove generated build and test output |

## TLS trust store

The Wii U curl port does not provide a system certificate store that Cobalt
can rely on. The build therefore fetches a Mozilla CA bundle into
`romfs/cacert.pem`.

The bundle is generated rather than committed, so it can be refreshed as the
Mozilla trust set changes:

```sh
make cacert
```

An offline build can still complete without the bundle, but HTTPS requests
will fail certificate verification on the console.

## Testing

There is no complete Wii U emulator in Cobalt's workflow, so the repository
keeps platform-independent checks on the build machine and uses the real
console for hardware-specific behaviour.

```sh
make test
```

This runs the host compile sweep, Wolfram link check when a host Wolfram build
is available, and the unit tests.

The test suite covers platform-independent logic including credential storage,
service URL handling, keyboard text handling, feed and notification parsing,
post layout, image-cache behaviour and other application logic. The Wolfram
configuration is also syntax-checked when a sibling Wolfram checkout is
available.

Additional checks are available when the corresponding Wolfram host build is
present:

```sh
make -C tests e2e
make -C tests snapshot
make -C tests sim
```

The live simulator runs the real application and renderer in two Wii U-sized
windows against a mock PDS. It is useful for UI work without a console, but
complements rather than replaces hardware testing.

See [tests/README.md](tests/README.md) for the host-test requirements and
individual targets.

## Repository layout

```
src/
├── main.c        entry point and frame loop
├── app/          screens and application state
├── ui/           rendering, theme and on-screen keyboard
├── input/        VPAD and controller input
├── net/          Wii U network status and HTTP
├── atproto/      Wolfram-backed session and protocol integration
├── cache/        credential storage
└── util/         logging, paths, entropy and time handling
tools/             asset generation and trust-store fetching
tests/             host checks, unit tests and simulator
romfs/             bundled application data and fonts
```

[AGENTS.md](AGENTS.md) contains the deeper architectural notes and platform
constraints behind the implementation.

## Accessibility

The GamePad is a first-class display and input surface, including touch input
and a dedicated on-screen keyboard.

The footer prompts are controls rather than captions: on the GamePad, tapping a
hint pill presses the button it names. That is what makes them readable by
someone holding the pad instead of memorised by someone using a Pro Controller
with the GamePad out of view.

Image alt text is surfaced directly in the UI because Wii U homebrew does not
provide Cobalt with a usable system screen reader. Images with alt text are
marked with an explicit **ALT** badge, and the focused post can display the
first image's alt text as a caption.

The post menu's "View image" opens a picture contain-fitted to the whole
surface — the TV's 720p where the card thumbnail was a fraction of that —
with the alt text beneath and Left/Right cycling when a post carries several.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) before making changes. Keep changes
focused, preserve the existing platform architecture, and run the relevant
host-side checks before opening a pull request.

## Licence

Cobalt is licensed under the GNU Affero General Public License v3.0. See
[LICENSE](LICENSE).

Bundled fonts and icon assets have their own licences where applicable; see
[romfs/FONTS.md](romfs/FONTS.md) for attribution and licence details.
