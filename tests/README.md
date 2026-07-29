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
make -C tests            # compile sweep + unit tests
make -C tests check      # unit tests only
make -C tests sweep      # compile sweep only
```

Requires host `libsdl2-dev`, `libsdl2-ttf-dev`, `libmbedtls-dev` and a cJSON
header. A sibling `../wolfram` checkout is picked up automatically if present,
which is what makes the sweep able to check Cobalt's calls against the real SDK
signatures; without it the sweep runs in the no-Wolfram configuration only.

Neither the sweep nor the unit tests are a substitute for running the build on
the console. They only rule out the failures that do not need hardware to find.
