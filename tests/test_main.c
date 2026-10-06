/*
 * Unit tests for Cobalt's platform-independent code.
 *
 * Scope is deliberately narrow, per AGENTS.md §10: the parts that can be tested
 * without a console are tested properly here, and nothing pretends to stand in
 * for a hardware pass. Rendering, input plumbing, ProcUI lifecycle and anything
 * that talks to a PDS are verified on the Wii U or not at all.
 */

#include "app/compose.h"
#include "app/graph.h"
#include "app/search.h"
#include "app/signin.h"
#include "app/update.h"
#include "app/entropyview.h"
#include "util/entropy_gather.h"
#include "atproto/actors.h"
#include "atproto/session.h"
#include "atproto/feed.h"
#include "atproto/notifications.h"
#include "atproto/prefs.h"
#include "cache/session_store.h"
#include "update/selfupdate.h"
#include "ui/imagecache.h"
#include "ui/imageview.h"
#include "ui/keyboard.h"
#include "ui/popup.h"
#include "ui/postcard.h"
#include "input/input.h"
#include "util/entropy.h"
#include "util/rng.h"
#include "util/timefmt.h"

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
   cobalt_keyboard_open(&kb, buffer, sizeof(buffer), false);

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

static void
test_keyboard_bounds(void)
{
   begin("keyboard respects the buffer it was given");

   char buffer[4] = "";
   cobalt_keyboard kb;
   cobalt_keyboard_open(&kb, buffer, sizeof(buffer), false);

   for (int i = 0; i < 10; i++) {
      type_key(&kb, 1, 0);
   }
   /* Three characters plus a terminator, and no overrun. */
   CHECK_STR(buffer, "qqq");

   /* Backspacing an empty buffer must not walk backwards out of it. */
   char empty[8] = "";
   cobalt_keyboard_open(&kb, empty, sizeof(empty), false);
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
   cobalt_keyboard_open(&kb, buffer, sizeof(buffer), false);

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
   cobalt_keyboard_open(&kb, buffer, sizeof(buffer), false);
   cobalt_keyboard_display_text(&kb, shown, sizeof(shown));
   CHECK_STR(shown, "hello_");

   /* Masked: one dot per character, never the characters themselves. */
   cobalt_keyboard_open(&kb, buffer, sizeof(buffer), true);
   cobalt_keyboard_display_text(&kb, shown, sizeof(shown));
   CHECK_STR(shown, "*****_");

   /* A masked multi-byte string is masked per character, not per byte. */
   char accented[32] = "caf\xc3\xa9";
   cobalt_keyboard_open(&kb, accented, sizeof(accented), true);
   cobalt_keyboard_display_text(&kb, shown, sizeof(shown));
   CHECK_STR(shown, "****_");

   /* Longer than the field: the tail is kept, because that is where the caret
    * is, and the window never opens mid-codepoint. */
   char lengthy[64] = "abcdefghijklmnopqrstuvwxyz";
   cobalt_keyboard_open(&kb, lengthy, sizeof(lengthy), false);
   char narrow[8];
   cobalt_keyboard_display_text(&kb, narrow, sizeof(narrow));
   CHECK_STR(narrow, "uvwxyz_");

   /* An empty buffer is just the caret. */
   char blank[8] = "";
   cobalt_keyboard_open(&kb, blank, sizeof(blank), false);
   cobalt_keyboard_display_text(&kb, shown, sizeof(shown));
   CHECK_STR(shown, "_");
}


/* --- timestamps --- */

static void
test_time_parse(void)
{
   begin("RFC 3339 parsing");

   int64_t epoch = 0;

   /* The epoch itself, and a value checked against a known Unix time. */
   CHECK(cobalt_time_parse_rfc3339("1970-01-01T00:00:00Z", &epoch));
   CHECK(epoch == 0);

   CHECK(cobalt_time_parse_rfc3339("2026-07-29T10:15:30Z", &epoch));
   CHECK(epoch == 1785320130);

   /* Fractional seconds are what a PDS actually emits, so this is the common
    * case rather than an edge one. */
   CHECK(cobalt_time_parse_rfc3339("2026-07-29T10:15:30.123Z", &epoch));
   CHECK(epoch == 1785320130);

   /* Leap-day handling, which is where hand-rolled date maths usually breaks:
    * 2000 is a leap year, 1900 and 2100 are not. */
   CHECK(cobalt_time_parse_rfc3339("2000-02-29T00:00:00Z", &epoch));
   CHECK(epoch == 951782400);
   CHECK(cobalt_time_parse_rfc3339("2024-02-29T12:00:00Z", &epoch));
   CHECK(epoch == 1709208000);

   /* Lowercase separators are legal RFC 3339. */
   CHECK(cobalt_time_parse_rfc3339("2026-07-29t10:15:30z", &epoch));
   CHECK(epoch == 1785320130);

   /*
    * A numeric offset must be refused, not read as if it were UTC. Accepting
    * it would put posts hours out of order in the feed, which is worse than
    * showing no timestamp at all.
    */
   CHECK(!cobalt_time_parse_rfc3339("2026-07-29T10:15:30+01:00", &epoch));

   /* Malformed input of various shapes. */
   CHECK(!cobalt_time_parse_rfc3339("", &epoch));
   CHECK(!cobalt_time_parse_rfc3339(NULL, &epoch));
   CHECK(!cobalt_time_parse_rfc3339("2026-07-29", &epoch));
   CHECK(!cobalt_time_parse_rfc3339("2026-07-29T10:15:30", &epoch));
   CHECK(!cobalt_time_parse_rfc3339("not-a-timestamp-at-all", &epoch));
   CHECK(!cobalt_time_parse_rfc3339("2026-13-01T00:00:00Z", &epoch));
   CHECK(!cobalt_time_parse_rfc3339("2026-07-29T10:15:30Ztrailing", &epoch));
}

static void
test_time_relative(void)
{
   begin("relative timestamps");

   char out[COBALT_RELATIVE_MAX];
   const int64_t now = 1785320130;

   cobalt_time_relative(now, now, out, sizeof(out));
   CHECK_STR(out, "0s");

   cobalt_time_relative(now - 45, now, out, sizeof(out));
   CHECK_STR(out, "45s");

   cobalt_time_relative(now - 60, now, out, sizeof(out));
   CHECK_STR(out, "1m");

   cobalt_time_relative(now - 3599, now, out, sizeof(out));
   CHECK_STR(out, "59m");

   cobalt_time_relative(now - 3600, now, out, sizeof(out));
   CHECK_STR(out, "1h");

   cobalt_time_relative(now - 86400 * 3, now, out, sizeof(out));
   CHECK_STR(out, "3d");

   cobalt_time_relative(now - 86400 * 20, now, out, sizeof(out));
   CHECK_STR(out, "2w");

   cobalt_time_relative(now - 86400 * 800, now, out, sizeof(out));
   CHECK_STR(out, "2y");

   /* A console with a slow clock produces future-dated posts. Clamping to
    * "now" is odd; "-4h" on every post would be worse. */
   cobalt_time_relative(now + 9999, now, out, sizeof(out));
   CHECK_STR(out, "0s");
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
   CHECK(!cobalt_prefs_add_word(&p, "", true, false));

   /* Matching itself is Wolfram's (wf_mod_match_mute_words) and is exercised in
    * tests/e2e_main.c, which links it; this binary has no SDK, so a word list
    * here never matches. */
   CHECK(cobalt_prefs_add_word(&p, "cat", true, false));
   CHECK(!cobalt_prefs_text_is_muted(&p, "I like my cat.", NULL, 0));
   CHECK(p.count == 1);

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

/* --- self-update core --- */

#define SHA_ABC "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"

/* Parsing, versions and SHA-256 are Wolfram's and are tested there against shared
 * vectors (test/vectors/update). What is Cobalt's, and tested here, is the policy
 * it puts on a manifest: the app, the repository, the file layout. */
static cobalt_update_status
parse_manifest_text(const char *text, wf_update_manifest *m)
{
   return cobalt_update_parse_manifest(text, strlen(text), COBALT_UPDATE_WUHB_MAX, m);
}

static void
test_update_manifest(void)
{
   begin("update manifest policy");
   wf_update_manifest m;
   const char *good =
      "{\"schema\":1,\"app\":\"cobalt\",\"version\":\"0.6.0\",\"notes\":\"Fixes.\","
      "\"asset\":{\"name\":\"cobalt-0.6.0.wuhb\",\"url\":"
      "\"https://github.com/ewanc26/cobalt/releases/download/v0.6.0/cobalt-0.6.0.wuhb\","
      "\"size\":1234,\"sha256\":\"" SHA_ABC "\"},\"signature\":null}";

   CHECK(parse_manifest_text(good, &m) == COBALT_UPDATE_OK);
   CHECK_STR(m.version, "0.6.0");
   CHECK_STR(m.asset.name, "cobalt-0.6.0.wuhb");
   CHECK(m.asset.size == 1234);
   CHECK(m.asset.sha256[0] == 0xba && m.asset.sha256[31] == 0xad);

   /* Every one of these is refused, and the output is left zeroed. */
   struct { const char *what; const char *from; const char *to; cobalt_update_status want; } bad[] = {
      {"schema 2", "\"schema\":1", "\"schema\":2", COBALT_UPDATE_BAD_MANIFEST},
      {"other app", "\"app\":\"cobalt\"", "\"app\":\"indigo\"", COBALT_UPDATE_BAD_POLICY},
      {"v-prefixed version", "\"version\":\"0.6.0\"", "\"version\":\"v0.6.0\"", COBALT_UPDATE_BAD_MANIFEST},
      {"http url", "https://github.com", "http://github.com", COBALT_UPDATE_BAD_MANIFEST},
      {"other host", "https://github.com/ewanc26", "https://evil.example/ewanc26", COBALT_UPDATE_BAD_POLICY},
      {"other repo", "ewanc26/cobalt/releases/download", "someone/else/releases/download", COBALT_UPDATE_BAD_POLICY},
      {"other tag", "download/v0.6.0/", "download/v0.5.0/", COBALT_UPDATE_BAD_POLICY},
      {"name with a slash", "\"name\":\"cobalt-0.6.0.wuhb\"", "\"name\":\"../cobalt-0.6.0.wuhb\"", COBALT_UPDATE_BAD_POLICY},
      {"zero size", "\"size\":1234", "\"size\":0", COBALT_UPDATE_BAD_MANIFEST},
      {"oversize", "\"size\":1234", "\"size\":99999999999", COBALT_UPDATE_BAD_MANIFEST},
      {"short sha", SHA_ABC, "ba7816bf", COBALT_UPDATE_BAD_MANIFEST},
   };
   for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
      char text[1024];
      const char *at = strstr(good, bad[i].from);
      CHECK(at != NULL);
      if (!at) continue;
      snprintf(text, sizeof text, "%.*s%s%s", (int) (at - good), good, bad[i].to,
               at + strlen(bad[i].from));
      cobalt_update_status st = parse_manifest_text(text, &m);
      if (st != bad[i].want) fprintf(stderr, "  case: %s -> %d\n", bad[i].what, (int) st);
      CHECK(st == bad[i].want);
      CHECK(m.version[0] == '\0');
   }
   CHECK(parse_manifest_text("not json", &m) == COBALT_UPDATE_BAD_MANIFEST);
   CHECK(parse_manifest_text("", &m) == COBALT_UPDATE_BAD_MANIFEST);

}

static bool
file_has(const char *path, const char *bytes, size_t n)
{
   FILE *f = fopen(path, "rb");
   char buf[256];
   if (!f) return false;
   size_t got = fread(buf, 1, sizeof buf, f);
   fclose(f);
   return got == n && memcmp(buf, bytes, n) == 0;
}

static bool
file_exists(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (f) fclose(f);
   return f != NULL;
}

static void
write_file(const char *path, const char *text)
{
   FILE *f = fopen(path, "wb");
   if (f) { fputs(text, f); fclose(f); }
}

static void
test_update_staging(const char *root)
{
   begin("update staging, replacement and recovery");

   char stage[300], installed[300];
   snprintf(stage, sizeof stage, "%s/update", root);
   snprintf(installed, sizeof installed, "%s/cobalt.wuhb", root);
   mkdir(stage, 0755);
   cobalt_update_paths p;
   CHECK(cobalt_update_paths_init(&p, installed, stage));
   CHECK(!cobalt_update_paths_init(&p, "", stage));
   CHECK(cobalt_update_paths_init(&p, installed, stage));

   remove(p.part); remove(p.staged); remove(p.old);
   write_file(p.installed, "OLD BUILD");

   unsigned char abc[32], other[32];
   wf_sha256_from_hex(SHA_ABC, 64, abc);
   memcpy(other, abc, 32);
   other[0] ^= 0xff;

   /* A wrong digest is refused before anything touches the card. */
   CHECK(cobalt_update_stage(&p, "abc", 3, other) == COBALT_STAGE_MISMATCH);
   CHECK(!file_exists(p.part) && !file_exists(p.staged));
   CHECK(file_has(p.installed, "OLD BUILD", 9));

   /* A truncated download is a different digest, so it is refused too. */
   CHECK(cobalt_update_stage(&p, "ab", 2, abc) == COBALT_STAGE_MISMATCH);

   /* Staging never touches the installed build. */
   CHECK(cobalt_update_stage(&p, "abc", 3, abc) == COBALT_STAGE_OK);
   CHECK(cobalt_update_has_staged(&p));
   CHECK(!file_exists(p.part));
   CHECK(file_has(p.installed, "OLD BUILD", 9));

   /* Applying keeps the old build until it is committed. */
   CHECK(cobalt_update_apply(&p));
   CHECK(file_has(p.installed, "abc", 3));
   CHECK(file_has(p.old, "OLD BUILD", 9));
   CHECK(!cobalt_update_has_staged(&p));
   CHECK(cobalt_update_recover(&p) == COBALT_RECOVER_NOTHING);   /* a healthy start keeps .old */
   CHECK(file_exists(p.old));
   CHECK(cobalt_update_commit(&p));
   CHECK(!file_exists(p.old));
   CHECK(!cobalt_update_commit(&p));

   /* Interrupted between the two renames: installed is gone, .old holds the build. */
   write_file(p.old, "OLD BUILD");
   remove(p.installed);
   write_file(p.staged, "abc");
   CHECK(cobalt_update_recover(&p) == COBALT_RECOVER_RESTORED);
   CHECK(file_has(p.installed, "OLD BUILD", 9));
   CHECK(!file_exists(p.old));
   CHECK(!file_exists(p.staged));

   /* Interrupted download and an unapplied staged file are cleaned up. */
   write_file(p.part, "half");
   CHECK(cobalt_update_recover(&p) == COBALT_RECOVER_CLEANED);
   CHECK(!file_exists(p.part));
   write_file(p.staged, "abc");
   CHECK(cobalt_update_recover(&p) == COBALT_RECOVER_CLEANED);
   CHECK(!file_exists(p.staged));
   CHECK(file_has(p.installed, "OLD BUILD", 9));

   /* Nothing to apply, or nothing to replace: refused, nothing changes. */
   CHECK(!cobalt_update_apply(&p));
   write_file(p.staged, "abc");
   remove(p.installed);
   CHECK(!cobalt_update_apply(&p));
   remove(p.staged);
}

/* A throwaway key made for these tests: its private half was deleted after it signed the
 * two manifests below, so no key material is in the repository. Cobalt's real key is
 * never used here. */
#define TEST_PUBKEY_HEX "8ae366e58408504443fb8c68fd88771d453e678d7342ed6273e0d7ca7e7605cb"
#define TEST_SIG_9_9_9 \
   "bf66f053a7e3f81e53838b301a4a03a3170712694f3c28f45e58e5c4a7763864" \
   "d5b0535c6858cbe2358d4126406154c8be2bdb2f9ad83a7d5ba84301c9b5080a"
#define TEST_SIG_0_5_0 \
   "1f167ee066832b8d405146ba25ecf70b0cf3432c6a52b63ccd65d4f6eef83775" \
   "292cfd921a972908ecba01c0763264c8e80adbb4055b5c7fabb2df17cfdd8e04"

static const char *s_fake_wuhb = "abc";
static bool s_fake_sig_present = true;
static bool s_fake_sig_tampered = false;
static bool s_fake_manifest_tampered = false;
static int s_fake_fetches = 0;
static bool s_fake_manifest_ok = true;
static const char *s_fake_version = "9.9.9";

static bool
fake_fetch(const char *url, size_t max_bytes, unsigned char **data, size_t *size)
{
   char body[1024];
   const char *text;

   s_fake_fetches++;
   if (strcmp(url, COBALT_UPDATE_MANIFEST_URL) == 0) {
      if (!s_fake_manifest_ok) return false;
      snprintf(body, sizeof body,
               "{\"schema\":1,\"app\":\"cobalt\",\"version\":\"%s\",\"notes\":\"n\",\"asset\":{"
               "\"name\":\"cobalt-%s.wuhb\",\"url\":\"" COBALT_UPDATE_ASSET_PREFIX "v%s/cobalt-%s.wuhb\","
               "\"size\":3,\"sha256\":\"" SHA_ABC "\"}}",
               s_fake_version, s_fake_version, s_fake_version, s_fake_version);
      if (s_fake_manifest_tampered) {
         /* one byte different from what was signed */
         body[strlen(body) - 3] = 'x';
      }
      text = body;
   } else if (strcmp(url, COBALT_UPDATE_SIGNATURE_URL) == 0) {
      if (!s_fake_sig_present) return false;
      text = strcmp(s_fake_version, "0.5.0") == 0 ? TEST_SIG_0_5_0 : TEST_SIG_9_9_9;
      if (s_fake_sig_tampered) {
         snprintf(body, sizeof body, "%s", text);
         body[10] = body[10] == '0' ? '1' : '0';
         text = body;
      }
   } else {
      text = s_fake_wuhb;
      if (strlen(text) > max_bytes) return false;
   }
   *size = strlen(text);
   *data = malloc(*size);
   memcpy(*data, text, *size);
   return true;
}

static void
test_update_view(const char *root)
{
   begin("update screen asks first, downloads on A, installs on quit");

   char data[300], installed[300];
   snprintf(data, sizeof data, "%s/cobalt", root);
   mkdir(data, 0755);
   snprintf(installed, sizeof installed, "%s.wuhb", data);
   write_file(installed, "RUNNING");

   cobalt_update_view v;
   cobalt_update_view_init(&v);
   v.threaded = false;
   v.fetch = fake_fetch;
   CHECK(wf_sha256_from_hex(TEST_PUBKEY_HEX, 64, v.pubkey) == WF_OK);
   snprintf(v.running, sizeof v.running, "0.5.0");
   cobalt_update_view_startup(&v, data);
   CHECK(v.have_paths);

   cobalt_input in;
   memset(&in, 0, sizeof in);

   /* Opening only checks. A newer version is offered, not fetched. */
   s_fake_fetches = 0;
   s_fake_wuhb = "abc";
   cobalt_update_view_open(&v);
   CHECK(cobalt_update_view_state(&v) == COBALT_UPDATE_AVAILABLE);
   CHECK(s_fake_fetches == 2); /* update.json and update.json.sig, nothing else */
   CHECK(!cobalt_update_has_staged(&v.paths));

   /* B leaves without downloading. */
   in.pressed[COBALT_BTN_BACK] = true;
   CHECK(cobalt_update_view_update(&v, &in) == COBALT_UPDATE_VIEW_BACK);
   CHECK(s_fake_fetches == 2);
   in.pressed[COBALT_BTN_BACK] = false;

   /* Quitting with nothing confirmed changes nothing. */
   CHECK(!cobalt_update_view_apply_on_quit(&v));
   CHECK(file_has(installed, "RUNNING", 7));

   /* A downloads, verifies and stages; the running build is untouched until quit. */
   in.pressed[COBALT_BTN_CONFIRM] = true;
   cobalt_update_view_update(&v, &in);
   in.pressed[COBALT_BTN_CONFIRM] = false;
   CHECK(cobalt_update_view_state(&v) == COBALT_UPDATE_READY);
   CHECK(file_has(installed, "RUNNING", 7));
   CHECK(cobalt_update_view_apply_on_quit(&v));
   CHECK(file_has(installed, "abc", 3));
   CHECK(file_exists(v.paths.old));

   /* The next start keeps the old build until the new one has run a frame. */
   cobalt_update_view v2;
   cobalt_update_view_init(&v2);
   cobalt_update_view_startup(&v2, data);
   CHECK(file_exists(v2.paths.old));
   cobalt_update_view_tick(&v2);
   CHECK(!file_exists(v2.paths.old));
   cobalt_update_view_destroy(&v2);

   /* A corrupt download is discarded and the build is untouched. */
   s_fake_wuhb = "xyz";
   cobalt_update_view_init(&v2);
   v2.threaded = false;
   v2.fetch = fake_fetch;
   CHECK(wf_sha256_from_hex(TEST_PUBKEY_HEX, 64, v2.pubkey) == WF_OK);
   snprintf(v2.running, sizeof v2.running, "0.5.0");
   cobalt_update_view_startup(&v2, data);
   cobalt_update_view_open(&v2);
   in.pressed[COBALT_BTN_CONFIRM] = true;
   cobalt_update_view_update(&v2, &in);
   in.pressed[COBALT_BTN_CONFIRM] = false;
   CHECK(cobalt_update_view_state(&v2) == COBALT_UPDATE_FAILED);
   CHECK(!cobalt_update_has_staged(&v2.paths));
   CHECK(!cobalt_update_view_apply_on_quit(&v2));
   CHECK(file_has(installed, "abc", 3));

   /* Same or older version: nothing to offer. Unreachable network: a failure, not a hang. */
   s_fake_version = "0.5.0";
   in.pressed[COBALT_BTN_CONFIRM] = true;
   cobalt_update_view_update(&v2, &in);
   in.pressed[COBALT_BTN_CONFIRM] = false;
   CHECK(cobalt_update_view_state(&v2) == COBALT_UPDATE_CURRENT);
   s_fake_manifest_ok = false;
   in.pressed[COBALT_BTN_CONFIRM] = true;
   cobalt_update_view_update(&v2, &in);
   in.pressed[COBALT_BTN_CONFIRM] = false;
   CHECK(cobalt_update_view_state(&v2) == COBALT_UPDATE_FAILED);
   s_fake_manifest_ok = true;
   s_fake_version = "9.9.9";

   /* An unsigned release, a damaged signature, a manifest changed after it was
    * signed and a signature under another key are all refused; nothing is fetched
    * beyond the two small files, so nothing can be staged. */
   s_fake_sig_present = false;
   s_fake_fetches = 0;
   in.pressed[COBALT_BTN_CONFIRM] = true;
   cobalt_update_view_update(&v2, &in);
   in.pressed[COBALT_BTN_CONFIRM] = false;
   CHECK(cobalt_update_view_state(&v2) == COBALT_UPDATE_FAILED);
   CHECK(s_fake_fetches == 2);
   s_fake_sig_present = true;
   s_fake_sig_tampered = true;
   in.pressed[COBALT_BTN_CONFIRM] = true;
   cobalt_update_view_update(&v2, &in);
   in.pressed[COBALT_BTN_CONFIRM] = false;
   CHECK(cobalt_update_view_state(&v2) == COBALT_UPDATE_FAILED);
   s_fake_sig_tampered = false;
   s_fake_manifest_tampered = true;
   in.pressed[COBALT_BTN_CONFIRM] = true;
   cobalt_update_view_update(&v2, &in);
   in.pressed[COBALT_BTN_CONFIRM] = false;
   CHECK(cobalt_update_view_state(&v2) == COBALT_UPDATE_FAILED);
   CHECK(!cobalt_update_has_staged(&v2.paths));
   s_fake_manifest_tampered = false;
   /* Cobalt's real key does not accept a manifest signed by the test key. */
   cobalt_update_public_key(v2.pubkey);
   in.pressed[COBALT_BTN_CONFIRM] = true;
   cobalt_update_view_update(&v2, &in);
   in.pressed[COBALT_BTN_CONFIRM] = false;
   CHECK(cobalt_update_view_state(&v2) == COBALT_UPDATE_FAILED);
   /* ... and with the right key it is accepted again, so the refusals above were the signature. */
   CHECK(wf_sha256_from_hex(TEST_PUBKEY_HEX, 64, v2.pubkey) == WF_OK);
   in.pressed[COBALT_BTN_CONFIRM] = true;
   cobalt_update_view_update(&v2, &in);
   in.pressed[COBALT_BTN_CONFIRM] = false;
   CHECK(cobalt_update_view_state(&v2) == COBALT_UPDATE_AVAILABLE);

   cobalt_update_view_destroy(&v2);
   cobalt_update_view_destroy(&v);
}

/* The manifest tools/make-update-manifest.sh produced, read by the real parser. */
static void
test_update_release_manifest(const char *manifest_path, const char *wuhb_path)
{
   begin("release script manifest is accepted by the reader");
   char text[2048];
   wf_update_manifest m;
   unsigned char digest[32];
   FILE *f = fopen(manifest_path, "rb");
   size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
   if (f) fclose(f);
   text[n] = '\0';
   CHECK(n > 0);
   CHECK(parse_manifest_text(text, &m) == COBALT_UPDATE_OK);
   CHECK_STR(m.version, "9.9.9");
   CHECK_STR(m.asset.name, "cobalt-9.9.9.wuhb");
   CHECK_STR(m.notes, "Fixed things.");
   CHECK(cobalt_sha256_file(wuhb_path, digest));
   CHECK(cobalt_sha256_equal(digest, m.asset.sha256));
   struct stat st;
   CHECK(stat(wuhb_path, &st) == 0 && (unsigned long) st.st_size == m.asset.size);
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
test_feed_link_domain(void)
{
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

static void
test_time_format(void)
{
   begin("RFC 3339 formatting");

   char out[32];

   CHECK(cobalt_time_format_rfc3339(0, out, sizeof(out)));
   CHECK_STR(out, "1970-01-01T00:00:00Z");

   CHECK(cobalt_time_format_rfc3339(1785320130, out, sizeof(out)));
   CHECK_STR(out, "2026-07-29T10:15:30Z");

   /* Leap day, the case the arithmetic is most likely to get wrong. */
   CHECK(cobalt_time_format_rfc3339(1709208000, out, sizeof(out)));
   CHECK_STR(out, "2024-02-29T12:00:00Z");

   /* Round-trips with the parser, which is the property that actually
    * matters — the two have to agree about the same instant. */
   const int64_t samples[] = { 0, 1, 951782400, 1709208000, 1785320130,
                               2000000000 };
   for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
      CHECK(cobalt_time_format_rfc3339(samples[i], out, sizeof(out)));
      int64_t back = -1;
      CHECK(cobalt_time_parse_rfc3339(out, &back));
      CHECK(back == samples[i]);
   }

   /* Too small a buffer is refused rather than truncated into a wrong date. */
   char tiny[8];
   CHECK(!cobalt_time_format_rfc3339(0, tiny, sizeof(tiny)));
   CHECK(!cobalt_time_format_rfc3339(0, NULL, 32));
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
   begin("profile tab filters and cycling");

   CHECK(strcmp(cobalt_profile_tab_name(COBALT_PROFILE_TAB_MEDIA), "Media") == 0);
   CHECK(strcmp(cobalt_profile_tab_filter(COBALT_PROFILE_TAB_REPLIES),
                "posts_with_replies") == 0);
   CHECK(strcmp(cobalt_profile_tab_filter(COBALT_PROFILE_TAB_MEDIA),
                "posts_with_media") == 0);
   CHECK(cobalt_profile_tab_filter(COBALT_PROFILE_TAB_LIKES) == NULL);

   /* Other people's likes are private, so the cycle skips that tab. */
   CHECK(cobalt_profile_tab_next(COBALT_PROFILE_TAB_POSTS, false) ==
         COBALT_PROFILE_TAB_REPLIES);
   CHECK(cobalt_profile_tab_next(COBALT_PROFILE_TAB_MEDIA, false) ==
         COBALT_PROFILE_TAB_POSTS);
   CHECK(cobalt_profile_tab_next(COBALT_PROFILE_TAB_MEDIA, true) ==
         COBALT_PROFILE_TAB_LIKES);
   CHECK(cobalt_profile_tab_next(COBALT_PROFILE_TAB_LIKES, true) ==
         COBALT_PROFILE_TAB_POSTS);
   /* A stale Likes tab on a profile that cannot show it falls back to Posts. */
   CHECK(cobalt_profile_tab_next(COBALT_PROFILE_TAB_LIKES, false) ==
         COBALT_PROFILE_TAB_POSTS);
   CHECK(!cobalt_session_begin_profile_tab(COBALT_PROFILE_TAB_MEDIA));
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
   CHECK(view.selected == 0 && view.scroll == 0);

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
   snprintf(post.handle, sizeof(post.handle), "@alice.test");

   static cobalt_compose c;
   cobalt_compose_quote(&c, &post);
   CHECK(cobalt_compose_is_quote(&c));
   CHECK(!cobalt_compose_is_reply(&c));
   CHECK(strcmp(c.quote_uri, post.uri) == 0);
   CHECK(strcmp(c.quote_cid, "bafycid") == 0);
   CHECK(strcmp(c.quote_handle, "@alice.test") == 0);

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

   CHECK(strcmp(cobalt_attach_mime("a/b/photo.JPG"), "image/jpeg") == 0);
   CHECK(strcmp(cobalt_attach_mime("x.jpeg"), "image/jpeg") == 0);
   CHECK(strcmp(cobalt_attach_mime("x.png"), "image/png") == 0);
   CHECK(cobalt_attach_mime("x.gif") == NULL);
   CHECK(cobalt_attach_mime("noext") == NULL);
   CHECK(cobalt_attach_mime(NULL) == NULL);

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

   static char names[COBALT_PICKER_MAX][COBALT_PICKER_NAME_MAX];
   CHECK(cobalt_compose_scan_images(dir, names, COBALT_PICKER_MAX) == 2);
   CHECK(strcmp(names[0], "a.jpg") == 0);
   CHECK(strcmp(names[1], "b.png") == 0);
   CHECK(cobalt_compose_scan_images("/nonexistent-cobalt-dir", names, 4) == 0);
   CHECK(cobalt_compose_scan_images(dir, names, 1) == 1);

   static cobalt_compose c;
   cobalt_compose_init(&c);
   snprintf(c.text, sizeof(c.text), "hello");
   c.confirming = true;
   c.confirm_choice = 3; /* image */

   /* Open the picker, move down, attach the second file. */
   cobalt_compose_open_picker(&c, dir);
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
   cobalt_compose_open_picker(&c, dir);
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

   CHECK(cobalt_session_begin_login("bsky.social", "someone.test", "app-pass"));

   /* A second request while one is in flight must be refused rather than
    * queued behind it or, worse, racing it onto the same wf_session. Guarded
    * because a host job finishes almost instantly and may already be done. */
   if (cobalt_session_busy()) {
      CHECK(!cobalt_session_begin_login("bsky.social", "other.test", "pass"));
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
   form.focus = 3;
   cobalt_input confirm = tap(COBALT_BTN_CONFIRM);
   CHECK(cobalt_signin_update(&form, &confirm) == COBALT_SIGNIN_STAY);
   CHECK(form.status[0] != '\0');
   CHECK(form.status_is_error);
   CHECK(form.focus == 1);

   /* Identifier but no password: this now selects browser OAuth. */
   snprintf(form.identifier, sizeof(form.identifier), "someone.bsky.social");
   form.focus = 3;
   CHECK(cobalt_signin_update(&form, &confirm) == COBALT_SIGNIN_SUBMIT);

   /* Supplying an app password still selects the legacy direct-login path. */
   snprintf(form.password, sizeof(form.password), "abcd-efgh-ijkl-mnop");
   form.focus = 3;
   CHECK(cobalt_signin_update(&form, &confirm) == COBALT_SIGNIN_SUBMIT);

   /* Signing out of the screen wipes the password rather than leaving it in
    * the app's allocation. */
   cobalt_signin_clear_password(&form);
   CHECK(form.password[0] == '\0');

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
   test_keyboard_bounds();
   test_keyboard_multibyte();
   test_keyboard_display();
   test_time_parse();
   test_time_relative();
   test_time_format();
   test_feed_text();
   test_feed_counts();
   test_feed_embeds();
   test_feed_link_domain();
   test_prefs();
   test_update_manifest();
   test_update_staging(root);
   test_update_view(root);
   test_entropy_gather(root);
   if (argc > 3) test_update_release_manifest(argv[2], argv[3]);
   test_actor_list_remove();
   test_interactions();
   test_delete_post_helpers();
   test_quote_helpers();
   test_follow_lists();
   test_likes_lists();
   test_profile_tabs();
   test_quote_compose();
   test_image_attach(root);
   test_search_mode_toggle();
   test_pinned_prepend();
   test_compose();
   test_both_sticks_navigate();
   test_popup_touch();
   test_post_refuses_partial_refs();
   test_notification_wording();
   test_paging_stops_when_the_window_fills();
   test_selection_survives_a_shrinking_list();
   test_session_request_handshake();
   test_signin_validation();
   test_image_fit_geometry();
   test_image_resample();
   test_image_circle_mask();
   test_postcard_contain_fit();
   test_imageview();

   printf("\n%d checks, %d failures\n", s_checks, s_failures);
   return s_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
