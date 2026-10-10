# Parity

This is the parity matrix for the three native clients: Cobalt (Wii U, this repository), [Indigo](https://github.com/ewanc26/indigo) (3DS) and [Platinum](https://github.com/ewanc26/platinum) (Mac OS 9, a Node bridge plus a C89 client). I wrote the Cobalt column from this repository's code. The Indigo and Platinum columns are what their own repositories claim: Indigo's `docs/PARITY.md` and Platinum's `parity` issues. I read their code to check the cells that looked doubtful, but I did not run either client.

Each cell starts with one of these words, and `tools/check-parity.sh` enforces that:

- `implemented`: the code is on `main`.
- `partial`: some of it is, with the issue for the rest.
- `issue`: a gap, with its issue. Written `issue #N` for this repository or `issue repo#N` for another.
- `declined`: a decision with a reason. Not the same as impossible.
- `impossible`: the hardware cannot do it, with the evidence. Nothing here is in this state, because I have not proved any of the declined rows against the hardware.
- `n/a`: the row does not apply to that client.

"Verified" means what was actually run. Cobalt's README says it is installed and running on a Wii U; that is the owner's report and I have no console, so nothing below is claimed on hardware by me. "Host" means the unit tests, mock-PDS end-to-end tests and the snapshot renderer, which are not an emulator and not a console.

## Sign-in

The two flows have different dependencies, so they are two rows.

| Flow | Cobalt | Indigo | Platinum | Evidence |
|---|---|---|---|---|
| App password | implemented | implemented | implemented | Cobalt: `src/app/signin.c`, the login job in `src/atproto/session_auth.c`; host and mock-PDS tests. Platinum: `bridge/src/auth/app-password.ts` and `macos9/apppw.c`. |
| OAuth | implemented | implemented | implemented | Cobalt: `run_oauth()` in `src/atproto/session_auth.c` through a Wolfram OAuth node (`docs/oauth-node.md` in Wolfram). An empty password in the form selects it. The pairing exchange (begin, a terminal error, completion into a session) has an e2e test against a mock node (`tests/e2e_main.c`); that test found wolfram#124, which had stopped the last step since 0.5.0. Not verified on a console by me. Platinum's bridge pairs differently (its own Node protocol). |
| Pairing client shared in Wolfram | implemented | implemented | n/a | Cobalt calls `wf_oauth_pair_run` (wolfram#101); covered by an e2e test against a mock node. |

OAuth on a console is not the AT Protocol browser flow running on the console. The node holds the OAuth session and the DPoP key; the console holds a bearer token for the node and never sees the account password, MFA or a refresh token. The token is not logged.

## Features

| Feature | Cobalt | Indigo | Platinum | Evidence |
|---|---|---|---|---|
| Diagnostics | implemented | implemented | issue platinum#37 | Cobalt: `ACTION_DIAGNOSTICS` in `src/app/app_home.c` |
| Persistent session, sign-out | implemented | implemented | implemented | Cobalt stores the session encrypted (`src/cache/session_store.c`); see AGENTS.md on what that is worth |
| Home timeline, paging | implemented | implemented | partial, issue platinum#26 | Platinum parses the cursor and never uses it |
| Thread view | implemented | implemented | issue platinum#27 | |
| Profiles | implemented | implemented | partial, issue platinum#31 | Platinum shows the signed-in account only |
| Follow, unfollow, followers, following | implemented | implemented | issue platinum#31 | |
| Profile tabs | implemented | implemented | issue platinum#31 | Cobalt: posts, replies, media, likes (`src/atproto/actor_profile.h`). Indigo: the same four, cycled from the header box on a person's posts (likes on the signed-in account only). |
| Pinned posts | implemented | implemented | issue platinum#31 | |
| Notifications | implemented | implemented | implemented | |
| Mark notifications seen | implemented | implemented | issue platinum#30 | |
| Avatars | implemented | implemented | issue platinum#32 | |
| Images on posts, with alt text | implemented | implemented | issue platinum#32 | |
| Full-size image viewer | implemented | implemented | issue platinum#32 | |
| Link cards | implemented | implemented | issue platinum#32 | |
| Who liked or reposted a post | implemented | implemented | issue platinum#36 | |
| Compose, reply, quote | implemented | implemented | partial, issue platinum#27 | Platinum posts only |
| Attach an image when composing | implemented | implemented | issue platinum#33 | Cobalt: `wf_agent_upload_blob_ex`, picker in `src/app/compose.c`. Indigo: picker and upload path are implemented; host-verified, but upload and DCIM listing have not been verified on a console (#19 remains open for that). |
| Reply gates | implemented | implemented | issue platinum#29 | |
| Like and repost, with undo | implemented | implemented | issue platinum#28 | |
| Delete your own post | implemented | implemented | issue platinum#42 | Cobalt: `confirm_delete` in `src/app/thread.h`. Indigo: explicit confirmation; worker checks the post AT-URI against the signed-in DID before deleting. Host tests and 3DS build pass; hardware behaviour is unverified. |
| Actor search, post search | implemented | implemented | issue platinum#34 | |
| Custom feeds | implemented | implemented | issue platinum#34 | Both read the account's saved feeds. Cobalt: `run_saved_feeds` in `src/atproto/session_lists.c`; B from a feed goes Home, not back to the picker. |
| Lists and members | implemented | implemented | issue platinum#34 | Read-only in both consoles |
| Mute and block, with lists | implemented | implemented | issue platinum#35 | |
| Muted words, hide reposts | implemented | implemented | issue platinum#35 | Both call Wolfram's `wf_muted_list` |
| Direct messages | issue #107 | issue indigo#22 | issue platinum#42 | Wolfram has `chat_typed.h`. |
| Post to a thread (several posts at once) | implemented | implemented | issue platinum#42 | Cobalt: `cobalt_compose_extend` (`src/app/compose.c`) and `wf_agent_post_thread`; text only. Indigo: Add to thread composes up to eight top-level text posts and uses `wf_agent_post_thread`; partial publication is reported and the draft cleared to avoid duplicates. Host tests and 3DS build pass; hardware behaviour is unverified. |
| Video poster and external-media embeds | partial, issue #102 | partial, issue indigo#22 | issue platinum#42 | Cobalt: video poster frame with cannot-play line; duration and other external media remain open. Indigo: video poster frame and cannot-play line are implemented and host-tested; duration metadata and other external-media variants remain open. |
| Open a link on a phone via QR code | implemented | implemented | issue platinum#42 | Cobalt: `cobalt_popup_show_text` in `src/ui/popup.c` encodes with Wolfram's `wf_qr_encode`; host-tested for the matrix, not scanned from a screen. Indigo: More-menu link view draws the QR on the top screen and the URL below; host-tested, not scanned from a physical screen. |
| Auto-update from GitHub releases | implemented | implemented | issue platinum#47 | Cobalt: Home, Updates (`src/app/update.c`, `src/update/`); confirm, SHA-256 checked, replaced on quit, previous build kept. Manifest signed (Ed25519, `update.json.sig`) and refused if unsigned. Host-verified only; the console run is #139. Indigo: implemented in More > Check for updates; host-verified only, not run on emulator or hardware (#23 closed). Manifest, version comparison and SHA-256 are Wolfram's `wolfram/update.h` (wolfram#106, v0.27.0). |
| Video and GIF playback | declined | declined | declined | Cobalt README: no decoder in the dependency and performance budget. This is a decision; I have not tried a decoder, so it is not recorded as impossible. |
| Push notifications | declined | declined | declined | No push service a homebrew or Classic Mac application can register with; notifications are fetched when the screen is opened. |

## Cobalt only

These are Wii U specific and have no row in the other clients.

| Item | Cobalt | Evidence |
|---|---|---|
| GamePad touch coverage of the popup and header pills | issue #112 | |
| TV layout uses the extra space | issue #111 | |
| Remember scroll position when returning from a thread or profile | partial #110 | The timeline keeps its cursor and scroll across a thread and across a profile, and notifications across a thread or profile; snapshot checks in `tests/snapshot_main.c` assert it on the host. I could not reproduce a rewind and have not seen it on a console, so issue #110 stays open for the owner to confirm. |
| More menu unified with the thread and profile screens | implemented | Y opens it on the timeline, thread, notifications and profile. The profile's X, Y and + moved into it; notifications get profile, post and refresh. Snapshot checks in `tests/snapshot_main.c`; search results are the timeline screen and already had it. Host only. |
| User guide with controls per screen and screenshots | implemented | `docs/guide.md`, checked by `tools/check-guide.py`. |
| First on-console acceptance pass | issue #24 | Needs the owner's console. |

## Duplication

Shared logic lives in Wolfram. Muted words, time, failure kinds, the update check and the OAuth pairing client are all Wolfram's in Cobalt and Indigo, and `tools/check-shared-logic.sh` fails if a copy grows back. What is left is the repeated list-screen scaffolding inside Cobalt (cobalt#185).

## Keeping this honest

`tools/check-parity.sh` fails when a cell does not start with a known state, when an `issue` cell has no reference, or when the README or AGENTS.md disagree with this page about OAuth. Change a feature and this page in the same pull request.
