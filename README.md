# Cobalt

A native AT Protocol / Bluesky client for the Nintendo Wii U, built as Aroma
homebrew with devkitPro/WUT and SDL2.

Cobalt is designed as a Wii U application rather than a web client squeezed
onto an old browser. The TV and GamePad are both first-class displays, with
support for normal two-screen play and Off-TV Play.

Cobalt is part of Ewan's AT Protocol projects, alongside
[Wolfram](https://github.com/ewanc26/wolfram), the SDK that provides its
wire-level AT Protocol implementation. The name follows the same mineral and
material naming convention as Wolfram, Malachite, Tourmaline, Inkwell and
Bismuth, while deliberately echoing [Channel Blue](https://github.com/ewanc26),
the Wii counterpart.

## Status

**Usable on real hardware.**

Cobalt is installed and running on a Wii U, and the core Bluesky client
workflow is usable end-to-end. It has also been through host-side compilation,
unit tests, link checks and the desktop simulator. Hardware remains the final
place to catch console-specific issues, but Cobalt is no longer a purely
host-tested project.

### Implemented

- TV + GamePad and Off-TV Play layouts
- Diagnostics screen covering paths, networking, TLS, SDK and session state
- App-password sign-in with an on-screen keyboard
- Encrypted session persistence and sign-out
- Home timeline with paging, reposts and threads
- Replies, posting, quote posts and reply gates
- Likes and reposts, including undo
- Notifications and mark-as-seen
- Profiles, follows and unfollows
- Followers, following and profile tabs
- Pinned posts
- Post, reply and quote images with alt text
- Link-card previews
- Avatars throughout posts, replies, notifications and profiles
- Actor search
- Custom feed browsing
- Read-only lists and list members
- Mute and block, including browsable lists
- Post search

### Deliberately not planned

Some Bluesky features do not have a realistic implementation path on the
Wii U:

- **Video and GIFs** — there is no suitable decoder available for this target
  at the performance and dependency budget Cobalt is working with.
- **Push notifications** — the Wii U has no service Cobalt can register with
  for modern push delivery.
- **OAuth sign-in** — the console has nowhere sensible to host the redirect
  target, making the browser-based flow a poor fit for this application.

These are platform constraints, not simply items that have not been reached yet.

## Requirements

Cobalt requires a Wii U with [Aroma](https://aroma.foryour.cafe/) already
installed. It does not install or facilitate the exploit required to get
Aroma running.

For networked AT Protocol features you also need a Bluesky **app password**.
Cobalt does not use or store your normal account password.

The development environment requires:

- devkitPro with devkitPPC and the WUT SDK
- Wii U SDL2, SDL2_ttf and SDL2_image
- Wii U curl and mbedTLS
- a sibling checkout of [Wolfram](https://github.com/ewanc26/wolfram) for
  AT Protocol functionality
- OpenSSL on the build machine for generating the per-installation entropy
  seed

## Building

Install the Wii U toolchain and port libraries through devkitPro, then build
Wolfram first:

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
make bundle
```

Cobalt looks for Wolfram at `../wolfram/build-wiiu` by default. The paths
can be overridden with `WOLFRAM_ROOT` and `WOLFRAM_BUILD`.

Without a built Wolfram checkout, Cobalt still builds, but AT Protocol
functionality is disabled and the diagnostics screen reports that state.

### Build targets

| Command | Purpose |
|---|---|
| `make` | Build the Wii U application |
| `make bundle` | Build and create a per-installation entropy seed |
| `make test` | Run host-side checks |
| `make cacert` | Refresh the bundled TLS trust store |
| `make clean` | Remove build and generated output |
| `make run` | Print the expected installation/push location |

## Installing

For a normal installation, copy `cobalt.wuhb` to:

```
sd:/wiiu/apps/
```

For a complete installation built with `make bundle`, copy the whole
`dist/wiiu` tree instead. The bundle includes an entropy seed that is
required for cryptographic operations on the Wii U.

Cobalt can also be loaded in Cemu for development. Cemu is useful for checking
the application loop and broad UI behaviour, but it does not reproduce every
GamePad, networking or TLS condition of a real console.

## Authentication and storage

Cobalt currently authenticates with Bluesky app passwords through
`com.atproto.server.createSession`.

The session is stored in:

| File | Contents |
|---|---|
| `sd:/wiiu/apps/cobalt/session.dat` | Encrypted PDS session |
| `sd:/wiiu/apps/cobalt/device.key` | Key used to encrypt the session |
| `sd:/wiiu/apps/cobalt/entropy.bin` | Per-installation entropy seed |
| `sd:/wiiu/apps/cobalt/cobalt.log` | Debug log |

Signing out overwrites the session and key files before removing them.

The session encryption is intentionally modest in its threat model. The Wii U
does not provide a homebrew-accessible keystore, so the key necessarily lives
alongside the encrypted session. It protects against incidental exposure of
the session file; it does not protect an attacker who has the entire SD card.

### Entropy

The Wii U does not expose a suitable application-facing cryptographically
secure random source to homebrew. Cobalt therefore provisions a unique
64-byte seed when `make bundle` is run and feeds that into Wolfram's
deterministic random generator.

The seed is unique to each installation and must not be shared between
consoles. Cobalt rotates it at boot. A missing seed prevents sign-in rather
than silently falling back to weaker randomness.

## TLS trust store

The Wii U build of curl uses mbedTLS, but the console does not provide a
system certificate store that Cobalt can rely on. The build therefore fetches
a CA bundle into `romfs/cacert.pem`.

The bundle is deliberately generated rather than committed so that an old
certificate set does not remain in the repository indefinitely. If HTTPS
connections start failing after a long period, run:

```sh
make cacert
```

An offline build can still complete without the trust store; network requests
will simply fail verification on the console.

## Testing

There is no complete Wii U emulator in Cobalt's workflow, so the repository
keeps the cheap checks on the build machine.

```sh
make test
make -C tests sweep
make -C tests linkcheck
make -C tests check
```

The test suite covers:

- host-side syntax checking with warnings treated as errors
- Cobalt's platform-independent unit tests
- linking the Wolfram-enabled configuration
- credential-store round trips and corruption handling
- service URL normalisation
- the on-screen keyboard text model

The test harness also provides a live desktop simulator:

```sh
make -C tests sim
```

It renders the application in TV and GamePad-sized windows and can exercise
the UI against a mock PDS. It complements the real-console testing rather than
replacing it.

## Layout

```
src/
├── main.c        entry point and frame loop
├── app/          screens and application state
├── ui/           rendering, theme and on-screen keyboard
├── input/        VPAD and controller input
├── net/          Wii U network status
├── atproto/      Wolfram-backed session and protocol integration
├── cache/        credential storage
└── util/         logging, paths and entropy
tools/            asset generation and trust-store fetching
tests/            host checks, unit tests and simulator
romfs/            bundled application data and fonts
```

`AGENTS.md` contains the project's deeper architectural notes, platform
constraints, implementation decisions and working rules.

## Accessibility

Cobalt treats the GamePad as a first-class display rather than a controller
peripheral. Touch input, readable card layouts and explicit alt-text
presentation are part of the application itself.

The Wii U homebrew environment does not provide Cobalt with a usable system
screen reader, so image alt text is surfaced directly in the UI. Images also
receive an explicit **ALT** marker when alternative text is available.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) before making changes. Contributions
should preserve the existing architecture and platform constraints, and
changes should include the relevant host-side verification.

## Licence

Cobalt is licensed under the GNU General Public License v3.0. See
[LICENSE](LICENSE).

Bundled fonts and icon assets may carry their own licences; see
[romfs/FONTS.md](romfs/FONTS.md) for their attribution and licence details.
