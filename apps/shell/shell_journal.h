/*
 * Play journal. Append-only record of what happened on the device, merged back by the
 * companion on the next sync (see docs/ipdb-format.md).
 *
 * The library itself is read-only, so counts, ratings and resume positions cannot be
 * written back into it. Each event is a fixed 16-byte record, appended and flushed, so a
 * power cut can lose at most the last record and leaves no partial state behind.
 */
#ifndef SHELL_JOURNAL_H
#define SHELL_JOURNAL_H

#include <stdbool.h>
#include <stdint.h>

enum journal_type {
    JOURNAL_PLAYED = 1,  /* track finished, or was played past the threshold */
    JOURNAL_SKIPPED = 2, /* track left early */
    JOURNAL_RATING = 3,  /* value = 0-5 */
    JOURNAL_POSITION = 4 /* value = elapsed ms, for audiobooks and podcasts */
};

void journal_init(void);

/* Records an event for a track uid. Silently does nothing when the journal cannot be
 * written: losing statistics must never interrupt playback. */
void journal_log(uint8_t type, uint32_t uid, uint32_t value);

/* Call on every track change and at shutdown. Decides played vs skipped from how much
 * of the track was heard, the way scrobbling does: half the track, or four minutes. */
void journal_track_finished(uint32_t uid, uint32_t elapsed_ms, uint32_t length_ms);

#endif
