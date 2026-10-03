/*
 * Album art cache. Reads pre-rendered RGB565 slots from /.ipodos/artwork.ipap.
 *
 * The UI thread never waits on storage: art_get() returns a cached image or NULL and
 * queues a load on a background thread. When the load finishes the screen is asked to
 * redraw, so a thumbnail appears a frame or two later instead of stalling the list.
 */
#ifndef SHELL_ART_H
#define SHELL_ART_H

#include <stdbool.h>
#include <stdint.h>

#include "ipdb.h" /* IPAP_* class ids */

/* Opened at startup; false when the pack is missing or its generation does not match
 * the library, in which case art_get() always returns NULL and the UI draws placeholders. */
bool art_init(uint64_t db_generation);
void art_shutdown(void);
bool art_available(void);

/* Returns w*h RGB565 pixels, or NULL if not resident yet. A NULL result queues a load
 * unless `prefetch_only` work is already saturating the queue. */
const uint16_t *art_get(uint32_t class_id, uint32_t art_id, int *w, int *h);

/* Queue a load without needing the result now (scroll-ahead). */
void art_prefetch(uint32_t class_id, uint32_t art_id);

/* Drop every queued request; call when the view changes so stale prefetches do not
 * crowd out what is now on screen. */
void art_cancel_pending(void);

/* True once since the last call if any load completed, meaning a redraw is due. */
bool art_take_dirty(void);

#endif
