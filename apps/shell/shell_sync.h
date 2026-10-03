#ifndef SHELL_SYNC_H
#define SHELL_SYNC_H

#include <stdbool.h>

#include "link_proto.h"

/* Filesystem operations the companion drives over the link. */
const struct link_fs *shell_sync_fs(void);

/* True once after the companion signals a sync finished; the shell reloads the library. */
bool shell_sync_take_complete(void);

/* Discards any partly written file, e.g. when the link drops mid-transfer. */
void shell_sync_abort(void);

#endif
