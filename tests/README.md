# Host tests

There is no emulator in this project's workflow (AGENTS.md §10), so the only
cheap place to catch a mistake is the build machine. Every run that reaches the
console costs a card swap or an FTP push, a boot, and a manual test pass — this
directory exists to make sure the obvious failures never get that far.

Two things run here, and they are different in kind:

**A compile sweep.** Every translation unit is put through the host compiler
with `-fsyntax-only -Wall -Wextra -Werror`, against the real SDL2, mbedTLS and
Wolfram headers. It does not link and it does not produce a Wii U binary — the
point is that a typo, a changed Wolfram signature, or a missing include is
reported in a second instead of after a five-minute round trip. Files that
depend on WUT (`main.c`, `net/net.c`, `util/log.c`, `util/paths.c`) are skipped,
since `<coreinit/...>`, `<nn/ac.h>` and `<whb/...>` only exist inside devkitPro.

**Actual unit tests.** AGENTS.md §10 allows for this exactly where it applies:
"pure-logic code (ATProto record parsing, cache logic) *can* reasonably be unit
tested if extracted into platform-independent files". So the credential store's
encode/decode round trip, the service-URL normaliser and the keyboard's text
model are tested for real, with assertions, off-console. Anything that touches
GX2, VPAD or the network is not — that is what the hardware pass is for.

## Running

```sh
make -C tests            # sweep + link check + unit tests
make -C tests sweep      # compile sweep only
make -C tests linkcheck  # link the Wolfram configuration
make -C tests check      # unit tests only
```

Requires host SDL2, SDL2_ttf, mbedTLS (`-lmbedcrypto`) and a cJSON header. A
sibling `../wolfram` checkout is picked up automatically if present, which is
what makes the sweep able to check Cobalt's calls against the real SDK
signatures; without it the sweep runs in the no-Wolfram configuration only.

### On macOS

```sh
brew install sdl2 sdl2_ttf sdl2_image cjson libmicrohttpd mbedtls
```

Two things bite on a machine that also has devkitPro installed, and both look
like something other than what they are:

- **devkitPro ships its own `pkg-config`** at `$DEVKITPRO/tools/bin`, and once
  devkitPro is on `PATH` it precedes Homebrew's. It knows nothing about
  `/opt/homebrew/lib/pkgconfig`, so `pkg-config --exists sdl2` fails and the
  sweep reports a missing `SDL_version.h` — which reads like an uninstalled
  dependency rather than a `PATH` ordering problem. Put Homebrew first:

  ```sh
  PATH=/opt/homebrew/bin:$PATH make test
  ```

- **`linkcheck` uses Wolfram's `build-host` by default**, and it links that
  library rather than compiling against current headers. A `build-host` left
  over from before a Wolfram API change fails with undefined `wf_*` symbols that
  have nothing to do with Cobalt. Rebuild it, or point at a current tree:

  ```sh
  make test WOLFRAM_HOST_BUILD=/path/to/wolfram/build-host-fresh
  ```

Note that Homebrew's `sdl2` is `sdl2-compat`; it provides both `sdl2.pc` and
`SDL2.pc`, so it works either way.

**A link check**, because the other two miss a whole class between them. The
sweep never resolves a symbol, and the unit tests link only the without-Wolfram
configuration — so a function deleted while callers remained slips past both if
it lives behind `COBALT_HAS_WOLFRAM`. `linkcheck` links that configuration into
a binary it never runs. It needs a host build of Wolfram:

```sh
cmake -S ../wolfram -B ../wolfram/build-host -DWOLFRAM_BUILD_EXAMPLES=OFF
cmake --build ../wolfram/build-host -j8 --target wolfram
```

and is skipped without one.

None of the three is a substitute for running the build on the console. They
only rule out the failures that do not need hardware to find.

## Live simulator (`make -C tests sim`)

Opens the real app and renderer in two windows sized like the Wii U outputs
(TV 1280x720, GamePad 854x480) so UI work does not need Cemu. Needs the same
Wolfram host build as `e2e`/`snapshot`. It runs against the shared mock PDS
(`fixtures.h`) and signs in as `alice.test`; `make -C tests sim SIM_ARGS=--live`
uses the real network and the app's sign-in screen instead. State goes to
`build/sim`.

Keys: arrows = D-pad, Return/Space = A, Esc/Backspace = B, Tab = +, X, Y.
Left-click in the GamePad window = touch (drag works). F5 saves `sim-tv.bmp` and
`sim-drc.bmp`. A game controller also works. Layout metrics and fonts are the
Wii U ones; only the GPU, SD card and network stack differ.
