# Bundled fonts

## `font.ttf` — Lato Regular (PLACEHOLDER)

- **Family:** Lato, by Łukasz Dziedzic.
- **Licence:** SIL Open Font License 1.1 — redistributable, including bundled
  inside an application, provided the licence text ships alongside it.
- **Status: placeholder.** AGENTS.md §5 calls for a *rounded, friendly* sans in
  the spirit of the Wii U menu's Fontworks Pop Happiness / Pop Joy. Lato is a
  humanist sans — legible at TV distance and safe to redistribute, so it is a
  reasonable stand-in for bringing text rendering up, but it is **not** the
  final "feels native on a Wii U" choice.

`OFL.txt` in this directory is the full SIL Open Font License 1.1 text
required by Lato's license — it ships inside the WUHB alongside `font.ttf`
since both live under `romfs/`.

### Before cutting a release

1. Decide on the real UI face (see AGENTS.md §5). Candidate rounded, openly
   licensed options worth evaluating: **Varela Round**, **Quicksand**,
   **Nunito**, **M PLUS Rounded 1c** (the last also carries broad CJK coverage,
   which matters for §5's Unicode note). If the replacement is also OFL-licensed,
   update `OFL.txt`'s copyright line accordingly; if it uses a different
   license, swap the file for that license's text instead.
2. Check codepoint coverage against real post content. Neither Lato nor most
   Latin-only rounded faces cover CJK, Cyrillic, or emoji — AGENTS.md §5 asks
   for a fallback/tofu strategy rather than assuming full coverage, so a second
   fallback face is likely needed regardless of which primary face wins.

Cobalt loads this file through `cobalt_content_path()` (see `src/util/paths.c`),
which resolves `/vol/content` when running from a WUHB and falls back to the
app directory on SD, so replacing the file here is all that is needed.
