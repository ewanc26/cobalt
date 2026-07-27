# Cobalt

A native Wii U homebrew client for [Bluesky](https://bsky.app)/AT Protocol, built for the [Aroma](https://aroma.foryour.cafe/) homebrew environment.

Cobalt is a sibling project to [Channel Blue](https://github.com/ewanc26) (a Wii homebrew Bluesky client, currently pre-alpha) — same idea, a parallel attempt on hardware with a much friendlier development flow than the original Wii. It's not a port; UI, networking and platform code are built fresh against the Wii U's tools (WUT, SDL2, curl/mbedTLS).

## Status

Cobalt is early and under active development, tested only against real Wii U hardware (there is no Cemu/emulator step in this project's workflow). Right now:

- The app boots, handles GamePad/Pro Controller input, and renders a native-style menu on both the TV and GamePad screens, including Off-TV Play (GamePad-only) mode.
- A **Diagnostics** screen reports asset path resolution, network status, linked library versions, and AT Protocol SDK status — useful for confirming a build is sound on a given console.
- **Sign in** and **Timeline** are visible in the menu but intentionally disabled ("Coming soon") — the AT Protocol session/feed layer isn't wired up yet. See `AGENTS.md` for the build order this project follows.

If you're looking for a client you can actually post from today, this isn't it yet. If you want to track progress or help test on hardware, read on.

## Requirements

- A Wii U with [Aroma](https://aroma.foryour.cafe/) installed. **Cobalt does not include or facilitate any console exploit** — get Aroma set up first, following its own documentation, before installing Cobalt.
- To build from source: [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `wut` (Wii U Toolchain) package, plus the `wiiu-sdl2`, `wiiu-sdl2_ttf`, `wiiu-curl`, and `wiiu-mbedtls` portlibs (installable via devkitPro's `pacman`).
- Optionally, [Wolfram](https://github.com/ewanc26) (Ewan's AT Protocol C SDK) checked out as a sibling directory at `../wolfram` with a Wii U build (`../wolfram/build-wiiu`). Cobalt builds and boots without it — the AT Protocol layer just reports itself as unavailable — but it's required for anything session/feed-related.

## Building

```sh
export DEVKITPRO=/opt/devkitpro   # adjust to your install
export DEVKITPPC=$DEVKITPRO/devkitPPC

make          # builds build/cobalt.wuhb
make bundle   # also stages dist/wiiu/apps/cobalt/, generating a per-install entropy seed
```

`make bundle` is the one you want for an installable copy: it generates a fresh, per-installation 64-byte entropy seed (`entropy.bin`) alongside the `.wuhb`. This seed is not distributable — it's specific to one console's install and must never be shared between installs or committed to the repository (see `AGENTS.md` §13 for why).

## Installing

Copy `dist/wiiu/apps/cobalt/` (the whole directory, including `entropy.bin`) to `sd:/wiiu/apps/` on a Wii U with Aroma installed, then launch Cobalt from the Wii U Menu / Aroma's app list. Alternatively, push the same directory over Aroma's FTP server during development.

## Known limitations

- **No OAuth.** Signing in will use app passwords (`com.atproto.server.createSession`) rather than a full browser-based OAuth flow — there's no practical way to host an OAuth redirect target from a Wii U. If you've disabled password-based login on your Bluesky account, Cobalt won't be able to sign you in yet.
- **Text input is slow.** Composing via the Wii U's software keyboard is not a fast experience; this is a platform constraint rather than something Cobalt can work around entirely.
- **Font is a placeholder.** The bundled UI font (Lato Regular) is a legible stand-in, not the final rounded, Wii-U-native look this project is aiming for — see `romfs/FONTS.md`.

## Contributing / development

See `AGENTS.md` for the full project brief: target platform details, architecture, coding conventions, and the current build order. It's written for an AI coding agent working on the repo, but it's equally the best orientation document for a human contributor.

## License

See `LICENSE`. Bundled third-party assets (fonts) carry their own licenses — see `romfs/FONTS.md` and the accompanying license text shipped alongside each font.
