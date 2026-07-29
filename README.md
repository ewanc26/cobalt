# Cobalt

A native AT Protocol / Bluesky client for the Nintendo Wii U, built as Aroma
homebrew with devkitPro/WUT and SDL2.

It is meant to feel like a Wii U app that happens to show Bluesky content —
rounded tiles, the system's blues and whites, a real GamePad layout — rather
than the Bluesky website rendered on a TV through the console's ageing browser.
Both screens are first-class: TV plus GamePad together, and Off-TV Play with the
GamePad on its own.

Cobalt is named after a blue mineral, following the naming convention across
Ewan's other projects (Wolfram, Malachite, Tourmaline, Inkwell, Bismuth), and
deliberately echoing [Channel Blue](https://github.com/ewanc26), its Wii
counterpart.

## Status

**Early, but usable in shape.** Reading, posting and interacting all exist;
nothing has been run on a Wii U yet.

| | |
|---|---|
| Boots on real hardware, exits cleanly to the Wii U Menu | done |
| TV + GamePad and Off-TV Play layouts | done |
| Diagnostics screen (paths, network, TLS, session) | done |
| Sign in with an app password, on-screen keyboard | done |
| Session saved across boots, sign out | done |
| Timeline — text posts, reposts, paging | done |
| Threads, with replies and re-rooting | done |
| Like and repost, with undo | done |
| Posting and replying | done |
| Notifications, with mark-as-seen | done |
| Profiles, follow and unfollow | done |
| Images, avatars, link cards | next — markers for now |
| Search, custom feeds, lists, mutes and blocks | not started |
| Video, GIFs, DMs, push notifications | not planned — see below |

**Nothing here has run on a Wii U yet.** It builds, and an extensive host test
suite passes, but every milestone's real acceptance test is the console and none
of them have had one. Treat the "done" column as "written and host-tested".

Embedded media is shown as a marker — a post with a picture reads `[image]`
rather than displaying it. Fetching and decoding blobs is real work that has
not been done, and silently dropping the embed would make an image-only post
look like an empty card.

Some things in the official client are not coming, because the console cannot
do them rather than because nobody has got to them yet: **video** and **GIFs**
(no decoder, and no realistic path to one at this clock speed), **push
notifications** (no service the console can register with), and **OAuth
sign-in** (nowhere to host a redirect target — see Authentication below).

## Requirements

You need a Wii U that **already** has [Aroma](https://aroma.foryour.cafe/)
installed. Cobalt does not install or facilitate the exploit that gets you
there, and never will — it assumes you have already done that yourself.

You also need a **Bluesky app password**, not your account password. Create one
under Settings → Privacy and Security → App Passwords. Accounts with two-factor
authentication enabled cannot use app passwords; see "Authentication" below.

## Installing

Copy `cobalt.wuhb` to `sd:/wiiu/apps/` on the console's SD card, then launch it
from the Wii U Menu (Aroma shows homebrew alongside your installed titles).

If you build it yourself, use `make bundle` and copy the whole `dist/wiiu` tree
across — that carries the per-installation entropy seed described below, which
the app needs and which is not part of the `.wuhb`.

## Building

```sh
# Toolchain (once)
sudo dkp-pacman -S wiiu-dev wiiu-sdl2 wiiu-sdl2_ttf wiiu-curl wiiu-mbedtls

# Wolfram — the AT Protocol SDK, as a sibling checkout.
# Needs a version with wf_xrpc_client_set_tls_rng(); an older one will fail to
# compile rather than quietly build without the TLS randomness fix.
git clone https://github.com/ewanc26/wolfram ../wolfram
cd ../wolfram
cmake -S . -B build-wiiu \
   -DCMAKE_TOOLCHAIN_FILE=$PWD/.devdeps/wiiu.cmake \
   -DWOLFRAM_BUILD_WIIU=ON -DWOLFRAM_BUILD_TESTS=OFF -DWOLFRAM_BUILD_EXAMPLES=OFF
cmake --build build-wiiu -j8 --target wolfram
cd -

# Cobalt
make bundle
```

`make` picks Wolfram up automatically from `../wolfram/build-wiiu`. Without it
Cobalt still builds and boots, but every ATProto feature is disabled and the
diagnostics screen says so.

Other targets:

| Target | What it does |
|---|---|
| `make` | Build `cobalt.wuhb` |
| `make bundle` | Build, plus a per-installation entropy seed, into `dist/` |
| `make test` | Host compile sweep and unit tests — see `tests/README.md` |
| `make cacert` | Re-fetch the bundled TLS trust store |
| `make clean` | Remove build output |

### The TLS trust store

devkitPro's `wiiu-curl` is built against mbedTLS, and the Wii U has no system
certificate store behind it. Without an explicit CA bundle every HTTPS request
fails verification, so `make` fetches one into `romfs/cacert.pem` via
`tools/fetch_cacert.sh` and Cobalt points Wolfram at it.

It is fetched rather than committed, because the Mozilla set expires and a stale
bundle in git fails on console in a way that reads as a network bug. If sign-in
starts failing with a connection error some months from now, `make cacert` is
the first thing to try. An offline build still succeeds — it just cannot reach a
PDS, and the diagnostics screen reports the trust store as missing.

## Authentication

Cobalt signs in with **app passwords** (`com.atproto.server.createSession`).

The browser-redirect OAuth flow that modern Bluesky clients use is not practical
here: there is nowhere on a console to host a redirect target, and driving an
authorization-code flow through the Wii U's browser and keyboard would be worse
than the thing it replaces. This is a real limitation rather than a temporary
gap — if you have two-factor authentication enabled on your account, app
passwords will not work for you and neither will Cobalt.

Typing happens on Cobalt's own on-screen keyboard rather than the system swkbd
overlay: touch on the GamePad, or the D-pad and A from a Pro Controller.

### What is stored, and where

| File | Contents |
|---|---|
| `sd:/wiiu/apps/cobalt/session.dat` | Your PDS session, encrypted |
| `sd:/wiiu/apps/cobalt/device.key` | The key that file is encrypted under |
| `sd:/wiiu/apps/cobalt/entropy.bin` | Entropy seed — required, see below |
| `sd:/wiiu/apps/cobalt/cobalt.log` | Debug log |

Signing out overwrites `session.dat` and `device.key` before deleting them, so
the tokens are not left recoverable in the card's free space.

Be clear-eyed about what that encryption is worth: the key sits next to the file
it protects. It stops your session leaking incidentally — from a log, a
screenshot of the card, a stray copy of `session.dat` — but anyone holding the
whole SD card has both halves. The Wii U gives homebrew no keystore and no
per-title secret to bind a key to, so there is nothing better available. Treat
the card as you would treat a logged-in device.

### The entropy seed

**Cobalt will not connect to anything without one.** This is the part of the
setup most likely to trip you up, so it is worth understanding why.

The Wii U has no application-facing cryptographically secure random number
generator. devkitPro's mbedTLS does provide `mbedtls_hardware_poll`, but it is
`srand(OSGetSystemTick())` followed by `rand()`, once per byte — a timer-seeded
libc PRNG, not an entropy source — and the portlib is built with
`MBEDTLS_NO_PLATFORM_ENTROPY`, so nothing sits behind it. Everything drawing on
that pool is a function of how long the console has been switched on.

That is not only a signing problem. It also covers the random values in every
HTTPS handshake, which is why Cobalt supplies its own generator to libcurl
rather than letting it use mbedTLS's.

`make bundle` generates a 64-byte seed into `dist/wiiu/apps/cobalt/entropy.bin`,
which Cobalt reads at boot and rotates for the next one. It is **one per
installation and must not be shared** — the generators are deterministic, so a
common seed would give every console identical key material and identical
handshake values. It is git-ignored for the same reason.

If the seed is missing, Cobalt boots but refuses to sign in and tells you so on
the Diagnostics screen. That is deliberate. It could connect anyway, and the
connection would look completely normal to you while being far weaker than it
appears — so it doesn't.

This is also why copying `cobalt.wuhb` on its own is not enough; copy the whole
`dist/wiiu` tree.

## Debugging on hardware

There is no emulator in this project's workflow, so Cobalt logs loudly. The
fastest loop is UDP:

```sh
nc -ul 4405
```

from any machine on the same network as the console. The same lines also go to
Cafe OS, Aroma's logging module and `sd:/wiiu/apps/cobalt/cobalt.log`, which
survives a hang.

The **Diagnostics** screen on the console itself reports the things that block
everything else: which content root the assets were found under, whether the
network came up and at what address, whether the trust store is present, the
linked SDL/curl/TLS versions, the ATProto SDK's status, and the current session.

## Layout

```
src/
├── main.c        entry point and frame loop
├── app/          screens and application state
├── ui/           rendering, theme, on-screen keyboard
├── input/        VPAD and controller input
├── net/          nn::ac network status
├── atproto/      Wolfram-backed session and XRPC
├── cache/        credential storage
└── util/         logging, paths, entropy
tools/            asset generation, trust store fetch
tests/            host compile sweep and unit tests
```

`AGENTS.md` is the full design document — platform constraints, decisions
already made, and what is deliberately not being done.

## Licence

See `LICENSE`. The bundled font is a placeholder; see `romfs/FONTS.md`.
