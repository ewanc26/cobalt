<p align="center">
  <img src="assets/icon.png" alt="Cobalt icon" width="128">
</p>

# Cobalt

A native AT Protocol / Bluesky client for the Nintendo Wii U, built as Aroma
homebrew with devkitPro/WUT and SDL2.

**Version 0.3.1**

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
- Persistent encrypted sessions and sign-out
- Home timeline with paging, reposts and threads
- Posting, replies, quote posts and reply gates
- Likes and reposts, including undo
- Notifications and mark-as-seen
- Profiles, follows and unfollows
- Followers, following and profile tabs
- Pinned posts
- Avatars
- Post, reply and quote images with alt text
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
- **OAuth sign-in** — the browser-based redirect flow is not a good fit for
  this application.

These are platform constraints rather than features merely waiting in the
backlog.

## Requirements

Cobalt requires a Wii U with [Aroma](https://aroma.foryour.cafe/) installed.
It does not install or facilitate the exploit required to run Aroma.

For networked AT Protocol features, you need a Bluesky **app password**.
Cobalt does not use your normal account password.

Building Cobalt requires:

- devkitPro with devkitPPC and the WUT SDK
- the Wii U SDL2, SDL2_ttf and SDL2_image portlibs
- Wii U curl and mbedTLS
- a sibling checkout of [Wolfram](https://github.com/ewanc26/wolfram)
- OpenSSL for generating the per-installation entropy seed

## Building

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

Cobalt can also be loaded in Cemu for development. Cemu is useful for broad
application and UI checks, but it does not reproduce every GamePad, networking,
TLS or hardware condition of a real Wii U.

## Authentication and storage

Cobalt authenticates with Bluesky app passwords through
`com.atproto.server.createSession`.

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

Image alt text is surfaced directly in the UI because Wii U homebrew does not
provide Cobalt with a usable system screen reader. Images with alt text are
marked with an explicit **ALT** badge, and the focused post can display the
first image's alt text as a caption.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) before making changes. Keep changes
focused, preserve the existing platform architecture, and run the relevant
host-side checks before opening a pull request.

## Licence

Cobalt is licensed under the GNU General Public License v3.0. See
[LICENSE](LICENSE).

Bundled fonts and icon assets have their own licences where applicable; see
[romfs/FONTS.md](romfs/FONTS.md) for attribution and licence details.
