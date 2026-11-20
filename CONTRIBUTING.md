# Contributing to Cobalt

Cobalt is a native AT Protocol / Bluesky client for the Nintendo Wii U, built as Aroma homebrew with devkitPro/WUT and SDL2, using [Wolfram](https://github.com/ewanc26/wolfram) for protocol functionality. Thanks for wanting to help!

Cobalt treats the Wii U as the platform it is rather than a browser target: the TV and GamePad are both first-class displays that divide the work, and Off-TV Play gives the GamePad the whole interface alone.

## Getting started

Set up devkitPro with the WUT toolchain and SDL2 per the [README](README.md), then build the Aroma package. CI runs host-side tests; real-hardware behaviour is verified by the maintainer.

## Notes for this project

- **This is a Wii U.** A weak PowerPC tri-core with limited memory: no allocations without thought, no heavyweight dependencies, no "just use a webview" suggestions, please.
- The TV/GamePad split is a design principle, not an accident — UI changes should make sense on both screens, and under Off-TV Play.
- Don't reimplement protocol code: protocol concerns belong in Wolfram; Cobalt is the client surface.
- If you can't test on hardware, say so plainly in the PR, and describe what you did verify (emulator, host tests).
- Conventional commits; focused PRs; explain how you verified.
- AI-assisted PRs welcome; stand behind every line and run what you can.

## Licence

AGPL-3.0 (see [LICENSE](LICENSE)). Contributions accepted under the same terms.