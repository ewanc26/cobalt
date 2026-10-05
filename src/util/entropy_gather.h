#pragma once

/*
 * First-run seed collection from the person holding the GamePad.
 *
 * Why this exists. `make bundle` writes a per-installation entropy.bin, but a
 * .wuhb installed from a GitHub release or the Homebrew App Store has none, and
 * without one Cobalt refuses to use the network (AGENTS.md §13: the console's
 * only entropy source is the tick counter). A seed shipped inside the download
 * would be the same for everyone, which is worse than none. So when the seed is
 * missing, Cobalt asks the person to draw on the GamePad and builds one from
 * the touch positions and the timing between samples.
 *
 * What it is and is not. Human input is real entropy, but nobody has measured
 * how much a stroke across a 854x480 panel carries on this hardware, so this
 * does not claim a figure. It credits at most one sample per 4 pixels of
 * movement, demands many samples spread over many parts of the panel (so
 * scribbling in one spot does not count), and hashes everything, with device
 * specific bytes mixed in but not credited. Treat it as better than the tick
 * counter alone, and prefer `make bundle` when you can.
 *
 * Pure: no SDL, no SD card, no clock. The caller supplies positions and a
 * high-resolution tick, which is what makes it testable on the host.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "update/selfupdate.h"
#include "util/entropy.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Accepted samples needed, and distinct 32x32 panel cells they must touch. */
#define COBALT_GATHER_SAMPLES 400
#define COBALT_GATHER_CELLS 48
#define COBALT_GATHER_MIN_MOVE 4
#define COBALT_GATHER_PANEL_W 854
#define COBALT_GATHER_PANEL_H 480
#define COBALT_GATHER_CELL 32

typedef struct {
   cobalt_sha256 hash;
   int accepted;
   int cells;
   bool have_last;
   int last_x;
   int last_y;
   uint8_t seen[((COBALT_GATHER_PANEL_W / COBALT_GATHER_CELL) + 1) *
                ((COBALT_GATHER_PANEL_H / COBALT_GATHER_CELL) + 1)];
} cobalt_gather;

/* `extra` is mixed in but earns no credit: console tick, time, anything unique. */
void cobalt_gather_init(cobalt_gather *g, const void *extra, size_t extra_len);

/*
 * Offer one touch sample (a frame where a finger is down). `tick` is a
 * high-resolution counter. Returns true if the sample counted.
 */
bool cobalt_gather_add(cobalt_gather *g, int x, int y, uint32_t tick);

/* 0..100. Whichever of the two requirements is further from done. */
int cobalt_gather_percent(const cobalt_gather *g);

bool cobalt_gather_done(const cobalt_gather *g);

/* Only succeeds once done. Wipes the collector either way. */
bool cobalt_gather_finish(cobalt_gather *g, unsigned char seed[COBALT_ENTROPY_SEED_SIZE]);

#ifdef __cplusplus
}
#endif
