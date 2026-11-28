#include "atproto/labels.h"
#include "atproto/session.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *
cobalt_labels_reason(const char *value)
{
   static const struct {
      const char *value;
      const char *reason;
   } names[] = {
      { "!hide", "Hidden by moderators" },
      { "!warn", "Content warning" },
      { "!no-unauthenticated", "Signed-in users only" },
      { "porn", "Adult content" },
      { "sexual", "Sexually suggestive" },
      { "nudity", "Non-sexual nudity" },
      { "graphic-media", "Graphic media" },
      { "gore", "Graphic media" },
   };
   if (!value || !value[0]) {
      return "Labelled content";
   }
   for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
      if (strcmp(names[i].value, value) == 0) {
         return names[i].reason;
      }
   }
   return value;
}

bool
cobalt_post_mod_covers(const cobalt_post_mod *mod)
{
   return mod && mod->action != COBALT_MOD_NONE;
}

#ifdef COBALT_HAS_WOLFRAM

#include <cJSON.h>
#include <wolfram/actor_prefs_typed.h>
#include <wolfram/agent.h>
#include <wolfram/labeler_typed.h>
#include <wolfram/moderation.h>
#include <wolfram/xrpc.h>

/*
 * The active context. Only the session worker builds it and only the worker
 * flattens posts, so it needs no lock of its own; the UI reads the flattened
 * cobalt_post_mod and never this.
 */
typedef struct {
   bool ready;
   wf_mod_label_def *defs;
   size_t def_count;
   /* A labeler's own name for each def, parallel to defs; NULL for the
    * global ones, which cobalt_labels_reason names. */
   char **def_names;
   wf_mod_opts opts;   /* opts.prefs arrays below are owned here */
} labels_ctx;

/*
 * The label context is read by the UI (under the session lock, as app.c holds
 * it while it draws) and rebuilt by the worker. Every entry point that touches
 * it takes the same lock for its whole body; the mutex is reentrant, so one
 * entry point may call another.
 */
static int
labels_guard_acquire(void)
{
   cobalt_session_lock();
   return 0;
}

static void
labels_guard_release(int *guard)
{
   (void) guard;
   cobalt_session_unlock();
}

#define LABELS_LOCKED \
   int labels_guard_ __attribute__((cleanup(labels_guard_release))) = labels_guard_acquire()

static labels_ctx g_ctx;
static char g_viewer[256];

void
cobalt_labels_set_viewer(const char *did)
{
   LABELS_LOCKED;
   if (strcmp(g_viewer, did ? did : "") != 0) {
      snprintf(g_viewer, sizeof g_viewer, "%s", did ? did : "");
   }
   g_ctx.opts.user_did = g_viewer[0] ? g_viewer : NULL;
}

static char *
dup_str(const char *s)
{
   if (!s) {
      return NULL;
   }
   const size_t n = strlen(s) + 1;
   char *out = malloc(n);
   if (out) {
      memcpy(out, s, n);
   }
   return out;
}

static void
ctx_free(labels_ctx *c)
{
   wf_mod_prefs *p = &c->opts.prefs;
   for (size_t i = 0; i < p->labeler_count; i++) {
      wf_mod_labeler_pref *lp = &p->labelers[i];
      free(lp->did);
      for (size_t j = 0; j < lp->label_count; j++) {
         free(lp->label_identifiers[j]);
      }
      free(lp->label_identifiers);
      free(lp->label_prefs);
   }
   free(p->labelers);
   for (size_t i = 0; i < p->global_label_count; i++) {
      free(p->global_label_identifiers[i]);
   }
   free(p->global_label_identifiers);
   free(p->global_label_prefs);
   if (c->def_names) {
      for (size_t i = 0; i < c->def_count; i++) {
         free(c->def_names[i]);
      }
      free(c->def_names);
   }
   wf_mod_label_defs_free(c->defs, c->def_count);
   memset(c, 0, sizeof(*c));
}

static wf_mod_label_pref
pref_from_visibility(const char *v, bool *known)
{
   *known = true;
   if (v && strcmp(v, "hide") == 0) {
      return WF_MOD_PREF_HIDE;
   }
   if (v && strcmp(v, "warn") == 0) {
      return WF_MOD_PREF_WARN;
   }
   if (v && (strcmp(v, "ignore") == 0 || strcmp(v, "show") == 0)) {
      return WF_MOD_PREF_IGNORE;
   }
   *known = false;
   return WF_MOD_PREF_WARN;
}

static wf_mod_labeler_pref *
find_labeler(wf_mod_prefs *p, const char *did)
{
   for (size_t i = 0; i < p->labeler_count; i++) {
      if (strcmp(p->labelers[i].did, did) == 0) {
         return &p->labelers[i];
      }
   }
   return NULL;
}

static bool
add_labeler(wf_mod_prefs *p, const char *did)
{
   if (!did || !did[0] || find_labeler(p, did)) {
      return true;
   }
   wf_mod_labeler_pref *grown =
      realloc(p->labelers, (p->labeler_count + 1) * sizeof(*grown));
   if (!grown) {
      return false;
   }
   p->labelers = grown;
   wf_mod_labeler_pref *lp = &p->labelers[p->labeler_count];
   memset(lp, 0, sizeof(*lp));
   lp->did = dup_str(did);
   if (!lp->did) {
      return false;
   }
   p->labeler_count++;
   return true;
}

/* Append one (identifier, pref) pair to parallel arrays. */
static bool
push_pref(char ***ids, wf_mod_label_pref **prefs, size_t *count, const char *id,
          wf_mod_label_pref pref)
{
   char **ni = realloc(*ids, (*count + 1) * sizeof(*ni));
   if (!ni) {
      return false;
   }
   *ids = ni;
   wf_mod_label_pref *np = realloc(*prefs, (*count + 1) * sizeof(*np));
   if (!np) {
      return false;
   }
   *prefs = np;
   ni[*count] = dup_str(id);
   if (!ni[*count]) {
      return false;
   }
   np[*count] = pref;
   (*count)++;
   return true;
}

static bool
ctx_build(labels_ctx *c, const wf_actor_preferences *src)
{
   memset(c, 0, sizeof(*c));
   c->opts.user_did = g_viewer[0] ? g_viewer : NULL;
   if (wf_mod_global_label_defs(&c->defs, &c->def_count) != WF_OK) {
      return false;
   }
   c->def_names = calloc(c->def_count, sizeof(*c->def_names));
   if (!c->def_names) {
      return false;
   }
   /* The engine reads definitions through opts; set them now so a lookup made
    * before any post is decided (the settings screen) sees the globals too. */
   c->opts.label_defs = c->defs;
   c->opts.label_def_count = c->def_count;
   wf_mod_prefs *p = &c->opts.prefs;

   /* Official clients always count Bluesky's own labeler. */
   if (!add_labeler(p, WF_MOD_BSKY_LABELER_DID)) {
      return false;
   }
   if (!src) {
      c->ready = true;
      return true;
   }

   /* Absent means off: the safe reading of a preference nobody set. */
   p->adult_content_enabled =
      src->adult_content.has_enabled && src->adult_content.enabled;

   for (size_t i = 0; i < src->labelers.labeler_count; i++) {
      if (!add_labeler(p, src->labelers.labelers[i])) {
         return false;
      }
   }

   for (size_t i = 0; i < src->content_label_count; i++) {
      const wf_actor_pref_content_label *cl = &src->content_labels[i];
      bool known = false;
      const wf_mod_label_pref pref = pref_from_visibility(cl->visibility, &known);
      if (!cl->label || !cl->label[0] || !known) {
         continue;
      }
      bool ok;
      if (cl->labeler_did && cl->labeler_did[0]) {
         if (!add_labeler(p, cl->labeler_did)) {
            return false;
         }
         wf_mod_labeler_pref *lp = find_labeler(p, cl->labeler_did);
         ok = push_pref(&lp->label_identifiers, &lp->label_prefs, &lp->label_count,
                        cl->label, pref);
      } else {
         ok = push_pref(&p->global_label_identifiers, &p->global_label_prefs,
                        &p->global_label_count, cl->label, pref);
      }
      if (!ok) {
         return false;
      }
   }
   c->ready = true;
   return true;
}

void
cobalt_labels_reset(void)
{
   LABELS_LOCKED;
   ctx_free(&g_ctx);
   if (!ctx_build(&g_ctx, NULL)) {
      ctx_free(&g_ctx);
      COBALT_LOGW("labels: out of memory building the default context");
   }
}

bool
cobalt_labels_from_prefs(const wf_actor_preferences *prefs)
{
   LABELS_LOCKED;
   labels_ctx next;
   if (!ctx_build(&next, prefs)) {
      ctx_free(&next);
      COBALT_LOGW("labels: out of memory; keeping the previous preferences");
      return false;
   }
   ctx_free(&g_ctx);
   g_ctx = next;
   COBALT_LOGI("labels: adult content %s, %u labeler(s), %u global pref(s)",
               g_ctx.opts.prefs.adult_content_enabled ? "on" : "off",
               (unsigned) g_ctx.opts.prefs.labeler_count,
               (unsigned) g_ctx.opts.prefs.global_label_count);
   return true;
}

int
cobalt_labels_accept_value(char *out, size_t out_size)
{
   LABELS_LOCKED;
   if (!out || out_size == 0) {
      return 0;
   }
   out[0] = '\0';
   if (!g_ctx.ready) {
      cobalt_labels_reset();
   }
   size_t used = 0;
   int written = 0;
   for (size_t i = 0; i < g_ctx.opts.prefs.labeler_count &&
                      written < COBALT_LABELS_ACCEPT_MAX; i++) {
      const char *did = g_ctx.opts.prefs.labelers[i].did;
      const bool bsky = strcmp(did, WF_MOD_BSKY_LABELER_DID) == 0;
      const int n = snprintf(out + used, out_size - used, "%s%s%s",
                             used ? ", " : "", did, bsky ? ";redact" : "");
      if (n < 0 || (size_t) n >= out_size - used) {
         out[used] = '\0';   /* drop the one that did not fit, keep the rest */
         break;
      }
      used += (size_t) n;
      written++;
   }
   return written;
}

bool
cobalt_labels_send_accept(struct wf_agent *agent)
{
   if (!agent) {
      return false;
   }
   char value[2048];
   cobalt_labels_accept_value(value, sizeof value);
   const wf_status st =
      wf_xrpc_client_set_accept_labelers(wf_agent_get_xrpc_client(agent), value);
   if (st != WF_OK) {
      COBALT_LOGW("labels: accept-labelers refused (%d)", (int) st);
      return false;
   }
   return true;
}

int
cobalt_labels_labelers(const char **out, int max)
{
   LABELS_LOCKED;
   if (!out || max <= 0) {
      return 0;
   }
   if (!g_ctx.ready) {
      cobalt_labels_reset();
   }
   int n = 0;
   for (size_t i = 0; i < g_ctx.opts.prefs.labeler_count && n < max; i++) {
      out[n++] = g_ctx.opts.prefs.labelers[i].did;
   }
   return n;
}

/* The English name a labeler gave a value, else its first one. */
static const char *
def_name(const wf_labeler_label_value_def *d)
{
   const char *first = NULL;
   for (size_t i = 0; i < d->locale_count; i++) {
      const wf_labeler_label_value_def_locale *l = &d->locales[i];
      if (!l->name || !l->name[0]) {
         continue;
      }
      if (l->lang && strncmp(l->lang, "en", 2) == 0) {
         return l->name;
      }
      if (!first) {
         first = l->name;
      }
   }
   return first;
}

static bool
have_def(const char *identifier, const char *by)
{
   for (size_t i = 0; i < g_ctx.def_count; i++) {
      const wf_mod_label_def *d = &g_ctx.defs[i];
      if (d->defined_by && strcmp(d->defined_by, by) == 0 &&
          strcmp(d->identifier, identifier) == 0) {
         return true;
      }
   }
   return false;
}

int
cobalt_labels_add_services(const wf_labeler_service_list *list)
{
   LABELS_LOCKED;
   if (!list || !g_ctx.ready) {
      return 0;
   }
   int added = 0;
   for (size_t s = 0; s < list->service_count; s++) {
      const wf_labeler_service_view *v = &list->services[s];
      const char *by = v->creator.did;
      if (!by || !by[0] || !v->has_policies) {
         continue;
      }
      for (size_t i = 0; i < v->policies.label_value_definition_count; i++) {
         const wf_labeler_label_value_def *d = &v->policies.label_value_definitions[i];
         if (!d->identifier || !d->identifier[0] || have_def(d->identifier, by)) {
            continue;
         }
         if (g_ctx.def_count >= COBALT_LABELS_DEFS_MAX) {
            COBALT_LOGW("labels: definition limit reached; the rest stay unsupported");
            return added;
         }
         wf_mod_label_def *grown =
            realloc(g_ctx.defs, (g_ctx.def_count + 1) * sizeof(*grown));
         if (!grown) {
            return added;
         }
         g_ctx.defs = grown;
         char **names =
            realloc(g_ctx.def_names, (g_ctx.def_count + 1) * sizeof(*names));
         if (!names) {
            return added;
         }
         g_ctx.def_names = names;
         wf_mod_label_def *def = &g_ctx.defs[g_ctx.def_count];
         if (wf_mod_interpret_label_def(def, d->identifier, by, d->blurs, d->severity,
                                        d->has_adult_only && d->adult_only,
                                        d->has_default_setting ? d->default_setting
                                                               : NULL) != WF_OK) {
            wf_mod_label_def_free(def);
            continue;
         }
         names[g_ctx.def_count] = dup_str(def_name(d));
         g_ctx.def_count++;
         added++;
      }
   }
   g_ctx.opts.label_defs = g_ctx.defs;
   g_ctx.opts.label_def_count = g_ctx.def_count;
   COBALT_LOGI("labels: %d labeler definition(s) loaded", added);
   return added;
}

const char *
cobalt_labels_visibility(const char *label)
{
   LABELS_LOCKED;
   if (!label) {
      return "warn";
   }
   if (!g_ctx.ready) {
      cobalt_labels_reset();
   }
   const wf_mod_label_def *def = wf_mod_find_label_def(&g_ctx.opts, label, NULL);
   if (!def) {
      return "warn";
   }
   switch (wf_mod_get_label_pref(&g_ctx.opts, def, NULL)) {
      case WF_MOD_PREF_HIDE: return "hide";
      case WF_MOD_PREF_IGNORE: return "ignore";
      default: return "warn";
   }
}

bool
cobalt_labels_adult_enabled(void)
{
   LABELS_LOCKED;
   return g_ctx.ready && g_ctx.opts.prefs.adult_content_enabled;
}

/* The name a cause is shown by: the labeler's own English name where it gave
 * one, else Cobalt's name for the value. */
static void
cause_name(const wf_mod_cause *cause, char *out, size_t out_size)
{
   const char *value = NULL;

   if (cause->type == WF_MOD_CAUSE_LABEL && cause->label_def && g_ctx.def_names &&
       cause->label_def >= g_ctx.defs && cause->label_def < g_ctx.defs + g_ctx.def_count) {
      const char *name = g_ctx.def_names[cause->label_def - g_ctx.defs];
      if (name && name[0]) {
         cobalt_feed_copy_text(out, out_size, name);
         return;
      }
   }
   if (cause->type == WF_MOD_CAUSE_LABEL) {
      value = cause->label_def && cause->label_def->identifier
                 ? cause->label_def->identifier
                 : cause->label.val;
   }
   snprintf(out, out_size, "%s", cobalt_labels_reason(value));
}

static void
set_reason(cobalt_post_mod *out, const wf_mod_cause *cause)
{
   if (out->reason[0] || !cause) {
      return;
   }
   cause_name(cause, out->reason, sizeof(out->reason));
}

bool
cobalt_labels_avatar_covered(const char *did, const cJSON *labels, char *reason,
                             size_t reason_size)
{
   LABELS_LOCKED;
   bool covered = false;

   if (reason && reason_size) {
      reason[0] = '\0';
   }
   if (!did || !did[0] || !labels) {
      return false;
   }
   if (!g_ctx.ready) {
      cobalt_labels_reset();
   }

   wf_mod_label *pl = NULL;
   size_t pn = 0;
   if (wf_mod_labels_from_cjson(&pl, &pn, labels) != WF_OK) {
      return false;
   }

   g_ctx.opts.label_defs = g_ctx.defs;
   g_ctx.opts.label_def_count = g_ctx.def_count;

   wf_mod_subject_profile subject;
   memset(&subject, 0, sizeof(subject));
   subject.did = did;
   subject.labels = pl;
   subject.label_count = pn;

   /* A profile page's avatar answers to two sets of labels, as upstream merges
    * them: the account's own (uri = the DID) and those on its profile record
    * (uri ends in /app.bsky.actor.profile/self). Wolfram splits them, so both
    * decisions are made and merged. */
   wf_mod_decision da;
   wf_mod_decision dp;
   wf_mod_decision d;
   memset(&da, 0, sizeof(da));
   memset(&dp, 0, sizeof(dp));
   memset(&d, 0, sizeof(d));
   if (wf_mod_decide_account(&da, &subject, &g_ctx.opts) == WF_OK &&
       wf_mod_decide_profile(&dp, &subject, &g_ctx.opts) == WF_OK &&
       wf_mod_decision_merge(&d, &da, &dp) == WF_OK) {
      wf_mod_ui ui;
      if (wf_mod_decision_ui(&d, WF_MOD_CTX_AVATAR, &ui) == WF_OK) {
         const wf_mod_cause *cause = NULL;

         if (ui.filter_count > 0) {
            cause = &ui.filters[0];
         } else if (ui.blur_count > 0) {
            cause = &ui.blurs[0];
         }
         if (cause) {
            covered = true;
            if (reason && reason_size) {
               cause_name(cause, reason, reason_size);
            }
         }
         wf_mod_ui_free(&ui);
      }
   }
   wf_mod_decision_free(&da);
   wf_mod_decision_free(&dp);
   wf_mod_decision_free(&d);
   wf_mod_labels_free(pl, pn);
   return covered;
}

/* The action `d` asks for: a list's decision first (filter or cover the
 * whole post), then the media one. A thread uses the same answer, so a post
 * covered in the feed is not uncovered by opening it. */
static cobalt_mod_action
action_for(const wf_mod_decision *d, wf_mod_context ctx, cobalt_post_mod *out)
{
   cobalt_mod_action action = COBALT_MOD_NONE;
   wf_mod_ui ui;
   if (wf_mod_decision_ui(d, ctx, &ui) != WF_OK) {
      return action;
   }
   if (ui.filter_count > 0) {
      action = COBALT_MOD_FILTER;
      set_reason(out, &ui.filters[0]);
   } else if (ui.blur_count > 0) {
      action = COBALT_MOD_BLUR_CONTENT;
      set_reason(out, &ui.blurs[0]);
   }
   if (action != COBALT_MOD_NONE && ui.no_override) {
      out->no_override = true;
   }
   wf_mod_ui_free(&ui);

   if (action == COBALT_MOD_NONE &&
       wf_mod_decision_ui(d, WF_MOD_CTX_CONTENT_MEDIA, &ui) == WF_OK) {
      if (ui.blur_count > 0) {
         action = COBALT_MOD_BLUR_MEDIA;
         set_reason(out, &ui.blurs[0]);
         if (ui.no_override) {
            out->no_override = true;
         }
      }
      wf_mod_ui_free(&ui);
   }
   return action;
}

static bool
source_counts(const char *src, const char *author_did)
{
   if (!src) {
      return false;
   }
   if (author_did && strcmp(src, author_did) == 0) {
      return true;
   }
   for (size_t i = 0; i < g_ctx.opts.prefs.labeler_count; i++) {
      if (strcmp(g_ctx.opts.prefs.labelers[i].did, src) == 0) {
         return true;
      }
   }
   return false;
}

static void
note_unsupported(cobalt_post_mod *out, const wf_mod_label *labels, size_t n,
                 const char *author_did)
{
   for (size_t i = 0; i < n; i++) {
      const wf_mod_label *l = &labels[i];
      if (l->neg || !l->val || !l->val[0] || !source_counts(l->src, author_did) ||
          wf_mod_find_label_def(&g_ctx.opts, l->val, l->src)) {
         continue;
      }
      if (strstr(out->unsupported, l->val)) {
         continue;
      }
      const size_t used = strlen(out->unsupported);
      const size_t need = strlen(l->val) + (used ? 2 : 0);
      if (used + need >= sizeof(out->unsupported)) {
         break;   /* a bounded line; the first few name the problem */
      }
      snprintf(out->unsupported + used, sizeof(out->unsupported) - used, "%s%s",
               used ? ", " : "", l->val);
   }
}

void
cobalt_labels_apply(cobalt_post_mod *out, const char *uri, const char *author_did,
                    const cJSON *labels, const cJSON *author_labels)
{
   LABELS_LOCKED;
   if (!out) {
      return;
   }
   memset(out, 0, sizeof(*out));
   if (!labels && !author_labels) {
      return;
   }
   if (!g_ctx.ready) {
      cobalt_labels_reset();
      if (!g_ctx.ready) {
         return;
      }
   }
   cobalt_labels_set_viewer(cobalt_session_did());

   wf_mod_label *pl = NULL;
   wf_mod_label *al = NULL;
   size_t pn = 0;
   size_t an = 0;
   if (labels && wf_mod_labels_from_cjson(&pl, &pn, labels) != WF_OK) {
      pl = NULL;
      pn = 0;
   }
   if (author_labels && wf_mod_labels_from_cjson(&al, &an, author_labels) != WF_OK) {
      al = NULL;
      an = 0;
   }

   g_ctx.opts.label_defs = g_ctx.defs;
   g_ctx.opts.label_def_count = g_ctx.def_count;

   wf_mod_subject_post subject;
   memset(&subject, 0, sizeof(subject));
   subject.uri = uri;
   subject.author.did = author_did;
   subject.author.labels = al;
   subject.author.label_count = an;
   subject.labels = pl;
   subject.label_count = pn;

   wf_mod_decision d;
   if (wf_mod_decide_post(&d, &subject, &g_ctx.opts) == WF_OK) {
      out->action = (unsigned char) action_for(&d, WF_MOD_CTX_CONTENT_LIST, out);
      wf_mod_decision_free(&d);
   }

   note_unsupported(out, pl, pn, author_did);
   note_unsupported(out, al, an, author_did);
   wf_mod_labels_free(pl, pn);
   wf_mod_labels_free(al, an);
}

#endif

#ifndef COBALT_HAS_WOLFRAM
bool
cobalt_labels_adult_enabled(void)
{
   return false;
}

const char *
cobalt_labels_visibility(const char *label)
{
   (void) label;
   return "warn";
}
#endif
