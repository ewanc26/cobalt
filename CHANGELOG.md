# Changelog

All notable changes to Cobalt. Versions follow [Semantic Versioning](https://semver.org/)
(pre-1.0: minor bumps for features, patch bumps for fixes and polish).
`tools/release.sh` reads the section for a version out of this file to build
the GitHub release notes, so keep the `## [x.y.z] - date` headings exact.

## [0.9.1] - 2026-10-08

### Fixed

- Fix OAuth sign-in failure (#212); wire COBALT_JOB_OAUTH; improve error message.
- Fix build environment for Wii U (build-wiiu/libwolfram.a v0.39.0).
- Fix version string (0.9.1 from src/util/version.h).

### Added

- Diagnostics include Wolfram build info in app_diag.c.

### Changed

- Cobalt version: 0.9.1 (linear from 0.9.0).


## [Unreleased]

### Changed

- Signing in needs only the handle and password: the account's PDS is discovered from its DID document (Wolfram's `wf_agent_login_discovered`), so the host the account lives on no longer has to be typed. An empty Server field starts from the default host. ([#201](https://github.com/ewanc26/cobalt/pull/201))
### Added

- The sign-in screen has a Show app password toggle, off by default. Turned on, the app password is drawn in plain text on the form and in the keyboard; it hides again when the screen is left. ([#PR](https://github.com/ewanc26/cobalt/pull/209))
- The image picker on a new post also lists the console's camera folder, `sd:/DCIM`, with photos one folder down, next to Cobalt's own images folder. Each row says where it came from. Needs Wolfram v0.39.0's `wf_attach_scan_images_tree`. ([#PR](https://github.com/ewanc26/cobalt/pull/206))

### Fixed

- Browser sign-in now reports its failure on the sign-in screen. A failed OAuth begin that left no message now says why: an unreachable node says to check the Server field and network; any other transport failure points at app-password sign-in as an alternative. A missing `COBALT_JOB_OAUTH` case in `handle_job_result` also meant the failure was never shown at all — fixed. ([#211](https://github.com/ewanc26/cobalt/pull/211), closes [#208](https://github.com/ewanc26/cobalt/issues/208))
- On the TV home screen, a long tile label such as "Notifications" no longer breaks mid-word. The label uses half the tile padding on each side. ([#210](https://github.com/ewanc26/cobalt/pull/210))
- The snapshot harness's `profile` frame showed Feeds: each menu step now returns to the timeline, picks its card by index and its row by popup kind, and checks the screen and popup before pressing. ([#204](https://github.com/ewanc26/cobalt/pull/204))
- Backing out of a profile's followers or following list keeps the profile's selection and scroll, rather than rewinding it to the header. The rule is `cobalt_listnav_restore`. Checked on the host; not on a Wii U ([#203](https://github.com/ewanc26/cobalt/pull/203), [#110](https://github.com/ewanc26/cobalt/issues/110)).

## [0.9.0] - 2026-10-07

### Added

- A video in a post shows its poster frame on the card, decoded at the thumbnail size (320 px), with a line saying it cannot play on the Wii U. The poster and its alt text come from Wolfram v0.37.0's `wf_post_embed`. ([#200](https://github.com/ewanc26/cobalt/pull/200))

## [0.8.1] - 2026-10-07

### Changed

- The profile tabs' names, filters and cycle come from Wolfram v0.36's `wolfram/profile_tab.h`; Cobalt's copies are deleted. ([#197](https://github.com/ewanc26/cobalt/pull/197))

## [0.8.0] - 2026-10-07

### Added

- A new post can be extended into a thread: "Add to thread" on the confirmation row keeps the text and starts the next post, up to eight, and Post publishes them as one thread through Wolfram's `wf_agent_post_thread`. Text only, no images, and not for replies or quotes. If a later post fails the earlier ones stay published and the notice says how many. ([#103](https://github.com/ewanc26/cobalt/issues/103))
- A link's page in the More menu now also shows the address as a QR code, so it can be scanned from a phone. ([#101](https://github.com/ewanc26/cobalt/issues/101))

### Changed

- The feed picker reads the saved feeds through Wolfram v0.35.0's `wf_agent_get_saved_feeds`; Cobalt's own preferences walk is deleted. A feed beyond the first 25 now gets its name too (the server takes 25 per call). ([#195](https://github.com/ewanc26/cobalt/pull/195))
- Reply gates are set with Wolfram v0.34.0's `wf_agent_set_reply_gate`; Cobalt's copy of the threadgate rules is deleted. ([#191](https://github.com/ewanc26/cobalt/pull/191))
- Attaching an image uses Wolfram v0.33.0's `wolfram/attach.h` for the type and size filter, the folder scan and the upload; Cobalt's copies are deleted. This also stops leaking the uploaded blob's strings on every attach. ([#190](https://github.com/ewanc26/cobalt/pull/190))
- Timestamps, muted-word matching and failure classification now come from Wolfram v0.31.0 (`wf_time_*`, `wf_muted_list`, `wf_failure_classify`); Cobalt's own copies are deleted. A server-side failure now says the server had a problem instead of reporting a refused request. ([#186](https://github.com/ewanc26/cobalt/pull/186))

## [0.7.0] - 2026-10-07

### Changed

- Cobalt now builds against Wolfram v0.30.0 (was v0.28.0). ([#181](https://github.com/ewanc26/cobalt/pull/181))

### Added

- Lists scroll by dragging a finger up or down the GamePad screen, a row for every row's height dragged, on the timeline, threads, profiles, notifications, search, lists and their members, and the followers and following lists. A drag no longer also counts as a tap when it ends on a row. Wolfram's `wf_drag` does the tap-versus-drag work. Checked on the host; not on a Wii U ([#181](https://github.com/ewanc26/cobalt/pull/181)).

## [0.6.0] - 2026-10-06

### Changed

- Cobalt now builds against Wolfram v0.28.0 (was v0.27.0). ([#179](https://github.com/ewanc26/cobalt/pull/179))

### Added

- Updates are signed. Each release's `update.json` gets a detached Ed25519 signature, `update.json.sig`, made by a GitHub Actions job with a key that exists only as a repository secret. Cobalt checks it against the public key built in before it reads the manifest, and refuses a release that has no signature or the wrong one, so a replaced release no longer passes on its SHA-256 alone. Wolfram's `wf_update_verify_signature` does the check. Releases up to 0.5.0 are unsigned ([#138](https://github.com/ewanc26/cobalt/issues/138)).

## [0.5.0] - 2026-10-06

### Changed
- The More menu (Y) now works on notifications and on profiles, not only on the timeline and a thread. On a profile it replaces X, Y and +, so followers, following and the next tab are menu entries and the footer is shorter; on a post it also has the post's own entries. On a notification it offers the profile, the post and refresh ([#173](https://github.com/ewanc26/cobalt/pull/173)).
- Cobalt now builds against Wolfram v0.27.0 instead of its main branch, and the updater uses Wolfram's `wolfram/update.h` for the manifest, version comparison and SHA-256. My own copies of those are deleted, and a CI guard fails if they grow back. Behaviour is the same ([#172](https://github.com/ewanc26/cobalt/pull/172)).
- Notifications now have a host test that the cursor and scroll survive opening a thread or profile and backing out, next to the timeline one. I could not reproduce the rewind in #110 on the host, so it stays open until it is confirmed on a console ([#169](https://github.com/ewanc26/cobalt/pull/169)).

### Fixed
- OAuth sign-in through a node can actually finish. The very last step, handing the node's session to Wolfram, was refused for every valid account by a Wolfram bug (wolfram#124), so Cobalt said "The OAuth node returned an unusable session." every time. I found it with the end-to-end test this change adds ([#162](https://github.com/ewanc26/cobalt/pull/162)).

### Added
- A user guide: what each screen does and which button does it, with screenshots from the host renderer. `tools/check-guide.py` fails CI if it names an image that isn't there or forgets a Home entry ([#165](https://github.com/ewanc26/cobalt/pull/165)).
- A Homebrew App Store listing, generated from the release: metadata, changelog, icon, banner and screenshots, checked in CI, with one command (`tools/submit-hbas.sh`) that opens the pull request on `fortheusers/wiiu-hbas-repo` in my name. Not submitted yet ([#164](https://github.com/ewanc26/cobalt/pull/164)).
- A first-run seed. If there is no `entropy.bin` (a `.wuhb` installed without `make bundle`), Cobalt asks me to scribble on the GamePad, builds a seed from the touches, saves it and asks for a restart, instead of silently refusing to go online ([#156](https://github.com/ewanc26/cobalt/pull/156)).
- Updates, from the Home menu. Cobalt checks this repository's latest release, shows the version and notes, and downloads only when you press A. The file is checked against the SHA-256 in the release, replaces `cobalt.wuhb` when you quit, and the old build is kept until the new one has started; an interrupted update is repaired on the next launch. `tools/publish.sh` now attaches the `.sha256` and an `update.json` manifest to each release. ([#140](https://github.com/ewanc26/cobalt/pull/140))
- The footer prompts are tappable. Tapping a hint pill on the GamePad presses the button it names, so "A: thread" opens the thread and "Y: more" opens the post menu without reaching for the pad. A prompt naming a range or a chord (`Up/Down: choose`) stays inert rather than half-applying, and an overlay that covers the footer — the post menu, the image viewer — keeps the tap. ([#129](https://github.com/ewanc26/cobalt/pull/129))
- A full-size image viewer. The post menu grows a "View image" entry when the selected post carries pictures, and the picture opens contain-fitted to the whole surface, centred, on both the TV and the GamePad. The author's alt text is drawn beneath the image when there is one, a post of several photographs says "2 of 4" in the corner and cycles with Left/Right, and B, A or a tap closes back to the screen it was opened from. The viewer holds copies of the post's image list, so a background refresh cannot rewrite the pictures out from under the person looking at them. ([#132](https://github.com/ewanc26/cobalt/pull/132))
- Who liked or reposted a post. The post menu grows "Liked by (N)" and "Reposted by (N)" entries when the post has either, opening the same avatar-row list the followers screen uses — A opens a profile, B returns to the screen the menu was opened from. Likes and reposts share one list (only one is on screen at a time), fetched through Wolfram's `getLikes`/`getRepostedBy` with the same paging as the other actor lists. ([#132](https://github.com/ewanc26/cobalt/pull/132))
- Browser-based AT Protocol OAuth sign-in through the hosted Wolfram OAuth node, with a short-lived pairing link that can be opened on another device. The PDS handles the account password and MFA; Cobalt never sees them. ([#128](https://github.com/ewanc26/cobalt/pull/128))

### Changed
- The OAuth pairing (begin, poll, the waiting loop) is Wolfram's `wf_oauth_pair_run` now, not Cobalt's own copy. A node that forgets the pairing ends the attempt at once instead of being polled for nine minutes, and quitting while it waits stops it ([#162](https://github.com/ewanc26/cobalt/pull/162)).
- The README has a Using section with the controls, the build requirements now sit under Building, and the storage section no longer says app passwords are the only way to sign in. ([#153](https://github.com/ewanc26/cobalt/pull/153))
- A new mark: a cut stone replaces the ring-C on the icon and both splash screens, and the README has a logo and badges to match the other repositories. Both come from one generator, `tools/gen_assets.py`. ([#144](https://github.com/ewanc26/cobalt/pull/144))
- The docs no longer say OAuth is not planned. Cobalt has had browser OAuth sign-in through a Wolfram OAuth node since the OAuth sign-in change (#128, never released on its own); the README, AGENTS.md and the sign-in header comment now say so, and `docs/PARITY.md` records both sign-in flows as separate rows next to Indigo and Platinum. ([#137](https://github.com/ewanc26/cobalt/pull/137))
- The right stick navigates like the left: both sticks drive the D-pad directions, and letting go of one does not release a direction the other is still holding. ([#132](https://github.com/ewanc26/cobalt/pull/132))
- The change flow is the stack's shared one, adopted from Wolfram: a PR template, the `flow` workflow (branch, title, description and commit checks, a drift check and the README style check), a `CI gate` job, and rebase-merge-only. Cobalt adds a check that code changes carry a changelog line and a doc change, a guard that turns a push to `main` without a PR red, and release gating: a `v*` tag must be on `main` with green CI, and `tools/publish.sh` refuses a commit whose CI is not green. ([#149](https://github.com/ewanc26/cobalt/pull/149))
- Muted-word matching is Wolfram's now (`wf_mod_match_mute_words`) instead of a Cobalt copy. It follows the official client's rules: a single word matches whole words with punctuation trimmed ("cat." matches "cat", "cat's" no longer does), a phrase matches as a substring. `tools/check-shared-logic.sh` stops the copy coming back. ([#152](https://github.com/ewanc26/cobalt/pull/152))
- AGENTS.md no longer says Cobalt has one hardcoded custom feed. It reads the account's saved feeds, as it has for a while ([#154](https://github.com/ewanc26/cobalt/pull/154)).
- The image cache takes its slot and loader counts at creation (`cobalt_imagecache_create_sized`) rather than fixing them at compile time. The viewer's caches are two slots and one loader each — a person looks at one picture at a time — decoded at the surface's own height (720 on the TV, 480 on the GamePad), where a card thumbnail stays capped at 320. ([#132](https://github.com/ewanc26/cobalt/pull/132))
- OAuth-node sessions use Wolfram's DPoP-backed upstream session instead of storing PDS refresh credentials on the Wii U. ([#128](https://github.com/ewanc26/cobalt/pull/128))
- A unified hosted OAuth node was added to the Wolfram SDK, and Cobalt is wired to it as a thin console client. ([#128](https://github.com/ewanc26/cobalt/pull/128))

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

[Unreleased]: https://github.com/ewanc26/cobalt/compare/v0.9.1...HEAD
[0.9.0]: https://github.com/ewanc26/cobalt/compare/v0.8.1...v0.9.0
[0.9.1]: https://github.com/ewanc26/cobalt/compare/v0.9.0...v0.9.1
[0.8.1]: https://github.com/ewanc26/cobalt/compare/v0.8.0...v0.8.1
[0.8.0]: https://github.com/ewanc26/cobalt/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/ewanc26/cobalt/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/ewanc26/cobalt/compare/v0.5.0...v0.6.0
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
