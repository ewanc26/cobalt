/*
 * Unit tests for Cobalt's platform-independent code.
 *
 * Scope is deliberately narrow, per AGENTS.md §10: the parts that can be tested
 * without a console are tested properly here, and nothing pretends to stand in
 * for a hardware pass. Rendering, input plumbing, ProcUI lifecycle and anything
 * that talks to a PDS are verified on the Wii U or not at all.
 */

#include "app/compose.h"
#include "app/post_queue.h"
#include "app/queue_store.h"
#include "app/settings_store.h"
#include "app/graph.h"
#include "app/search.h"
#include "app/timeline.h"
#include "app/signin.h"
#include "app/entropyview.h"
#include "util/entropy_gather.h"
#include "atproto/actors.h"
#include "atproto/session.h"
#include "atproto/feed.h"
#include "atproto/notifications.h"
#include "atproto/chat.h"
#include "app/chat_thread.h"
#include "app/messages.h"
#include "atproto/prefs.h"
#include "cache/session_store.h"
#include "ui/imagecache.h"
#include "ui/imageview.h"
#include "ui/keyboard.h"
#include "ui/listnav.h"
#include "ui/popup.h"
#include "ui/postcard.h"
#include "ui/videoduration.h"
#include "input/input.h"
#include "util/entropy.h"
#include "util/rng.h"
#include "util/clock.h"

#include <math.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void cobalt_test_set_root(const char *path);
void cobalt_test_log_verbose(int on);

static int s_checks = 0;
static int s_failures = 0;
static const char *s_current = "";

#define CHECK(cond)                                                            \
   do {                                                                        \
      s_checks++;                                                              \
      if (!(cond)) {                                                           \
         s_failures++;                                                         \
         fprintf(stderr, "  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__,       \
                 s_current, #cond);                                            \
      }                                                                        \
   } while (0)

#define CHECK_STR(actual, expected)                                            \
   do {                                                                        \
      s_checks++;                                                              \
      if (strcmp((actual), (expected)) != 0) {                                 \
         s_failures++;                                                         \
         fprintf(stderr, "  FAIL %s:%d in %s: got \"%s\", wanted \"%s\"\n",    \
                 __FILE__, __LINE__, s_current, (actual), (expected));         \
      }                                                                        \
   } while (0)

static void
begin(const char *name)
{
   s_current = name;
   printf("- %s\n", name);
}

/* --- service URL normalisation --- */

static void
test_normalise_service(void)
{
   begin("service URL normalisation");

   char out[COBALT_SERVICE_MAX];

   /* Nothing typed falls back to the default PDS rather than failing. */
   CHECK(cobalt_session_normalise_service("", out, sizeof(out)));
   CHECK_STR(out, cobalt_session_default_service());

   CHECK(cobalt_session_normalise_service(NULL, out, sizeof(out)));
   CHECK_STR(out, cobalt_session_default_service());

   /* A bare host gets https, because nobody is typing a scheme on a D-pad. */
   CHECK(cobalt_session_normalise_service("bsky.social", out, sizeof(out)));
   CHECK_STR(out, "https://bsky.social");

   /* Surrounding whitespace and trailing slashes both go: Wolfram builds
    * "<base>/xrpc/<nsid>", so a trailing slash would double up. */
   CHECK(cobalt_session_normalise_service("  bsky.social/  ", out, sizeof(out)));
   CHECK_STR(out, "https://bsky.social");

   CHECK(cobalt_session_normalise_service("https://pds.example.com///", out,
                                          sizeof(out)));
   CHECK_STR(out, "https://pds.example.com");

   /* An explicit scheme is respected, including http for a PDS on the LAN. */
   CHECK(cobalt_session_normalise_service("http://10.0.0.5:3000", out, sizeof(out)));
   CHECK_STR(out, "http://10.0.0.5:3000");

   /* "://" after a path separator is not a scheme. */
   CHECK(cobalt_session_normalise_service("example.com/a://b", out, sizeof(out)));
   CHECK_STR(out, "https://example.com/a://b");

   /* Too long to hold is a refusal, not a truncation that would silently point
    * at the wrong host. */
   char oversized[COBALT_SERVICE_MAX + 32];
   memset(oversized, 'a', sizeof(oversized) - 1);
   oversized[sizeof(oversized) - 1] = '\0';
   CHECK(!cobalt_session_normalise_service(oversized, out, sizeof(out)));
   CHECK_STR(out, "");
}

/* --- the application RNG --- */

/* A fixed stand-in for the 64 bytes `make bundle` writes to entropy.bin. */
static void
fill_test_seed(unsigned char seed[COBALT_ENTROPY_SEED_SIZE], unsigned char tag)
{
   for (size_t i = 0; i < COBALT_ENTROPY_SEED_SIZE; i++) {
      seed[i] = (unsigned char) (i * 7u + tag);
   }
}

static void
test_rng(void)
{
   begin("application RNG");

   unsigned char buffer[64];

   /* Unseeded, it must refuse rather than return anything at all — the whole
    * reason this module exists is that the platform's own fallback is not
    * usable, so falling back is never the right answer. */
   cobalt_rng_shutdown();
   CHECK(!cobalt_rng_ready());
   memset(buffer, 0xAA, sizeof(buffer));
   CHECK(!cobalt_rng_bytes(buffer, sizeof(buffer)));

   /* And it zeroes the caller's buffer, so a failure cannot be mistaken for
    * randomness by a caller that forgot to check. */
   bool all_zero = true;
   for (size_t i = 0; i < sizeof(buffer); i++) {
      if (buffer[i] != 0) {
         all_zero = false;
      }
   }
   CHECK(all_zero);

   /* The mbedTLS-shaped entry point must report failure in mbedTLS's terms,
    * because this is what libcurl calls mid-handshake. */
   CHECK(cobalt_rng_mbedtls(NULL, buffer, sizeof(buffer)) != 0);

   unsigned char seed[COBALT_ENTROPY_SEED_SIZE];
   fill_test_seed(seed, 0x11);
   CHECK(cobalt_rng_init(seed, sizeof(seed)));
   CHECK(cobalt_rng_ready());

   unsigned char first[64];
   CHECK(cobalt_rng_bytes(first, sizeof(first)));
   CHECK(cobalt_rng_mbedtls(NULL, buffer, sizeof(buffer)) == 0);

   /* Successive draws differ — a DRBG that returned its state verbatim would
    * pass a "did it produce bytes" check but nothing else. */
   CHECK(memcmp(first, buffer, sizeof(first)) != 0);

   /* Re-seeding with the same seed reproduces the same stream. That is the
    * DRBG being deterministic, which is exactly why the seed has to be
    * rotated on every boot (see provision_entropy in atproto/atproto.c). */
   CHECK(cobalt_rng_init(seed, sizeof(seed)));
   unsigned char again[64];
   CHECK(cobalt_rng_bytes(again, sizeof(again)));
   CHECK(memcmp(first, again, sizeof(first)) == 0);

   /* A different seed gives a different stream. */
   unsigned char other_seed[COBALT_ENTROPY_SEED_SIZE];
   fill_test_seed(other_seed, 0x77);
   CHECK(cobalt_rng_init(other_seed, sizeof(other_seed)));
   unsigned char different[64];
   CHECK(cobalt_rng_bytes(different, sizeof(different)));
   CHECK(memcmp(first, different, sizeof(first)) != 0);

   /* Rubbish input is refused rather than producing a weakly seeded generator. */
   CHECK(!cobalt_rng_init(NULL, 64));
   CHECK(!cobalt_rng_init(seed, 0));

   /* Leave it seeded for the store tests that follow. */
   CHECK(cobalt_rng_init(seed, sizeof(seed)));
   CHECK(cobalt_rng_ready());
}

/* --- credential store --- */

static long
file_size(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f) {
      return -1;
   }
   fseek(f, 0, SEEK_END);
   long size = ftell(f);
   fclose(f);
   return size;
}

/* Read a file and report whether `needle` appears anywhere in its bytes. */
static bool
file_contains(const char *path, const char *needle)
{
   FILE *f = fopen(path, "rb");
   if (!f) {
      return false;
   }

   static char buffer[65536];
   size_t len = fread(buffer, 1, sizeof(buffer) - 1, f);
   fclose(f);
   buffer[len] = '\0';

   const size_t needle_len = strlen(needle);
   if (needle_len == 0 || needle_len > len) {
      return false;
   }
   for (size_t i = 0; i + needle_len <= len; i++) {
      if (memcmp(buffer + i, needle, needle_len) == 0) {
         return true;
      }
   }
   return false;
}

static void
fill_session(cobalt_stored_session *s)
{
   memset(s, 0, sizeof(*s));
   snprintf(s->service, sizeof(s->service), "https://pds.example.com");
   snprintf(s->handle, sizeof(s->handle), "someone.bsky.social");
   snprintf(s->did, sizeof(s->did), "did:plc:abcdefghijklmnopqrstuvwx");
   snprintf(s->access_jwt, sizeof(s->access_jwt),
            "eyJhbGciOiJIUzI1NiJ9.ACCESSTOKENPAYLOAD.signaturegoeshere");
   snprintf(s->refresh_jwt, sizeof(s->refresh_jwt),
            "eyJhbGciOiJIUzI1NiJ9.REFRESHTOKENPAYLOAD.othersignature");
}

static void
test_session_store_roundtrip(const char *root)
{
   begin("credential store round trip");

   char session_path[512];
   snprintf(session_path, sizeof(session_path), "%s/session.dat", root);

   cobalt_stored_session written;
   fill_session(&written);

   CHECK(!cobalt_session_store_exists());
   CHECK(cobalt_session_store_save(&written));
   CHECK(cobalt_session_store_exists());

   cobalt_stored_session read;
   CHECK(cobalt_session_store_load(&read));
   CHECK_STR(read.service, written.service);
   CHECK_STR(read.handle, written.handle);
   CHECK_STR(read.did, written.did);
   CHECK_STR(read.access_jwt, written.access_jwt);
   CHECK_STR(read.refresh_jwt, written.refresh_jwt);

   /* Proves file_contains is actually reading the file, so the four negative
    * assertions below mean something rather than passing vacuously. */
   CHECK(file_contains(session_path, "COBALTSN"));

   /* The whole point of the module: AGENTS.md §7's "at minimum not in
    * plaintext". If a token can be grepped out of the file, it failed. */
   CHECK(!file_contains(session_path, "ACCESSTOKENPAYLOAD"));
   CHECK(!file_contains(session_path, "REFRESHTOKENPAYLOAD"));
   CHECK(!file_contains(session_path, "someone.bsky.social"));
   CHECK(!file_contains(session_path, "did:plc:"));

   /* Saving again must overwrite rather than append. */
   long first = file_size(session_path);
   CHECK(cobalt_session_store_save(&written));
   CHECK(file_size(session_path) == first);

   /*
    * A fresh nonce per save means two saves of identical credentials must not
    * produce identical ciphertext — otherwise CTR would be reusing a keystream
    * across writes, and the XOR of two saved sessions would leak.
    *
    * Compared from HEADER_BYTES onwards deliberately: the header carries the
    * nonce, which of course differs, so including it would make this pass
    * even if the payload were not encrypted at all.
    */
   enum { HEADER_BYTES = 32 };
   static char first_bytes[512];
   static char second_bytes[512];
   FILE *f = fopen(session_path, "rb");
   size_t n1 = f ? fread(first_bytes, 1, sizeof(first_bytes), f) : 0;
   if (f) fclose(f);
   CHECK(cobalt_session_store_save(&written));
   f = fopen(session_path, "rb");
   size_t n2 = f ? fread(second_bytes, 1, sizeof(second_bytes), f) : 0;
   if (f) fclose(f);
   CHECK(n1 > (size_t) HEADER_BYTES && n1 == n2);
   if (n1 > (size_t) HEADER_BYTES && n1 == n2) {
      CHECK(memcmp(first_bytes + HEADER_BYTES, second_bytes + HEADER_BYTES,
                   n1 - HEADER_BYTES) != 0);
   }

   memset(&written, 0, sizeof(written));
   memset(&read, 0, sizeof(read));
}

static void
test_session_store_clear(const char *root)
{
   begin("credential store sign-out");

   char session_path[512];
   char key_path[512];
   snprintf(session_path, sizeof(session_path), "%s/session.dat", root);
   snprintf(key_path, sizeof(key_path), "%s/device.key", root);

   cobalt_stored_session written;
   fill_session(&written);
   CHECK(cobalt_session_store_save(&written));

   CHECK(cobalt_session_store_clear());
   CHECK(!cobalt_session_store_exists());
   CHECK(file_size(session_path) == -1);
   /* The device key goes too, so any copy of session.dat taken before signing
    * out is left without the key it was written under. */
   CHECK(file_size(key_path) == -1);

   cobalt_stored_session read;
   CHECK(!cobalt_session_store_load(&read));
   CHECK_STR(read.handle, "");
}

static void
test_session_store_rejects_damage(const char *root)
{
   begin("credential store rejects a damaged or foreign file");

   char session_path[512];
   char key_path[512];
   snprintf(session_path, sizeof(session_path), "%s/session.dat", root);
   snprintf(key_path, sizeof(key_path), "%s/device.key", root);

   cobalt_stored_session written;
   fill_session(&written);
   CHECK(cobalt_session_store_save(&written));

   /* Flip a bit in the ciphertext: the payload checksum must catch it rather
    * than handing back a mangled token that would fail confusingly later. */
   FILE *f = fopen(session_path, "r+b");
   CHECK(f != NULL);
   if (f) {
      fseek(f, 40, SEEK_SET);
      int byte = fgetc(f);
      fseek(f, 40, SEEK_SET);
      fputc(byte ^ 0x40, f);
      fclose(f);
   }

   cobalt_stored_session read;
   CHECK(!cobalt_session_store_load(&read));

   /* A session file written under a different device key must be rejected,
    * not silently decrypted into garbage. */
   CHECK(cobalt_session_store_save(&written));
   CHECK(remove(key_path) == 0);
   CHECK(!cobalt_session_store_load(&read));

   /* Something that is not a session file at all. */
   f = fopen(session_path, "wb");
   CHECK(f != NULL);
   if (f) {
      fputs("this is not a session file", f);
      fclose(f);
   }
   CHECK(!cobalt_session_store_load(&read));

   cobalt_session_store_clear();
}

static void
test_session_store_needs_entropy(void)
{
   begin("credential store refuses to run without entropy");

   cobalt_stored_session written;
   fill_session(&written);

   /*
    * With no seeded generator there is no safe way to mint a device key or a
    * CTR nonce, and the platform's own fallback is the tick counter. Saving
    * has to fail rather than produce a file whose key is guessable from the
    * console's uptime.
    */
   cobalt_rng_shutdown();
   CHECK(!cobalt_session_store_save(&written));
   CHECK(!cobalt_session_store_exists());

   unsigned char seed[COBALT_ENTROPY_SEED_SIZE];
   fill_test_seed(seed, 0x11);
   CHECK(cobalt_rng_init(seed, sizeof(seed)));
   CHECK(cobalt_session_store_save(&written));

   cobalt_session_store_clear();
   memset(&written, 0, sizeof(written));
}

/* --- keyboard --- */

/* One frame in which exactly `btn` was pressed. */
static cobalt_input
tap(cobalt_button btn)
{
   cobalt_input in;
   memset(&in, 0, sizeof(in));
   in.pressed[btn] = true;
   in.held[btn] = true;
   return in;
}

static void
type_key(cobalt_keyboard *kb, int row, int col)
{
   kb->row = row;
   kb->col = col;
   cobalt_input in = tap(COBALT_BTN_CONFIRM);
   cobalt_keyboard_update(kb, &in);
}

static void
test_keyboard_typing(void)
{
   begin("keyboard typing and layers");

   char buffer[16] = "";
   cobalt_keyboard kb;
   cobalt_keyboard_open(&kb, buffer, sizeof(buffer), false, false);

   /* Row 1 is "qwertyuiop". */
   type_key(&kb, 1, 0);
   type_key(&kb, 1, 1);
   CHECK_STR(buffer, "qw");

   /* Function row, first key: shift. It is sticky, so both following letters
    * come out capitalised. */
   type_key(&kb, 4, 0);
   type_key(&kb, 1, 0);
   type_key(&kb, 1, 1);
   CHECK_STR(buffer, "qwQW");

   /* Shift again to go back down. */
   type_key(&kb, 4, 0);
   type_key(&kb, 1, 2);
   CHECK_STR(buffer, "qwQWe");

   /* Space bar is the third function key. */
   type_key(&kb, 4, 2);
   CHECK_STR(buffer, "qwQWe ");

   /* B is backspace, without having to walk the focus to the Del key. */
   cobalt_input back = tap(COBALT_BTN_BACK);
   cobalt_keyboard_update(&kb, &back);
   CHECK_STR(buffer, "qwQWe");

   /* Del key (fourth function key) does the same thing. */
   type_key(&kb, 4, 3);
   CHECK_STR(buffer, "qwQW");

   /* OK and Cancel are reported to the caller rather than editing. */
   kb.row = 4;
   kb.col = 4;
   cobalt_input confirm = tap(COBALT_BTN_CONFIRM);
   CHECK(cobalt_keyboard_update(&kb, &confirm) == COBALT_KB_ACCEPTED);
   kb.row = 4;
   kb.col = 5;
   CHECK(cobalt_keyboard_update(&kb, &confirm) == COBALT_KB_CANCELLED);
   CHECK_STR(buffer, "qwQW");

   /* The Emoji key swaps in a layer of multi-byte characters; typing one
    * appends its whole UTF-8 sequence and Del removes it in one step. */
   buffer[0] = '\0';
   type_key(&kb, 4, 6);
   type_key(&kb, 0, 0);
   CHECK_STR(buffer, "\xF0\x9F\x98\x80");
   type_key(&kb, 4, 3);
   CHECK_STR(buffer, "");
}

/*
 * A post's draft needs paragraph breaks, so the keyboard it is typed on has a
 * Ret key; a handle, a password or a query is one line and keeps the wider
 * space bar. The text renderer already breaks on '\n', so nothing else has to
 * change for the break to survive into the card.
 */
static void
test_keyboard_multiline(void)
{
   begin("a post's keyboard types line breaks; a one-line field has no Ret");

   char buffer[32] = "";
   cobalt_keyboard kb;
   cobalt_keyboard_open(&kb, buffer, sizeof(buffer), false, true);

   /* Row 1 is "qwertyuiop"; the function row is Shift, #+=, Space, Del, Ret,
    * OK, Cancel, Emoji. */
   type_key(&kb, 1, 0);
   type_key(&kb, 4, 4);
   type_key(&kb, 1, 1);
   CHECK_STR(buffer, "q\nw");

   /* B steps over a break like any other byte. */
   cobalt_input back = tap(COBALT_BTN_BACK);
   cobalt_keyboard_update(&kb, &back);
   CHECK_STR(buffer, "q\n");

   /* The same key on a single-line field is OK, which reports to the caller
    * and leaves the buffer alone. */
   char single[8] = "";
   cobalt_keyboard_open(&kb, single, sizeof(single), false, false);
   type_key(&kb, 4, 4);
   CHECK_STR(single, "");
}

static void
test_keyboard_bounds(void)
{
   begin("keyboard respects the buffer it was given");

   char buffer[4] = "";
   cobalt_keyboard kb;
   cobalt_keyboard_open(&kb, buffer, sizeof(buffer), false, false);

   for (int i = 0; i < 10; i++) {
      type_key(&kb, 1, 0);
   }
   /* Three characters plus a terminator, and no overrun. */
   CHECK_STR(buffer, "qqq");

   /* Backspacing an empty buffer must not walk backwards out of it. */
   char empty[8] = "";
   cobalt_keyboard_open(&kb, empty, sizeof(empty), false, false);
   cobalt_input back = tap(COBALT_BTN_BACK);
   cobalt_keyboard_update(&kb, &back);
   cobalt_keyboard_update(&kb, &back);
   CHECK_STR(empty, "");
}

static void
test_keyboard_multibyte(void)
{
   begin("keyboard deletes whole codepoints");

   /* Seeded from stored text rather than typed: display names and handles are
    * UTF-8, and deleting one byte of a multi-byte sequence would leave a
    * dangling continuation byte that renders as tofu. */
   char buffer[32] = "caf\xc3\xa9";   /* "café" */
   cobalt_keyboard kb;
   cobalt_keyboard_open(&kb, buffer, sizeof(buffer), false, false);

   cobalt_input back = tap(COBALT_BTN_BACK);
   cobalt_keyboard_update(&kb, &back);
   CHECK_STR(buffer, "caf");
}

static void
test_keyboard_display(void)
{
   begin("keyboard display text");

   char buffer[64] = "hello";
   cobalt_keyboard kb;
   char shown[32];

   /* Plain text, with a caret so an empty field still reads as focused. */
   cobalt_keyboard_open(&kb, buffer, sizeof(buffer), false, false);
   cobalt_keyboard_display_text(&kb, shown, sizeof(shown));
   CHECK_STR(shown, "hello_");

   /* Masked: one dot per character, never the characters themselves. */
   cobalt_keyboard_open(&kb, buffer, sizeof(buffer), true, false);
   cobalt_keyboard_display_text(&kb, shown, sizeof(shown));
   CHECK_STR(shown, "*****_");

   /* A masked multi-byte string is masked per character, not per byte. */
   char accented[32] = "caf\xc3\xa9";
   cobalt_keyboard_open(&kb, accented, sizeof(accented), true, false);
   cobalt_keyboard_display_text(&kb, shown, sizeof(shown));
   CHECK_STR(shown, "****_");

   /* Longer than the field: the tail is kept, because that is where the caret
    * is, and the window never opens mid-codepoint. */
   char lengthy[64] = "abcdefghijklmnopqrstuvwxyz";
   cobalt_keyboard_open(&kb, lengthy, sizeof(lengthy), false, false);
   char narrow[8];
   cobalt_keyboard_display_text(&kb, narrow, sizeof(narrow));
   CHECK_STR(narrow, "uvwxyz_");

   /* An empty buffer is just the caret. */
   char blank[8] = "";
   cobalt_keyboard_open(&kb, blank, sizeof(blank), false, false);
   cobalt_keyboard_display_text(&kb, shown, sizeof(shown));
   CHECK_STR(shown, "_");
}


/* --- feed formatting --- */

static void
test_feed_text(void)
{
   begin("post text truncation");

   char out[16];

   cobalt_feed_copy_text(out, sizeof(out), "short");
   CHECK_STR(out, "short");

   cobalt_feed_copy_text(out, sizeof(out), "");
   CHECK_STR(out, "");

   cobalt_feed_copy_text(out, sizeof(out), NULL);
   CHECK_STR(out, "");

   /* Exactly filling the buffer must not be treated as overflow. */
   cobalt_feed_copy_text(out, sizeof(out), "123456789012345");
   CHECK_STR(out, "123456789012345");

   /* One byte over: truncated with an ellipsis, still NUL-terminated. */
   cobalt_feed_copy_text(out, sizeof(out), "1234567890123456");
   CHECK(strlen(out) < sizeof(out));
   CHECK(strcmp(out, "1234567890123456") != 0);
   CHECK(strstr(out, "...") != NULL);

   /*
    * Post text is arbitrary UTF-8 (AGENTS.md §5). A cut through a multi-byte
    * sequence would render as tofu, so truncation has to land on a codepoint
    * boundary — checked by confirming no trailing continuation byte survives.
    */
   char wide[24];
   cobalt_feed_copy_text(wide, sizeof(wide),
                         "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"
                         "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"
                         "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
   size_t n = strlen(wide);
   CHECK(n < sizeof(wide));
   for (size_t i = 0; i < n; i++) {
      /* Every lead byte must be followed by its full complement of
       * continuation bytes; a dangling one means the cut was mid-sequence. */
      unsigned char c = (unsigned char) wide[i];
      if ((c & 0xE0) == 0xC0) {
         CHECK(i + 1 < n && ((unsigned char) wide[i + 1] & 0xC0) == 0x80);
      } else if ((c & 0xF0) == 0xE0) {
         CHECK(i + 2 < n && ((unsigned char) wide[i + 2] & 0xC0) == 0x80);
      }
   }
}

static void
test_feed_counts(void)
{
   begin("engagement counts");

   char out[COBALT_POST_META_MAX];

   /* A post nobody has touched gets no line at all, rather than three zeroes. */
   cobalt_feed_format_counts(out, sizeof(out), 0, 0, 0);
   CHECK_STR(out, "");

   cobalt_feed_format_counts(out, sizeof(out), 0, 0, 1);
   CHECK_STR(out, "1 like");

   cobalt_feed_format_counts(out, sizeof(out), 1, 0, 0);
   CHECK_STR(out, "1 reply");

   /* Plurals, and the separator only between present items. */
   cobalt_feed_format_counts(out, sizeof(out), 2, 0, 5);
   CHECK_STR(out, "2 replies \xc2\xb7 5 likes");

   cobalt_feed_format_counts(out, sizeof(out), 12, 30, 88);
   CHECK_STR(out, "12 replies \xc2\xb7 30 reposts \xc2\xb7 88 likes");

   /* Negative counts should never arrive, but must not produce "-1 likes". */
   cobalt_feed_format_counts(out, sizeof(out), -1, 0, 3);
   CHECK_STR(out, "3 likes");

   /* A tiny buffer drops what will not fit rather than overflowing. */
   char tiny[12];
   cobalt_feed_format_counts(tiny, sizeof(tiny), 1000000, 2000000, 3000000);
   CHECK(strlen(tiny) < sizeof(tiny));
}

static void
test_feed_embeds(void)
{
   begin("embed notes");

   /* The wire carries view variants, so matching is on prefix. */
   CHECK_STR(cobalt_feed_embed_note("app.bsky.embed.images#view"), "[image]");
   CHECK_STR(cobalt_feed_embed_note("app.bsky.embed.video#view"), "[video]");
   CHECK_STR(cobalt_feed_embed_note("app.bsky.embed.external#view"), "[link]");
   CHECK_STR(cobalt_feed_embed_note("app.bsky.embed.record#viewRecord"), "[quote]");

   /* recordWithMedia is a longer prefix than record and must win. */
   CHECK_STR(cobalt_feed_embed_note("app.bsky.embed.recordWithMedia#view"),
             "[quote + media]");

   /* An unknown or missing type draws nothing rather than a wrong guess. */
   CHECK_STR(cobalt_feed_embed_note("app.bsky.embed.somethingNew#view"), "");
   CHECK_STR(cobalt_feed_embed_note(""), "");
   CHECK_STR(cobalt_feed_embed_note(NULL), "");
}

static void
test_prefs(void)
{
   begin("muted words and hide reposts");

   cobalt_prefs p;
   cobalt_prefs_clear(&p);
   CHECK(!cobalt_prefs_text_is_muted(&p, "anything", NULL, 0));
   CHECK(!wf_muted_list_add(&p.muted, "", true, false, false, NULL));

   /* The matching rules are Wolfram's and pinned by its vectors; here, that a
    * list reaches them and that a tag mute applies to a post's tag facets. */
   CHECK(wf_muted_list_add(&p.muted, "cat", true, false, false, NULL));
   CHECK(wf_muted_list_add(&p.muted, "spoilers", false, true, false, NULL));
   CHECK(cobalt_prefs_text_is_muted(&p, "I like my cat.", NULL, 0));
   CHECK(!cobalt_prefs_text_is_muted(&p, "a category", NULL, 0));
   const char *tags[] = {"Spoilers"};
   CHECK(cobalt_prefs_text_is_muted(&p, "text", tags, 1));
   CHECK(!cobalt_prefs_text_is_muted(&p, "spoilers in the text", NULL, 0));
   CHECK(!cobalt_prefs_text_is_muted(NULL, "cat", NULL, 0));
   CHECK(p.muted.count == 2);

   /* Hide reposts is Cobalt's own rule, and only on the home timeline. */
   static cobalt_feed feed;
   memset(&feed, 0, sizeof(feed));
   cobalt_prefs_clear(&p);
   p.hide_reposts = true;
   for (int i = 0; i < 4; i++) {
      snprintf(feed.posts[i].text, sizeof(feed.posts[i].text), "post %d", i);
   }
   snprintf(feed.posts[2].reposted_by, sizeof(feed.posts[2].reposted_by), "someone");
   feed.count = 4;

   CHECK(cobalt_prefs_filter_feed(&p, &feed, 0, false) == 0);
   CHECK(feed.count == 4);
   CHECK(cobalt_prefs_filter_feed(&p, &feed, 1, true) == 1);
   CHECK(feed.count == 3);
   CHECK_STR(feed.posts[1].text, "post 1");
   CHECK_STR(feed.posts[2].text, "post 3");
}

/* --- first-run seed from touch input --- */

static void
feed_scribble(cobalt_gather *g, int n, int step_x, int step_y, uint32_t tick0)
{
   /* A zig-zag across the panel, offset every call so cells differ. */
   for (int i = 0; i < n; i++) {
      int x = (i * step_x) % COBALT_GATHER_PANEL_W;
      int y = (i * step_y) % COBALT_GATHER_PANEL_H;
      cobalt_gather_add(g, x, y, tick0 + (uint32_t) i * 7919u);
   }
}

static void
test_entropy_gather(const char *root)
{
   begin("first-run seed from touch input");

   cobalt_gather g;
   unsigned char seed[COBALT_ENTROPY_SEED_SIZE];

   /* A resting finger, or jitter under 4px, is not a stream of samples. */
   cobalt_gather_init(&g, "dev", 3);
   for (int i = 0; i < 5000; i++) {
      cobalt_gather_add(&g, 100 + (i & 1), 100 + ((i >> 1) & 1), (uint32_t) i);
   }
   CHECK(g.accepted <= 1);
   CHECK(!cobalt_gather_done(&g));
   CHECK(!cobalt_gather_finish(&g, seed));
   {
      int zero = 1;
      for (int i = 0; i < COBALT_ENTROPY_SEED_SIZE; i++) if (seed[i]) zero = 0;
      CHECK(zero);   /* refusal leaves nothing behind */
   }

   /* Plenty of movement in one small area: enough samples, too few places. */
   cobalt_gather_init(&g, NULL, 0);
   for (int i = 0; i < 2000; i++) {
      cobalt_gather_add(&g, 100 + (i % 2) * 20, 100 + (i % 3) * 10, (uint32_t) i);
   }
   CHECK(g.accepted == COBALT_GATHER_SAMPLES);
   CHECK(g.cells < COBALT_GATHER_CELLS);
   CHECK(!cobalt_gather_done(&g));
   CHECK(cobalt_gather_percent(&g) < 100);

   /* Off-panel coordinates are refused. */
   CHECK(!cobalt_gather_add(&g, -1, 5, 0));
   CHECK(!cobalt_gather_add(&g, 5, COBALT_GATHER_PANEL_H, 0));

   /* A real scribble completes, and different strokes give different seeds. */
   unsigned char a[COBALT_ENTROPY_SEED_SIZE], b[COBALT_ENTROPY_SEED_SIZE];
   cobalt_gather_init(&g, "dev", 3);
   feed_scribble(&g, 3000, 37, 23, 1000);
   CHECK(cobalt_gather_done(&g));
   CHECK(cobalt_gather_percent(&g) == 100);
   CHECK(cobalt_gather_finish(&g, a));
   cobalt_gather_init(&g, "dev", 3);
   feed_scribble(&g, 3000, 41, 29, 1000);
   CHECK(cobalt_gather_finish(&g, b));
   CHECK(memcmp(a, b, sizeof a) != 0);
   /* Same input, same device bytes: same seed (it is a hash, not a coin). */
   cobalt_gather_init(&g, "dev", 3);
   feed_scribble(&g, 3000, 37, 23, 1000);
   unsigned char c[COBALT_ENTROPY_SEED_SIZE];
   CHECK(cobalt_gather_finish(&g, c));
   CHECK(memcmp(a, c, sizeof a) == 0);
   /* Different device bytes, same strokes: different seed. */
   cobalt_gather_init(&g, "other", 5);
   feed_scribble(&g, 3000, 37, 23, 1000);
   CHECK(cobalt_gather_finish(&g, c));
   CHECK(memcmp(a, c, sizeof a) != 0);
   /* The two halves are not the same bytes. */
   CHECK(memcmp(a, a + 32, 32) != 0);

   /* The screen: collects from touch, saves, then asks for a restart. */
   char path[300];
   snprintf(path, sizeof path, "%s/gathered.bin", root);
   remove(path);
   cobalt_entropy_view v;
   cobalt_entropy_view_init(&v, path, "dev", 3);
   cobalt_input in;
   memset(&in, 0, sizeof in);
   CHECK(!cobalt_entropy_seed_exists(path));
   for (int i = 0; i < 3000 && v.state == COBALT_ENTROPY_VIEW_COLLECTING; i++) {
      in.touch_down = true;
      in.touch_x = (i * 37) % COBALT_GATHER_PANEL_W;
      in.touch_y = (i * 23) % COBALT_GATHER_PANEL_H;
      CHECK(cobalt_entropy_view_update(&v, &in, 5000u + (uint32_t) i * 7919u) == COBALT_ENTROPY_VIEW_STAY);
   }
   CHECK(v.state == COBALT_ENTROPY_VIEW_SAVED);
   CHECK(cobalt_entropy_seed_exists(path));
   unsigned char loaded[COBALT_ENTROPY_SEED_SIZE];
   CHECK(cobalt_entropy_seed_load(path, loaded));
   in.touch_down = false;
   in.pressed[COBALT_BTN_CONFIRM] = true;
   CHECK(cobalt_entropy_view_update(&v, &in, 0) == COBALT_ENTROPY_VIEW_QUIT);
   remove(path);

   /* An unwritable path fails visibly and can be retried. */
   cobalt_entropy_view_init(&v, "/nonexistent-dir/x/entropy.bin", NULL, 0);
   memset(&in, 0, sizeof in);
   for (int i = 0; i < 3000 && v.state == COBALT_ENTROPY_VIEW_COLLECTING; i++) {
      in.touch_down = true;
      in.touch_x = (i * 37) % COBALT_GATHER_PANEL_W;
      in.touch_y = (i * 23) % COBALT_GATHER_PANEL_H;
      cobalt_entropy_view_update(&v, &in, (uint32_t) i);
   }
   CHECK(v.state == COBALT_ENTROPY_VIEW_FAILED);
   memset(&in, 0, sizeof in);
   in.pressed[COBALT_BTN_BACK] = true;
   CHECK(cobalt_entropy_view_update(&v, &in, 0) == COBALT_ENTROPY_VIEW_SKIP);
}

static void
test_video_duration_format(void)
{
   char out[16];

   begin("a video duration reads as m:ss, or h:mm:ss from an hour");
   CHECK(cobalt_video_duration_format(0.0, out, sizeof(out)));
   CHECK_STR(out, "0:00");
   CHECK(cobalt_video_duration_format(5.4, out, sizeof(out)));
   CHECK_STR(out, "0:05");
   CHECK(cobalt_video_duration_format(83.4, out, sizeof(out)));
   CHECK_STR(out, "1:23");
   CHECK(cobalt_video_duration_format(754.6, out, sizeof(out)));
   CHECK_STR(out, "12:35");
   CHECK(cobalt_video_duration_format(3723.0, out, sizeof(out)));
   CHECK_STR(out, "1:02:03");
   CHECK(cobalt_video_duration_format(36000.0, out, sizeof(out)));
   CHECK_STR(out, "10:00:00");

   begin("a duration that is negative, not finite or too long for the text is unknown");
   out[0] = 'x';
   CHECK(!cobalt_video_duration_format(-1.0, out, sizeof(out)));
   CHECK_STR(out, "");
   CHECK(!cobalt_video_duration_format(NAN, out, sizeof(out)));
   CHECK_STR(out, "");
   CHECK(!cobalt_video_duration_format(83.0, out, 3));
   CHECK_STR(out, "");
}

/* A master playlist names its variants; the duration is read from one of them. */
static const char *const MASTER_TWO_VARIANTS =
   "#EXTM3U\r\n"
   "#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=1280x720\r\nhigh/index.m3u8\r\n"
   "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360\r\nlow/index.m3u8\r\n";
static const char *const MASTER_URL = "https://video.test/pl/master.m3u8";
static const char *const LOW_URL = "https://video.test/pl/low/index.m3u8";
/* A variant that is itself a master must not be followed a second time. */
static const char *const NESTED_MASTER =
   "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=100000\ndeeper/index.m3u8\n";
static const char *const VOD_BODY =
   "#EXTM3U\n#EXTINF:10.000,\nseg0.ts\n#EXT-X-ENDLIST\n";

static const char *
variant_of(const char *body, const char *base, char *out, size_t out_size)
{
   if (!cobalt_video_master_variant(body, strlen(body), base, out, out_size)) {
      out[0] = '\0';
   }
   return out;
}

static void
test_video_master_variant(void)
{
   char out[COBALT_VIDEO_DURATION_URI_MAX];

   begin("a master playlist picks the lowest BANDWIDTH variant, not the first");
   CHECK_STR(variant_of(MASTER_TWO_VARIANTS, MASTER_URL, out, sizeof(out)), LOW_URL);

   begin("a master playlist resolves relative and absolute variant URIs");
   CHECK_STR(variant_of("#EXT-X-STREAM-INF:BANDWIDTH=900000\nrel/a.m3u8\n"
                        "#EXT-X-STREAM-INF:BANDWIDTH=300000\n"
                        "https://cdn.test/abs/b.m3u8\n",
                        MASTER_URL, out, sizeof(out)),
             "https://cdn.test/abs/b.m3u8");
   CHECK_STR(variant_of("#EXT-X-STREAM-INF:BANDWIDTH=1\nrel/a.m3u8\n",
                        "https://video.test/pl/master.m3u8?sig=1&x=2", out, sizeof(out)),
             "https://video.test/pl/rel/a.m3u8");
   CHECK_STR(variant_of("#EXT-X-STREAM-INF:BANDWIDTH=1\n/root/a.m3u8\n",
                        MASTER_URL, out, sizeof(out)),
             "https://video.test/root/a.m3u8");
   CHECK_STR(variant_of("#EXT-X-STREAM-INF:BANDWIDTH=1\n//cdn.test/s.m3u8\n",
                        MASTER_URL, out, sizeof(out)),
             "https://cdn.test/s.m3u8");

   begin("a variant with no BANDWIDTH is the first one; a bandwidth beats none");
   CHECK_STR(variant_of("#EXT-X-STREAM-INF:RESOLUTION=640x360\nfirst.m3u8\n"
                        "#EXT-X-STREAM-INF:RESOLUTION=1280x720\nsecond.m3u8\n",
                        MASTER_URL, out, sizeof(out)),
             "https://video.test/pl/first.m3u8");
   CHECK_STR(variant_of("#EXT-X-STREAM-INF:RESOLUTION=640x360\nnobw.m3u8\n"
                        "#EXT-X-STREAM-INF:BANDWIDTH=500\nbw.m3u8\n",
                        MASTER_URL, out, sizeof(out)),
             "https://video.test/pl/bw.m3u8");

   begin("attribute lists are read whole: quoted commas and AVERAGE-BANDWIDTH do not confuse BANDWIDTH");
   /* A split on commas would read BANDWIDTH=1 out of the quoted CODECS and pick r
    * only by luck of order; the walk reads 500000 and picks r for its 100. */
   CHECK_STR(variant_of("#EXT-X-STREAM-INF:CODECS=\"BANDWIDTH=1,x\",BANDWIDTH=500000\r\nq.m3u8\r\n"
                        "#EXT-X-STREAM-INF:BANDWIDTH=100\nr.m3u8\n",
                        MASTER_URL, out, sizeof(out)),
             "https://video.test/pl/r.m3u8");
   CHECK_STR(variant_of("#EXT-X-STREAM-INF:AVERAGE-BANDWIDTH=10,BANDWIDTH=900000\nq.m3u8\n"
                        "#EXT-X-STREAM-INF:AVERAGE-BANDWIDTH=1,BANDWIDTH=5000\nr.m3u8\n",
                        MASTER_URL, out, sizeof(out)),
             "https://video.test/pl/r.m3u8");
   CHECK_STR(variant_of("#EXT-X-STREAM-INF:AVERAGE-BANDWIDTH=1,BANDWIDTH=900\na.m3u8\n"
                        "#EXT-X-STREAM-INF:AVERAGE-BANDWIDTH=9000000,BANDWIDTH=100\nb.m3u8\n",
                        MASTER_URL, out, sizeof(out)),
             "https://video.test/pl/b.m3u8");

   begin("a playlist with no STREAM-INF, or a STREAM-INF with no URI, has no variant");
   CHECK(!cobalt_video_master_variant(VOD_BODY, strlen(VOD_BODY), MASTER_URL,
                                      out, sizeof(out)));
   CHECK(!cobalt_video_master_variant("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\n",
                                      strlen("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\n"),
                                      MASTER_URL, out, sizeof(out)));
   CHECK(!cobalt_video_master_variant(NULL, 0, MASTER_URL, out, sizeof(out)));
}

/* The fake fetch answers by URL and records each request in order. */
#define FETCH_LOG_MAX 8
static char s_fetch_log[FETCH_LOG_MAX][COBALT_VIDEO_DURATION_URI_MAX];
static int s_fetch_log_n;
static const char *s_fetch_answer_master;
static const char *s_fetch_answer_variant;

static bool
logging_fetch(const char *url, cobalt_http_response *out)
{
   if (s_fetch_log_n < FETCH_LOG_MAX) {
      snprintf(s_fetch_log[s_fetch_log_n], sizeof(s_fetch_log[0]), "%s", url);
   }
   s_fetch_log_n++;

   const char *body = NULL;
   if (strcmp(url, MASTER_URL) == 0) {
      body = s_fetch_answer_master;
   } else if (strcmp(url, LOW_URL) == 0) {
      body = s_fetch_answer_variant;
   }
   if (!body) {
      return false;
   }
   const size_t n = strlen(body);
   out->data = malloc(n + 1);
   if (!out->data) {
      return false;
   }
   memcpy(out->data, body, n + 1);
   out->size = n;
   out->status = 200;
   return true;
}

static void
test_video_master_fetch_count(void)
{
   cobalt_video_duration_set_fetcher(logging_fetch);

   begin("a VOD playlist is one fetch");
   cobalt_video_duration_reset_for_test();
   s_fetch_log_n = 0;
   s_fetch_answer_master = VOD_BODY;
   CHECK(cobalt_video_duration_lookup(MASTER_URL) == NULL);
   CHECK(cobalt_video_duration_drain() == 1);
   CHECK(cobalt_video_duration_fetch_count() == 1);
   CHECK(s_fetch_log_n == 1);

   begin("a master playlist is followed to one variant: two fetches, the lowest variant second");
   cobalt_video_duration_reset_for_test();
   s_fetch_log_n = 0;
   s_fetch_answer_master = MASTER_TWO_VARIANTS;
   s_fetch_answer_variant = VOD_BODY;
   CHECK(cobalt_video_duration_lookup(MASTER_URL) == NULL);
   CHECK(cobalt_video_duration_drain() == 1);
   CHECK(cobalt_video_duration_fetch_count() == 2);
   CHECK(s_fetch_log_n == 2);
   CHECK_STR(s_fetch_log[0], MASTER_URL);
   CHECK_STR(s_fetch_log[1], LOW_URL);

   begin("a variant that is itself a master stops the chain: no third fetch");
   cobalt_video_duration_reset_for_test();
   s_fetch_log_n = 0;
   s_fetch_answer_master = MASTER_TWO_VARIANTS;
   s_fetch_answer_variant = NESTED_MASTER;
   CHECK(cobalt_video_duration_lookup(MASTER_URL) == NULL);
   CHECK(cobalt_video_duration_drain() == 1);
   CHECK(cobalt_video_duration_fetch_count() == 2);
   CHECK(s_fetch_log_n == 2);
   CHECK(cobalt_video_duration_lookup(MASTER_URL) == NULL);
   CHECK(cobalt_video_duration_drain() == 0);
   CHECK(cobalt_video_duration_fetch_count() == 2);

   cobalt_video_duration_set_fetcher(NULL);
   cobalt_video_duration_reset_for_test();
}

static void
test_feed_link_domain(void)
{
   begin("a card is a link or a video poster");
   {
      cobalt_post post;

      memset(&post, 0, sizeof(post));
      CHECK(!cobalt_post_has_card(&post));
      post.link.video = true;
      CHECK(cobalt_post_has_card(&post));
      post.link.video = false;
      snprintf(post.link.uri, sizeof(post.link.uri), "https://e.x/p");
      CHECK(cobalt_post_has_card(&post));
   }

   begin("sign-in starts from the typed host, or the default when empty");
   CHECK(strcmp(cobalt_session_entry_service(""), cobalt_session_default_service()) == 0);
   CHECK(strcmp(cobalt_session_entry_service("https://pds.example"), "https://pds.example") == 0);

   begin("link card domain extraction");

   char out[64];

   cobalt_feed_link_domain("https://bsky.app/profile/ewan.bsky.social", out,
                           sizeof(out));
   CHECK_STR(out, "bsky.app");

   /* A leading www. is stripped; it adds noise without information. */
   cobalt_feed_link_domain("https://www.example.com/path?q=1#frag", out,
                           sizeof(out));
   CHECK_STR(out, "example.com");

   /* No scheme is a legitimate shape for a link-card URI to carry. */
   cobalt_feed_link_domain("example.com/path", out, sizeof(out));
   CHECK_STR(out, "example.com");

   /* A userinfo component is stripped, not folded into the host. */
   cobalt_feed_link_domain("https://user@example.com/", out, sizeof(out));
   CHECK_STR(out, "example.com");

   /* A port is not part of the displayed host. */
   cobalt_feed_link_domain("https://example.com:8080/", out, sizeof(out));
   CHECK_STR(out, "example.com");

   cobalt_feed_link_domain(NULL, out, sizeof(out));
   CHECK_STR(out, "");
   cobalt_feed_link_domain("", out, sizeof(out));
   CHECK_STR(out, "");

   /* A tiny buffer truncates rather than overflowing. */
   char tiny[5];
   cobalt_feed_link_domain("https://example.com/", tiny, sizeof(tiny));
   CHECK(strlen(tiny) < sizeof(tiny));
}


/* --- actor lists (muted/blocked accounts) --- */

static void
test_actor_list_remove(void)
{
   begin("removing a row from an actor list");

   cobalt_actor_list list;
   memset(&list, 0, sizeof(list));
   list.count = 3;
   snprintf(list.actors[0].did, sizeof(list.actors[0].did), "did:plc:alice");
   snprintf(list.actors[1].did, sizeof(list.actors[1].did), "did:plc:bob");
   snprintf(list.actors[2].did, sizeof(list.actors[2].did), "did:plc:carol");

   /* Removing the middle row shifts the tail down rather than leaving a
    * hole — the list draws by index 0..count. */
   CHECK(cobalt_actor_list_remove(&list, "did:plc:bob"));
   CHECK(list.count == 2);
   CHECK_STR(list.actors[0].did, "did:plc:alice");
   CHECK_STR(list.actors[1].did, "did:plc:carol");

   /* A refresh can replace the list while a request is in flight, so the
    * row legitimately may not be there any more — that is not a bug. */
   CHECK(!cobalt_actor_list_remove(&list, "did:plc:bob"));
   CHECK(list.count == 2);

   CHECK(!cobalt_actor_list_remove(NULL, "did:plc:alice"));
   CHECK(!cobalt_actor_list_remove(&list, NULL));
}

/* --- optimistic interactions --- */

static void
seed_feed(cobalt_feed *feed, const char *uri, int likes, int reposts)
{
   memset(feed, 0, sizeof(*feed));
   feed->count = 1;
   snprintf(feed->posts[0].uri, sizeof(feed->posts[0].uri), "%s", uri);
   feed->posts[0].like_count = likes;
   feed->posts[0].repost_count = reposts;
}

static void
test_interactions(void)
{
   begin("optimistic like and repost");

   static cobalt_feed feed;
   const char *uri = "at://did:plc:abc/app.bsky.feed.post/xyz";

   seed_feed(&feed, uri, 10, 4);

   /* Liking records the record URI and moves the count. */
   CHECK(cobalt_feed_apply_like(&feed, uri, "at://did:plc:me/app.bsky.feed.like/1"));
   CHECK(feed.posts[0].like_count == 11);
   CHECK(feed.posts[0].viewer_like[0] != '\0');
   CHECK(strstr(feed.posts[0].meta, "11 likes") != NULL);

   /*
    * A duplicate confirmation must not double-count. The server can answer the
    * same request twice from the app's point of view — a retry after a refresh
    * handler fired mid-request — and the count has to stay where the server
    * thinks it is.
    */
   CHECK(cobalt_feed_apply_like(&feed, uri, "at://did:plc:me/app.bsky.feed.like/1"));
   CHECK(feed.posts[0].like_count == 11);

   /* Undoing puts it back and clears the record. */
   CHECK(cobalt_feed_apply_like(&feed, uri, NULL));
   CHECK(feed.posts[0].like_count == 10);
   CHECK(feed.posts[0].viewer_like[0] == '\0');

   /* And a duplicate undo does not go below the server's value. */
   CHECK(cobalt_feed_apply_like(&feed, uri, NULL));
   CHECK(feed.posts[0].like_count == 10);

   /* Reposts are independent of likes. */
   CHECK(cobalt_feed_apply_repost(&feed, uri, "at://did:plc:me/app.bsky.feed.repost/1"));
   CHECK(feed.posts[0].repost_count == 5);
   CHECK(feed.posts[0].like_count == 10);
   CHECK(feed.posts[0].viewer_like[0] == '\0');
   CHECK(feed.posts[0].viewer_repost[0] != '\0');

   /* A count already at zero must never go negative. */
   seed_feed(&feed, uri, 0, 0);
   CHECK(cobalt_feed_apply_like(&feed, uri, NULL));
   CHECK(feed.posts[0].like_count == 0);

   /*
    * A refresh can replace the feed while a request is in flight, so the post
    * may be gone by the time the answer arrives. That has to be a clean miss,
    * not a write into whatever is at that index now.
    */
   CHECK(!cobalt_feed_apply_like(&feed, "at://did:plc:abc/app.bsky.feed.post/gone",
                                 "at://x"));
   CHECK(!cobalt_feed_apply_like(NULL, uri, NULL));

   /* The same helpers drive a loaded thread, since a post is frequently on
    * screen in both places at once. */
   static cobalt_thread thread;
   memset(&thread, 0, sizeof(thread));
   thread.count = 1;
   snprintf(thread.posts[0].uri, sizeof(thread.posts[0].uri), "%s", uri);
   thread.posts[0].like_count = 2;

   CHECK(cobalt_thread_apply_like(&thread, uri, "at://did:plc:me/app.bsky.feed.like/2"));
   CHECK(thread.posts[0].like_count == 3);
   CHECK(!cobalt_thread_apply_like(&thread, "at://nope", NULL));
}


/* --- composing --- */

static void
test_compose_thread(void)
{
   begin("composing a thread");

   static cobalt_compose c;
   cobalt_compose_init(&c);

   /* Nothing written yet: nothing to keep. */
   CHECK(cobalt_compose_can_extend(&c));
   CHECK(!cobalt_compose_extend(&c));

   snprintf(c.text, sizeof(c.text), "one");
   CHECK(cobalt_compose_extend(&c));
   CHECK(c.thread_count == 1);
   CHECK_STR(c.text, "");
   CHECK(!c.confirming);

   snprintf(c.text, sizeof(c.text), "two");
   CHECK(cobalt_compose_extend(&c));
   snprintf(c.text, sizeof(c.text), "three");

   const char *texts[COBALT_THREAD_POSTS_MAX];
   CHECK(cobalt_compose_thread_texts(&c, texts) == 3);
   CHECK_STR(texts[0], "one");
   CHECK_STR(texts[1], "two");
   CHECK_STR(texts[2], "three");

   /* The thread stops growing at the limit, counting the post being written. */
   while (cobalt_compose_can_extend(&c)) {
      snprintf(c.text, sizeof(c.text), "more");
      CHECK(cobalt_compose_extend(&c));
   }
   CHECK(c.thread_count + 1 == COBALT_THREAD_POSTS_MAX);
   snprintf(c.text, sizeof(c.text), "last");
   CHECK(!cobalt_compose_extend(&c));
   CHECK(cobalt_compose_thread_texts(&c, texts) == COBALT_THREAD_POSTS_MAX);

   /* Replies, quotes and posts with an image are not threads. */
   cobalt_compose_init(&c);
   snprintf(c.attach_path, sizeof(c.attach_path), "/x.jpg");
   CHECK(!cobalt_compose_can_extend(&c));
   cobalt_compose_init(&c);
   snprintf(c.parent_uri, sizeof(c.parent_uri), "at://p");
   CHECK(!cobalt_compose_can_extend(&c));
   cobalt_compose_init(&c);
   snprintf(c.quote_uri, sizeof(c.quote_uri), "at://q");
   CHECK(!cobalt_compose_can_extend(&c));
   CHECK(!cobalt_compose_can_extend(NULL));
}

static void
test_compose(void)
{
   begin("composing a post or reply");

   cobalt_compose compose;
   cobalt_compose_init(&compose);

   CHECK(!cobalt_compose_is_reply(&compose));
   CHECK(cobalt_compose_remaining(&compose) == COBALT_COMPOSE_GRAPHEMES);
   CHECK(compose.text[0] == '\0');

   /* The keyboard writes straight into the buffer, so the counter has to track
    * bytes actually present rather than anything the screen remembers. */
   snprintf(compose.text, sizeof(compose.text), "hello");
   CHECK(cobalt_compose_remaining(&compose) == COBALT_COMPOSE_GRAPHEMES - 5);

   /*
    * Counted in codepoints, not bytes. A post of accented or CJK text would
    * otherwise appear to blow the limit at a third of its real length — the
    * exact case AGENTS.md §5 warns about treating as ASCII.
    */
   snprintf(compose.text, sizeof(compose.text), "caf\xc3\xa9");
   CHECK(cobalt_compose_remaining(&compose) == COBALT_COMPOSE_GRAPHEMES - 4);

   snprintf(compose.text, sizeof(compose.text),
            "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
   CHECK(cobalt_compose_remaining(&compose) == COBALT_COMPOSE_GRAPHEMES - 3);

   /* Over the limit reads negative rather than clamping, so the UI can say by
    * how much. */
   memset(compose.text, 'a', COBALT_COMPOSE_GRAPHEMES + 10);
   compose.text[COBALT_COMPOSE_GRAPHEMES + 10] = '\0';
   CHECK(cobalt_compose_remaining(&compose) == -10);

   CHECK(cobalt_compose_remaining(NULL) == COBALT_COMPOSE_GRAPHEMES);

   /*
    * A reply must carry its thread root. The feed and thread parsers record a
    * top-level post as its own root, so replying to one produces refs that
    * point at itself — which is correct, and is what stops a reply to a
    * top-level post being sent with an empty root.
    */
   cobalt_post parent;
   memset(&parent, 0, sizeof(parent));
   snprintf(parent.uri, sizeof(parent.uri), "at://did:plc:a/app.bsky.feed.post/1");
   snprintf(parent.cid, sizeof(parent.cid), "cid-one");
   snprintf(parent.root_uri, sizeof(parent.root_uri), "%s", parent.uri);
   snprintf(parent.root_cid, sizeof(parent.root_cid), "%s", parent.cid);
   snprintf(parent.handle, sizeof(parent.handle), "@someone.bsky.social");

   cobalt_compose_reply_to(&compose, &parent);
   CHECK(cobalt_compose_is_reply(&compose));
   CHECK_STR(compose.parent_uri, parent.uri);
   CHECK_STR(compose.root_uri, parent.uri);
   CHECK_STR(compose.reply_to, "@someone.bsky.social");
   /* Starting a reply clears any draft from a previous compose. */
   CHECK(compose.text[0] == '\0');

   /* Replying to something that is itself a reply keeps the real root, not the
    * parent — getting this wrong puts the reply in the wrong conversation for
    * every other client. */
   snprintf(parent.uri, sizeof(parent.uri), "at://did:plc:b/app.bsky.feed.post/2");
   snprintf(parent.cid, sizeof(parent.cid), "cid-two");
   snprintf(parent.root_uri, sizeof(parent.root_uri),
            "at://did:plc:a/app.bsky.feed.post/1");
   snprintf(parent.root_cid, sizeof(parent.root_cid), "cid-one");

   cobalt_compose_reply_to(&compose, &parent);
   CHECK_STR(compose.parent_uri, "at://did:plc:b/app.bsky.feed.post/2");
   CHECK_STR(compose.parent_cid, "cid-two");
   CHECK_STR(compose.root_uri, "at://did:plc:a/app.bsky.feed.post/1");
   CHECK_STR(compose.root_cid, "cid-one");
}

/* The three settings round-trip, a damaged file is refused whole, and both the
 * theme and the accessibility override actually reach the palette. */
static void
test_settings_store(void)
{
   begin("the settings file round-trips, and its values reach the palette");
   static cobalt_settings s, back;
   static char buf[COBALT_SETTINGS_FILE_MAX];
   const char *path = "settings-test.dat";

   cobalt_theme_set_high_contrast(false);
   cobalt_theme_apply(COBALT_THEME_LIGHT);
   const SDL_Color light_tile = COBALT_COLOUR_TILE;
   const SDL_Color light_bg = COBALT_COLOUR_BG_TOP;

   cobalt_settings_defaults(&s);
   CHECK(s.theme == COBALT_THEME_LIGHT);
   CHECK(s.text_scale == COBALT_TEXT_SCALE_NORMAL);
   CHECK(!s.high_contrast);
   CHECK(cobalt_settings_encode(&s, buf, sizeof(buf)));
   CHECK(cobalt_settings_decode(buf, strlen(buf), &back));
   CHECK(back.theme == COBALT_THEME_LIGHT);
   CHECK(back.text_scale == COBALT_TEXT_SCALE_NORMAL);
   CHECK(!back.high_contrast);

   s.theme = COBALT_THEME_DARK;
   s.text_scale = COBALT_TEXT_SCALE_LARGE;
   s.high_contrast = true;
   CHECK(cobalt_settings_encode(&s, buf, sizeof(buf)));
   CHECK(cobalt_settings_decode(buf, strlen(buf), &back));
   CHECK(back.theme == COBALT_THEME_DARK);
   CHECK(back.text_scale == COBALT_TEXT_SCALE_LARGE);
   CHECK(back.high_contrast);

   /* Every truncation is refused, and leaves all three at their defaults. */
   for (size_t cut = 0; cut < strlen(buf); cut++) {
      CHECK(!cobalt_settings_decode(buf, cut, &back));
      CHECK(back.theme == COBALT_THEME_LIGHT);
      CHECK(back.text_scale == COBALT_TEXT_SCALE_NORMAL);
      CHECK(!back.high_contrast);
   }
   {
      const char *wrong_version = "cobalt-settings 2\nend\n";
      const char *trailing = "cobalt-settings 1\nend\nextra\n";

      CHECK(!cobalt_settings_decode(wrong_version, strlen(wrong_version), &back));
      CHECK(!cobalt_settings_decode(trailing, strlen(trailing), &back));
   }

   /* Unknown keys are skipped, so a newer Cobalt's file still loads. A file
    * from before the two new rows existed keeps its theme and gains defaults
    * for the keys it does not contain. */
   {
      const char *newer = "cobalt-settings 1\nfuture=1\ntheme=dark\n"
                          "text_scale=130\nhigh_contrast=1\nend\n";
      const char *older = "cobalt-settings 1\ntheme=dark\nend\n";

      CHECK(cobalt_settings_decode(newer, strlen(newer), &back));
      CHECK(back.theme == COBALT_THEME_DARK);
      CHECK(back.text_scale == COBALT_TEXT_SCALE_LARGE);
      CHECK(back.high_contrast);

      CHECK(cobalt_settings_decode(older, strlen(older), &back));
      CHECK(back.theme == COBALT_THEME_DARK);
      CHECK(back.text_scale == COBALT_TEXT_SCALE_NORMAL);
      CHECK(!back.high_contrast);
   }

   /* A value outside the three known scales is ignored rather than rounded:
    * "101" is in range but is not a size the renderer knows how to apply. */
   {
      const char *mixed = "cobalt-settings 1\ntheme=light\ntext_scale=101\n"
                          "high_contrast=yes\nend\n";
      CHECK(cobalt_settings_decode(mixed, strlen(mixed), &back));
      CHECK(back.text_scale == COBALT_TEXT_SCALE_NORMAL);
      CHECK(!back.high_contrast);
   }

   CHECK(cobalt_settings_save(path, &s));
   CHECK(cobalt_settings_load(path, &back));
   CHECK(back.theme == COBALT_THEME_DARK);
   CHECK(back.text_scale == COBALT_TEXT_SCALE_LARGE);
   CHECK(back.high_contrast);
   remove(path);
   CHECK(cobalt_settings_load(path, &back));
   CHECK(back.theme == COBALT_THEME_LIGHT);
   CHECK(back.text_scale == COBALT_TEXT_SCALE_NORMAL);
   CHECK(!back.high_contrast);

   /* Encoding a struct with impossible values normalises it, so no caller can
    * write a file this build would refuse to read back. */
   {
      cobalt_settings bad;

      cobalt_settings_defaults(&bad);
      bad.theme = (cobalt_theme) 9;
      bad.text_scale = (cobalt_text_scale) 7;
      bad.high_contrast = true;
      CHECK(cobalt_settings_encode(&bad, buf, sizeof(buf)));
      CHECK(cobalt_settings_decode(buf, strlen(buf), &back));
      CHECK(back.theme == COBALT_THEME_LIGHT);
      CHECK(back.text_scale == COBALT_TEXT_SCALE_NORMAL);
      CHECK(back.high_contrast);
   }

   const unsigned generation = cobalt_theme_generation();
   cobalt_theme_apply(COBALT_THEME_DARK);
   CHECK(cobalt_theme_current() == COBALT_THEME_DARK);
   CHECK(cobalt_theme_generation() != generation);
   CHECK(COBALT_COLOUR_TILE.r != light_tile.r ||
         COBALT_COLOUR_TILE.g != light_tile.g ||
         COBALT_COLOUR_TILE.b != light_tile.b);
   CHECK(COBALT_COLOUR_CHIP.r != 0xEE || COBALT_COLOUR_CHIP.g != 0xF1 ||
         COBALT_COLOUR_CHIP.b != 0xF3);
   /* Header titles sit on the band, so they stay light even though the card
    * surface behind other text turned dark. */
   CHECK(COBALT_COLOUR_HEADER_TEXT.r > 0xE0 &&
         COBALT_COLOUR_HEADER_TEXT.g > 0xE0 &&
         COBALT_COLOUR_HEADER_TEXT.b > 0xE0);
   CHECK(COBALT_COLOUR_HEADER_TEXT.r != COBALT_COLOUR_TILE.r ||
         COBALT_COLOUR_HEADER_TEXT.g != COBALT_COLOUR_TILE.g ||
         COBALT_COLOUR_HEADER_TEXT.b != COBALT_COLOUR_TILE.b);

   /* High contrast takes precedence over the theme but is not itself a third
    * theme: switching it off returns to whichever theme is still selected. */
   const unsigned contrast_gen = cobalt_theme_generation();
   cobalt_theme_set_high_contrast(true);
   CHECK(cobalt_theme_high_contrast());
   CHECK(cobalt_theme_generation() != contrast_gen);
   CHECK(cobalt_theme_current() == COBALT_THEME_DARK);
   CHECK(COBALT_COLOUR_BG_TOP.r == 0x00 && COBALT_COLOUR_BG_TOP.g == 0x00 &&
         COBALT_COLOUR_BG_TOP.b == 0x00);
   CHECK(COBALT_COLOUR_HEADER_TEXT.r == 0xFF &&
         COBALT_COLOUR_HEADER_TEXT.g == 0xFF &&
         COBALT_COLOUR_HEADER_TEXT.b == 0xFF);
   cobalt_theme_apply(COBALT_THEME_LIGHT);
   CHECK(cobalt_theme_current() == COBALT_THEME_LIGHT);
   CHECK(COBALT_COLOUR_BG_TOP.r == 0x00 && COBALT_COLOUR_BG_TOP.g == 0x00 &&
         COBALT_COLOUR_BG_TOP.b == 0x00);
   cobalt_theme_set_high_contrast(false);
   CHECK(!cobalt_theme_high_contrast());
   CHECK(COBALT_COLOUR_BG_TOP.r == light_bg.r &&
         COBALT_COLOUR_BG_TOP.g == light_bg.g &&
         COBALT_COLOUR_BG_TOP.b == light_bg.b);
   CHECK(COBALT_COLOUR_TILE.r == light_tile.r &&
         COBALT_COLOUR_TILE.g == light_tile.g &&
         COBALT_COLOUR_TILE.b == light_tile.b);
}

/* WCAG 2.x relative luminance and contrast ratio. sRGB values are linearised
 * before weighting, which is what makes 0x80 grey read as roughly half the
 * light of white rather than 128/255 of it. */
static double
linear_channel(Uint8 v)
{
   const double c = (double) v / 255.0;
   return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double
relative_luminance(SDL_Color c)
{
   return 0.2126 * linear_channel(c.r) +
          0.7152 * linear_channel(c.g) +
          0.0722 * linear_channel(c.b);
}

static double
contrast_ratio(SDL_Color a, SDL_Color b)
{
   const double la = relative_luminance(a);
   const double lb = relative_luminance(b);
   const double hi = la > lb ? la : lb;
   const double lo = la > lb ? lb : la;
   return (hi + 0.05) / (lo + 0.05);
}

/* Accessibility is a property of the palette, not of a screenshot. Every role
 * that carries words is checked against the surface it is actually drawn on,
 * in all three palettes. Normal text gets 4.5:1 (WCAG AA), the large title
 * gets 3:1, and high contrast is held to AA for normal text everywhere. */
static void
test_palette_contrast(void)
{
   begin("every palette keeps text above the WCAG AA contrast floor");

   cobalt_theme_set_high_contrast(false);
   cobalt_theme_apply(COBALT_THEME_LIGHT);
   CHECK(contrast_ratio(COBALT_COLOUR_TEXT, COBALT_COLOUR_BG_TOP) >= 4.5);
   CHECK(contrast_ratio(COBALT_COLOUR_TEXT, COBALT_COLOUR_TILE) >= 4.5);
   CHECK(contrast_ratio(COBALT_COLOUR_TEXT_DIM, COBALT_COLOUR_TILE) >= 4.5);
   CHECK(contrast_ratio(COBALT_COLOUR_TEXT_DIM, COBALT_COLOUR_CHIP) >= 4.5);
   CHECK(contrast_ratio(COBALT_COLOUR_ACCENT_TEXT, COBALT_COLOUR_TILE) >= 4.5);
   CHECK(contrast_ratio(COBALT_COLOUR_ON_ACCENT, COBALT_COLOUR_ACCENT) >= 4.5);
   CHECK(contrast_ratio(COBALT_COLOUR_HEADER_TEXT, COBALT_COLOUR_BAND_TOP) >= 3.0);

   cobalt_theme_apply(COBALT_THEME_DARK);
   CHECK(contrast_ratio(COBALT_COLOUR_TEXT, COBALT_COLOUR_BG_TOP) >= 4.5);
   CHECK(contrast_ratio(COBALT_COLOUR_TEXT, COBALT_COLOUR_TILE) >= 4.5);
   CHECK(contrast_ratio(COBALT_COLOUR_TEXT_DIM, COBALT_COLOUR_TILE) >= 4.5);
   CHECK(contrast_ratio(COBALT_COLOUR_TEXT_DIM, COBALT_COLOUR_CHIP) >= 4.5);
   CHECK(contrast_ratio(COBALT_COLOUR_ACCENT_TEXT, COBALT_COLOUR_TILE) >= 4.5);
   CHECK(contrast_ratio(COBALT_COLOUR_ON_ACCENT, COBALT_COLOUR_ACCENT) >= 4.5);
   CHECK(contrast_ratio(COBALT_COLOUR_HEADER_TEXT, COBALT_COLOUR_BAND_TOP) >= 3.0);

   cobalt_theme_set_high_contrast(true);
   CHECK(contrast_ratio(COBALT_COLOUR_TEXT, COBALT_COLOUR_BG_TOP) >= 7.0);
   CHECK(contrast_ratio(COBALT_COLOUR_TEXT, COBALT_COLOUR_TILE) >= 7.0);
   CHECK(contrast_ratio(COBALT_COLOUR_TEXT_DIM, COBALT_COLOUR_TILE) >= 7.0);
   CHECK(contrast_ratio(COBALT_COLOUR_TEXT_DIM, COBALT_COLOUR_CHIP) >= 7.0);
   CHECK(contrast_ratio(COBALT_COLOUR_ACCENT_TEXT, COBALT_COLOUR_TILE) >= 7.0);
   CHECK(contrast_ratio(COBALT_COLOUR_ON_ACCENT, COBALT_COLOUR_ACCENT) >= 7.0);
   CHECK(contrast_ratio(COBALT_COLOUR_HEADER_TEXT, COBALT_COLOUR_BAND_TOP) >= 7.0);

   cobalt_theme_set_high_contrast(false);
   cobalt_theme_apply(COBALT_THEME_LIGHT);
}

static void
test_post_refuses_partial_refs(void)
{
   begin("a reply with incomplete refs is refused");

   cobalt_session_init();

   /* Empty text is nothing to send. */
   CHECK(!cobalt_session_begin_post("", NULL, NULL, NULL, NULL, 0, NULL, NULL));
   CHECK(!cobalt_session_begin_post(NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL));

   /*
    * A parent without a root, or a root without a cid, must be refused rather
    * than sent. A reply naming the wrong conversation is worse than one that
    * never got posted: it is visible, wrong, and not obviously Cobalt's fault.
    */
   CHECK(!cobalt_session_begin_post("hi", "at://parent", NULL, NULL, NULL, 0, NULL, NULL));
   CHECK(!cobalt_session_begin_post("hi", "at://parent", "cid", NULL, NULL, 0, NULL, NULL));
   CHECK(!cobalt_session_begin_post("hi", "at://parent", "cid", "at://root", NULL, 0, NULL, NULL));
   CHECK(!cobalt_session_begin_post("hi", "at://parent", "cid", "at://root", "", 0, NULL, NULL));

   /* A complete set is accepted (and fails later for want of an SDK, which is
    * not what is being checked here). */
   CHECK(cobalt_session_begin_post("hi", "at://parent", "cid", "at://root",
                                   "rcid", 0, NULL, NULL));

   cobalt_job_result result;
   for (int i = 0; i < 500 && !cobalt_session_poll(&result); i++) {
      SDL_Delay(10);
   }

   cobalt_session_shutdown();
}


/* --- direct messages --- */

static void
test_chat_start_target(void)
{
   begin("starting a conversation: target parsing and validation");

   char out[COBALT_CHAT_TARGET_MAX];

   /* Input that is nothing, or only spaces and an '@'. */
   CHECK(!cobalt_chat_target_normalise(NULL, out, sizeof(out)));
   CHECK(!cobalt_chat_target_normalise("", out, sizeof(out)));
   CHECK(!cobalt_chat_target_normalise("   \t ", out, sizeof(out)));
   CHECK(!cobalt_chat_target_normalise("@", out, sizeof(out)));
   CHECK(!cobalt_chat_target_normalise(" @ \n", out, sizeof(out)));
   CHECK(!cobalt_chat_target_normalise("x", NULL, sizeof(out)));
   CHECK(!cobalt_chat_target_normalise("x", out, 0));

   /* Trimming, and one leading '@' from a pasted handle. */
   CHECK(cobalt_chat_target_normalise("  @alice.bsky.social \n", out, sizeof(out)));
   CHECK_STR(out, "alice.bsky.social");
   CHECK(cobalt_chat_target_normalise("did:plc:abc123", out, sizeof(out)));
   CHECK_STR(out, "did:plc:abc123");

   /* Too long for the buffer, with its terminator counted: refused, not cut. */
   char long_handle[200];
   memset(long_handle, 'a', 120);
   memcpy(long_handle + 120, ".bsky.social", sizeof(".bsky.social"));
   CHECK(!cobalt_chat_target_normalise(long_handle, out, sizeof(out)));
   CHECK(!cobalt_chat_target_normalise(long_handle, out, COBALT_CHAT_TARGET_MAX));
   CHECK(cobalt_chat_target_normalise("a.bsky.social", out, COBALT_CHAT_TARGET_MAX));

   /* A DID: did:<lowercase method>:<id>. */
   CHECK(cobalt_chat_target_kind_of("did:plc:ewjcjxlzmo3rjtgvqxubkjgy") == COBALT_CHAT_TARGET_DID);
   CHECK(cobalt_chat_target_kind_of("did:web:example.com") == COBALT_CHAT_TARGET_DID);
   CHECK(cobalt_chat_target_kind_of("did:plc:") == COBALT_CHAT_TARGET_INVALID);
   CHECK(cobalt_chat_target_kind_of("did::abc") == COBALT_CHAT_TARGET_INVALID);
   CHECK(cobalt_chat_target_kind_of("did:PLC:abc") == COBALT_CHAT_TARGET_INVALID);
   CHECK(cobalt_chat_target_kind_of("did:plc:abc def") == COBALT_CHAT_TARGET_INVALID);
   CHECK(cobalt_chat_target_kind_of("did:plc:abc:") == COBALT_CHAT_TARGET_INVALID);
   CHECK(cobalt_chat_target_kind_of("did:plc:abc/x") == COBALT_CHAT_TARGET_INVALID);

   /* A handle: at least two labels, a letter-led TLD, hyphens inside only. */
   CHECK(cobalt_chat_target_kind_of("alice.bsky.social") == COBALT_CHAT_TARGET_HANDLE);
   CHECK(cobalt_chat_target_kind_of("Alice-1.example.co.uk") == COBALT_CHAT_TARGET_HANDLE);
   CHECK(cobalt_chat_target_kind_of("alice") == COBALT_CHAT_TARGET_INVALID);
   CHECK(cobalt_chat_target_kind_of(".example.com") == COBALT_CHAT_TARGET_INVALID);
   CHECK(cobalt_chat_target_kind_of("example.com.") == COBALT_CHAT_TARGET_INVALID);
   CHECK(cobalt_chat_target_kind_of("a..example.com") == COBALT_CHAT_TARGET_INVALID);
   CHECK(cobalt_chat_target_kind_of("-bad.example.com") == COBALT_CHAT_TARGET_INVALID);
   CHECK(cobalt_chat_target_kind_of("bad-.example.com") == COBALT_CHAT_TARGET_INVALID);
   CHECK(cobalt_chat_target_kind_of("under_score.example.com") == COBALT_CHAT_TARGET_INVALID);
   CHECK(cobalt_chat_target_kind_of("ex ample.com") == COBALT_CHAT_TARGET_INVALID);
   CHECK(cobalt_chat_target_kind_of("example.123") == COBALT_CHAT_TARGET_INVALID);
   CHECK(cobalt_chat_target_kind_of("https://example.com") == COBALT_CHAT_TARGET_INVALID);
   CHECK(cobalt_chat_target_kind_of("") == COBALT_CHAT_TARGET_INVALID);
   CHECK(cobalt_chat_target_kind_of(NULL) == COBALT_CHAT_TARGET_INVALID);

   /* A label of 63 bytes is fine; 64 is not. */
   char label63[80];
   char label64[80];
   memset(label63, 'a', 63);
   memcpy(label63 + 63, ".example.com", sizeof(".example.com"));
   memset(label64, 'a', 64);
   memcpy(label64 + 64, ".example.com", sizeof(".example.com"));
   CHECK(cobalt_chat_target_kind_of(label63) == COBALT_CHAT_TARGET_HANDLE);
   CHECK(cobalt_chat_target_kind_of(label64) == COBALT_CHAT_TARGET_INVALID);

   /* Over the target buffer is refused outright, whatever its shape. */
   char long_did[COBALT_CHAT_TARGET_MAX + 16];
   memcpy(long_did, "did:plc:", 8);
   memset(long_did + 8, 'a', COBALT_CHAT_TARGET_MAX + 7);
   long_did[COBALT_CHAT_TARGET_MAX + 15] = '\0';
   CHECK(cobalt_chat_target_kind_of(long_did) == COBALT_CHAT_TARGET_INVALID);

   /* Self is only self when both are set and equal. */
   CHECK(cobalt_chat_is_self("did:plc:me", "did:plc:me"));
   CHECK(!cobalt_chat_is_self("did:plc:me", "did:plc:you"));
   CHECK(!cobalt_chat_is_self("", ""));
   CHECK(!cobalt_chat_is_self(NULL, "did:plc:you"));

   /* A convo id from the server: printable, no spaces, fits the field. */
   char id[COBALT_CHAT_ID_MAX];
   CHECK(cobalt_chat_convo_id_copy(id, sizeof(id), "3lcvi2xygxn2t"));
   CHECK_STR(id, "3lcvi2xygxn2t");
   CHECK(!cobalt_chat_convo_id_copy(id, sizeof(id), NULL));
   CHECK(!cobalt_chat_convo_id_copy(id, sizeof(id), ""));
   CHECK(!cobalt_chat_convo_id_copy(id, sizeof(id), "has space"));
   CHECK(!cobalt_chat_convo_id_copy(id, sizeof(id), "tab\there"));
   CHECK(!cobalt_chat_convo_id_copy(id, sizeof(id), "new\nline"));
   CHECK(!cobalt_chat_convo_id_copy(id, sizeof(id), "caf\xc3\xa9"));
   CHECK(!cobalt_chat_convo_id_copy(id, 4, "3lcvi2"));
   CHECK(!cobalt_chat_convo_id_copy(NULL, sizeof(id), "3lcvi2"));
   char long_id[COBALT_CHAT_ID_MAX + 8];
   memset(long_id, 'x', COBALT_CHAT_ID_MAX);
   long_id[COBALT_CHAT_ID_MAX] = '\0';
   CHECK(!cobalt_chat_convo_id_copy(id, sizeof(id), long_id));

   /* The begin call refuses bad input without queueing a job. */
   cobalt_session_init();
   CHECK(!cobalt_session_begin_chat_start(NULL));
   CHECK(!cobalt_session_begin_chat_start(""));
   CHECK(!cobalt_session_begin_chat_start("   "));
   CHECK(!cobalt_session_begin_chat_start("@"));
   CHECK(!cobalt_session_begin_chat_start("not a handle"));
   CHECK(!cobalt_session_begin_chat_start(long_handle));
   /* One job at a time, so only the first valid start can be queued here. */
   CHECK(cobalt_session_begin_chat_start("@alice.bsky.social"));
   CHECK_STR(cobalt_session_chat_started_convo(), "");
}

static void
test_chat_inbox_counterpart(void)
{
   begin("direct message counterpart and inbox reset");

   const char *one_to_one[] = { "did:plc:them", "did:plc:me" };
   const char *reversed[] = { "did:plc:me", "did:plc:them" };
   const char *alone[] = { "did:plc:me" };
   const char *group[] = { "did:plc:me", "did:plc:a", "did:plc:b" };
   const char *blank[] = { "", "did:plc:them", "did:plc:me" };

   /* The counterpart is whoever is not the viewer, wherever they sit. */
   CHECK(cobalt_chat_pick_counterpart(one_to_one, 2, "did:plc:me") == 0);
   CHECK(cobalt_chat_pick_counterpart(reversed, 2, "did:plc:me") == 1);

   /* A convo with only the viewer has nobody to show. */
   CHECK(cobalt_chat_pick_counterpart(alone, 1, "did:plc:me") == -1);
   CHECK(cobalt_chat_pick_counterpart(NULL, 0, "did:plc:me") == -1);

   /* A member with no DID is skipped rather than taken as the counterpart. */
   CHECK(cobalt_chat_pick_counterpart(blank, 3, "did:plc:me") == 1);

   /* Without a viewer DID, the first member is the counterpart. */
   CHECK(cobalt_chat_pick_counterpart(one_to_one, 2, NULL) == 0);

   CHECK(cobalt_chat_other_member_count(one_to_one, 2, "did:plc:me") == 1);
   CHECK(cobalt_chat_other_member_count(group, 3, "did:plc:me") == 2);
   CHECK(cobalt_chat_other_member_count(alone, 1, "did:plc:me") == 0);

   cobalt_chat_inbox inbox;
   memset(&inbox, 0, sizeof(inbox));
   inbox.count = 4;
   inbox.unread = 9;
   inbox.has_more = true;
   snprintf(inbox.cursor, sizeof(inbox.cursor), "%s", "page-2");
   cobalt_chat_inbox_reset(&inbox);
   CHECK(inbox.count == 0);
   CHECK(inbox.unread == 0);
   CHECK(!inbox.has_more);
   CHECK(inbox.cursor[0] == '\0');
   cobalt_chat_inbox_reset(NULL);

   CHECK(!cobalt_chat_inbox_can_page(NULL));
   inbox.has_more = false;
   CHECK(!cobalt_chat_inbox_can_page(&inbox));
   inbox.has_more = true;
   CHECK(cobalt_chat_inbox_can_page(&inbox));
   inbox.count = COBALT_CHAT_CONVOS_MAX;
   CHECK(!cobalt_chat_inbox_can_page(&inbox));
}

static cobalt_chat_convo
test_row(const char *id, int unread)
{
   cobalt_chat_convo c;
   memset(&c, 0, sizeof(c));
   snprintf(c.id, sizeof(c.id), "%s", id);
   c.unread = unread;
   return c;
}

static void
test_chat_request_rows(void)
{
   begin("request rows: find, remove and add to the front");

   cobalt_chat_inbox box;
   memset(&box, 0, sizeof(box));
   box.items[0] = test_row("c1", 2);
   box.items[1] = test_row("c2", 0);
   box.items[2] = test_row("c3", 4);
   box.count = 3;
   box.unread = 6;

   CHECK(cobalt_chat_inbox_find(&box, "c2") == &box.items[1]);
   CHECK(cobalt_chat_inbox_find(&box, "nope") == NULL);
   CHECK(cobalt_chat_inbox_find(NULL, "c1") == NULL);
   CHECK(cobalt_chat_inbox_find(&box, NULL) == NULL);

   /* Removing a row keeps the order of the rest, and takes its unread with it. */
   CHECK(!cobalt_chat_inbox_remove(&box, "nope"));
   CHECK(!cobalt_chat_inbox_remove(NULL, "c1"));
   CHECK(box.count == 3);
   CHECK(cobalt_chat_inbox_remove(&box, "c3"));
   CHECK(box.count == 2);
   CHECK(box.unread == 2);
   CHECK_STR(box.items[0].id, "c1");
   CHECK_STR(box.items[1].id, "c2");
   CHECK(cobalt_chat_inbox_find(&box, "c3") == NULL);

   /* A row added goes first; the same id is never held twice. */
   cobalt_chat_convo fresh = test_row("c9", 1);
   CHECK(cobalt_chat_inbox_add_front(&box, &fresh));
   CHECK(box.count == 3);
   CHECK_STR(box.items[0].id, "c9");
   CHECK_STR(box.items[1].id, "c1");
   CHECK(box.unread == 3);
   CHECK(!cobalt_chat_inbox_add_front(&box, &fresh));
   CHECK(box.count == 3);
   CHECK(!cobalt_chat_inbox_add_front(NULL, &fresh));
   CHECK(!cobalt_chat_inbox_add_front(&box, NULL));

   /* Full: the last row goes, and its unread messages leave the total. */
   box.count = COBALT_CHAT_CONVOS_MAX;
   for (int i = 0; i < COBALT_CHAT_CONVOS_MAX; i++) {
      char id[COBALT_CHAT_ID_MAX];
      snprintf(id, sizeof(id), "f%d", i);
      box.items[i] = test_row(id, 1);
   }
   box.unread = COBALT_CHAT_CONVOS_MAX;
   cobalt_chat_convo newest = test_row("new", 0);
   CHECK(cobalt_chat_inbox_add_front(&box, &newest));
   CHECK(box.count == COBALT_CHAT_CONVOS_MAX);
   CHECK_STR(box.items[0].id, "new");
   CHECK(cobalt_chat_inbox_find(&box, "f48") != NULL);
   CHECK(cobalt_chat_inbox_find(&box, "f49") == NULL);
   CHECK(box.unread == COBALT_CHAT_CONVOS_MAX - 1);
}

static void
test_message_decision_states(void)
{
   begin("accepting or declining a request: one job at a time");

   cobalt_messages_view view;
   cobalt_messages_view_init(&view);
   CHECK(view.mode == COBALT_MESSAGES_MODE_INBOX);
   CHECK(!view.decision.pending);

   /* Finishing with nothing in flight is nothing. */
   CHECK(cobalt_messages_decision_finished(&view, true) == COBALT_MESSAGES_DECISION_NONE);
   CHECK(cobalt_messages_decision_finished(NULL, true) == COBALT_MESSAGES_DECISION_NONE);

   /* A refused begin marks nothing in flight, so its finish is nothing. */
   view.decision.accept = true;
   snprintf(view.decision.convo_id, sizeof(view.decision.convo_id), "%s", "c1");
   cobalt_messages_decision_began(&view, false);
   CHECK(!view.decision.pending);
   CHECK(cobalt_messages_decision_finished(&view, true) == COBALT_MESSAGES_DECISION_NONE);

   /* An accept that began and succeeded reports once. */
   cobalt_messages_decision_began(&view, true);
   CHECK(view.decision.pending);
   CHECK(cobalt_messages_decision_finished(&view, true) == COBALT_MESSAGES_DECISION_ACCEPTED);
   CHECK(!view.decision.pending);
   CHECK(cobalt_messages_decision_finished(&view, true) == COBALT_MESSAGES_DECISION_NONE);

   /* A decline reports declined. */
   view.decision.accept = false;
   cobalt_messages_decision_began(&view, true);
   CHECK(cobalt_messages_decision_finished(&view, true) == COBALT_MESSAGES_DECISION_DECLINED);

   /* A failure reports failed, clears the flag, and keeps the target so the same
    * request can be pressed again. */
   view.decision.accept = true;
   cobalt_messages_decision_began(&view, true);
   CHECK(cobalt_messages_decision_finished(&view, false) == COBALT_MESSAGES_DECISION_FAILED);
   CHECK(!view.decision.pending);
   CHECK_STR(view.decision.convo_id, "c1");
   cobalt_messages_decision_began(NULL, true);
}

static void
test_chat_requests_count_label(void)
{
   begin("request inbox count label");

   char out[32];

   CHECK(cobalt_chat_requests_count_label(0, out, sizeof(out)));
   CHECK_STR(out, "No requests");
   CHECK(cobalt_chat_requests_count_label(-3, out, sizeof(out)));
   CHECK_STR(out, "No requests");
   CHECK(cobalt_chat_requests_count_label(1, out, sizeof(out)));
   CHECK_STR(out, "1 request");
   CHECK(cobalt_chat_requests_count_label(2, out, sizeof(out)));
   CHECK_STR(out, "2 requests");
   CHECK(cobalt_chat_requests_count_label(50, out, sizeof(out)));
   CHECK_STR(out, "50 requests");

   /* A label that does not fit is refused, not cut short. "No requests" needs
    * eleven bytes with its terminator. */
   CHECK(!cobalt_chat_requests_count_label(0, out, 11));
   CHECK(cobalt_chat_requests_count_label(0, out, 12));
   CHECK_STR(out, "No requests");
   CHECK(!cobalt_chat_requests_count_label(0, out, 0));
   CHECK(!cobalt_chat_requests_count_label(0, NULL, sizeof(out)));
}

static cobalt_chat_join_request
test_join_row(const char *did)
{
   cobalt_chat_join_request r;
   memset(&r, 0, sizeof(r));
   snprintf(r.did, sizeof(r.did), "%s", did);
   return r;
}

static void
test_chat_join_request_rows(void)
{
   begin("group join requests: find, remove and paging");

   cobalt_chat_join_requests list;
   memset(&list, 0, sizeof(list));
   list.items[0] = test_join_row("did:plc:aa");
   list.items[1] = test_join_row("did:plc:bb");
   list.items[2] = test_join_row("did:plc:cc");
   list.count = 3;
   snprintf(list.convo_id, sizeof(list.convo_id), "%s", "convo-1");

   CHECK(cobalt_chat_join_requests_find(&list, "did:plc:bb") == &list.items[1]);
   CHECK(cobalt_chat_join_requests_find(&list, "did:plc:zz") == NULL);
   CHECK(cobalt_chat_join_requests_find(NULL, "did:plc:aa") == NULL);
   CHECK(cobalt_chat_join_requests_find(&list, NULL) == NULL);

   /* Removing a row keeps the order of the rest, and an unknown DID is not it. */
   CHECK(!cobalt_chat_join_requests_remove(&list, "did:plc:zz"));
   CHECK(!cobalt_chat_join_requests_remove(NULL, "did:plc:aa"));
   CHECK(!cobalt_chat_join_requests_remove(&list, NULL));
   CHECK(cobalt_chat_join_requests_remove(&list, "did:plc:aa"));
   CHECK(list.count == 2);
   CHECK_STR(list.items[0].did, "did:plc:bb");
   CHECK_STR(list.items[1].did, "did:plc:cc");
   CHECK(cobalt_chat_join_requests_find(&list, "did:plc:aa") == NULL);
   CHECK(!cobalt_chat_join_requests_remove(&list, "did:plc:aa"));

   /* Paging stops when the window fills, as the other lists do. */
   list.has_more = false;
   CHECK(!cobalt_chat_join_requests_can_page(&list));
   list.has_more = true;
   CHECK(cobalt_chat_join_requests_can_page(&list));
   list.count = COBALT_CHAT_JOIN_REQUESTS_MAX;
   CHECK(!cobalt_chat_join_requests_can_page(&list));
   CHECK(!cobalt_chat_join_requests_can_page(NULL));

   /* Reset clears everything, including the group they belonged to. */
   cobalt_chat_join_requests_reset(&list);
   CHECK(list.count == 0);
   CHECK(list.convo_id[0] == '\0');
   CHECK(!list.has_more);
   cobalt_chat_join_requests_reset(NULL);
}

static void
test_chat_thread_decision_states(void)
{
   begin("approving or rejecting a join request: one job at a time");

   cobalt_chat_thread_view view;
   cobalt_chat_thread_view_init(&view);
   CHECK(!view.join_decision.pending);

   /* Finishing with nothing in flight is nothing. */
   CHECK(cobalt_chat_thread_decision_finished(&view, true) == COBALT_CHAT_THREAD_DECISION_NONE);
   CHECK(cobalt_chat_thread_decision_finished(NULL, true) == COBALT_CHAT_THREAD_DECISION_NONE);

   /* A refused begin marks nothing in flight, so its finish is nothing. */
   view.join_decision.approve = true;
   snprintf(view.join_decision.did, sizeof(view.join_decision.did), "%s", "did:plc:aa");
   cobalt_chat_thread_decision_began(&view, false);
   CHECK(!view.join_decision.pending);
   CHECK(cobalt_chat_thread_decision_finished(&view, true) == COBALT_CHAT_THREAD_DECISION_NONE);

   /* An approve that began and succeeded reports once. */
   cobalt_chat_thread_decision_began(&view, true);
   CHECK(view.join_decision.pending);
   CHECK(cobalt_chat_thread_decision_finished(&view, true) == COBALT_CHAT_THREAD_DECISION_APPROVED);
   CHECK(!view.join_decision.pending);
   CHECK(cobalt_chat_thread_decision_finished(&view, true) == COBALT_CHAT_THREAD_DECISION_NONE);

   /* A reject reports rejected. */
   view.join_decision.approve = false;
   cobalt_chat_thread_decision_began(&view, true);
   CHECK(cobalt_chat_thread_decision_finished(&view, true) == COBALT_CHAT_THREAD_DECISION_REJECTED);

   /* A failure reports failed, clears the flag, and keeps the target so the same
    * request can be pressed again. */
   view.join_decision.approve = true;
   cobalt_chat_thread_decision_began(&view, true);
   CHECK(cobalt_chat_thread_decision_finished(&view, false) == COBALT_CHAT_THREAD_DECISION_FAILED);
   CHECK(!view.join_decision.pending);
   CHECK_STR(view.join_decision.did, "did:plc:aa");
   cobalt_chat_thread_decision_began(NULL, true);
}

/* A string one byte longer than a reply may be. */
static const char *
std_pad_1024(void)
{
   static char big[COBALT_CHAT_REPLY_MAX + 1];

   memset(big, 'a', sizeof(big) - 1);
   big[sizeof(big) - 1] = '\0';
   return big;
}

static void
test_chat_thread_state(void)
{
   begin("direct message thread state");

   cobalt_chat_thread thread;
   memset(&thread, 0, sizeof(thread));
   snprintf(thread.convo_id, sizeof(thread.convo_id), "%s", "convo-1");
   thread.count = 3;
   cobalt_chat_thread_reset(&thread);
   CHECK(thread.count == 0);
   CHECK(thread.convo_id[0] == '\0');
   cobalt_chat_thread_reset(NULL);

   /* Opening a conversation starts at the newest messages, with its name kept. */
   cobalt_chat_thread_view view;
   cobalt_chat_thread_view_init(&view);
   view.offset = 4;
   cobalt_chat_thread_view_open(&view, "Bob", "convo-bob", false);
   CHECK(view.offset == 0);
   CHECK(strcmp(view.title, "Bob") == 0);
   CHECK(strcmp(view.convo_id, "convo-bob") == 0);
   CHECK(!view.group);
   CHECK(view.mode == COBALT_CHAT_THREAD_MODE_MESSAGES);

   /* A reply must have something other than spaces in it, and fit the buffer. */
   CHECK(!cobalt_chat_reply_valid(NULL));
   CHECK(!cobalt_chat_reply_valid(""));
   CHECK(!cobalt_chat_reply_valid("   \t "));
   CHECK(cobalt_chat_reply_valid("  see you  "));
   CHECK(!cobalt_chat_reply_valid(std_pad_1024()));

   /* The draft survives reopening the same conversation, and a sent reply clears
    * it. A different conversation starts with no draft, so it cannot be sent by
    * mistake. */
   snprintf(view.reply, sizeof(view.reply), "%s", "half a thought");
   cobalt_chat_thread_view_open(&view, "Bob", "convo-bob", false);
   CHECK(strcmp(view.reply, "half a thought") == 0);
   cobalt_chat_thread_view_open(&view, "Cat", "convo-cat", false);
   CHECK(view.reply[0] == '\0');
   snprintf(view.reply, sizeof(view.reply), "%s", "thanks");
   cobalt_chat_thread_view_sent(&view);
   CHECK(view.reply[0] == '\0');

   /* A group remembers that it has join requests to open, and opening another
    * conversation comes back to its messages rather than a stale sub-mode. */
   cobalt_chat_thread_view_open(&view, "Group", "convo-group", true);
   CHECK(view.group);
   CHECK(view.mode == COBALT_CHAT_THREAD_MODE_MESSAGES);
   view.mode = COBALT_CHAT_THREAD_MODE_JOIN_REQUESTS;
   cobalt_chat_thread_view_open(&view, "Bob", "convo-bob", false);
   CHECK(!view.group);
   CHECK(view.mode == COBALT_CHAT_THREAD_MODE_MESSAGES);
}

/* --- starting a conversation from the inbox --- */

/* Keyboard OK is the function row's fifth key, Cancel the sixth. */
#define KB_FN_ROW  4
#define KB_OK_COL  4
#define KB_CANCEL_COL 5

static void
test_messages_start(void)
{
   begin("starting a conversation from the inbox");

   cobalt_messages_view view;
   cobalt_messages_view_init(&view);
   CHECK(!view.start.editing);
   CHECK(!view.start.pending);

   /* X opens the keyboard, and nothing is sent yet. */
   cobalt_input x = tap(COBALT_BTN_ALT_X);
   CHECK(cobalt_messages_view_update(&view, &x) == COBALT_MESSAGES_STAY);
   CHECK(view.start.editing);
   CHECK(view.start.target[0] == '\0');

   /* While the keyboard is open, B is its backspace, not back out of the screen. */
   snprintf(view.start.target, sizeof(view.start.target), "%s", "ewancroft.uk");
   cobalt_input b = tap(COBALT_BTN_BACK);
   CHECK(cobalt_messages_view_update(&view, &b) == COBALT_MESSAGES_STAY);
   CHECK(view.start.editing);
   CHECK_STR(view.start.target, "ewancroft.u");

   /* OK on nothing stays on the keyboard. */
   snprintf(view.start.target, sizeof(view.start.target), "%s", "");
   view.start.kb.row = KB_FN_ROW;
   view.start.kb.col = KB_OK_COL;
   cobalt_input ok = tap(COBALT_BTN_CONFIRM);
   CHECK(cobalt_messages_view_update(&view, &ok) == COBALT_MESSAGES_STAY);
   CHECK(view.start.editing);

   /* OK on a handle hands it to the caller to begin the job, and closes the keyboard. */
   snprintf(view.start.target, sizeof(view.start.target), "%s", "@ewancroft.uk");
   view.start.kb.row = KB_FN_ROW;
   view.start.kb.col = KB_OK_COL;
   CHECK(cobalt_messages_view_update(&view, &ok) == COBALT_MESSAGES_START);
   CHECK(!view.start.editing);
   CHECK(!view.start.pending);

   /* The job began: the name is shown without the '@', and X does nothing while in flight. */
   cobalt_messages_start_began(&view.start, true);
   CHECK(view.start.pending);
   char name[COBALT_CHAT_TARGET_MAX];
   CHECK(cobalt_messages_start_name(&view.start, name, sizeof(name)));
   CHECK_STR(name, "ewancroft.uk");
   CHECK(cobalt_messages_view_update(&view, &x) == COBALT_MESSAGES_STAY);
   CHECK(!view.start.editing);

   /* A finish with no id is a failure: the typed handle stays for another try. */
   CHECK(cobalt_messages_start_finished(&view.start, true, "") == COBALT_MESSAGES_START_FAILED);
   CHECK(!view.start.pending);
   CHECK_STR(view.start.target, "@ewancroft.uk");

   /* A failed job is a failure whatever id it names. */
   cobalt_messages_start_began(&view.start, true);
   CHECK(cobalt_messages_start_finished(&view.start, false, "convo-1") ==
         COBALT_MESSAGES_START_FAILED);
   CHECK_STR(view.start.target, "@ewancroft.uk");

   /* A finish with an id opens that conversation, and the field is clear for next time. */
   cobalt_messages_start_began(&view.start, true);
   CHECK(cobalt_messages_start_finished(&view.start, true, "convo-alice") ==
         COBALT_MESSAGES_START_OPEN);
   CHECK(!view.start.pending);
   CHECK(view.start.target[0] == '\0');

   /* A finish that was not ours is ignored, so a stray result cannot open anything. */
   CHECK(cobalt_messages_start_finished(&view.start, true, "convo-x") ==
         COBALT_MESSAGES_START_NONE);
   CHECK(cobalt_messages_start_finished(NULL, true, "convo-x") == COBALT_MESSAGES_START_NONE);

   /* A refused start (no job) opens the keyboard again on the same text. */
   cobalt_messages_start_open(&view.start);
   snprintf(view.start.target, sizeof(view.start.target), "%s", "did:plc:abc");
   view.start.kb.row = KB_FN_ROW;
   view.start.kb.col = KB_OK_COL;
   CHECK(cobalt_messages_view_update(&view, &ok) == COBALT_MESSAGES_START);
   cobalt_messages_start_began(&view.start, false);
   CHECK(view.start.editing);
   CHECK(!view.start.pending);
   CHECK_STR(view.start.target, "did:plc:abc");

   /* Cancel on the keyboard leaves it, with the text as it was. */
   view.start.kb.row = KB_FN_ROW;
   view.start.kb.col = KB_CANCEL_COL;
   cobalt_input cancel = tap(COBALT_BTN_CONFIRM);
   CHECK(cobalt_messages_view_update(&view, &cancel) == COBALT_MESSAGES_STAY);
   CHECK(!view.start.editing);
   CHECK_STR(view.start.target, "did:plc:abc");

   /* A name that is not a target at all is refused by the caller, not normalised into one. */
   CHECK(!cobalt_messages_start_name(NULL, name, sizeof(name)));
   CHECK(!cobalt_messages_start_name(&view.start, NULL, sizeof(name)));
   snprintf(view.start.target, sizeof(view.start.target), "%s", "   ");
   CHECK(!cobalt_messages_start_name(&view.start, name, sizeof(name)));
}

/* --- notifications --- */

static void
test_notification_wording(void)
{
   begin("notification wording and subject resolution");

   CHECK_STR(cobalt_notification_summary("like"), "liked your post");
   CHECK_STR(cobalt_notification_summary("repost"), "reposted your post");
   CHECK_STR(cobalt_notification_summary("follow"), "followed you");
   CHECK_STR(cobalt_notification_summary("reply"), "replied to you");
   CHECK_STR(cobalt_notification_summary("quote"), "quoted your post");
   CHECK_STR(cobalt_notification_summary("mention"), "mentioned you");

   /*
    * Bluesky adds reasons over time. An unrecognised one shows verbatim —
    * which reads like a lexicon but at least says what happened, rather than
    * a generic "did something" that says nothing.
    */
   CHECK_STR(cobalt_notification_summary("some-future-reason"),
             "some-future-reason");
   CHECK_STR(cobalt_notification_summary(NULL), "");

   /*
    * Which field holds the thing to open. A reply, mention or quote IS a post
    * and is its own subject; a like or repost points at what the viewer wrote.
    * Getting this backwards opens a plausible-looking wrong post.
    */
   CHECK(cobalt_notification_subject_is_self("reply"));
   CHECK(cobalt_notification_subject_is_self("mention"));
   CHECK(cobalt_notification_subject_is_self("quote"));
   CHECK(!cobalt_notification_subject_is_self("like"));
   CHECK(!cobalt_notification_subject_is_self("repost"));
   CHECK(!cobalt_notification_subject_is_self("follow"));
   CHECK(!cobalt_notification_subject_is_self(NULL));

   static cobalt_notifications list;
   memset(&list, 0, sizeof(list));
   list.count = 3;
   list.unread = 2;
   cobalt_notifications_reset(&list);
   CHECK(list.count == 0);
   CHECK(list.unread == 0);
}

/* --- regressions --- */

static void
test_paging_stops_when_the_window_fills(void)
{
   begin("paging stops when the window fills");

   /*
    * Regression. The window is fixed, so once it is full every further page
    * appends nothing while the server still hands back a cursor. Screens read
    * `has_more` to decide whether to auto-page on reaching the last row, so a
    * full window meant requesting the next page forever — spinning the worker,
    * holding `busy` true so no like or thread-open ever ran, and earning a
    * rate limit. The screen looked alive and was permanently unresponsive.
    */
   static cobalt_feed feed;
   memset(&feed, 0, sizeof(feed));

   /* Nothing fetched yet: no cursor, nothing to page towards. */
   CHECK(!cobalt_feed_can_page(&feed));

   /* A partly-filled window with a cursor is the normal paging case. */
   feed.count = 20;
   feed.has_more = true;
   CHECK(cobalt_feed_can_page(&feed));

   /* Full window, and the server still sent a cursor. This is the bug. */
   feed.count = COBALT_FEED_MAX_POSTS;
   CHECK(!cobalt_feed_can_page(&feed));

   /* One short of full still pages, so the cap is not off by one. */
   feed.count = COBALT_FEED_MAX_POSTS - 1;
   CHECK(cobalt_feed_can_page(&feed));

   /* No cursor means the end regardless of how full it is. */
   feed.has_more = false;
   CHECK(!cobalt_feed_can_page(&feed));

   CHECK(!cobalt_feed_can_page(NULL));

   /* Notifications carry the same rule and the same window problem. */
   static cobalt_notifications notes;
   memset(&notes, 0, sizeof(notes));
   notes.count = 5;
   notes.has_more = true;
   CHECK(cobalt_notifications_can_page(&notes));
   notes.count = COBALT_NOTIFICATIONS_MAX;
   CHECK(!cobalt_notifications_can_page(&notes));
   CHECK(!cobalt_notifications_can_page(NULL));
}

static void
test_selection_survives_a_shrinking_list(void)
{
   begin("selection survives a list shrinking under it");

   /*
    * Regression. Replying re-roots the thread on the parent, which is usually
    * far shorter than what was on screen. The cursor stayed where it was, and
    * the scroll maths only ever raises `scroll` to meet `selected` — so it
    * could not recover. The draw loop started past the end, the screen went
    * blank, and it took one press of UP per row to escape. The user's own
    * reply was invisible, which is the one thing that path exists to show.
    */
   int selected = 25;
   int scroll = 25;

   cobalt_list_clamp(&selected, &scroll, 4);
   CHECK(selected == 3);
   CHECK(scroll == 0);

   /* An in-range cursor is left alone — this must not fight normal scrolling. */
   selected = 7;
   scroll = 5;
   cobalt_list_clamp(&selected, &scroll, 20);
   CHECK(selected == 7);
   CHECK(scroll == 5);

   /* `scroll` is never allowed past `selected`, which would draw the cursor
    * off the top of the viewport. */
   selected = 2;
   scroll = 9;
   cobalt_list_clamp(&selected, &scroll, 20);
   CHECK(selected == 2);
   CHECK(scroll == 2);

   /* An empty list reports -1 rather than 0, so a caller that forgot its own
    * emptiness check indexes out of range loudly rather than reading row 0. */
   selected = 7;
   scroll = 3;
   cobalt_list_clamp(&selected, &scroll, 0);
   CHECK(selected == -1);
   CHECK(scroll == 0);

   /* Negative input is brought back rather than propagated. */
   selected = -5;
   scroll = -5;
   cobalt_list_clamp(&selected, &scroll, 10);
   CHECK(selected == 0);
   CHECK(scroll == 0);

   /* NULL is ignored rather than crashing. */
   cobalt_list_clamp(NULL, &scroll, 10);
   cobalt_list_clamp(&selected, NULL, 10);
}

/* --- deleting your own post --- */

static void
test_profile_tabs(void)
{
  begin("profile tab switch without a session");

  /* Names, filters and the cycle are Wolfram's and tested there. */
  CHECK(!cobalt_session_begin_profile_tab(WF_PROFILE_TAB_MEDIA));
}

static void
test_follow_lists(void)
{
   begin("followers and following need an actor");

   CHECK(!cobalt_session_begin_followers(NULL, false));
   CHECK(!cobalt_session_begin_followers("", true));
   CHECK(!cobalt_session_begin_following(NULL, false));
   CHECK(cobalt_session_followers_list()->count == 0);
   CHECK(cobalt_session_following_list()->count == 0);
   CHECK(cobalt_session_follow_list_actor()[0] == '\0');
}

static void
test_likes_lists(void)
{
   begin("likes and reposts need a post URI");

   CHECK(!cobalt_session_begin_likes(NULL, false));
   CHECK(!cobalt_session_begin_likes("", true));
   CHECK(!cobalt_session_begin_reposted_by(NULL, false));
   CHECK(cobalt_session_likes_list()->count == 0);

   /* The graph open mirrors the follows open: it rewinds the cursor and
    * records which post the list belongs to. */
   cobalt_graph_view view;
   memset(&view, 0, sizeof(view));
   cobalt_graph_view_open_likes(&view, COBALT_GRAPH_LIKES, "at://did:plc:x");
   CHECK(view.kind == COBALT_GRAPH_LIKES);
   CHECK_STR(view.actor, "at://did:plc:x");
   CHECK(view.nav.selected == 0 && view.nav.scroll == 0);

   /* A wrong kind is refused rather than half-opened. */
   cobalt_graph_view_open_likes(&view, COBALT_GRAPH_MUTED, "at://did:plc:x");
   CHECK(view.kind == COBALT_GRAPH_LIKES);

   /* A follows kind is not a likes kind and vice versa, but both open
    * profiles on A rather than undoing something. */
   CHECK(cobalt_graph_kind_is_follows(COBALT_GRAPH_LIKES));
   CHECK(cobalt_graph_kind_is_follows(COBALT_GRAPH_REPOSTED));
   CHECK(cobalt_graph_kind_is_follows(COBALT_GRAPH_FOLLOWERS));
   CHECK(!cobalt_graph_kind_is_follows(COBALT_GRAPH_MUTED));
}

static void
test_quote_helpers(void)
{
   begin("quote post flattening");

   cobalt_post post;
   memset(&post, 0, sizeof(post));
   snprintf(post.embed_note, sizeof(post.embed_note), "[quote]");

   cobalt_feed_set_quote(&post, "Ada", "ada.test", "hello\nworld");
   CHECK(post.quote.present);
   CHECK(strcmp(post.quote.author, "Ada") == 0);
   CHECK(strcmp(post.quote.handle, "@ada.test") == 0);
   CHECK(strcmp(post.quote.text, "hello\nworld") == 0);
   CHECK(post.embed_note[0] == '\0');

   /* No display name falls back to the handle. */
   cobalt_feed_set_quote(&post, "", "bob.test", NULL);
   CHECK(strcmp(post.quote.author, "bob.test") == 0);
   CHECK(post.quote.text[0] == '\0');

   /* No handle means nothing drawable; the note must survive. */
   memset(&post, 0, sizeof(post));
   snprintf(post.embed_note, sizeof(post.embed_note), "[quote]");
   cobalt_feed_set_quote(&post, "Ghost", NULL, "x");
   CHECK(!post.quote.present);
   CHECK(strcmp(post.embed_note, "[quote]") == 0);

   /* A "[quote + media]" note is not ours to clear. */
   snprintf(post.embed_note, sizeof(post.embed_note), "[quote + media]");
   cobalt_feed_set_quote(&post, "Ada", "ada.test", "hi");
   CHECK(strcmp(post.embed_note, "[quote + media]") == 0);
}

static void
test_pinned_prepend(void)
{
   begin("pinned post goes first and is not duplicated");

   static cobalt_feed feed;
   static cobalt_post pin;
   memset(&feed, 0, sizeof(feed));
   memset(&pin, 0, sizeof(pin));
   for (int i = 0; i < 3; i++) {
      snprintf(feed.posts[i].uri, sizeof(feed.posts[i].uri), "at://a/p/%d", i);
   }
   feed.count = 3;
   snprintf(pin.uri, sizeof(pin.uri), "at://a/p/2");
   snprintf(pin.reposted_by, sizeof(pin.reposted_by), "someone");

   CHECK(cobalt_feed_prepend_pinned(&feed, &pin));
   CHECK(feed.count == 3);
   CHECK(strcmp(feed.posts[0].uri, "at://a/p/2") == 0);
   CHECK(feed.posts[0].pinned);
   CHECK(feed.posts[0].reposted_by[0] == '\0');
   CHECK(strcmp(feed.posts[1].uri, "at://a/p/0") == 0);
   CHECK(!feed.posts[1].pinned);

   snprintf(pin.uri, sizeof(pin.uri), "at://a/p/new");
   CHECK(cobalt_feed_prepend_pinned(&feed, &pin));
   CHECK(feed.count == 4);
   CHECK(strcmp(feed.posts[0].uri, "at://a/p/new") == 0);

   /* A full window drops the last row rather than overflowing. */
   feed.count = COBALT_FEED_MAX_POSTS;
   snprintf(pin.uri, sizeof(pin.uri), "at://a/p/other");
   CHECK(cobalt_feed_prepend_pinned(&feed, &pin));
   CHECK(feed.count == COBALT_FEED_MAX_POSTS);

   pin.uri[0] = '\0';
   CHECK(!cobalt_feed_prepend_pinned(&feed, &pin));
}

static void
test_quote_compose(void)
{
   begin("quote compose");

   cobalt_post post;
   memset(&post, 0, sizeof(post));
   snprintf(post.uri, sizeof(post.uri), "at://did:plc:abc/app.bsky.feed.post/1");
   snprintf(post.cid, sizeof(post.cid), "bafycid");
   snprintf(post.handle, sizeof(post.handle), "@ewancroft.uk");

   static cobalt_compose c;
   cobalt_compose_quote(&c, &post);
   CHECK(cobalt_compose_is_quote(&c));
   CHECK(!cobalt_compose_is_reply(&c));
   CHECK(strcmp(c.quote_uri, post.uri) == 0);
   CHECK(strcmp(c.quote_cid, "bafycid") == 0);
   CHECK(strcmp(c.quote_handle, "@ewancroft.uk") == 0);

   /* A placeholder with no refs cannot be quoted. */
   post.cid[0] = '\0';
   cobalt_compose_quote(&c, &post);
   CHECK(!cobalt_compose_is_quote(&c));

   cobalt_compose_init(&c);
   CHECK(!cobalt_compose_is_quote(&c));

   CHECK(!cobalt_session_begin_quote("hi", NULL, "cid", 0, NULL, NULL));
   CHECK(!cobalt_session_begin_quote("hi", "at://x", NULL, 0, NULL, NULL));
   CHECK(!cobalt_session_begin_quote("", "at://x", "cid", 0, NULL, NULL));
   CHECK(!cobalt_session_begin_quote(NULL, "at://x", "cid", 0, NULL, NULL));

   /* The post language starts at none and cycles back round. */
   const char *first = cobalt_session_post_lang();
   int steps = 0;
   do {
      cobalt_session_cycle_post_lang();
      steps++;
   } while (strcmp(cobalt_session_post_lang(), first) != 0 && steps < 32);
   CHECK(steps > 1 && steps < 32);
}

static void
test_search_mode_toggle(void)
{
   begin("search people/posts toggle");

   cobalt_search_view v;
   cobalt_search_view_init(&v);
   cobalt_search_view_open(&v);
   CHECK(!v.posts);

   cobalt_input in = tap(COBALT_BTN_ALT_Y);
   CHECK(cobalt_search_view_update(&v, &in) == COBALT_SEARCH_VIEW_STAY);
   CHECK(v.posts);
   CHECK(cobalt_search_view_update(&v, &in) == COBALT_SEARCH_VIEW_STAY);
   CHECK(!v.posts);

   cobalt_search_view_open(&v);
   v.posts = true;
   cobalt_search_view_open(&v);
   CHECK(!v.posts);
}

static void
test_image_attach(const char *root)
{
   begin("image attach");

   char dir[512];
   snprintf(dir, sizeof(dir), "%s/images", root);
   mkdir(dir, 0755);
   const char *files[] = { "b.png", "a.jpg", ".hidden.png", "c.gif", "empty.png" };
   for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
      char fp[640];
      snprintf(fp, sizeof(fp), "%s/%s", dir, files[i]);
      FILE *f = fopen(fp, "wb");
      if (f) {
         if (strcmp(files[i], "empty.png") != 0) {
            fputs("data", f);
         }
         fclose(f);
      }
   }

   static cobalt_compose c;
   cobalt_compose_init(&c);
   snprintf(c.text, sizeof(c.text), "hello");
   c.confirming = true;
   c.confirm_choice = 3; /* image */

   /* Open the picker, move down, attach the second file. */
   cobalt_compose_open_picker(&c, dir, NULL);
   CHECK(c.picking && c.picker_count == 2);
   cobalt_input in = tap(COBALT_BTN_DOWN);
   CHECK(cobalt_compose_update(&c, &in) == COBALT_COMPOSE_STAY);
   CHECK(c.picker_sel == 1);
   in = tap(COBALT_BTN_CONFIRM);
   CHECK(cobalt_compose_update(&c, &in) == COBALT_COMPOSE_STAY);
   CHECK(!c.picking && c.alt_editing);
   /* Alt text is typed next; B there is backspace and keeps the mode open. */
   in = tap(COBALT_BTN_BACK);
   cobalt_compose_update(&c, &in);
   CHECK(c.alt_editing && c.attach_alt[0] == '\0');
   c.alt_editing = false;
   char want[640];
   snprintf(want, sizeof(want), "%s/b.png", dir);
   CHECK(strcmp(c.attach_path, want) == 0);

   /* Choosing the image button again removes it. */
   in = tap(COBALT_BTN_CONFIRM);
   CHECK(cobalt_compose_update(&c, &in) == COBALT_COMPOSE_STAY);
   CHECK(c.attach_path[0] == '\0');

   /* B backs out of the picker without attaching. */
   cobalt_compose_open_picker(&c, dir, NULL);
   in = tap(COBALT_BTN_BACK);
   CHECK(cobalt_compose_update(&c, &in) == COBALT_COMPOSE_STAY);
   CHECK(!c.picking && c.attach_path[0] == '\0' && c.confirming);

   /* Replies offer the image button too: RIGHT from Post lands on it. */
   cobalt_post post;
   memset(&post, 0, sizeof(post));
   snprintf(post.uri, sizeof(post.uri), "at://a/p/1");
   snprintf(post.cid, sizeof(post.cid), "cid");
   cobalt_compose_reply_to(&c, &post);
   snprintf(c.text, sizeof(c.text), "hi");
   c.confirming = true;
   c.confirm_choice = 0;
   in = tap(COBALT_BTN_RIGHT);
   cobalt_compose_update(&c, &in);
   CHECK(c.confirm_choice == 3);

   CHECK(!cobalt_session_begin_quote("hi", "at://x", NULL, 0, "x.png", "alt"));
}

/* The picker's row paths: the app folder and the camera folder are listed
 * together, and each row opens from the folder it came from. */
static void
test_picker_paths(const char *root)
{
   begin("picker paths");

   char out[640];
   CHECK(cobalt_picker_join(out, sizeof(out), "sd:/DCIM", "100WIIU/IMG.jpg"));
   CHECK(strcmp(out, "sd:/DCIM/100WIIU/IMG.jpg") == 0);
   char tiny[8];
   CHECK(!cobalt_picker_join(tiny, sizeof(tiny), "sd:/DCIM", "x.jpg"));
   CHECK(!cobalt_picker_join(out, sizeof(out), NULL, "x.jpg"));

   char app[512], cam[512], sub[512], f[640];
   snprintf(app, sizeof(app), "%s/pk", root);
   mkdir(app, 0755);
   snprintf(app, sizeof(app), "%s/pk/images", root);
   mkdir(app, 0755);
   snprintf(cam, sizeof(cam), "%s/pk/DCIM", root);
   mkdir(cam, 0755);
   snprintf(sub, sizeof(sub), "%s/100WIIU", cam);
   mkdir(sub, 0755);
   const char *files[][2] = {
      { app, "a.jpg" }, { sub, "IMG.jpg" }, { cam, "top.png" },
   };
   for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
      snprintf(f, sizeof(f), "%s/%s", files[i][0], files[i][1]);
      FILE *fp = fopen(f, "wb");
      if (fp) {
         fputs("data", fp);
         fclose(fp);
      }
   }

   static cobalt_compose c;
   cobalt_compose_init(&c);
   cobalt_compose_open_picker(&c, app, cam);
   CHECK(c.picker_count == 3);
   /* App rows first, then the camera rows, each sorted by strcmp. */
   CHECK(strcmp(cobalt_picker_row_source(&c, 0), "Images") == 0);
   CHECK(strcmp(cobalt_picker_row_source(&c, 1), "Camera") == 0);
   CHECK(strcmp(c.picker_names[1], "100WIIU/IMG.jpg") == 0);
   CHECK(strcmp(c.picker_names[2], "top.png") == 0);
   CHECK(cobalt_picker_row_source(&c, 3) == NULL);
   CHECK(strcmp(cobalt_picker_row_dir(&c, 1), cam) == 0);
   CHECK(strcmp(cobalt_picker_row_dir(&c, 0), app) == 0);

   char want[640];
   snprintf(want, sizeof(want), "%s/100WIIU/IMG.jpg", cam);
   CHECK(cobalt_picker_join(out, sizeof(out), cobalt_picker_row_dir(&c, 1),
                            c.picker_names[1]));
   CHECK(strcmp(out, want) == 0);

   /* With no camera folder the picker lists only the app's images. */
   cobalt_compose_open_picker(&c, app, NULL);
   CHECK(c.picker_count == 1 && c.picker_src[0] == COBALT_PICKER_SRC_APP);
   CHECK(c.picker_dirs[COBALT_PICKER_SRC_CAMERA][0] == '\0');
}

static void
test_delete_post_helpers(void)
{
   begin("own-post detection and local removal");

   CHECK(cobalt_post_uri_is_by("at://did:plc:abc/app.bsky.feed.post/1",
                               "did:plc:abc"));
   /* A DID that merely starts the same way is someone else. */
   CHECK(!cobalt_post_uri_is_by("at://did:plc:abcd/app.bsky.feed.post/1",
                                "did:plc:abc"));
   CHECK(!cobalt_post_uri_is_by("at://did:plc:abc/app.bsky.feed.post/1", ""));
   CHECK(!cobalt_post_uri_is_by("at://did:plc:abc/app.bsky.feed.post/1", NULL));
   CHECK(!cobalt_post_uri_is_by(NULL, "did:plc:abc"));
   CHECK(!cobalt_post_uri_is_by("https://did:plc:abc/x", "did:plc:abc"));

   static cobalt_feed feed;
   memset(&feed, 0, sizeof(feed));
   snprintf(feed.posts[0].uri, sizeof(feed.posts[0].uri), "at://a/p/1");
   snprintf(feed.posts[1].uri, sizeof(feed.posts[1].uri), "at://a/p/2");
   snprintf(feed.posts[2].uri, sizeof(feed.posts[2].uri), "at://a/p/1");
   snprintf(feed.posts[3].uri, sizeof(feed.posts[3].uri), "at://a/p/3");
   feed.count = 4;

   /* The same post as an original and as a repost leaves both places. */
   CHECK(cobalt_feed_remove_post(&feed, "at://a/p/1") == 2);
   CHECK(feed.count == 2);
   CHECK(strcmp(feed.posts[0].uri, "at://a/p/2") == 0);
   CHECK(strcmp(feed.posts[1].uri, "at://a/p/3") == 0);
   CHECK(cobalt_feed_remove_post(&feed, "at://a/p/9") == 0);
   CHECK(cobalt_feed_remove_post(&feed, "") == 0);
   CHECK(cobalt_feed_remove_post(NULL, "at://a/p/2") == 0);

   /* Not signed in, or someone else's post: nothing is submitted. */
   CHECK(!cobalt_session_begin_delete_post(NULL));
   CHECK(!cobalt_session_begin_delete_post("at://did:plc:other/p/1"));
}

/* --- the async request handshake --- */

/* A loopback listener that accepts connections but never answers, so a login
 * against it stays in flight until the listener is closed. That makes the
 * "refused while busy" check deterministic, instead of racing a host job that
 * can finish before the second request is made. Returns -1 on failure. */
static int
hold_listener(int *port_out)
{
   struct sockaddr_in addr;
   socklen_t len = sizeof(addr);
   int fd = socket(AF_INET, SOCK_STREAM, 0);

   if (fd < 0) {
      return -1;
   }
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   addr.sin_port = 0;
   if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 || listen(fd, 4) != 0 ||
       getsockname(fd, (struct sockaddr *) &addr, &len) != 0) {
      close(fd);
      return -1;
   }
   *port_out = ntohs(addr.sin_port);
   return fd;
}

/*
 * Exercises the worker thread end to end. Without Wolfram every job fails with
 * a fixed message, which is uninteresting in itself — the point is that a
 * request is accepted, runs off the calling thread, and comes back through
 * cobalt_session_poll exactly once. That handshake is the part that would
 * otherwise only ever be exercised on the console.
 */
static void
test_session_request_handshake(void)
{
   begin("async request handshake");

   cobalt_session_init();

   /* If this ever fails on a host the fallback path is being tested instead of
    * the threaded one, and the rest of this test means much less. */
   CHECK(cobalt_session_threaded());

   CHECK(cobalt_session_state() == COBALT_AUTH_SIGNED_OUT);
   CHECK(!cobalt_session_busy());

   cobalt_job_result result;
   CHECK(!cobalt_session_poll(&result));

   /* A request with nothing to send is refused before it reaches the worker. */
   CHECK(!cobalt_session_begin_login("bsky.social", "", "app-pass"));
   CHECK(!cobalt_session_begin_login("bsky.social", "someone.test", ""));

   /* A second request while one is in flight must be refused rather than
    * queued behind it or, worse, racing it onto the same wf_session. The first
    * login goes to a listener that never answers, so it is still in flight when
    * the second is made. */
   int port = 0;
   int listener = hold_listener(&port);
   CHECK(listener >= 0);
   char held[64];
   snprintf(held, sizeof held, "http://127.0.0.1:%d", port);
   CHECK(cobalt_session_begin_login(held, "someone.test", "app-pass"));
   CHECK(cobalt_session_busy());
   CHECK(!cobalt_session_begin_login(held, "other.test", "pass"));
   /* Closing the listener resets the held connection, so the job fails fast. */
   if (listener >= 0) {
      close(listener);
   }

   bool completed = false;
   for (int i = 0; i < 500 && !completed; i++) {
      completed = cobalt_session_poll(&result);
      if (!completed) {
         SDL_Delay(10);
      }
   }

   CHECK(completed);
   if (completed) {
      CHECK(result.kind == COBALT_JOB_LOGIN);
      /* No Wolfram in a host build, so this is the expected outcome; what is
       * being checked is that a reason came back at all. */
      CHECK(!result.ok);
      CHECK(result.message[0] != '\0');
   }

   /* The result is delivered once and only once. */
   CHECK(!cobalt_session_poll(&result));

   /* A failed sign-in must leave the user signed out, not in limbo. */
   CHECK(cobalt_session_state() == COBALT_AUTH_SIGNED_OUT);
   CHECK(!cobalt_session_busy());

   cobalt_session_shutdown();
}

/* --- sign-in screen validation --- */

static void
test_signin_validation(void)
{
   begin("sign-in refuses to submit an incomplete form");

   cobalt_session_init();

   cobalt_signin form;
   cobalt_signin_init(&form);

   /* Focus starts on the identifier rather than the prefilled server field. */
   CHECK(form.focus == 1);
   CHECK(form.editing == -1);

   /* Submitting with nothing filled in must not fire a request; it should say
    * what is missing and move the focus there. */
   form.focus = COBALT_SIGNIN_ROWS - 1;
   cobalt_input confirm = tap(COBALT_BTN_CONFIRM);
   CHECK(cobalt_signin_update(&form, &confirm) == COBALT_SIGNIN_STAY);
   CHECK(form.status[0] != '\0');
   CHECK(form.status_is_error);
   CHECK(form.focus == 1);

   /* Identifier but no password: this now selects browser OAuth. */
   snprintf(form.identifier, sizeof(form.identifier), "someone.bsky.social");
   form.focus = COBALT_SIGNIN_ROWS - 1;
   CHECK(cobalt_signin_update(&form, &confirm) == COBALT_SIGNIN_SUBMIT);

   /* Supplying an app password still selects the legacy direct-login path. */
   snprintf(form.password, sizeof(form.password), "abcd-efgh-ijkl-mnop");
   form.focus = COBALT_SIGNIN_ROWS - 1;
   CHECK(cobalt_signin_update(&form, &confirm) == COBALT_SIGNIN_SUBMIT);

   /* Signing out of the screen wipes the password rather than leaving it in
    * the app's allocation. */
   cobalt_signin_clear_password(&form);
   CHECK(form.password[0] == '\0');

   cobalt_session_shutdown();
}

static void
test_signin_show_password_toggle(void)
{
   begin("sign-in shows the app password only when the toggle is on");

   cobalt_session_init();

   cobalt_signin form;
   cobalt_signin_init(&form);

   /* Off by default: the password stays masked on the form. */
   CHECK(!form.show_password);

   /* The toggle is the row above Sign in, and pressing it flips the flag. */
   form.focus = COBALT_SIGNIN_ROWS - 2;
   cobalt_input press = tap(COBALT_BTN_CONFIRM);
   CHECK(cobalt_signin_update(&form, &press) == COBALT_SIGNIN_STAY);
   CHECK(form.show_password);
   CHECK(form.editing == -1);

   press = tap(COBALT_BTN_CONFIRM);
   CHECK(cobalt_signin_update(&form, &press) == COBALT_SIGNIN_STAY);
   CHECK(!form.show_password);

   /* Leaving the screen hides it again, so the next visit starts masked. */
   press = tap(COBALT_BTN_CONFIRM);
   cobalt_signin_update(&form, &press);
   CHECK(form.show_password);
   cobalt_signin_clear_password(&form);
   CHECK(!form.show_password);

   cobalt_session_shutdown();
}

/* --- image scaling --- */

static void
test_image_fit_geometry(void)
{
   begin("image scaling crops and sizes correctly");

   SDL_Rect src;
   int w = 0, h = 0;

   /* Landscape, contained: the longest edge becomes the limit and the aspect
    * ratio is preserved. 1000x500 to 64 is 64x32. */
   cobalt_image_fit_rects(COBALT_IMAGE_FIT_CONTAIN, 64, 1000, 500, &src, &w, &h);
   CHECK(src.x == 0 && src.y == 0 && src.w == 1000 && src.h == 500);
   CHECK(w == 64 && h == 32);

   /* Portrait: the limit applies to the height instead. */
   cobalt_image_fit_rects(COBALT_IMAGE_FIT_CONTAIN, 64, 500, 1000, &src, &w, &h);
   CHECK(w == 32 && h == 64);

   /* Rounding, not truncation. 3:2 at 100 is 100x66.67; truncating gives 66,
    * which is a whole pixel of distortion at this size. */
   cobalt_image_fit_rects(COBALT_IMAGE_FIT_CONTAIN, 100, 300, 200, &src, &w, &h);
   CHECK(w == 100 && h == 67);

   /* Never upscale: a 16x16 icon asked for at 64 stays 16x16. Stretching it
    * would cost four times the texture memory for a blurrier result. */
   cobalt_image_fit_rects(COBALT_IMAGE_FIT_CONTAIN, 64, 16, 16, &src, &w, &h);
   CHECK(w == 16 && h == 16);

   /* Circle: centre-crop to a square first. A 200x100 banner used as an avatar
    * should take the middle 100x100, not the left edge. */
   cobalt_image_fit_rects(COBALT_IMAGE_FIT_CIRCLE, 64, 200, 100, &src, &w, &h);
   CHECK(src.x == 50 && src.y == 0 && src.w == 100 && src.h == 100);
   CHECK(w == 64 && h == 64);

   /* Circle, already square and already small: untouched. */
   cobalt_image_fit_rects(COBALT_IMAGE_FIT_CIRCLE, 64, 48, 48, &src, &w, &h);
   CHECK(src.x == 0 && src.y == 0 && src.w == 48 && src.h == 48);
   CHECK(w == 48 && h == 48);

   /* Degenerate input reports nothing rather than a negative rect. */
   cobalt_image_fit_rects(COBALT_IMAGE_FIT_CONTAIN, 64, 0, 0, &src, &w, &h);
   CHECK(w == 0 && h == 0);
}

/* Fill an ARGB8888 surface via a callback, so the tests below can describe a
 * pattern rather than repeat the pitch arithmetic. */
static SDL_Surface *
make_surface(int w, int h, uint32_t (*pixel)(int x, int y))
{
   SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
                                                   SDL_PIXELFORMAT_ARGB8888);
   if (!s) {
      return NULL;
   }
   for (int y = 0; y < h; y++) {
      uint32_t *row = (uint32_t *) ((uint8_t *) s->pixels + (size_t) y * (size_t) s->pitch);
      for (int x = 0; x < w; x++) {
         row[x] = pixel(x, y);
      }
   }
   return s;
}

static uint32_t
quadrant_pattern(int x, int y)
{
   /* A 4x4 split into four 2x2 blocks of one colour each, so a halving scale
    * has an answer that can be written down: each output pixel is exactly one
    * block's colour. */
   const uint32_t left = (x < 2) ? 0x00u : 0xFFu;
   const uint32_t top = (y < 2) ? 0x00u : 0xFFu;
   return 0xFF000000u | (left << 16) | (top << 8);
}

static uint32_t
opaque_white(int x, int y)
{
   (void) x;
   (void) y;
   return 0xFFFFFFFFu;
}

static uint32_t
pixel_at(SDL_Surface *s, int x, int y)
{
   return ((const uint32_t *) ((const uint8_t *) s->pixels +
                               (size_t) y * (size_t) s->pitch))[x];
}

static void
test_image_resample(void)
{
   begin("image resampling averages rather than samples");

   SDL_Surface *src = make_surface(4, 4, quadrant_pattern);
   CHECK(src != NULL);
   if (!src) {
      return;
   }

   /* 4x4 down to 2x2: each output pixel covers exactly one uniform block, so
    * the averaging must reproduce the colours exactly. Nearest-neighbour would
    * also pass this one — the point is that averaging does not *lose* to it. */
   SDL_Surface *half = cobalt_image_resample(src, 2, COBALT_IMAGE_FIT_CONTAIN);
   CHECK(half != NULL);
   if (half) {
      CHECK(half->w == 2 && half->h == 2);
      CHECK(pixel_at(half, 0, 0) == 0xFF000000u);
      CHECK(pixel_at(half, 1, 0) == 0xFFFF0000u);
      CHECK(pixel_at(half, 0, 1) == 0xFF00FF00u);
      CHECK(pixel_at(half, 1, 1) == 0xFFFFFF00u);
      SDL_FreeSurface(half);
   }

   /*
    * 4x4 down to 1x1: the whole image in one pixel. Nearest-neighbour returns
    * whichever corner it lands on — here 0xFF000000, black. Averaging returns
    * the mean, half red and half green. This is the check that would catch a
    * silent fall back to SDL_BlitScaled.
    */
   SDL_Surface *one = cobalt_image_resample(src, 1, COBALT_IMAGE_FIT_CONTAIN);
   CHECK(one != NULL);
   if (one) {
      CHECK(one->w == 1 && one->h == 1);
      const uint32_t p = pixel_at(one, 0, 0);
      CHECK(((p >> 24) & 0xFFu) == 0xFFu);
      CHECK(((p >> 16) & 0xFFu) == 0x80u); /* 2 of 4 columns red */
      CHECK(((p >> 8) & 0xFFu) == 0x80u);  /* 2 of 4 rows green */
      SDL_FreeSurface(one);
   }

   SDL_FreeSurface(src);

   /* A surface that is not ARGB8888 is refused rather than read as if it were:
    * the scaler indexes 32-bit words directly. */
   SDL_Surface *rgb24 =
      SDL_CreateRGBSurfaceWithFormat(0, 8, 8, 24, SDL_PIXELFORMAT_RGB24);
   if (rgb24) {
      CHECK(cobalt_image_resample(rgb24, 4, COBALT_IMAGE_FIT_CONTAIN) == NULL);
      SDL_FreeSurface(rgb24);
   }
}

static void
test_image_circle_mask(void)
{
   begin("circular fit clears the corners and keeps the middle");

   SDL_Surface *src = make_surface(64, 64, opaque_white);
   CHECK(src != NULL);
   if (!src) {
      return;
   }

   SDL_Surface *out = cobalt_image_resample(src, 32, COBALT_IMAGE_FIT_CIRCLE);
   SDL_FreeSurface(src);
   CHECK(out != NULL);
   if (!out) {
      return;
   }

   CHECK(out->w == 32 && out->h == 32);

   /* Corners fall outside the inscribed circle and must be fully transparent,
    * or the avatar draws as a square with rounded shading. */
   CHECK(((pixel_at(out, 0, 0) >> 24) & 0xFFu) == 0);
   CHECK(((pixel_at(out, 31, 0) >> 24) & 0xFFu) == 0);
   CHECK(((pixel_at(out, 0, 31) >> 24) & 0xFFu) == 0);
   CHECK(((pixel_at(out, 31, 31) >> 24) & 0xFFu) == 0);

   /* The centre is untouched, and the colour survives the premultiply round
    * trip rather than coming back a shade off. */
   CHECK(pixel_at(out, 16, 16) == 0xFFFFFFFFu);

   /* The rim is feathered, not binary. The inscribed circle touches the box at
    * the midpoint of each edge, so the outermost pixel of the widest row is
    * the one the boundary cuts through: it must come back partly transparent
    * rather than snapped to on or off. */
   const uint32_t right = (pixel_at(out, 31, 16) >> 24) & 0xFFu;
   const uint32_t left = (pixel_at(out, 0, 16) >> 24) & 0xFFu;
   CHECK(right > 0 && right < 0xFFu);
   CHECK(left == right); /* and symmetric, or the centre is off by a half-pixel */

   SDL_FreeSurface(out);
}

static void
test_postcard_contain_fit(void)
{
   begin("embed contain-fit sizes without cropping");

   int w = 0, h = 0;

   /* A wide box, a taller-than-wide source: the height axis is tighter, so
    * the image comes back pillarboxed rather than spilling past the box's
    * height into the next line. */
   cobalt_postcard_contain_fit(400, 100, 200, 400, &w, &h);
   CHECK(h == 100);
   CHECK(w == 50); /* 400 * (200/400) */

   /* A tall box, a wider-than-tall source: the width axis is tighter. */
   cobalt_postcard_contain_fit(100, 400, 400, 200, &w, &h);
   CHECK(w == 100);
   CHECK(h == 50);

   /* A source that already matches the box's aspect ratio fills it exactly
    * on both axes, not just the tighter one. */
   cobalt_postcard_contain_fit(200, 100, 400, 200, &w, &h);
   CHECK(w == 200);
   CHECK(h == 100);

   /* An unknown source size (still loading, no aspect data) fills the box —
    * the caller draws the flat placeholder frame at full size until then. */
   cobalt_postcard_contain_fit(200, 100, 0, 0, &w, &h);
   CHECK(w == 200 && h == 100);

   /* A degenerate box produces no rectangle to draw, not a division fault. */
   cobalt_postcard_contain_fit(0, 100, 200, 200, &w, &h);
   CHECK(w == 0 && h == 0);
}

/* --- image viewer --- */

static void
test_imageview(void)
{
   begin("image viewer opens, cycles and closes");

   cobalt_imageview view;
   memset(&view, 0, sizeof(view));

   /* A post with no pictures is a no-op rather than an empty viewer. */
   cobalt_post bare;
   memset(&bare, 0, sizeof(bare));
   cobalt_imageview_open(&view, &bare);
   CHECK(!view.open);

   cobalt_post post;
   memset(&post, 0, sizeof(post));
   post.image_count = 3;
   for (int i = 0; i < 3; i++) {
      snprintf(post.images[i].thumb, sizeof(post.images[i].thumb),
               "https://cdn.example/%d.jpg", i);
      post.images[i].aspect_w = 4;
      post.images[i].aspect_h = 3;
   }
   snprintf(post.images[1].alt, sizeof(post.images[1].alt), "A description.");

   cobalt_imageview_open(&view, &post);
   CHECK(view.open);
   CHECK(view.image_count == 3);
   CHECK(view.index == 0);

   /* The viewer holds copies: rewriting the post afterwards cannot change
    * what the person is looking at. */
   snprintf(post.images[0].thumb, sizeof(post.images[0].thumb), "changed");
   CHECK_STR(view.images[0].thumb, "https://cdn.example/0.jpg");

   /* Empty input does nothing but still consumes the frame: the screen
    * underneath must not act on presses the viewer already saw. */
   cobalt_input in;
   memset(&in, 0, sizeof(in));
   CHECK(cobalt_imageview_update(&view, &in));
   CHECK(view.open);

   /* Left and right cycle through the pictures, wrapping at both ends. */
   view = (cobalt_imageview){0};
   cobalt_imageview_open(&view, &post);
   in = tap(COBALT_BTN_RIGHT);
   CHECK(cobalt_imageview_update(&view, &in));
   CHECK(view.index == 1);
   in = tap(COBALT_BTN_RIGHT);
   cobalt_imageview_update(&view, &in);
   CHECK(view.index == 2);
   in = tap(COBALT_BTN_RIGHT);
   cobalt_imageview_update(&view, &in);
   CHECK(view.index == 0); /* wrapped */
   in = tap(COBALT_BTN_LEFT);
   cobalt_imageview_update(&view, &in);
   CHECK(view.index == 2); /* wrapped back */

   /* B closes, and the frame is consumed either way. */
   in = tap(COBALT_BTN_BACK);
   CHECK(cobalt_imageview_update(&view, &in));
   CHECK(!view.open);
   CHECK(view.image_count == 0);

   /* A single-image post does not cycle on Left/Right — there is nothing
    * to cycle to, and the hints already say so. */
   cobalt_post one;
   memset(&one, 0, sizeof(one));
   one.image_count = 1;
   snprintf(one.images[0].thumb, sizeof(one.images[0].thumb), "https://x/y.jpg");
   cobalt_imageview_open(&view, &one);
   in = tap(COBALT_BTN_RIGHT);
   cobalt_imageview_update(&view, &in);
   CHECK(view.index == 0);
   cobalt_imageview_close(&view);
   CHECK(!view.open);
}

/* --- sticks and touch --- */

static void
axis_event(cobalt_input *in, SDL_GameControllerAxis axis, int value)
{
   SDL_Event e;
   memset(&e, 0, sizeof(e));
   e.type = SDL_CONTROLLERAXISMOTION;
   e.caxis.axis = (Uint8) axis;
   e.caxis.value = (Sint16) value;
   cobalt_input_handle_event(in, &e);
}

static void
finger_event(cobalt_input *in, Uint32 type, float x, float y)
{
   SDL_Event e;
   memset(&e, 0, sizeof(e));
   e.type = type;
   e.tfinger.x = x;
   e.tfinger.y = y;
   cobalt_input_handle_event(in, &e);
}

static void
test_touch_drag_scrolls_a_list(void)
{
   begin("a touch drag scrolls a list by rows; a tap is still a tap");
   cobalt_input in;
   SDL_Rect hit[3] = { {0, 0, 100, 100}, {0, 100, 100, 100}, {0, 200, 100, 100} };
   int selected = 0;
   const float px = 1.0f / (float) COBALT_DRC_HEIGHT;

   cobalt_input_init(&in);

   /* A tap: no drag, a release inside the rect is a tap, the list does not move. */
   finger_event(&in, SDL_FINGERDOWN, 0.1f, 50.0f * px);
   cobalt_input_drag_list(&in, &selected, 10, hit, 3);
   CHECK(selected == 0);
   finger_event(&in, SDL_FINGERUP, 0.1f, 50.0f * px);
   CHECK(in.touch_ended && !in.touch_dragged);
   CHECK(cobalt_input_tapped(&in, &hit[0]));

   /* Dragging up by two rows' height (100 px each) moves forward two, and the
    * release is not a tap even though it ends inside a row. */
   cobalt_input_begin_frame(&in, 0);
   finger_event(&in, SDL_FINGERDOWN, 0.1f, 250.0f * px);
   finger_event(&in, SDL_FINGERMOTION, 0.1f, 150.0f * px);
   cobalt_input_drag_list(&in, &selected, 10, hit, 3);
   CHECK(selected == 1);
   finger_event(&in, SDL_FINGERMOTION, 0.1f, 50.0f * px);
   cobalt_input_drag_list(&in, &selected, 10, hit, 3);
   CHECK(selected == 2);
   finger_event(&in, SDL_FINGERUP, 0.1f, 50.0f * px);
   CHECK(in.touch_ended && in.touch_dragged);
   CHECK(!cobalt_input_tapped(&in, &hit[0]));

   /* Dragging down goes back, never past the first row; a long drag up stops at
    * the last row; with no touch or no rows nothing moves. */
   cobalt_input_begin_frame(&in, 0);
   CHECK(!in.touch_dragged);
   cobalt_input_drag_list(&in, &selected, 10, hit, 3);
   CHECK(selected == 2);
   finger_event(&in, SDL_FINGERDOWN, 0.1f, 50.0f * px);
   finger_event(&in, SDL_FINGERMOTION, 0.1f, 470.0f * px);
   cobalt_input_drag_list(&in, &selected, 10, NULL, 0);
   CHECK(selected == 0);
   finger_event(&in, SDL_FINGERMOTION, 0.1f, 0.0f);
   cobalt_input_drag_list(&in, &selected, 3, hit, 3);
   CHECK(selected >= 0 && selected <= 2);
   finger_event(&in, SDL_FINGERUP, 0.1f, 0.0f);
}

static void
button_event(cobalt_input *in, Uint8 button, Uint32 type)
{
   SDL_Event e;
   memset(&e, 0, sizeof(e));
   e.type = type;
   e.cbutton.button = button;
   cobalt_input_handle_event(in, &e);
}

static void
test_refresh_rewinds_detail(void)
{
   begin("a refresh rewinds the timeline and starts its detail column at the top");
   cobalt_timeline view;
   cobalt_timeline_init(&view);
   view.detail_scroll = 2;
   view.detail_max = 4;
   cobalt_timeline_rewind(&view);
   CHECK(view.detail_scroll == 0);
   CHECK(view.nav.selected == 0);
}

static void
test_shoulder_buttons(void)
{
   begin("L and R press their own buttons, and no face button or stick");
   cobalt_input in;
   cobalt_input_init(&in);

   button_event(&in, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLERBUTTONDOWN);
   CHECK(cobalt_input_pressed(&in, COBALT_BTN_SHOULDER_R));
   CHECK(!cobalt_input_pressed(&in, COBALT_BTN_SHOULDER_L));
   CHECK(!cobalt_input_pressed(&in, COBALT_BTN_DOWN));
   CHECK(!cobalt_input_pressed(&in, COBALT_BTN_ALT_X));
   button_event(&in, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLERBUTTONUP);

   cobalt_input_begin_frame(&in, 0);
   button_event(&in, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLERBUTTONDOWN);
   CHECK(cobalt_input_pressed(&in, COBALT_BTN_SHOULDER_L));
   CHECK(!cobalt_input_pressed(&in, COBALT_BTN_SHOULDER_R));
   CHECK(!cobalt_input_pressed(&in, COBALT_BTN_UP));
   button_event(&in, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLERBUTTONUP);
}

static void
test_both_sticks_navigate(void)
{
   begin("both sticks act as a D-pad");
   cobalt_input in;
   cobalt_input_init(&in);

   axis_event(&in, SDL_CONTROLLER_AXIS_RIGHTY, 30000);
   CHECK(cobalt_input_pressed(&in, COBALT_BTN_DOWN));
   axis_event(&in, SDL_CONTROLLER_AXIS_RIGHTX, -30000);
   CHECK(cobalt_input_pressed(&in, COBALT_BTN_LEFT));

   /* Both sticks hold DOWN; centring one must not release it. */
   axis_event(&in, SDL_CONTROLLER_AXIS_LEFTY, 30000);
   axis_event(&in, SDL_CONTROLLER_AXIS_RIGHTY, 0);
   CHECK(cobalt_input_held(&in, COBALT_BTN_DOWN));
   axis_event(&in, SDL_CONTROLLER_AXIS_LEFTY, 0);
   CHECK(!cobalt_input_held(&in, COBALT_BTN_DOWN));

   axis_event(&in, SDL_CONTROLLER_AXIS_RIGHTX, 0);
   CHECK(!cobalt_input_held(&in, COBALT_BTN_LEFT));

   /* Inside the deadzone is nothing. */
   axis_event(&in, SDL_CONTROLLER_AXIS_RIGHTY, 5000);
   CHECK(!cobalt_input_held(&in, COBALT_BTN_DOWN));
}

static void
touch_release_at(cobalt_input *in, int x, int y)
{
   memset(in, 0, sizeof(*in));
   in->touch_x = x;
   in->touch_y = y;
   in->touch_ended = true;
}

static void
test_popup_touch(void)
{
   begin("popup rows take taps, outside closes");
   cobalt_popup p;
   memset(&p, 0, sizeof(p));
   cobalt_popup_open(&p, "More");
   cobalt_popup_add(&p, COBALT_POPUP_PROFILE, "Profile", "");
   cobalt_popup_add(&p, COBALT_POPUP_LINK, "Link", "");
   p.panel = (SDL_Rect){ 100, 100, 300, 200 };
   p.hit[0] = (SDL_Rect){ 100, 120, 300, 40 };
   p.hit[1] = (SDL_Rect){ 100, 160, 300, 40 };
   p.hit_valid = true;

   cobalt_input in;
   touch_release_at(&in, 150, 180);
   CHECK(cobalt_popup_update(&p, &in) == 1);
   CHECK(p.selected == 1);
   CHECK(p.open);

   touch_release_at(&in, 150, 130);
   CHECK(cobalt_popup_update(&p, &in) == 0);

   /* Inside the panel but on no row: ignored, stays open. */
   touch_release_at(&in, 150, 290);
   CHECK(cobalt_popup_update(&p, &in) == -1);
   CHECK(p.open);

   touch_release_at(&in, 10, 10);
   CHECK(cobalt_popup_update(&p, &in) == -2);
   CHECK(!p.open);
}

static void
test_listnav(void)
{
   begin("list navigation shared by the scrolling screens");

   static cobalt_listnav nav;
   cobalt_listnav_init(&nav);
   CHECK(nav.selected == 0 && nav.scroll == 0 && nav.last_visible == -1);
   CHECK(!nav.hit_valid && nav.hit_count == 0);

   /* The D-pad moves one row and stops at both ends. */
   cobalt_input in = tap(COBALT_BTN_DOWN);
   CHECK(cobalt_listnav_move(&nav, &in, 3) == -1);
   CHECK(nav.selected == 1);
   cobalt_listnav_move(&nav, &in, 3);
   cobalt_listnav_move(&nav, &in, 3);
   CHECK(nav.selected == 2);
   in = tap(COBALT_BTN_UP);
   cobalt_listnav_move(&nav, &in, 3);
   CHECK(nav.selected == 1);

   /* A shrunken list pulls the selection back in; an empty one is left alone. */
   nav.selected = 9;
   cobalt_listnav_move(&nav, &in, 2);
   CHECK(nav.selected <= 1);
   nav.selected = 5;
   CHECK(cobalt_listnav_move(&nav, &in, 0) == -1);
   CHECK(nav.selected == 5);
   CHECK(cobalt_listnav_move(NULL, &in, 3) == -1);

   /* A frame's rows are recorded, and a tap on one selects it and reports it. */
   cobalt_listnav_init(&nav);
   const SDL_Rect a = { 0, 0, 100, 40 };
   const SDL_Rect b = { 0, 50, 100, 40 };
   cobalt_listnav_draw_begin(&nav, true);
   cobalt_listnav_draw_add(&nav, true, &a, 4);
   cobalt_listnav_draw_add(&nav, true, &b, 5);
   cobalt_listnav_draw_end(&nav, true, 5);
   CHECK(nav.hit_valid && nav.hit_count == 2 && nav.last_visible == 5);
   touch_release_at(&in, 10, 60);
   CHECK(cobalt_listnav_move(&nav, &in, 10) == 5);
   CHECK(nav.selected == 5);

   /* The TV frame never touches the hit rectangles. */
   cobalt_listnav_draw_begin(&nav, false);
   cobalt_listnav_draw_add(&nav, false, &a, 0);
   CHECK(nav.hit_count == 2);

   /* Following: back is exact, forward uses what the last frame fitted. */
   nav.selected = 8;
   nav.scroll = 4;
   nav.last_visible = 6;
   cobalt_listnav_follow(&nav);
   CHECK(nav.scroll == 6);
   nav.selected = 2;
   cobalt_listnav_follow(&nav);
   CHECK(nav.scroll == 2);
   nav.selected = 20;
   nav.last_visible = -1;
   nav.scroll = 3;
   cobalt_listnav_follow(&nav);
   CHECK(nav.scroll == 3);   /* nothing fitted yet, so nothing to scroll by */

   /* An empty list publishes "nothing visible", and rewind goes to the top. */
   cobalt_listnav_draw_empty(&nav, true);
   CHECK(nav.last_visible == -1);
   nav.selected = 7;
   nav.scroll = 3;
   cobalt_listnav_rewind(&nav);
   CHECK(nav.selected == 0 && nav.scroll == 0 && nav.last_visible == -1);
}

/* The queue on the SD card: what is saved comes back exactly, and a damaged or
 * truncated file is refused rather than loaded half-way. */
static void
test_queue_store(void)
{
   begin("the queue file round-trips, and a damaged file is refused");
   static cobalt_post_queue q, back;
   static char buf[8192];
   const char *path = "queue-test.dat";

   cobalt_post_queue_init(&q);
   CHECK(cobalt_post_queue_add(&q, "line one\nline two\twith a tab \\ slash", 100));
   CHECK(cobalt_post_queue_add(&q, "plain", 200));

   CHECK(cobalt_queue_encode(&q, buf, sizeof(buf)));
   CHECK(cobalt_queue_decode(buf, strlen(buf), &back));
   CHECK(back.count == 2);
   CHECK(strcmp(back.items[0].text, q.items[0].text) == 0);
   CHECK(back.items[1].queued_at == 200);

   /* Cut short: no "end" line, so the whole file is refused and nothing loads. */
   CHECK(!cobalt_queue_decode(buf, strlen(buf) - 4, &back));
   CHECK(back.count == 0);
   CHECK(!cobalt_queue_decode("not a queue\n", 12, &back));
   CHECK(back.count == 0);

   /* Through the filesystem. A missing file is an empty queue, not an error. */
   remove(path);
   cobalt_post_queue_init(&back);
   CHECK(cobalt_queue_load(path, &back));
   CHECK(back.count == 0);
   CHECK(cobalt_queue_save(path, &q));
   CHECK(cobalt_queue_load(path, &back));
   CHECK(back.count == 2);
   CHECK(strcmp(back.items[1].text, "plain") == 0);
   remove(path);
}

static void
test_post_queue(void)
{
   begin("offline queue keeps order, refuses what it would cut, and sends only on request");
   static cobalt_post_queue q;
   static cobalt_queued_post got;
   cobalt_post_queue_init(&q);

   CHECK(cobalt_post_queue_get(&q, 0) == NULL);
   CHECK(!cobalt_post_queue_add(&q, "", 1));
   CHECK(!cobalt_post_queue_add(&q, NULL, 1));
   CHECK(q.count == 0);

   CHECK(cobalt_post_queue_add(&q, "first", 100));
   CHECK(cobalt_post_queue_add(&q, "second", 200));
   CHECK(cobalt_post_queue_add(&q, "third", 300));
   CHECK(q.count == 3);
   CHECK(strcmp(cobalt_post_queue_get(&q, 0)->text, "first") == 0);
   CHECK(cobalt_post_queue_get(&q, 2)->queued_at == 300);
   CHECK(cobalt_post_queue_get(&q, 3) == NULL);

   /* Discarding the middle one keeps the rest in order. */
   CHECK(cobalt_post_queue_discard(&q, 1));
   CHECK(q.count == 2);
   CHECK(strcmp(cobalt_post_queue_get(&q, 0)->text, "first") == 0);
   CHECK(strcmp(cobalt_post_queue_get(&q, 1)->text, "third") == 0);
   CHECK(!cobalt_post_queue_discard(&q, 5));

   /* Taking returns the post and removes it; only the person's own choice does
    * this, one post at a time. */
   CHECK(cobalt_post_queue_take(&q, 0, &got));
   CHECK(strcmp(got.text, "first") == 0);
   CHECK(q.count == 1);
   CHECK(strcmp(cobalt_post_queue_get(&q, 0)->text, "third") == 0);
   CHECK(!cobalt_post_queue_take(&q, 4, &got));

   /* Text that would have to be cut is refused, not shortened. */
   {
      char big[COBALT_POST_QUEUE_TEXT_MAX + 1];
      memset(big, 'a', sizeof big - 1);
      big[sizeof big - 1] = '\0';
      CHECK(!cobalt_post_queue_add(&q, big, 400));
      CHECK(q.count == 1);
   }

   /* Full at COBALT_POST_QUEUE_MAX: the next add is refused and nothing moves. */
   cobalt_post_queue_init(&q);
   for (unsigned i = 0; i < COBALT_POST_QUEUE_MAX; i++) {
      CHECK(cobalt_post_queue_add(&q, "p", (long long)i));
   }
   CHECK(!cobalt_post_queue_add(&q, "overflow", 999));
   CHECK(q.count == COBALT_POST_QUEUE_MAX);
   CHECK(cobalt_post_queue_get(&q, COBALT_POST_QUEUE_MAX - 1)->queued_at ==
         (long long)(COBALT_POST_QUEUE_MAX - 1));
}

/* Backing out of a list restores the cursor it was left on (#110). */
static void
test_listnav_restore(void)
{
   begin("a saved list position is restored, and out-of-range values are pulled in");
   static cobalt_listnav nav;
   cobalt_listnav_init(&nav);
   nav.selected = 9;
   nav.scroll = 6;
   nav.last_visible = 12;

   cobalt_listnav_restore(&nav, 5, 3);
   CHECK(nav.selected == 5 && nav.scroll == 3 && nav.last_visible == -1);

   cobalt_listnav_restore(&nav, 2, 4);
   CHECK(nav.selected == 2 && nav.scroll == 2);

   cobalt_listnav_restore(&nav, -3, -1);
   CHECK(nav.selected == 0 && nav.scroll == 0);

   cobalt_listnav_restore(NULL, 4, 4);
   CHECK(nav.selected == 0 && nav.scroll == 0);
}

static void
test_popup_link_qr(void)
{
   begin("a link's text page carries a QR code");
   static cobalt_popup p;

   cobalt_popup_show_text(&p, "Link", "https://example.com/a?b=1");
   CHECK(p.text_mode);
   CHECK(p.qr_size >= 21 && p.qr_size <= WF_QR_MAX_SIZE);
   /* The top-left finder pattern starts with a dark module and has a dark
    * border seven wide; a code with the wrong layout would not. */
   CHECK(p.qr[0] == 1 && p.qr[6] == 1 && p.qr[6 * p.qr_size] == 1);
   CHECK(p.qr[1 * p.qr_size + 1] == 0);

   cobalt_popup_show_text(&p, "Handle", "alice.example.com");
   CHECK(p.qr_size == 0);
   CHECK(!cobalt_popup_text_is_link(NULL));
   CHECK(cobalt_popup_text_is_link("http://x"));
}

int
main(int argc, char **argv)
{
   const char *root = (argc > 1) ? argv[1] : "build-test-scratch";

   if (getenv("COBALT_TEST_VERBOSE")) {
      cobalt_test_log_verbose(1);
   }
   cobalt_test_set_root(root);

   /* Start from a clean slate so a previous run cannot mask a failure. */
   char path[512];
   snprintf(path, sizeof(path), "%s/session.dat", root);
   remove(path);
   snprintf(path, sizeof(path), "%s/device.key", root);
   remove(path);

   printf("cobalt host tests\n");

   test_normalise_service();
   test_rng();
   test_session_store_roundtrip(root);
   test_session_store_clear(root);
   test_session_store_rejects_damage(root);
   test_session_store_needs_entropy();
   test_keyboard_typing();
   test_keyboard_multiline();
   test_keyboard_bounds();
   test_keyboard_multibyte();
   test_keyboard_display();
   test_feed_text();
   test_feed_counts();
   test_feed_embeds();
   test_feed_link_domain();
   test_video_duration_format();
   test_video_master_variant();
   test_video_master_fetch_count();
   test_prefs();
   test_entropy_gather(root);
   test_actor_list_remove();
   test_interactions();
   test_delete_post_helpers();
   test_quote_helpers();
   test_follow_lists();
   test_likes_lists();
   test_profile_tabs();
   test_quote_compose();
   test_image_attach(root);
   test_picker_paths(root);
   test_search_mode_toggle();
   test_pinned_prepend();
   test_compose();
   test_compose_thread();
   test_both_sticks_navigate();
   test_shoulder_buttons();
   test_refresh_rewinds_detail();
   test_touch_drag_scrolls_a_list();
   test_popup_touch();
   test_popup_link_qr();
   test_listnav();
   test_listnav_restore();
   test_post_queue();
   test_queue_store();
   test_settings_store();
   test_palette_contrast();
   test_post_refuses_partial_refs();
   test_notification_wording();
   test_chat_inbox_counterpart();
   test_chat_requests_count_label();
   test_chat_request_rows();
   test_chat_join_request_rows();
   test_message_decision_states();
   test_chat_thread_decision_states();
   test_chat_thread_state();
   test_messages_start();
   test_chat_start_target();
   test_paging_stops_when_the_window_fills();
   test_selection_survives_a_shrinking_list();
   test_session_request_handshake();
   test_signin_validation();
  test_signin_show_password_toggle();
   test_image_fit_geometry();
   test_image_resample();
   test_image_circle_mask();
   test_postcard_contain_fit();
   test_imageview();

   printf("\n%d checks, %d failures\n", s_checks, s_failures);
   return s_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
