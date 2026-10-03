#include "shell_journal.h"

#include <string.h>

#include "file.h"
#include "timefuncs.h"
#include <time.h>

#include "shell.h"

#define JOURNAL_PATH SHELL_DIR "/journal.bin"
#define RECORD_SIZE 16
/* Guard against a runaway file if the companion never drains it. At 16 bytes a record
 * this is ~65000 events, far more than a sync interval produces. */
#define JOURNAL_MAX (1024u * 1024u)

static bool journal_full;

void journal_init(void)
{
    journal_full = false;
}

static uint32_t now_unix(void)
{
    struct tm *t = get_time();
    if (!t)
        return 0;
    return (uint32_t)mktime(t);
}

void journal_log(uint8_t type, uint32_t uid, uint32_t value)
{
    uint8_t rec[RECORD_SIZE];

    if (journal_full || uid == IPDB_NONE)
        return;

    /* Seek to the end explicitly rather than relying on O_APPEND, which the
     * simulator's file layer does not honour. */
    int fd = open(JOURNAL_PATH, O_WRONLY | O_CREAT, 0666);
    if (fd < 0)
        return;
    off_t size = ffilesize(fd);
    if (size >= (off_t)JOURNAL_MAX) {
        journal_full = true;
        close(fd);
        return;
    }
    lseek(fd, size, SEEK_SET);

    memset(rec, 0, sizeof rec);
    rec[0] = type;
    uint32_t t = now_unix();
    memcpy(rec + 4, &uid, 4);
    memcpy(rec + 8, &t, 4);
    memcpy(rec + 12, &value, 4);
    write(fd, rec, sizeof rec);
    fsync(fd);
    close(fd);
}

void journal_track_finished(uint32_t uid, uint32_t elapsed_ms, uint32_t length_ms)
{
    if (uid == IPDB_NONE || length_ms == 0)
        return;
    /* The usual scrobbling rule: counted as played at half the track or four minutes,
     * whichever comes first. Anything less is a skip. */
    uint32_t threshold = length_ms / 2;
    if (threshold > 4u * 60u * 1000u)
        threshold = 4u * 60u * 1000u;
    journal_log(elapsed_ms >= threshold ? JOURNAL_PLAYED : JOURNAL_SKIPPED, uid, elapsed_ms);
}
