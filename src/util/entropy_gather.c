#include "util/entropy_gather.h"

#include <string.h>

static int
cells_wide(void)
{
   return COBALT_GATHER_PANEL_W / COBALT_GATHER_CELL + 1;
}

void
cobalt_gather_init(cobalt_gather *g, const void *extra, size_t extra_len)
{
   memset(g, 0, sizeof(*g));
   wf_sha256_init(&g->hash);
   /* A domain tag, so this hash cannot be confused with any other use. */
   wf_sha256_update(&g->hash, "cobalt-seed-v1", 14);
   if (extra && extra_len) {
      wf_sha256_update(&g->hash, extra, extra_len);
   }
}

bool
cobalt_gather_add(cobalt_gather *g, int x, int y, uint32_t tick)
{
   if (x < 0 || y < 0 || x >= COBALT_GATHER_PANEL_W || y >= COBALT_GATHER_PANEL_H) {
      return false;
   }
   if (g->have_last) {
      const int dx = x > g->last_x ? x - g->last_x : g->last_x - x;
      const int dy = y > g->last_y ? y - g->last_y : g->last_y - y;
      if (dx < COBALT_GATHER_MIN_MOVE && dy < COBALT_GATHER_MIN_MOVE) {
         return false; /* resting finger: not a new sample */
      }
   }
   g->have_last = true;
   g->last_x = x;
   g->last_y = y;

   /* Everything is hashed, even past the target; only the count is capped. */
   unsigned char rec[12];
   rec[0] = (unsigned char) (x >> 8);
   rec[1] = (unsigned char) x;
   rec[2] = (unsigned char) (y >> 8);
   rec[3] = (unsigned char) y;
   rec[4] = (unsigned char) (tick >> 24);
   rec[5] = (unsigned char) (tick >> 16);
   rec[6] = (unsigned char) (tick >> 8);
   rec[7] = (unsigned char) tick;
   rec[8] = (unsigned char) (g->accepted >> 8);
   rec[9] = (unsigned char) g->accepted;
   rec[10] = 0;
   rec[11] = 0;
   wf_sha256_update(&g->hash, rec, sizeof(rec));

   const int cell = (y / COBALT_GATHER_CELL) * cells_wide() + (x / COBALT_GATHER_CELL);
   if (!g->seen[cell]) {
      g->seen[cell] = 1;
      g->cells++;
   }
   if (g->accepted < COBALT_GATHER_SAMPLES) {
      g->accepted++;
   }
   return true;
}

int
cobalt_gather_percent(const cobalt_gather *g)
{
   const int a = g->accepted * 100 / COBALT_GATHER_SAMPLES;
   const int c = (g->cells > COBALT_GATHER_CELLS ? COBALT_GATHER_CELLS : g->cells) * 100 /
                 COBALT_GATHER_CELLS;
   const int p = a < c ? a : c;
   return p > 100 ? 100 : p;
}

bool
cobalt_gather_done(const cobalt_gather *g)
{
   return g->accepted >= COBALT_GATHER_SAMPLES && g->cells >= COBALT_GATHER_CELLS;
}

bool
cobalt_gather_finish(cobalt_gather *g, unsigned char seed[COBALT_ENTROPY_SEED_SIZE])
{
   unsigned char digest[32];
   wf_sha256 a, b;
   const bool done = cobalt_gather_done(g);

   if (!done) {
      memset(seed, 0, COBALT_ENTROPY_SEED_SIZE);
      memset(g, 0, sizeof(*g));
      return false;
   }
   wf_sha256_final(&g->hash, digest);
   /* Two domain-separated halves make the 64 bytes the seed file wants. */
   wf_sha256_init(&a);
   wf_sha256_update(&a, digest, 32);
   wf_sha256_update(&a, "\x00", 1);
   wf_sha256_final(&a, seed);
   wf_sha256_init(&b);
   wf_sha256_update(&b, digest, 32);
   wf_sha256_update(&b, "\x01", 1);
   wf_sha256_final(&b, seed + 32);
   memset(digest, 0, sizeof(digest));
   memset(&a, 0, sizeof(a));
   memset(&b, 0, sizeof(b));
   memset(g, 0, sizeof(*g));
   return true;
}
