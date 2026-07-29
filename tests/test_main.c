/*
 * Unit tests for Cobalt's platform-independent code.
 *
 * Scope is deliberately narrow, per AGENTS.md §10: the parts that can be tested
 * without a console are tested properly here, and nothing pretends to stand in
 * for a hardware pass. Rendering, input plumbing, ProcUI lifecycle and anything
 * that talks to a PDS are verified on the Wii U or not at all.
 */

#include "app/signin.h"
#include "atproto/session.h"
#include "cache/session_store.h"
#include "ui/keyboard.h"
#include "util/entropy.h"
#include "util/rng.h"

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

   /* Identifier but no password: same again, focus on the password. */
   snprintf(form.identifier, sizeof(form.identifier), "someone.bsky.social");
   form.focus = 3;
   CHECK(cobalt_signin_update(&form, &confirm) == COBALT_SIGNIN_STAY);
   CHECK(form.focus == 2);

   /* Both present: now it submits. */
   snprintf(form.password, sizeof(form.password), "abcd-efgh-ijkl-mnop");
   form.focus = 3;
   CHECK(cobalt_signin_update(&form, &confirm) == COBALT_SIGNIN_SUBMIT);

   /* Signing out of the screen wipes the password rather than leaving it in
    * the app's allocation. */
   cobalt_signin_clear_password(&form);
   CHECK(form.password[0] == '\0');

   cobalt_session_shutdown();
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
   test_session_request_handshake();
   test_signin_validation();

   printf("\n%d checks, %d failures\n", s_checks, s_failures);
   return s_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
