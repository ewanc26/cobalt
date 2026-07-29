# AGENTS.md — Cobalt

**Cobalt** is a native Wii U homebrew client for the AT Protocol / Bluesky. It's related to [Channel Blue](https://github.com/ewanc26) (the Wii homebrew Bluesky client) in spirit and naming, but Channel Blue is pre-alpha — it isn't really working yet. Cobalt isn't picking up where a finished Channel Blue left off; it's a parallel attempt on hardware with a noticeably easier development flow than the original Wii. It targets the Aroma homebrew environment and is built with devkitPro/WUT.

This file exists to orient an AI coding agent working on this repository. Read it in full before making changes. If something here conflicts with what you observe in the code, the code wins — update this file to match reality rather than silently diverging from it.

---

## 1. Project Intent

- Give Wii U owners with homebrew installed a native, responsive way to read and post to Bluesky/ATProto, without relying on the Wii U's aging WebKit-based Internet Browser.
- Take advantage of Wii U-specific hardware the Wii didn't have: a second screen (GamePad), a faster CPU/GPU, and more RAM — and a generally friendlier homebrew dev flow than the Wii's, which is a real factor given Channel Blue stalled at pre-alpha.
- Preserve the naming convention used across Ewan's other projects (Wolfram, Malachite, Tourmaline, Inkwell, Bismuth) — this one is **Cobalt**, a blue mineral, deliberately echoing "Channel *Blue*" while fitting the gemstone/mineral pattern.
- Ship something that actually works on real hardware. **Ewan does not have Cemu set up and is developing/testing directly against a real Wii U with Aroma installed** — there is no emulator safety net here. Every milestone's acceptance test is "it works on the actual console."

## 2. Relationship to Channel Blue

Channel Blue is **pre-alpha** — it doesn't really work yet. So this isn't a "port a working app to new hardware" situation; it's closer to taking a second run at the same idea on hardware and tooling that's much less painful to develop for. Treat Channel Blue as a source of *concepts and lessons*, not working code to build on.

Some platform-level reasoning still applies regardless of Channel Blue's state: the Wii (PowerPC "Broadway" CPU, 88 MB RAM, GX for graphics, no second screen) and Wii U (Espresso CPU, larger RAM pool, GX2, GamePad) are different enough that:

- UI layout logic should be rewritten around a **two-mode model**: TV+GamePad together (TV for the primary feed/thread view, GamePad for compose/notifications/navigation) *and* GamePad-only, self-sufficient Off-TV Play — see §5 for details — rather than ported as-is.
- Networking code should be built fresh against WUT's `nsysnet`/socket layer and a modern TLS library (see §6), since the Wii U's transport is curl-over-mbedTLS while the Wii's is a hand-rolled socket/TLS stack on libogc.
- Shared *concepts* (feed rendering approach, ATProto record caching strategy, offline queue for posts made without connectivity) are worth porting; shared *code* mostly isn't, given how different the two platforms' SDKs are.

If you find yourself tempted to reuse or adapt something from Channel Blue, verify carefully rather than assuming it is a solid foundation, and ask Ewan before sinking significant effort into adapting Wii-specific code.

**Correction (see also §13):** the "pre-alpha, doesn't really work" framing above is out of date in one respect that matters. Channel Blue is not a desktop mock — it is a linked MVP candidate that cross-builds to a booting DOL, has a host test suite, and **already uses Wolfram** for XRPC, auth and Wii HTTPS over mbedTLS. What its own AGENTS.md and README leave unverified is the *real-hardware* path (login, TLS, session refresh, live social flows), not whether the code exists.

The practical consequence: Channel Blue's Wii networking is worth reading, not dismissing. Cobalt's entropy provisioning was taken directly from it rather than invented — same load/rotate/save/commit discipline, same file format — precisely so the two projects do not diverge on a security-sensitive detail. Check what Channel Blue already solved before solving it again here.

## 3. Target Platform & Toolchain

- **Hardware:** Nintendo Wii U (Espresso tri-core PowerPC CPU, Latte GPU).
- **Homebrew environment:** [Aroma](https://aroma.foryour.cafe/) — the current, actively maintained Wii U CFW/homebrew environment (successor to Tiramisu). Do not target the old Homebrew Launcher/ELF flow; Aroma + RPX/WUHB is the supported path.
- **Toolchain:** devkitPro + **WUT** (Wii U Toolchain), installed via devkitPro's `pacman`. Executables are built as `.rpx` (native Wii U executable format, a modified ELF) and packaged as `.wuhb` for distribution via the Homebrew App Store / Aroma's Wii U Menu integration.
- **Build system:** devkitPro Makefiles (the de facto standard for WUT projects) with `wut_create_rpx`/`wuhbtool` for packaging. CMake is an acceptable alternative if it simplifies dependency management, but don't mix build systems within the repo.
- **Iteration target:** real hardware only — no Cemu in this workflow. Expect the deploy loop (build → copy to SD or push via Aroma's FTP server → run on console) to be slower than an emulator-based one; budget for that rather than assuming Cemu-speed iteration. Lean on liberal logging (see §9) to make each real-hardware run count, since there's no emulator step to catch obvious mistakes cheaply first.
- **Key WUT headers/subsystems you'll touch:**
  - `coreinit` — core OS functions, threading, memory.
  - `proc_ui`/`ProcUI` — process lifecycle; **must** be handled correctly or the app won't back out to the Wii U Menu cleanly.
  - `vpad` — GamePad input (buttons, touch screen, stylus).
  - `padscore` — Wii Remote/Pro Controller input, if supported as a secondary input method.
  - `nsysnet` / BSD-style sockets — networking.
  - `nn::ac` — network/account configuration status (confirm the console actually has a network connection before making requests).
  - `sysapp` — for launching system applications/returning to the menu if needed.
  - `gx2` — graphics; likely used indirectly via SDL2 rather than directly (see §6).

**Important platform quirk:** Aroma intercepts the HOME button before it reaches homebrew applications — `VPAD_BUTTON_HOME` will not arrive in Cobalt's input handling. Do not build exit/pause flows that assume you can catch HOME; use `ProcUI`'s foreground/background/exit callbacks instead, and provide an in-app way to quit (e.g. a menu option or dedicated button combo).

## 4. Repository Structure

```
cobalt/
├── AGENTS.md                 # this file
├── README.md                 # human-facing project overview
├── Makefile                  # devkitPro build entry point
├── src/
│   ├── main.c / main.cpp     # entry point, ProcUI lifecycle
│   ├── app/                  # application state machine, screen management
│   ├── ui/                   # rendering, layout, TV + GamePad screen composition
│   ├── input/                # VPAD/padscore input handling
│   ├── net/                  # HTTP/TLS client, XRPC request plumbing
│   ├── atproto/              # ATProto lexicon types, session/auth, record (de)serialisation
│   ├── cache/                # local feed/thread/session cache (SD card storage)
│   └── util/                 # logging, error handling, shared helpers
├── assets/                   # fonts, icons, WUHB metadata (icon.png, meta.xml equivalent)
├── romfs/                    # bundled assets shipped inside the RPX/WUHB
├── tools/                    # helper scripts (packaging, deployment to SD/FTP)
├── tests/                    # host compile sweep + unit tests (see §10 and §13)
└── third_party/              # vendored or portlib-pinned dependencies
```

Keep `atproto/` platform-agnostic where realistically possible — it's the part most likely to be useful if a future project needs ATProto logic outside of Cobalt's UI/rendering layer.

## 5. UI & Interaction Model

- **Native look, not a web app in a window.** This is the point of building Cobalt as SDL2/WUT homebrew rather than the browser-applet approach `juxtaposition` takes (see §8): Cobalt should look and feel like it belongs on the Wii U, not like a website rendered on a TV. Concretely, that means matching the Wii U Menu's visual language rather than reaching for flat, web-influenced UI conventions:
  - **Rounded, glassy, slightly skeuomorphic tiles** rather than flat rectangles — the Wii U Menu's whole visual identity is soft-edged tile icons with subtle gradients/highlights and gentle drop shadows, not flat-color cards.
  - **Depth and motion over flatness:** tiles that scale/bounce slightly on focus, smooth slide transitions between screens, and a general sense of tactile responsiveness — closer to the Wii U Menu or Miiverse's native applet feel than to a typical flat mobile-app aesthetic.
  - **Typography:** the system uses Fontworks' Pop Happiness/Pop Joy typeface family for most in-OS UI text. Bundling a close-enough rounded, friendly sans font (rather than a generic system/web sans) will do more for "this feels native" than almost anything else — check licensing before bundling any Fontworks font itself, and have a fallback plan (see the Unicode note below) either way.
  - **Colour:** lean on the Wii U's characteristic blues/whites/light-grey palette as a baseline rather than importing Bluesky's own butterfly-blue web branding wholesale — Cobalt should read as "a Wii U app that happens to show Bluesky content," not "the Bluesky website with a Wii U wrapper."
  - This is a design *direction*, not a mandate to pixel-match the system UI exactly — deliberate departures for readability or the two-screen layout are fine, but they should read as intentional choices within a native aesthetic, not as leftover web-UI defaults.
- **TV screen (primary, when in use):** feed/timeline, thread view, profile view. This is what most users will be looking at during a TV-based session — optimise for readability at typical TV viewing distance, not phone-screen information density.
- **GamePad screen (secondary, or standalone):** best suited to compose (typing via GamePad's own on-screen or the Wii U's software keyboard swkbd), notifications, and quick navigation when paired with the TV, since it's held close and touch-capable. But the GamePad must also work as **the only screen** — Cobalt should support Off-TV Play the way many retail Wii U titles do (Nintendo's own term for this exact "whole game/app playable on the GamePad alone" feature), with a complete, self-sufficient UI on the GamePad for users who want to play with the TV off entirely. Technical specifics worth designing around from the start:
  - The GamePad's screen is a fixed 854×480 (FWVGA, 16:9), notably lower-resolution and a different aspect ratio's worth of usable pixel density than a typical 720p/1080p TV output — GamePad-only layouts need their own type scale and spacing, not the TV layout scaled down, or text will be uncomfortably small or UI elements uncomfortably cramped.
  - This is standard GX2 functionality, not a hack: GX2 supports separate render targets for the TV output and the DRC (GamePad) output in the same frame, so Cobalt can legitimately render two different layouts — full TV view and full GamePad view — from the same app state simultaneously, rather than mirroring one buffer to both screens. Community tools like SwapDRC exploit this same TV/DRC buffer separation from the outside (for games that didn't build in Off-TV Play); Cobalt should support both outputs properly at the source instead of relying on a buffer swap.
  - Practically: don't design any critical function as TV-screen-exclusive; every screen/view needs a real GamePad-native layout built for FWVGA, not just a cropped or scaled-down copy of the TV layout.
  - Treat the TV+GamePad dual-screen layout described above as one *mode*, and GamePad-only as a second, equally-supported mode — not a fallback or an afterthought bolted on later. Worth deciding early whether this is autodetected (e.g. no meaningful TV output present) or user-toggled, since it affects how the render/layout code is structured from the start — a toggle is simpler to build first and still lets a user choose GamePad-only even with a TV connected, which autodetection alone wouldn't cover.
- Support both GamePad touch input and a D-pad/button-based navigation path in every mode — some users play with a Pro Controller and no GamePad screen in view, others play GamePad-only with no separate controller at all.
- Font rendering and text layout need to handle Unicode reasonably (display names, bios, and post content will include emoji and non-Latin scripts) — plan for this early rather than retrofitting; neither the Wii U's system font nor a bundled Pop-style font is guaranteed to cover every codepoint an ATProto post might contain, so plan a fallback/tofu strategy rather than assuming full coverage.

## 6. Networking & Rendering Stack

- **Rendering:** use the **SDL2 port for Wii U** (built on top of WUT, designed to be close to the Switch/PC SDL2 API) rather than hand-rolling GX2 calls, unless there's a specific, justified performance reason not to. This keeps the codebase more approachable and closer to patterns used in Channel Blue's contemporaries.
- **Networking:** the Wii U's native TLS support is dated and should not be trusted for talking to modern ATProto PDS/AppView endpoints over HTTPS. `devkitPro/wut-packages` confirms both **curl** and **mbedtls** are packaged for Wii U (`wiiu-curl`, `wiiu-mbedtls` via pacman) — use those rather than sourcing/porting TLS from scratch. curl-over-mbedtls is a reasonable default HTTP+TLS stack; confirm current package versions when setting up, since pinned versions in devkitPro's repo can lag upstream CVE fixes.
- **HTTP:** `curl` (confirmed packaged for Wii U, see above) is the sane default rather than hand-rolling HTTP framing on top of raw sockets.
- **JSON:** two real options, both plain ANSI C with no exotic dependencies, so both should build fine under devkitPPC:
  - **cJSON** — a full DOM-style parser/serialiser (parse into a tree, walk it, build responses the same way). Easier to work with for XRPC's fairly deep/nested response shapes, at the cost of allocating a full tree per response.
  - **jsmn** — a single-header, allocation-free tokenizer designed specifically for resource-limited/embedded targets. It doesn't build a tree; it hands back token boundaries into the original buffer, which suits the Wii U's more limited memory budget better, at the cost of writing more manual token-walking code yourself.
  - Given the Wii U isn't as memory-constrained as jsmn's typical microcontroller use case, cJSON's ergonomics are probably worth the extra allocation for a first pass — but if profiling later shows JSON parsing is a real memory/perf cost during feed scrolling, jsmn is the natural fallback. Don't commit to one without at least trying a representative XRPC response (e.g. `getTimeline`) through both.
  - Keep payload parsing defensive either way — a PDS or AppView response should never be able to crash the client.
- **XRPC basics to implement early:**
  - `com.atproto.server.createSession` / session refresh via app password (see §7 on auth — full OAuth is likely impractical on this hardware).
  - `app.bsky.feed.getTimeline`, `app.bsky.feed.getPostThread` for reading.
  - `com.atproto.repo.createRecord` for posting (`app.bsky.feed.post`).
  - `app.bsky.notification.listNotifications`.
- Always check `nn::ac` network connection status before firing off requests, and fail gracefully (clear on-screen message, not a hang or crash) if the console has no network access.

## 7. Authentication

Full ATProto OAuth (the browser-redirect-based flow used by Inkwell and modern Bluesky clients) is very likely **not practical** on Wii U homebrew — there's no good way to host a redirect target or reliably drive a full OAuth authorization-code flow through the console's limited browser/keyboard UX. Default assumption for Cobalt:

- Use **app passwords** (`com.atproto.server.createSession` with identifier + app password) as the primary auth method, entered via the Wii U's software keyboard (`swkbd`).
- Store the resulting session/refresh token on the SD card, encrypted or at minimum not in plaintext next to other save data, and provide a clear "sign out" that wipes it.
- If Ewan wants to explore a proper OAuth flow later (e.g. a device-code-style flow if any PDS supports one, or delegating auth to a paired phone/PC), treat that as a distinct, larger effort — don't block v1 on it.

## 8. Templates & Reference Repos to Build On

**First and most obviously: evaluate Wolfram before building `atproto/` from scratch.** Wolfram is Ewan's own C SDK for AT Protocol — if its lexicon/session/record handling is portable C without hard dependencies on a desktop OS (POSIX threads, glibc-only APIs, dynamic linking assumptions that don't hold under devkitPPC's static-linking PowerPC toolchain), it may be a faster and more consistent path than reimplementing session handling, record types, and XRPC request shaping again from zero. Concretely, check early:
- What networking/TLS layer Wolfram assumes — if it's written against a desktop libcurl or a specific TLS library, confirm the same (or an equivalent) is available under WUT (see the curl/mbedtls note in §6) before assuming a clean drop-in.
- Whether it does any dynamic allocation patterns or threading assumptions that don't map cleanly onto the Wii U's more constrained environment.
- Whether it's tied to a JSON library that isn't portable to devkitPPC, in which case the JSON evaluation in §6 still applies to whatever glue code sits between Wolfram and Cobalt's UI layer.

If Wolfram turns out to need real adaptation work to build under devkitPPC, that's still likely worth doing rather than starting the ATProto logic fresh — it keeps lexicon/session logic consistent with the rest of Ewan's ATProto tooling rather than forking a third implementation. Don't assume either way without actually trying to compile it under the toolchain first.

**If Wolfram turns out to be incomplete for what Cobalt needs (missing lexicons, gaps in session/OAuth handling, etc.), the right move is to develop Wolfram itself alongside Cobalt, not fork or duplicate ATProto logic inside Cobalt's own tree.** Extending the shared SDK keeps Wolfram useful for other projects and avoids ending up with two divergent, half-correct ATProto implementations across Ewan's own codebase. When extending Wolfram, validate new lexicon/record/session handling against other established ATProto SDKs rather than working from the spec alone or guessing at edge cases — the lexicon docs don't always cover every real-world quirk (optional fields, error response shapes, pagination cursors) that a mature client implementation has already had to work out in practice:
- **`bluesky-social/atproto`** (TypeScript) — the canonical, Bluesky-PBC-maintained reference implementation, listed as the leading implementation on `atproto.com/sdks`. When in doubt about correct behaviour for a given lexicon or endpoint, this is the first place to check, even though it's a different language entirely — its type definitions and request/response handling reflect what the real network actually does, not just what the spec says it should do.
- **`ATProtoKit`** (Swift) — arguably the closest validation target to Wolfram in spirit: also a from-scratch, non-JS, native-platform client SDK (built for iOS/macOS) rather than a JS runtime wrapper. Worth comparing its approach to session handling, XRPC request shaping, and record types specifically, since it's already solved a lot of the "not the reference TS implementation, on a constrained-ish native platform" problems Wolfram will run into.
- **`bluesky-social/indigo`** (Go) and **`atrium`** (Rust) — both Bluesky/community-maintained implementations in compiled, statically-typed languages closer to C's own constraints than TypeScript is. Useful for cross-checking how a non-JS implementation handles things like CBOR/CAR encoding, DID resolution, or the repo/MST side of the protocol if Wolfram needs to go beyond basic XRPC read/write.
- When any of these disagree on behaviour not fully pinned down by the lexicon spec, treat the official TypeScript implementation as the tiebreaker, since it's the one the actual PDS/AppView implementations are built and tested against.

Beyond that, don't start the rest of the project from a blank Makefile either — there's enough of an ecosystem here to build on real starting points instead of reinventing the toolchain setup:

- **`devkitPro/wut`'s own `samples/` directory** — canonical, always-current "hello world" examples for both Makefile- and CMake-based WUT projects (`samples/cmake/helloworld` is the minimal CMake starting point referenced in WUT's own docs). This is the right base for getting a bare RPX booting and exiting cleanly before anything else is added.
- **`GaryOderNichts/SDL_mirror`** — the current, actively maintained SDL2 port for Wii U (audio, GamePad joystick/touchscreen input, GX2-backed hardware-accelerated rendering, timers, threading). Build against this rather than an older/abandoned SDL2-for-Wii-U fork (older forks like `yawut/sdl2-wiiu` point to this as their successor).
- **`KarvinJ/wii-u-tetris`** — a small, complete SDL2 starter template for Wii U (requires WUT + SDL2 + `libromfs-wiiu`). Worth cloning and reading through even if none of its code is reused directly — it's a working example of the exact stack (WUT + SDL2 + romfs assets) Cobalt is planning to use, at a scale small enough to actually read end-to-end in one sitting.
- **`yawut/libromfs-wiiu`** — romfs implementation for bundling assets (fonts, icons) into the RPX/WUHB, referenced in §4's `romfs/` directory. Use this rather than hand-rolling asset loading from SD card paths.
- **`devkitPro/wut-packages`** — the actual package definitions for `SDL2`, `SDL2_image`, `SDL2_ttf`, `curl`, `mbedtls`, `physfs`, and more. Useful both as documentation of what's available via pacman and, if a package needs a patch or a newer version than what's currently released, as a reference for how these are built for the WUT toolchain.
- **`wiiu-fling`** (community pacman repo) — supplementary packages (`libiosuhax`, `libutils`, etc.) not yet upstreamed into devkitPro's own repos. Only reach for this if something's genuinely missing from the official devkitPro packages; prefer official packages where both exist, since fling occasionally ships "transitional" stopgap versions that get silently superseded.
- **hb-appstore, Fireplace-WiiU, WiiU-Shell** — cited by the WiiUBrew wiki as real-world apps built on WUT + SDL2. Useful as examples of a *shipped* app's project structure and packaging (WUHB metadata, icon conventions) rather than as code to lift from directly.
- **`PretendoNetwork/juxtaposition`** — worth knowing about, but it's a different kind of thing than the rest of this list, not a native-homebrew template. It's Pretendo's Miiverse server (Node/TypeScript, `apps/miiverse-api` + `apps/juxtaposition-ui`), built to be talked to by the Wii U's built-in browser applet over the console's old XML/AJAX-based web API — not code that runs as a native RPX on-console. It doesn't fit alongside SDL_mirror or wut samples as something to build Cobalt's client on top of. What it *is* useful for:
  - A concrete example of the alternative architecture Cobalt isn't taking: driving the Wii U's system browser against a custom backend, rather than a native homebrew app. Worth a skim if the native-SDL2 approach ever hits a wall serious enough to reconsider that tradeoff, but not a reason to switch given the goals in §1.
  - A real example of designing a modern backend that has to accommodate a genuinely old, fixed client-side web API it can't change — a constraint-shaped-by-legacy-console problem, which is at least thematically close to what Cobalt's `atproto/` and `net/` layers deal with (working within XRPC as specified, not free to invent a nicer protocol).
  - Its monorepo shape (`apps/`, `packages/`, `migrations/`) is a reasonable reference if Cobalt ever grows a companion server component, but that's speculative — nothing in the current plan calls for one.

When setting up the initial project skeleton, start from the WUT sample's CMake/Makefile structure, then layer in SDL2 (SDL_mirror) and romfs (libromfs-wiiu) the way `wii-u-tetris` demonstrates, rather than assembling the build system from scratch by trial and error.

## 9. Coding Conventions

- Language: C, with C++ acceptable for UI/state-management code where it meaningfully reduces boilerplate (RAII for WUT resource cleanup is a reasonable use case). Don't mix idioms gratuitously within a single file.
- Match the WUT ecosystem's general style: snake_case for functions/variables, explicit resource cleanup (`OSScreen`/GX2/SDL objects freed on every exit path, including error paths — the Wii U does not forgive a leaked GX2 context the way a desktop OS forgives a leaked file handle).
- Every `ProcUI` foreground/background transition must be handled — don't assume the app stays foregrounded for its whole lifetime; the OS can background it (e.g. HOME menu overlays even though the button press itself isn't delivered to the app the same way).
- Log liberally during development (to console via any available debug output, or to a log file on SD) since on-device debugging is much harder than on a desktop target. Strip or gate verbose logging behind a debug build flag before release builds.
- No dynamic memory allocation inside per-frame render/input loops where avoidable — allocate once, reuse buffers. The Wii U's memory model is more forgiving than the Wii's but still nowhere near desktop-class.

## 10. Testing & Verification

- **Real hardware only:** there is no Cemu step in this project's workflow. Every build gets deployed straight to Ewan's Wii U via SD card or Aroma's FTP server and verified there — confirm no crashes/hangs on launch, feed load, thread view, and compose directly on console.
- Because there's no emulator to catch mistakes cheaply first, favour smaller, more frequent real-hardware test passes over large batches of untested changes — a build that hangs or crashes on console is more costly to debug here than it would be with an emulator in the loop.
- Networking behaviour must be verified on real hardware as a matter of course, not as an extra precaution — it's the only environment being tested against.
- There is no unit-testing framework standard to this ecosystem; prioritise integration-level manual test passes over trying to force a desktop-style test suite onto platform-specific code. Pure-logic code (ATProto record parsing, cache logic) *can* reasonably be unit tested if extracted into platform-independent files — do this where practical, since it's the one part of the codebase that can be tested off-console.

## 11. Known Constraints & Risks

- **Legal/distribution grey area:** Wii U homebrew requires the end user to have already exploited their own console; Cobalt itself doesn't need to (and must not) include or facilitate that exploit. Keep the README's setup instructions scoped to "assuming you already have Aroma installed."
- **TLS/crypto library availability:** confirm early which TLS portlib is realistically usable, since this gates all networking work. Don't build extensive networking code against an assumed library without confirming it builds and actually completes a TLS handshake against a real ATProto endpoint first.
- **No OAuth (see §7):** app-password auth is a real limitation for users who've disabled password-based login on their account; be upfront about this in the README rather than treating it as a temporary gap.
- **Text input ergonomics:** composing a post via the Wii U software keyboard is slow. Consider whether a companion approach (e.g. drafting via GamePad touch keyboard, which is generally faster than the swkbd overlay) is worth prioritising early.
- **Small homebrew community:** fewer reference implementations to lean on than, say, 3DS homebrew. Budget extra time for reverse-engineering-adjacent debugging against WiiUBrew wiki documentation, which is itself community-maintained and occasionally incomplete.

## 12. Suggested Build Order

1. Bare WUT + SDL2 "hello world" that boots on the real Wii U, handles ProcUI lifecycle correctly, and exits cleanly.
2. Networking spike: confirm TLS handshake + a successful `com.atproto.server.createSession` call against a real PDS, logged to screen/SD — before any UI work depends on it.
3. Minimal timeline view: fetch and render a plain-text timeline (no images, no rich formatting) on the TV screen.
4. GamePad integration: build both the TV+GamePad paired layout and the standalone GamePad-only (Off-TV Play) layout together, since neither should be an afterthought bolted onto the other — see §5.
5. Session persistence, sign-out, error states (no network, expired session, rate limiting).
6. Rich rendering: avatars/embedded images, link cards, thread view.
7. Notifications.
8. Packaging polish: WUHB icon/metadata, README, release build flags.

Treat steps 1–2 as blocking for everything else — if networking doesn't reliably work on real hardware, nothing downstream matters yet.

**Where this stands:** step 1 is done. Step 2 is written and passes the host checks, but has *not* been through a hardware pass yet — the first console run is the one that settles whether curl completes a TLS handshake against a real PDS, whether `SDL_CreateThread` works under the Wii U SDL port, and whether `createSession` comes back. Step 5's session persistence and sign-out landed alongside it, since resuming a stored session is the same code path. Step 3 (timeline) is deliberately not started: per the paragraph above, it waits on step 2 being confirmed on the console rather than on the build machine.

---

## 13. Decisions Already Made (keep this current)

Findings and choices settled in the code. Where these contradict an earlier section, these win — the earlier text describes the plan, this describes what was actually built.

### SDL2 owns ProcUI — do not drive it yourself

The Wii U SDL2 port drives ProcUI internally: `WIIU_VideoInit` calls `ProcUIInitEx` and registers the save/acquire/release callbacks, `WIIU_PumpEvents` calls `ProcUIProcessMessages` and turns the result into `SDL_QUIT` and the `SDL_APP_*` lifecycle events, and `WIIU_VideoQuit` calls `ProcUIShutdown`. (Verified against the symbols in `libSDL2.a`'s `SDL_wiiuvideo.o`.)

So Cobalt must **not** call `ProcUIInit`/`ProcUIProcessMessages`/`ProcUIShutdown`. Two consumers of one message queue means whichever loop runs first eats the foreground-release notifications the other needs — a fast route to an app that will not return to the Wii U Menu. §9's "handle every foreground/background transition" is satisfied by handling `SDL_APP_WILLENTERBACKGROUND` / `SDL_APP_DIDENTERFOREGROUND` in `src/main.c`. Textures do not survive a foreground release, so the text caches are flushed on re-acquire.

### wut owns nn::ac — do not initialise or finalise it

`__init_wut_socket` (linked in as soon as anything references sockets, which curl does) runs `socket_lib_init()`, `AddDevice()`, `ACInitialize()` and `ACConnectAsync()`. `__fini_wut_socket` calls `ACClose()` and `ACFinalize()` at exit.

`src/net/net.c` therefore only *queries* AC and uses synchronous `ACConnect()` as a fallback for wut's async connect not having landed yet. Calling `ACInitialize`/`ACFinalize` here would double-finalize against wut's own teardown.

### Wolfram is in, and its Wii U support was built out here

§8's question is settled: Wolfram is used, not reimplemented. Its Wii U support was a stub and has been filled in (in the Wolfram repo, not forked into Cobalt):

- `src/platform/wiiu_platform.c` returned `WF_ERR_NOT_IMPLEMENTED`, a NULL mutex allocator and a clock stuck at 0. Now implemented against `coreinit` `OSMutex` and `OSGetTime`.
- Wolfram lumped every console into one "embedded" bucket and routed the Wii U through the hand-rolled socket/TLS transport, which depends on libogc's `net_*` API. The Wii U has a real libcurl portlib, so it now uses the same curl transport as desktop — which is also what §6 already called for.
- `.devdeps/wiiu.cmake` passed `-mwiiu`, which devkitPPC's gcc does not accept; it now delegates to devkitPro's own `WiiU.cmake`.

Build Wolfram before Cobalt; the Makefile picks it up automatically from `../wolfram/build-wiiu` and defines `COBALT_HAS_WOLFRAM`. Without it Cobalt still builds and boots, and the diagnostics screen says the SDK is absent. Everything protocol-shaped goes behind `src/atproto/`.

### The Wii U has no usable CSPRNG — signing fails closed

devkitPro's mbedTLS for Wii U does define `mbedtls_hardware_poll`, so seeding a DRBG from it compiles and runs. Disassembly shows it is `srand(OSGetSystemTick()); rand()` — a timer-seeded libc PRNG whose entire state is the console's tick counter.

This matters because Wolfram's `wii_tls_random` feeds **P-256 key generation and ECDSA signing**. Wolfram already refuses exactly this class of entropy on the Wii, so the Wii U now follows the same rule: `src/crypto/wiiu_random.c` fails closed until the application provisions 64 real bytes via `wf_wiiu_set_entropy_seed()`.

Consequences to design around:
- Read-only use (timeline, threads) is unaffected.
- App-password login is unaffected — curl runs its own TLS stack.
- **Seed provisioning is implemented**, following Channel Blue rather than inventing a second scheme. `make bundle` generates a fresh 64-byte seed with `openssl rand` into `dist/wiiu/apps/cobalt/entropy.bin`; the app reads it from `sd:/wiiu/apps/cobalt/entropy.bin` and on every boot does load → set → rotate → save → commit (`provision_entropy()` in `src/atproto/atproto.c`, `src/util/entropy.c`). The ordering is load-bearing: the DRBG is deterministic, so booting twice on one seed regenerates identical key material, and the commit is withheld unless the replacement actually reached the card. A missing seed is not fatal — the app boots and reports it, and only signing is disabled.
- The seed is **not distributable**. One per installation; `.gitignore` blocks `entropy.bin` and `dist/`.
- **Still unresolved:** curl/mbedTLS's *own* TLS randomness on Wii U draws on the same weak poll. That affects client randoms and ephemeral ECDHE keys for every HTTPS request, and Wolfram's seed does not fix it, because curl has its own mbedTLS entropy context. Worth a decision before treating the transport as trustworthy for anything sensitive.

### Assets and font

`assets/` artwork is generated, not hand-drawn: `python3 tools/gen_assets.py` writes the 128×128 icon and both splash screens from a palette defined in that script (stdlib only — no Pillow or ImageMagick needed). Regenerate after a palette change rather than editing the PNGs.

`romfs/font.ttf` is **Lato Regular, a placeholder** — see `romfs/FONTS.md`. It is OFL-1.1 and safe to redistribute, but it is not the rounded, Pop-style face §5 asks for, and the OFL text still needs to ship alongside it before any release.

### Asset paths are probed, not assumed

`src/util/paths.c` probes candidate content roots for a `content.marker` sentinel and logs every probe, so a failure to find assets on hardware shows up as an explicit list of what was tried rather than a bare "font load failed". `/vol/content` (WUHB) and the SD app directory (bare RPX) are both covered.

### Logging

`src/util/log.c` fans out to every sink wut offers — Cafe OS, UDP on port 4405, Aroma's LoggingModule, and `sd:/wiiu/apps/cobalt/cobalt.log` (flushed per line so it survives a hang). Given there is no emulator, `nc -ul 4405` from a PC on the same LAN is the fastest debug loop available.

### The trust store is fetched at build time, not committed

devkitPro's `wiiu-curl` is built against mbedTLS and the Wii U has no system certificate store behind it, so without an explicit `CURLOPT_CAINFO` every HTTPS request fails verification. This is the first thing that blocks §12's step 2, and it is not obvious from the failure: curl reports it as a connection error, which reads as a network problem.

`make` therefore runs `tools/fetch_cacert.sh` into `romfs/cacert.pem`, and `src/atproto/session.c` hands that path to `wf_xrpc_client_set_ca_bundle()`. The bundle is git-ignored on purpose — the Mozilla set expires, and a stale copy committed to the repo would fail on console months later looking like a network bug. `make cacert` forces a refresh. An offline build is not a hard failure: it produces a working RPX that simply cannot reach a PDS, and the diagnostics screen reports the trust store as missing.

**Still unresolved from the entropy note above:** a trust store fixes certificate *verification*. It does not fix curl/mbedTLS's own TLS randomness on this platform, which still draws on the weak poll for client randoms and ephemeral ECDHE keys. That remains open.

### Network I/O runs on a worker thread

Wolfram's transport is blocking libcurl. A sign-in against a cold PDS is several seconds of DNS, TLS handshake and server-side password hashing. Doing that on the frame loop would stop the app pumping SDL events — and since SDL owns ProcUI here, a stalled event pump is a stalled ProcUI message queue, i.e. a console sitting on a frozen frame that will not return to the Wii U Menu.

`src/atproto/session.c` therefore owns an SDL worker thread with a one-job-at-a-time handoff: the UI submits a request, keeps drawing, and calls `cobalt_session_poll()` each frame for the result. If `SDL_CreateThread` fails, requests fall back to running synchronously on the caller — functional, but visibly stalling — and the diagnostics screen reports which mode is live, because "slow" and "hung" look identical from the couch otherwise.

Any future network call belongs behind the same job mechanism. Adding a synchronous one anywhere on the frame path reintroduces the freeze.

### The session layer drives `wf_session`, not `wf_agent`

`wf_agent` is the ergonomic entry point, but it is opaque and exposes no way to reach its XRPC client — which means no way to set the CA bundle, which on this platform means no TLS at all. So Cobalt composes the layer below it instead: `wf_session_new()` (a public struct, so `session->client` is reachable), `wf_xrpc_client_set_ca_bundle()`, and `wf_xrpc_client_set_refresh_handler()` for transparent token refresh. Timeline work should follow the same shape — `wf_xrpc_query_params()` on that client, then Wolfram's own `wf_agent_parse_feed()` on the body — rather than reaching for `wf_agent` and losing the CA bundle.

Two things are worth fixing in Wolfram rather than working around here (§8: extend the shared SDK, do not fork it):

- **No CA bundle accessor on `wf_agent`.** A `wf_agent_set_ca_bundle()` would make the high-level API usable on platforms without a system trust store, which is every console.
- **`wf_session_login` swallows the XRPC error envelope.** It returns a bare `wf_status`, so a wrong password, a takendown account and a missing 2FA token are all `WF_ERR_HTTP`. Cobalt's sign-in errors are written to be useful without it, but that is a workaround — the envelope is right there in the response.

### Credentials are stored encrypted, and that is worth less than it sounds

`src/cache/session_store.c` persists the session as AES-256-CTR under a 32-byte `device.key` generated per installation, satisfying §7's "encrypted or at minimum not in plaintext". Sign-out overwrites both files before unlinking so the tokens are not left in free clusters on the card.

Do not oversell it in the README or anywhere else. The key lives beside the file it protects, because the Wii U gives homebrew no keystore and no per-title secret to bind to. It defends against incidental exposure — a log, a screenshot, a stray copy of `session.dat` — and against nothing at all once someone has the whole card. If a future decision depends on this being real at-rest encryption, that decision is wrong.

### Text entry is Cobalt's own keyboard, not swkbd

`src/ui/keyboard.c` draws a QWERTY grid rather than calling the system software keyboard. swkbd is a C++ `nn::` API that composites itself into the app's GX2 render passes, and Cobalt only reaches GX2 through SDL2 — driving it would mean punching through the abstraction everything else is built on. §11 already flagged the swkbd overlay as slow and a GamePad touch keyboard as the faster option, so this is that.

It obeys §5's two-input rule by construction: touch on the GamePad, and a focus moved by D-pad or stick for someone on a Pro Controller. B is backspace; leaving a field is the Cancel key.

### There is a host test harness, and it is not a substitute for hardware

`tests/` does two things with the build machine's own compiler. It runs a `-fsyntax-only -Werror` sweep over every translation unit that does not need devkitPro headers — in both the with-Wolfram and without-Wolfram configurations, so a changed SDK signature is caught in a second rather than after a card swap. And it unit tests the genuinely platform-independent logic §10 carves out: the credential store's round trip and its refusal of damaged or foreign files, the service-URL normaliser, the keyboard's text model, and the async request handshake.

`make test` runs both. It is a filter on the obvious failures, not evidence anything works — every milestone's acceptance test is still the console.

---

*This file should be kept current as the project evolves. If you (the agent) make an architectural decision not reflected here — e.g. picking a specific TLS portlib, or discovering GamePad-only UI isn't viable for some reason — update the relevant section rather than leaving this file to drift out of sync with the codebase.*