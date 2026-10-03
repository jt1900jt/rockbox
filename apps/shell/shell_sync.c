/* Filesystem back end for the link protocol: what the companion drives during a sync.
 *
 * Writes are staged to a temporary file and renamed on commit, so an interrupted sync
 * never leaves a half-written file under its real name. The shell reloads the library
 * when the companion signals the sync is complete.
 */
#include <stdio.h>
#include <string.h>

#include "dir.h"
#include "disk.h"
#include "file.h"
#include "pathfuncs.h"
#include "storage.h"
#ifndef SIMULATOR
#include "fat.h"
#endif
#include "string-extra.h"

#include "shell.h"
#include "shell_sync.h"

#define TMP_SUFFIX ".ipodos-tmp"

static int put_fd = -1;
static char put_target[LINK_PATH_MAX];
static char put_tmp[LINK_PATH_MAX + sizeof TMP_SUFFIX];
static bool sync_complete;

static int fs_stat(void *ctx, const char *path, int *kind, uint32_t *size, uint32_t *mtime)
{
    (void)ctx;
    *kind = 0;
    *size = 0;
    *mtime = 0;

    DIR *d = opendir(path);
    if (d) {
        closedir(d);
        *kind = 2;
        return 0;
    }
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        off_t n = ffilesize(fd);
        close(fd);
        *kind = 1;
        *size = n > 0 ? (uint32_t)n : 0;
        return 0;
    }
    return 0; /* missing is not an error: the companion asks about files that may not exist */
}

static int fs_list(void *ctx, const char *path,
                   void (*emit)(void *, const char *, int, uint32_t, uint32_t), void *emit_ctx)
{
    (void)ctx;
    DIR *d = opendir(path);
    if (!d)
        return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        struct dirinfo info = dir_get_info(d, e);
        int kind = (info.attribute & ATTR_DIRECTORY) ? 2 : 1;
        emit(emit_ctx, e->d_name, kind, (uint32_t)info.size, 0);
    }
    closedir(d);
    return 0;
}

static int fs_mkdir(void *ctx, const char *path)
{
    (void)ctx;
    if (mkdir(path) == 0)
        return 0;
    /* Already existing is success: the companion creates parents unconditionally. */
    DIR *d = opendir(path);
    if (d) {
        closedir(d);
        return 0;
    }
    return -1;
}

static int fs_remove(void *ctx, const char *path)
{
    (void)ctx;
    if (remove(path) == 0)
        return 0;
    return rmdir(path) == 0 ? 0 : -1;
}

/* Creates every missing parent of `path`. */
static void make_parents(const char *path)
{
    char buf[LINK_PATH_MAX];
    strlcpy(buf, path, sizeof buf);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        mkdir(buf);
        *p = '/';
    }
}

static int fs_put_begin(void *ctx, const char *path, uint32_t size)
{
    (void)ctx;
    (void)size;
    if (put_fd >= 0) {
        close(put_fd);
        put_fd = -1;
        remove(put_tmp);
    }
    strlcpy(put_target, path, sizeof put_target);
    snprintf(put_tmp, sizeof put_tmp, "%s%s", path, TMP_SUFFIX);
    make_parents(path);
    put_fd = open(put_tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    return put_fd >= 0 ? 0 : -1;
}

static int fs_put_data(void *ctx, const void *buf, size_t n)
{
    (void)ctx;
    if (put_fd < 0)
        return -1;
    return write(put_fd, buf, n) == (ssize_t)n ? 0 : -1;
}

static int fs_put_end(void *ctx, bool commit)
{
    (void)ctx;
    if (put_fd < 0)
        return -1;
    fsync(put_fd);
    close(put_fd);
    put_fd = -1;
    if (!commit) {
        remove(put_tmp);
        return 0;
    }
    remove(put_target); /* rename does not overwrite on FAT */
    return rename(put_tmp, put_target) == 0 ? 0 : -1;
}

static int fs_get(void *ctx, const char *path, int (*send)(void *, const void *, size_t), void *send_ctx)
{
    (void)ctx;
    static uint8_t buf[16384];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int total = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        if (send(send_ctx, buf, (size_t)n) < 0) {
            close(fd);
            return -1;
        }
        total += (int)n;
    }
    close(fd);
    return n < 0 ? -1 : total;
}

static int fs_freespace(void *ctx, uint64_t *freebytes, uint64_t *total)
{
    (void)ctx;
#ifdef SIMULATOR
    /* The simulator has no FAT volume; report a large figure so the space check in the
     * companion does not block testing. */
    *freebytes = 8ull * 1024 * 1024 * 1024;
    *total = 16ull * 1024 * 1024 * 1024;
    return 0;
#else
    sector_t f = 0, t = 0;
    if (!fat_size(IF_MV(0,) &t, &f))
        return -1;
    /* fat_size reports kibibytes. */
    *freebytes = (uint64_t)f * 1024;
    *total = (uint64_t)t * 1024;
    return 0;
#endif
}

static void fs_sync_done(void *ctx)
{
    (void)ctx;
    sync_complete = true;
}

static const struct link_fs shell_fs = {
    .stat = fs_stat,
    .list = fs_list,
    .mkdir = fs_mkdir,
    .remove = fs_remove,
    .put_begin = fs_put_begin,
    .put_data = fs_put_data,
    .put_end = fs_put_end,
    .get = fs_get,
    .freespace = fs_freespace,
    .sync_done = fs_sync_done,
    .ctx = NULL,
};

const struct link_fs *shell_sync_fs(void)
{
    return &shell_fs;
}

bool shell_sync_take_complete(void)
{
    bool c = sync_complete;
    sync_complete = false;
    return c;
}

void shell_sync_abort(void)
{
    if (put_fd >= 0) {
        close(put_fd);
        put_fd = -1;
        remove(put_tmp);
    }
}
