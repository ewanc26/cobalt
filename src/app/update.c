#include "app/update.h"

#include "net/http.h"
#include "ui/theme.h"
#include "util/log.h"
#include "util/version.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define JOB_NONE 0
#define JOB_CHECK 1
#define JOB_INSTALL 2

static bool
default_fetch(const char *url, size_t max_bytes, unsigned char **data, size_t *size)
{
   cobalt_http_response resp;

   memset(&resp, 0, sizeof(resp));
   if (!cobalt_http_get(url, max_bytes, &resp)) {
      return false;
   }
   if (resp.status != 200 || !resp.data || resp.size == 0) {
      cobalt_http_response_free(&resp);
      return false;
   }
   /* Take ownership of the buffer. */
   *data = resp.data;
   *size = resp.size;
   resp.data = NULL;
   cobalt_http_response_free(&resp);
   return true;
}

void
cobalt_update_view_init(cobalt_update_view *v)
{
   memset(v, 0, sizeof(*v));
   v->fetch = default_fetch;
   v->threaded = true;
   v->lock = SDL_CreateMutex();
   snprintf(v->running, sizeof(v->running), "%s", COBALT_VERSION);
   if (!cobalt_update_public_key(v->pubkey)) {
      /* Cannot happen with a well-formed update_key.h; an all-zero key verifies nothing. */
      memset(v->pubkey, 0, sizeof(v->pubkey));
   }
}

static void
set_state(cobalt_update_view *v, cobalt_update_state st, const char *fmt, ...)
{
   char msg[sizeof(v->message)];
   va_list ap;

   msg[0] = '\0';
   if (fmt) {
      va_start(ap, fmt);
      vsnprintf(msg, sizeof(msg), fmt, ap);
      va_end(ap);
   }
   SDL_LockMutex(v->lock);
   v->state = st;
   snprintf(v->message, sizeof(v->message), "%s", msg);
   SDL_UnlockMutex(v->lock);
}

void
cobalt_update_view_destroy(cobalt_update_view *v)
{
   if (v->thread) {
      SDL_WaitThread(v->thread, NULL);
      v->thread = NULL;
   }
   if (v->lock) {
      SDL_DestroyMutex(v->lock);
      v->lock = NULL;
   }
}

void
cobalt_update_view_startup(cobalt_update_view *v, const char *data_root)
{
   char stage[COBALT_UPDATE_PATH_MAX];
   char installed[COBALT_UPDATE_PATH_MAX];
   const char *slash;
   int n;

   v->have_paths = false;
   if (!data_root || !data_root[0]) {
      return;
   }
   /* The data root is <sd>/wiiu/apps/cobalt; the build is <sd>/wiiu/apps/cobalt.wuhb. */
   n = snprintf(installed, sizeof(installed), "%s.wuhb", data_root);
   if (n <= 0 || (size_t) n >= sizeof(installed)) {
      return;
   }
   slash = strrchr(data_root, '/');
   (void) slash;
   n = snprintf(stage, sizeof(stage), "%s/update", data_root);
   if (n <= 0 || (size_t) n >= sizeof(stage)) {
      return;
   }
   mkdir(stage, 0777);
   if (!cobalt_update_paths_init(&v->paths, installed, stage)) {
      return;
   }
   v->have_paths = true;
   switch (cobalt_update_recover(&v->paths)) {
      case COBALT_RECOVER_RESTORED:
         COBALT_LOGW("update: an interrupted update left no build installed; restored the previous one");
         break;
      case COBALT_RECOVER_CLEANED:
         COBALT_LOGI("update: removed a leftover partial update");
         break;
      case COBALT_RECOVER_NOTHING:
         break;
   }
}

void
cobalt_update_view_tick(cobalt_update_view *v)
{
   if (v->committed || !v->have_paths) {
      return;
   }
   v->committed = true;
   if (cobalt_update_commit(&v->paths)) {
      COBALT_LOGI("update: the updated build has started; the previous one was removed");
   }
}

/* --- jobs --------------------------------------------------------------- */

static void
do_check(cobalt_update_view *v)
{
   unsigned char *body = NULL;
   unsigned char *sig = NULL;
   size_t len = 0;
   size_t sig_len = 0;
   wf_update_manifest m;

   if (!v->fetch(COBALT_UPDATE_MANIFEST_URL, COBALT_UPDATE_MANIFEST_MAX, &body, &len)) {
      set_state(v, COBALT_UPDATE_FAILED, "Could not reach GitHub. Check the network and try again.");
      return;
   }
   /* The signature is checked on the bytes as downloaded, before anything is parsed. A
    * release that has not been signed (yet) is refused, not trusted on its SHA-256. */
   if (!v->fetch(COBALT_UPDATE_SIGNATURE_URL, COBALT_UPDATE_SIGNATURE_MAX, &sig, &sig_len)) {
      free(body);
      COBALT_LOGW("update: no signature for the latest release; refusing it");
      set_state(v, COBALT_UPDATE_FAILED, "The latest release is not signed, so it was not used.");
      return;
   }
   cobalt_update_status st = cobalt_update_verify_manifest((const char *) body, len,
                                                           (const char *) sig, sig_len, v->pubkey);
   free(sig);
   if (st != COBALT_UPDATE_OK) {
      free(body);
      COBALT_LOGW("update: manifest refused: %s", cobalt_update_status_string(st));
      set_state(v, COBALT_UPDATE_FAILED, "The update was not signed by Cobalt's key, so it was not used.");
      return;
   }
   st = cobalt_update_parse_manifest((const char *) body, len, COBALT_UPDATE_WUHB_MAX, &m);
   free(body);
   if (st != COBALT_UPDATE_OK) {
      COBALT_LOGW("update: manifest refused: %s", cobalt_update_status_string(st));
      set_state(v, COBALT_UPDATE_FAILED, "The update information was not usable.");
      return;
   }
   int bad = 0;
   const int c = wf_update_compare_versions(m.version, v->running, &bad);
   if (bad) {
      set_state(v, COBALT_UPDATE_FAILED, "The update information was not usable.");
      return;
   }
   SDL_LockMutex(v->lock);
   v->manifest = m;
   SDL_UnlockMutex(v->lock);
   if (c > 0) {
      COBALT_LOGI("update: %s is available (running %s)", m.version, v->running);
      set_state(v, COBALT_UPDATE_AVAILABLE, NULL);
   } else {
      set_state(v, COBALT_UPDATE_CURRENT, NULL);
   }
}

static void
do_install(cobalt_update_view *v)
{
   unsigned char *data = NULL;
   size_t len = 0;
   wf_update_manifest m;

   SDL_LockMutex(v->lock);
   m = v->manifest;
   SDL_UnlockMutex(v->lock);

   if (!v->have_paths) {
      set_state(v, COBALT_UPDATE_FAILED, "Cobalt does not know where it is installed.");
      return;
   }
   if (!v->fetch(m.asset.url, m.asset.size, &data, &len)) {
      set_state(v, COBALT_UPDATE_FAILED, "The download failed. Nothing was changed.");
      return;
   }
   if (len != m.asset.size) {
      free(data);
      set_state(v, COBALT_UPDATE_FAILED, "The download was the wrong size. Nothing was changed.");
      return;
   }
   const cobalt_stage_result r = cobalt_update_stage(&v->paths, data, len, m.asset.sha256);
   free(data);
   switch (r) {
      case COBALT_STAGE_OK:
         COBALT_LOGI("update: %s downloaded and verified; it will be installed on quit", m.version);
         set_state(v, COBALT_UPDATE_READY, NULL);
         return;
      case COBALT_STAGE_MISMATCH:
         set_state(v, COBALT_UPDATE_FAILED,
                   "The download did not match its checksum and was discarded. Nothing was changed.");
         return;
      case COBALT_STAGE_WRITE_FAILED:
         set_state(v, COBALT_UPDATE_FAILED, "Could not write to the SD card. Nothing was changed.");
         return;
      case COBALT_STAGE_RENAME_FAILED:
         set_state(v, COBALT_UPDATE_FAILED, "Could not stage the update. Nothing was changed.");
         return;
   }
}

static int
worker_main(void *arg)
{
   cobalt_update_view *v = arg;

   if (v->job == JOB_CHECK) {
      do_check(v);
   } else if (v->job == JOB_INSTALL) {
      do_install(v);
   }
   v->job = JOB_NONE;
   return 0;
}

static void
start_job(cobalt_update_view *v, int job, cobalt_update_state busy)
{
   if (v->thread) {
      SDL_WaitThread(v->thread, NULL);
      v->thread = NULL;
   }
   v->job = job;
   set_state(v, busy, NULL);
   if (v->threaded) {
      v->thread = SDL_CreateThread(worker_main, "cobalt-update", v);
      if (v->thread) {
         return;
      }
   }
   worker_main(v);
}

void
cobalt_update_view_open(cobalt_update_view *v)
{
   const cobalt_update_state st = cobalt_update_view_state(v);

   /* Reopening mid-download must not start a second one. */
   if (st == COBALT_UPDATE_CHECKING || st == COBALT_UPDATE_DOWNLOADING ||
       st == COBALT_UPDATE_READY || st == COBALT_UPDATE_AVAILABLE) {
      return;
   }
   start_job(v, JOB_CHECK, COBALT_UPDATE_CHECKING);
}

cobalt_update_state
cobalt_update_view_state(cobalt_update_view *v)
{
   cobalt_update_state st;

   SDL_LockMutex(v->lock);
   st = v->state;
   SDL_UnlockMutex(v->lock);
   return st;
}

cobalt_update_view_action
cobalt_update_view_update(cobalt_update_view *v, const cobalt_input *in)
{
   const cobalt_update_state st = cobalt_update_view_state(v);

   if (st == COBALT_UPDATE_AVAILABLE && cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      /* The user has seen the version and notes and chosen this. */
      start_job(v, JOB_INSTALL, COBALT_UPDATE_DOWNLOADING);
      return COBALT_UPDATE_VIEW_STAY;
   }
   if ((st == COBALT_UPDATE_FAILED || st == COBALT_UPDATE_CURRENT) &&
       cobalt_input_pressed(in, COBALT_BTN_CONFIRM)) {
      start_job(v, JOB_CHECK, COBALT_UPDATE_CHECKING);
      return COBALT_UPDATE_VIEW_STAY;
   }
   /* B leaves at any point; a download in progress finishes in the background
    * and the result is there when the screen is opened again. */
   if (cobalt_input_pressed(in, COBALT_BTN_BACK)) {
      return COBALT_UPDATE_VIEW_BACK;
   }
   return COBALT_UPDATE_VIEW_STAY;
}

bool
cobalt_update_view_apply_on_quit(cobalt_update_view *v)
{
   if (v->thread) {
      SDL_WaitThread(v->thread, NULL);
      v->thread = NULL;
   }
   if (!v->have_paths || cobalt_update_view_state(v) != COBALT_UPDATE_READY ||
       !cobalt_update_has_staged(&v->paths)) {
      return false;
   }
   if (cobalt_update_apply(&v->paths)) {
      COBALT_LOGI("update: installed %s; the previous build is kept until it has run", v->manifest.version);
      return true;
   }
   COBALT_LOGW("update: could not put the update in place; the current build is untouched");
   return false;
}

void
cobalt_update_view_draw(cobalt_update_view *v, cobalt_render *r, int top)
{
   const cobalt_metrics *m = cobalt_render_metrics(r);
   char line[256];
   cobalt_update_state st;
   wf_update_manifest man;
   char msg[sizeof(v->message)];

   SDL_LockMutex(v->lock);
   st = v->state;
   man = v->manifest;
   snprintf(msg, sizeof(msg), "%s", v->message);
   SDL_UnlockMutex(v->lock);

   SDL_Rect panel = {m->pad_edge, top, m->width - 2 * m->pad_edge,
                     m->height - top - m->pad_edge - 40};
   cobalt_draw_tile(r, &panel, 0.0f);

   const int x = panel.x + m->pad_tile;
   const int w = panel.w - 2 * m->pad_tile;
   int y = panel.y + m->pad_tile;
   const int lh = cobalt_font_line_height(r, COBALT_FONT_BODY) + m->line_gap;

   snprintf(line, sizeof(line), "This version: %s", v->running);
   cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, line, x, y, w, 1, COBALT_COLOUR_TEXT);
   y += lh;

   switch (st) {
      case COBALT_UPDATE_IDLE:
      case COBALT_UPDATE_CHECKING:
         cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, "Checking GitHub for a newer release...",
                                  x, y, w, 2, COBALT_COLOUR_TEXT_DIM);
         cobalt_draw_hints(r, "B: back");
         break;
      case COBALT_UPDATE_CURRENT:
         cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, "Cobalt is up to date.", x, y, w, 2,
                                  COBALT_COLOUR_TEXT);
         cobalt_draw_hints(r, "A: check again  B: back");
         break;
      case COBALT_UPDATE_AVAILABLE:
         snprintf(line, sizeof(line), "Version %s is available (%lu KB).", man.version,
                  (man.asset.size + 1023) / 1024);
         cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, line, x, y, w, 2, COBALT_COLOUR_ACCENT_TEXT);
         y += lh * 2;
         if (man.notes[0]) {
            cobalt_draw_text_wrapped(r, COBALT_FONT_CAPTION, man.notes, x, y, w, 8,
                                     COBALT_COLOUR_TEXT);
            y += lh * 4;
         }
         cobalt_draw_text_wrapped(r, COBALT_FONT_CAPTION,
                                  "Nothing is downloaded until you choose. The file is checked "
                                  "against its published SHA-256 before it is used, and your "
                                  "current build is kept until the new one has started.",
                                  x, y, w, 5, COBALT_COLOUR_TEXT_DIM);
         cobalt_draw_hints(r, "A: download  B: back");
         break;
      case COBALT_UPDATE_DOWNLOADING:
         cobalt_draw_text_wrapped(r, COBALT_FONT_BODY,
                                  "Downloading and verifying. Please wait; do not power off.", x,
                                  y, w, 3, COBALT_COLOUR_TEXT);
         cobalt_draw_hints(r, "B: back (it keeps going)");
         break;
      case COBALT_UPDATE_READY:
         snprintf(line, sizeof(line),
                  "Version %s is downloaded and verified. It is installed when you quit Cobalt; "
                  "start it again from the Wii U Menu.",
                  man.version);
         cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, line, x, y, w, 5, COBALT_COLOUR_TEXT);
         cobalt_draw_hints(r, "B: back");
         break;
      case COBALT_UPDATE_FAILED:
         cobalt_draw_text_wrapped(r, COBALT_FONT_BODY, msg[0] ? msg : "The update failed.", x, y, w,
                                  4, COBALT_COLOUR_ERROR);
         cobalt_draw_hints(r, "A: try again  B: back");
         break;
   }
}
