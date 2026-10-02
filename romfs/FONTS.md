# Bundled fonts

## `font.ttf` - M PLUS Rounded 1c Medium

- **Author:** The M+ FONTS Project Authors.
- **Licence:** SIL Open Font License 1.1 (`OFL.txt`, shipped alongside it).
- **Why:** a rounded, friendly sans in the spirit of the Wii U menu and
  Miiverse type (AGENTS.md section 5), with Japanese kana and kanji built in.
- Cobalt relies only on bundled fonts, never the console's system fonts.

## `fallback.ttf` (optional)

If present beside `font.ttf` it is used for any codepoint `font.ttf` lacks
(e.g. Hangul, Simplified Chinese forms). Emoji have no bundled face and draw
as a box.
