# Changelog

All notable changes to Cobalt. Versions follow [Semantic Versioning](https://semver.org/)
(pre-1.0: minor bumps for features, patch bumps for fixes and polish).
`tools/release.sh` reads the section for a version out of this file to build
the GitHub release notes, so keep the `## [x.y.z] - date` headings exact.

## [Unreleased]

## [0.3.1] - 2026-10-03

### Fixed
- Pills no longer touch: wider gaps, and the GamePad header hint row clears the Back pill.
- Timeline cards are sized to their real text, so a one-line post no longer reserves two lines.

## [0.3.0] - 2026-10-03

### Added
- A "More" menu (Y on a post) that opens a mention's profile, searches a tag, shows a link's URL, and holds quote, delete, new post and refresh.

### Changed
- Footers are shorter and drop the duplicate "B: back" hint.

## [0.2.0] - 2026-10-03

### Added
- Rich text: links, mentions and tags are drawn in the accent colour (links underlined).
- The app version is shown in the header and on the diagnostics screen.

## [0.1.5] - 2026-10-03

### Added
- Synthesised UI sounds, and build number, commit and date on the diagnostics screen.
- Thread view shows the selected post in full and scrolls long text with Up/Down.
- Phosphor icons on post counts; one pill style for header and footer.

## [0.1.4] - 2026-10-03

### Changed
- Wii U / Miiverse look, then the cobalt-blue accent palette; avatar outside the card, header Back pill, uncramped control pills.
- Performance: shared curl connections, fonts read once, header band baked to one texture, curated CA set, worker threads off the render core.
- Emoji and CJK glyph fallback, emoji keyboard layer, live two-window host simulator.

## [0.1.3] - 2026-10-02

### Added
- Delete your own posts, quoted posts as nested cards, followers/following lists, profile tabs, pinned posts.
- Compose quote posts, attach one image with alt text (also on replies), post-language setting, post search.
- Host end-to-end test against Wolfram's mock PDS.

## [0.1.2] - 2026-08-19

### Added
- Actor search, custom feeds, browse-only curated lists, and reply gates on new top-level posts.

## [0.1.1] - 2026-08-11

### Added
- Post images, link cards and alt text; mute and block from a profile, with muted and blocked account screens.

## [0.1.0] - 2026-07-29

### Added
- First working client: app-password sign-in, timeline, threads, likes, reposts, composing, notifications, profiles and avatars.

[Unreleased]: https://github.com/ewanc26/cobalt/compare/v0.3.1...HEAD
[0.3.1]: https://github.com/ewanc26/cobalt/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/ewanc26/cobalt/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/ewanc26/cobalt/compare/v0.1.5...v0.2.0
[0.1.5]: https://github.com/ewanc26/cobalt/compare/v0.1.4...v0.1.5
[0.1.4]: https://github.com/ewanc26/cobalt/compare/v0.1.3...v0.1.4
[0.1.3]: https://github.com/ewanc26/cobalt/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/ewanc26/cobalt/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/ewanc26/cobalt/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/ewanc26/cobalt/commits/v0.1.0
