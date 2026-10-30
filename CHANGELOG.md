# Changelog

All notable changes to Cobalt. Versions follow [Semantic Versioning](https://semver.org/)
(pre-1.0: minor bumps for features, patch bumps for fixes and polish).
`tools/release.sh` reads the section for a version out of this file to build
the GitHub release notes, so keep the `## [x.y.z] - date` headings exact.

## [Unreleased]

### Added

- Groups are in. R on Messages starts one: a name, then a comma-separated list of handles or DIDs, which Wolfram resolves, de-duplicates and checks. R on a group conversation opens its settings, where A renames it, Y adds members, X removes the selected one and R creates or enables/disables its join link, shown as `https://bsky.app/chat/<code>`. The inbox recognises a group from the conversation's `kind` rather than its member count, and the group's details are read through Wolfram's typed `getConvo`/`getConvoMembers`. ([#107](https://github.com/ewanc26/cobalt/issues/107))
- A Credits screen, reached from the home menu, lists the repositories behind the stack: Cobalt, Indigo, Wolfram, Platinum and Channel Blue. Each `https://github.com/ewanc26/...` address is printed in full and the screen is passive, because the console cannot open a browser; B or the Back pill leaves it. The home menu gained a fourteenth entry.
- A link whose external view carries `associatedRefs` is hydrated through the public AppView into the card its record describes: an article with its publication and reading time, a gallery with up to four pictures, or a livestream marked live or ended. The request runs once, only when the card is first drawn, and is cached for the session; a link without a usable record, or a failed request, keeps the plain card. ([#102](https://github.com/ewanc26/cobalt/issues/102))

### Changed

- Wolfram is pinned to v0.43.2. The follower and following lists are populated again, because the typed calls now read the arrays the endpoints send, and its public headers compile under `-Wpedantic -Werror` on Clang 16 and older. ([wolfram#226](https://github.com/ewanc26/wolfram/pull/226))

## [0.14.0] - 2026-10-28

### Added

- Gallery posts draw their first four pictures in the same row as an image post, and a link card names the publication and reading time when the external view supplies them (`source.title`/`readingTime`), falling back to the bare host otherwise. Both are read through Wolfram's embed-view parser, including the media half of a quote post; video posters and their duration are unchanged. ([#102](https://github.com/ewanc26/cobalt/issues/102))
- A Settings screen, reached from the home menu, with a Light/Dark theme switch, three post-text sizes (100%, 115% and 130%), a high-contrast palette and a Reset to defaults row. Theme and contrast apply immediately on both screens -- including the baked background gradient and header band, which are rebuilt when the palette changes -- and text scale reopens the post face and drops the renderer's text and wrap caches, so long posts re-wrap at the new size on the next frame. All three are saved in `settings.dat`, so they survive closing Cobalt; reset restores and saves the defaults in one action. The screen moved to `src/app/settings_view.c` rather than growing `app_home.c`. The audit behind it also fixed the surfaces that had been hard-coded white: control-prompt pills and popups now follow the theme, prompt key chips choose a readable ink over the accent, page titles use a header colour that stays white rather than the card surface (which turns dark), and the TV home grid shrinks its fourth row to fit the thirteenth entry. Host tests enforce WCAG AA contrast for the palette roles in all three modes, and snapshot cases cover high contrast on the popup, image viewer and chat screen. ([compare](https://github.com/ewanc26/cobalt/compare/v0.13.0...main))

## [0.13.0] - 2026-10-28

### Added

- A post's draft can hold line breaks. The keyboard a post is typed on now has a **Ret** key, which it did not before — the only way to start a new line was missing, so every draft came out as one paragraph. Ret is paid for out of the space bar rather than out of every key, so the rest of the grid is the size it was and a handle, password or search box (one-line fields) is unchanged. Cards, the compose box, the TV's draft panel and the confirm preview already broke on `\n`; only getting one into the buffer was missing. ([#256](https://github.com/ewanc26/cobalt/pull/256))
- A group conversation has its join requests. On the conversation, X opens the group's pending requests; A approves the highlighted one, which lets that person in, and X rejects it. B returns to the conversation. Only a group's owner can list them, nothing is approved or rejected until a button is pressed, and a failure is shown and not retried. ([#107](https://github.com/ewanc26/cobalt/issues/107))

### Changed

- The GamePad's control prompts — the footer pills and the Back pill in the corner — are fingertip sized: body type in a 44-pixel-tall pill, up from a 33-pixel caption, which was the smallest touch target left on the pad and the one the actions now live in. The TV keeps its caption pills, because nothing on a TV is touched. Content stops at a computed `cobalt_content_bottom()` rather than a literal 28 pixels from the bottom, so no screen's last row can end up under a taller pill, and the header band only takes half the prompts when a row of them fits there — with fingers, it does not, so they all sit along the bottom. ([#256](https://github.com/ewanc26/cobalt/pull/256))
- In split play the GamePad's profile header is the handle and the counts, with the posts under it, instead of a full card with the bio, the big avatar and the follow button: those are reading, the TV has them, and on a 480-line screen they cost the posts the room they need to be a list. Following is still on the footer with the rest of the actions, and mute or block now show beside the counts instead of on the status line the compact header does not have. ([#256](https://github.com/ewanc26/cobalt/pull/256))
- The GamePad's home screen says what the pad is doing — "Remote for the TV" or "Off-TV play" — rather than "Off-TV ready" while it is paired with the TV. ([#256](https://github.com/ewanc26/cobalt/pull/256))
- With both screens on, the TV and the GamePad now divide the work instead of showing the same picture twice. The TV keeps the full reading layout — every line of a post, its pictures and its quoted post — and the GamePad is the remote: the same items as a compact touch list, one line of text and no media each, with the actions on its footer. While a keyboard is up the TV shows the draft at reading size rather than a second copy of the keys, passwords still masked. Off-TV Play is unchanged and still gives the GamePad every line and every picture, because the split is a display mode and not a cut-down view: one role flag decides which of the two a surface is drawing, and `FULL` is the same screens with nothing trimmed. ([#256](https://github.com/ewanc26/cobalt/pull/256))
- The GamePad's keyboard has 44-pixel keys, up from 36: about 7 mm on the 480-line panel, a jump of a fifth in the one control the on-screen keyboard is used through. The compose box gives up one line to pay for it, and now shows the last lines of the draft rather than the first, so the line being typed stays in view. ([#256](https://github.com/ewanc26/cobalt/pull/256))
- The GamePad's Home menu, saved-feeds list and message conversations scroll by dragging, a row per row of finger travel, instead of only by D-pad. Half the twelve Home entries sat below the fold with no way to reach them by touch. ([#256](https://github.com/ewanc26/cobalt/pull/256))
- The account screen's four action rows are 44 pixels tall on the GamePad instead of 34, the smallest touch targets in the app; its panel now carries two lines (handle, then the DID and server) rather than three, which is what pays for them. ([#256](https://github.com/ewanc26/cobalt/pull/256))
- The TV's Home menu is four columns of tiles in three rows rather than six and two. Six columns fitted the height but not the words: "Notifications" and "Queued posts" had to drop to the caption face while the rest stayed body-sized, so one row read at a different size from the other. Every label is now body-sized and centred in its tile, and a tile that cannot be opened is dimmed rather than captioned "Sign in first" seven times over — the strip below the grid already says why the focused tile is unavailable. ([#256](https://github.com/ewanc26/cobalt/pull/256))
- A profile's footer names what its buttons do on the row that is selected: follow or unfollow, mute and block on the account, like and repost on a post, and only the More menu on your own profile, where the other three are refused. The tab being shown (Posts, Replies) moved from a non-button pill in the footer to the header beside the handle. ([#256](https://github.com/ewanc26/cobalt/pull/256))
- A control prompt — the pills along the bottom of every screen — is one draw per shape instead of seven. Each pill's body and its accent key chip are kept as tinted masks at the size they are drawn, so a footer of five prompts asks the renderer for ten commands where it asked for seventy, and the pill's antialiased ends are drawn at their own radius rather than a stretched tile. A screen's prompts cost a few kilobytes of texture between them. ([#256](https://github.com/ewanc26/cobalt/pull/256))
- A tile draws only what shows. Its shadow is filled where the body does not cover it rather than across the whole card, a focused tile's accent ring is drawn as the border it is instead of a whole card of accent, the glassy sheen is composited into the body first and skipped where that leaves the body's own colour — which, on a white tile, it always does — and opaque fills are written instead of blended, the background gradient included. On the development machine the timeline's TV frame fell from 2.3 ms to 1.9 ms and a thread's from 2.2 ms to 1.9 ms, most of what is left being the software renderer presenting the frame. ([#256](https://github.com/ewanc26/cobalt/pull/256))
- Text is laid out once and remembered. The renderer keeps where each string broke into lines at the width it was drawn, so a card no longer wraps its own text twice a frame, and the walk that finds the breaks goes word by word rather than byte by byte, so it no longer costs the square of the line's length. On the development machine a timeline frame's text work fell from about 2.7 ms to 2.3 ms and a thread's from 3.2 ms to 2.2 ms; `make -C tests bench` measures it. ([#256](https://github.com/ewanc26/cobalt/pull/256))

### Fixed

- `tools/check-parity.sh` works on a Mac. Its state regex used `\b`, which is a GNU extension BSD `regexec` does not implement, so every cell in `docs/PARITY.md` failed on macOS while Linux CI stayed green — the check was silently only running in one place. It now matches a word end with a trailing group instead, and the selftest that proves it still catches a bad state word passes on both. ([#256](https://github.com/ewanc26/cobalt/pull/256))
- A menu's rows on the GamePad are 44 pixels tall rather than 40, and their text is centred in the row instead of sitting at its top — both were just short of a fingertip, and the rows are tapped. The compose screen's confirm buttons are 44 too, up from 42. ([#256](https://github.com/ewanc26/cobalt/pull/256))
- A conversation's reply has a field. The draft was drawn straight onto the background, so an empty one was a lone caret under the last message; it now gets the same rounded box the compose screen gives its text. ([#256](https://github.com/ewanc26/cobalt/pull/256))
- The GamePad's conversation screen no longer stacks two B pills in its header. The footer's "B: back" duplicated the header's Back button and the hint splitter moved it into the band beside it; conversations now name only reply, older/newer and refresh. ([#256](https://github.com/ewanc26/cobalt/pull/256))
- Search and the new-conversation screen show their placeholders. Both chose between the typed text and "Search accounts..." / "Handle or DID..." by testing the display string, which always ends in a caret and so was never empty — an empty field showed a bare underscore instead. ([#256](https://github.com/ewanc26/cobalt/pull/256))
- The thread's text scrollbar sits inside the card's padding instead of on its edge, where it read as a second border beside the tile's own and a focused card's accent ring ran two pixels from it. ([#256](https://github.com/ewanc26/cobalt/pull/256))
- An unread notification or conversation is marked with the accent bar down the left of its row that the code always claimed to draw, rather than a single dot stranded in the row's bottom-right corner. ([#256](https://github.com/ewanc26/cobalt/pull/256))
- Messages and Queued posts no longer repeat "B: back" in the footer under the header's Back pill. ([#256](https://github.com/ewanc26/cobalt/pull/256))

### Removed

- Cobalt no longer updates itself. The Updates entry is gone from the Home menu, and with it the in-app download, the signature check and the replacement of the running `.wuhb` on quit; the screenshots and the entry indices that pointed at it are gone too. Updates come from the Homebrew App Store, which carries each release's file, or from a `.wuhb` you copy across yourself. ([#252](https://github.com/ewanc26/cobalt/pull/252))
- Releases no longer carry an `update.json` manifest or its `update.json.sig` signature, and the job that signed it is gone with them; `tools/publish.sh` attaches the `.wuhb` and its checksum only. Releases published before this keep their manifest on GitHub. ([#253](https://github.com/ewanc26/cobalt/pull/253))

## [0.12.0] - 2026-10-22

### Added

- A video whose playlist is a master playlist shows the length of one variant (the lowest bandwidth), followed once, at most two playlist fetches per video. ([#102](https://github.com/ewanc26/cobalt/issues/102))
- Message requests can be accepted or declined. On Messages, Y lists the requests; A accepts the highlighted one, which joins the inbox and opens, and X declines it, which removes it from the list. Each is one press and one request, nothing is sent on entry, and a failure is shown and not retried. Groups are still open. (#107)
- A video's poster shows its length, read from the video's HLS playlist with Wolfram's `wf_embed_video_playlist_duration`. The playlist is fetched off the frame loop, only when the poster is drawn, and once per playlist. A master or live playlist, or a failed fetch, shows no duration. Needs Wolfram's unreleased `wf_embed_video_playlist_duration` (on Wolfram main, PR #217); it is not in the pinned v0.41.0. (#102)

### Changed

- On the TV, the selected post in the timeline and thread shows as much of its text as fits in the right-hand column. A longer post ends in a "More below" line, and L and R scroll its text one line at a time. The list selection does not move while scrolling, and moving to another post starts its text at the top. The GamePad layout is unchanged. (#111)

### Fixed

- Going back to a thread fetches its conversation again. The return stack remembers which thread each screen showed, so after a nested unwind the outer thread no longer shows the inner conversation. (#110)
- Backing out of a thread opened from a profile that was itself opened from a thread no longer bounces between the profile and the outer thread. Each screen returns to the one it came from, down to the timeline, through a return stack in place of the single return slots. The snapshot suite now walks that chain. (#110)

## [0.11.1] - 2026-10-16

### Fixed

- Reply controls on a post are set with Wolfram v0.40.0's `wf_agent_set_reply_gate`, which writes the gate under the post's own record key, so the AppView links it to that post and the controls apply. ([#203](https://github.com/ewanc26/wolfram/issues/203))

## [0.11.0] - 2026-10-15

### Added

- Messages on the home menu opens a read-only direct message inbox: each conversation shows who it is with (groups say "Group"), the last message and how many are unread, and the list pages. Confirm opens a conversation as bubbles, newest at the bottom, with Up and Down to reach older messages (the newest 50 are held). A writes a reply on the keyboard, Y sends it, and a reply that fails to send stays as the draft ([#107](https://github.com/ewanc26/cobalt/issues/107)).
- Messages can start a new conversation: X on the inbox opens the keyboard for a handle or DID, and the conversation opens once the service has found or created it. A refusal or failure keeps what was typed to correct. ([#227](https://github.com/ewanc26/cobalt/pull/227))
- Save a new post for later, with Save for later in the confirm list. Queued posts are kept on the SD card (up to 16), and a Queued posts screen on the home menu sends or discards each one. Nothing is sent without you choosing it; a failed send stays queued with the reason shown. ([#233](https://github.com/ewanc26/cobalt/pull/233))
- Direct messages `COBALT_SCREEN_DMS` and a Messages entry on the home menu, listing conversations, fetching messages, and sending new messages via `chat.bsky.convo.*` through Wolfram. ([#107](https://github.com/ewanc26/cobalt/issues/107))
- Added a `duration` field to `cobalt_post_link` and display code for video poster frames, ready for Wolfram to expose video length. ([#102](https://github.com/ewanc26/cobalt/issues/102))

### Changed

- On the TV the timeline uses the width: the list takes two-fifths on the left, and the selected post is drawn in full on the right, so a long post no longer has to be cut to fit a card. The GamePad keeps the single column. ([#111](https://github.com/ewanc26/cobalt/issues/111))

### Fixed

- The Updater can put the new build in place on a real console. Aroma holds the running `.wuhb`'s content mounted for the life of the process and refuses to rename the file while it is, so a downloaded update was verified and staged but never installed. Cobalt now releases the mount through Aroma's RPXLoadingModule immediately before the replacement, on the quit path. ([#139](https://github.com/ewanc26/cobalt/issues/139))

### Fixed

- The Account screen's error line no longer runs under the button hints, and the Home menu's Notifications tile no longer breaks the word in two on the TV. ([#111](https://github.com/ewanc26/cobalt/issues/111))

## [0.10.0] - 2026-10-12

### Changed

- Keep C modules under `src/` and discover C++ translation units only under `cpp/`, with separate Makefile source lists. ([#218](https://github.com/ewanc26/cobalt/pull/218))

- Signing in needs only the handle and password: the account's PDS is discovered from its DID document (Wolfram's `wf_agent_login_discovered`), so the host the account lives on no longer has to be typed. An empty Server field starts from the default host. ([#201](https://github.com/ewanc26/cobalt/pull/201))
- Merge the v0.9.1 release into main. ([#213](https://github.com/ewanc26/cobalt/pull/213))

### Added

- Releasing now refuses to publish without a fresh TLS trust store. `tools/fetch_cacert.sh --check` validates an existing bundle (present, enough certificates, not older than 30 days) and `tools/publish.sh` re-fetches and re-checks before a release is tagged, so a build cannot ship a stale trust store that fails on a console as a network bug. ([#215](https://github.com/ewanc26/cobalt/pull/215))
- The sign-in screen has a Show app password toggle, off by default. Turned on, the app password is drawn in plain text on the form and in the keyboard; it hides again when the screen is left. ([#PR](https://github.com/ewanc26/cobalt/pull/209))
- The image picker on a new post also lists the console's camera folder, `sd:/DCIM`, with photos one folder down, next to Cobalt's own images folder. Each row says where it came from. Needs Wolfram v0.39.0's `wf_attach_scan_images_tree`. ([#PR](https://github.com/ewanc26/cobalt/pull/206))
- The user guide now shows a profile screenshot, generated from the snapshot harness's `profile` frame. ([#166](https://github.com/ewanc26/cobalt/issues/166))

### Fixed

- The Updates screen now says why a check or download failed. `net/http.c` records a reason for every failed fetch, classified with Wolfram's `wf_failure_classify`, so a stale or missing TLS trust store, an unseeded entropy store, DNS/connect, a timeout, a rate limit and an HTTP status are told apart instead of all reading "Could not reach GitHub". The diagnostics screen shows the fetch client's state alongside the trust store. ([#215](https://github.com/ewanc26/cobalt/pull/215))
- Browser sign-in now reports its failure on the sign-in screen. A failed OAuth begin that left no message now says why: an unreachable node says to check the Server field and network; any other transport failure points at app-password sign-in as an alternative. A missing `COBALT_JOB_OAUTH` case in `handle_job_result` also meant the failure was never shown at all — fixed. ([#211](https://github.com/ewanc26/cobalt/pull/211), closes [#208](https://github.com/ewanc26/cobalt/issues/208))
- On the TV home screen, a long tile label such as "Notifications" no longer breaks mid-word. The label uses half the tile padding on each side. ([#210](https://github.com/ewanc26/cobalt/pull/210))
- The snapshot harness's `profile` frame showed Feeds: each menu step now returns to the timeline, picks its card by index and its row by popup kind, and checks the screen and popup before pressing. ([#204](https://github.com/ewanc26/cobalt/pull/204))
- Backing out of a profile's followers or following list keeps the profile's selection and scroll, rather than rewinding it to the header. The rule is `cobalt_listnav_restore`. Checked on the host; not on a Wii U ([#203](https://github.com/ewanc26/cobalt/pull/203), [#110](https://github.com/ewanc26/cobalt/issues/110)).

## [0.9.1] - 2026-10-08

### Fixed

- Fix OAuth sign-in failure (#212); wire COBALT_JOB_OAUTH; improve error message.
- Fix build environment for Wii U (build-wiiu/libwolfram.a v0.39.0).
- Fix version string (0.9.1 from src/util/version.h).
- Browser sign-in reports failure with HTTP status. (closes #208)

### Added

- Diagnostics include Wolfram build info in app_diag.c.

### Changed

- Cobalt version: 0.9.1 (linear from 0.9.0).

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

[Unreleased]: https://github.com/ewanc26/cobalt/compare/v0.14.0...HEAD
[0.14.0]: https://github.com/ewanc26/cobalt/compare/v0.13.0...v0.14.0
[0.13.0]: https://github.com/ewanc26/cobalt/compare/v0.12.0...v0.13.0
[0.12.0]: https://github.com/ewanc26/cobalt/compare/v0.11.1...v0.12.0
[0.10.0]: https://github.com/ewanc26/cobalt/compare/v0.9.1...v0.10.0
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
