# Bundled fonts

## `font.ttf` - M PLUS Rounded 1c Medium

- **Author:** The M+ FONTS Project Authors.
- **Licence:** SIL Open Font License 1.1 (`OFL.txt`, shipped alongside it).
- **Why:** a rounded, friendly sans in the spirit of the Wii U menu and
  Miiverse type (AGENTS.md section 5), with Japanese kana and kanji built in.
- Cobalt relies only on bundled fonts, never the console's system fonts.

## `fallback.ttf` - Noto Emoji (monochrome)

- **Author:** Google LLC / The Noto Project Authors
  (https://github.com/google/fonts/tree/main/ofl/notoemoji).
- **Licence:** SIL Open Font License 1.1 (same text as `OFL.txt`; copyright line
  "Copyright 2013 Google LLC").
- **Why:** used for any codepoint `font.ttf` lacks. Emoji draw as monochrome
  outlines in the text colour. SDL_ttf cannot paint colour COLRv1 glyphs, so a
  monochrome face is the reliable choice (AGENTS.md section 5).
- It is optional: delete it and uncovered codepoints draw as a box again.
