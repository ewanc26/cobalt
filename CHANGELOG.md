# Changelog

All notable changes to Cobalt. Versions follow [Semantic Versioning](https://semver.org/)
(pre-1.0: minor bumps for features, patch bumps for fixes and polish).
`tools/release.sh` reads the section for a version out of this file to build
the GitHub release notes, so keep the `## [x.y.z] - date` headings exact.

## [Unreleased]

### Fixed
- OAuth sign-in through a node can actually finish. Since 0.5.0 the very last step, handing the node's session to Wolfram, was refused for every valid account by a Wolfram bug (wolfram#124), so Cobalt said "The OAuth node returned an unusable session." every time. I found it with the end-to-end test this change adds ([#157](https://github.com/ewanc26/cobalt/pull/157)).

### Added
- Updates, from the Home menu. Cobalt checks this repository's latest release, shows the version and notes, and downloads only when you press A. The file is checked against the SHA-256 in the release, replaces `cobalt.wuhb` when you quit, and the old build is kept until the new one has started; an interrupted update is repaired on the next launch. `tools/publish.sh` now attaches the `.sha256` and an `update.json` manifest to each release.
- The footer prompts are tappable. Tapping a hint pill on the GamePad presses the button it names, so "A: thread" opens the thread and "Y: more" opens the post menu without reaching for the pad. A prompt naming a range or a chord (`Up/Down: choose`) stays inert rather than half-applying, and an overlay that covers the footer — the post menu, the image viewer — keeps the tap.

- A full-size image viewer. The post menu grows a "View image" entry when the selected post carries pictures, and the picture opens contain-fitted to the whole surface, centred, on both the TV and the GamePad. The author's alt text is drawn beneath the image when there is one, a post of several photographs says "2 of 4" in the corner and cycles with Left/Right, and B, A or a tap closes back to the screen it was opened from. The viewer holds copies of the post's image list, so a background refresh cannot rewrite the pictures out from under the person looking at them.

- Who liked or reposted a post. The post menu grows "Liked by (N)" and "Reposted by (N)" entries when the post has either, opening the same avatar-row list the followers screen uses — A opens a profile, B returns to the screen the menu was opened from. Likes and reposts share one list (only one is on screen at a time), fetched through Wolfram's `getLikes`/`getRepostedBy` with the same paging as the other actor lists.

### Changed
- The OAuth pairing (begin, poll, the waiting loop) is Wolfram's `wf_oauth_pair_run` now, not Cobalt's own copy. A node that forgets the pairing ends the attempt at once instead of being polled for nine minutes, and quitting while it waits stops it ([#157](https://github.com/ewanc26/cobalt/pull/157)).
- A new mark: a cut stone replaces the ring-C on the icon and both splash screens, and the README has a logo and badges to match the other repositories. Both come from one generator, `tools/gen_assets.py`.
- The docs no longer say OAuth is not planned. Cobalt has had browser OAuth sign-in through a Wolfram OAuth node since 0.5.0; the README, AGENTS.md and the sign-in header comment now say so, and `docs/PARITY.md` records both sign-in flows as separate rows next to Indigo and Platinum.
- The right stick navigates like the left: both sticks drive the D-pad directions, and letting go of one does not release a direction the other is still holding.

### Internal
- The change flow is the stack's shared one, adopted from Wolfram: a PR template, the `flow` workflow (branch, title, description and commit checks, a drift check and the README style check), a `CI gate` job, and rebase-merge-only. Cobalt adds a check that code changes carry a changelog line and a doc change, a guard that turns a push to `main` without a PR red, and release gating: a `v*` tag must be on `main` with green CI, and `tools/publish.sh` refuses a commit whose CI is not green.
- Muted-word matching is Wolfram's now (`wf_mod_match_mute_words`) instead of a Cobalt copy. It follows the official client's rules: a single word matches whole words with punctuation trimmed ("cat." matches "cat", "cat's" no longer does), a phrase matches as a substring. `tools/check-shared-logic.sh` stops the copy coming back.
- The image cache takes its slot and loader counts at creation (`cobalt_imagecache_create_sized`) rather than fixing them at compile time. The viewer's caches are two slots and one loader each — a person looks at one picture at a time — decoded at the surface's own height (720 on the TV, 480 on the GamePad), where a card thumbnail stays capped at 320.

## [0.5.0] - 2026-10-04

### Added
- Browser-based AT Protocol OAuth sign-in through the hosted Wolfram OAuth node, with a short-lived pairing link that can be opened on another device. The PDS handles the account password and MFA; Cobalt never sees them.

### Changed
- OAuth-node sessions use Wolfram's DPoP-backed upstream session instead of storing PDS refresh credentials on the Wii U.

### Internal
- Added a unified hosted OAuth node to the Wolfram SDK and wired Cobalt to it as a thin console client.

## [0.4.0] - 2026-10-03

### Added
- Opening a follow notification now opens the new follower's profile instead of doing nothing.
- Your account's muted words (content and tags, expired ones skipped) and "hide reposts" for the home timeline are now honoured.

### Changed
- Image and avatar downloads go through Wolfram's generic GET instead of Cobalt's own libcurl client; https-only, redirect cap, timeout, size ceiling, CA bundle and TLS RNG are now Wolfram client settings. Needs a Wolfram with the fetch-policy setters (`wf_xrpc_client_set_https_only` and friends).

### Fixed
- After opening a hashtag or post search, going Home and choosing Timeline no longer shows the search results in place of your home timeline.

### Internal
- CI builds the host tests, e2e and snapshots, and bundles the .wuhb in a devkitPro container.
- A snapshot check that backing out of a thread or profile keeps the timeline selection and scroll.

## [0.3.2] - 2026-10-03

### Fixed
- Removed the duplicate "B: back" footer hint on the diagnostics and list-member screens.

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

[Unreleased]: https://github.com/ewanc26/cobalt/compare/v0.5.0...HEAD
[0.5.0]: https://github.com/ewanc26/cobalt/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/ewanc26/cobalt/compare/v0.3.2...v0.4.0
[0.3.2]: https://github.com/ewanc26/cobalt/compare/v0.3.1...v0.3.2
[0.3.1]: https://github.com/ewanc26/cobalt/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/ewanc26/cobalt/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/ewanc26/cobalt/compare/v0.1.5...v0.2.0
[0.1.5]: https://github.com/ewanc26/cobalt/compare/v0.1.4...v0.1.5
[0.1.4]: https://github.com/ewanc26/cobalt/compare/v0.1.3...v0.1.4
[0.1.3]: https://github.com/ewanc26/cobalt/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/ewanc26/cobalt/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/ewanc26/cobalt/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/ewanc26/cobalt/commits/v0.1.0
