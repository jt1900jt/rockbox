#include "shell_art.h"

#include <stdio.h>
#include <string.h>

#include "core_alloc.h"
#include "file.h"
#include "kernel.h"
#include "system.h"
#include "thread.h"
#include "semaphore.h"

#include "shell.h"

#define ART_PATH SHELL_DIR "/artwork.ipap"

/* Cache sizing. Thumbnails dominate (a screenful is 6), so they get the most slots;
 * the large and blurred classes are one per screen. */
#define THUMB_SLOTS 24
#define HEAD_SLOTS  4
#define LARGE_SLOTS 3
#define BLUR_SLOTS  3

#define THUMB_PX (28 * 28)
#define HEAD_PX  (52 * 52)
#define LARGE_PX (116 * 116)
#define BLUR_PX  (80 * 60)

#define QUEUE_MAX 16

struct cache_class {
    uint32_t id;
    int slots;
    int pixels;
    uint16_t *store;   /* slots * pixels */
    uint32_t *art_ids; /* IPDB_NONE when free */
    uint32_t *stamp;   /* for least-recently-used eviction */
};

static ipap_pack pack;
static int pack_fd = -1;
static bool available;
static uint32_t clock_tick;

static uint16_t *cache_mem;
static int cache_handle = -1;
static uint32_t ids_thumb[THUMB_SLOTS], ids_head[HEAD_SLOTS], ids_large[LARGE_SLOTS], ids_blur[BLUR_SLOTS];
static uint32_t st_thumb[THUMB_SLOTS], st_head[HEAD_SLOTS], st_large[LARGE_SLOTS], st_blur[BLUR_SLOTS];

static struct cache_class classes[4] = {
    { IPAP_THMB, THUMB_SLOTS, THUMB_PX, NULL, ids_thumb, st_thumb },
    { IPAP_HEAD, HEAD_SLOTS, HEAD_PX, NULL, ids_head, st_head },
    { IPAP_LRGE, LARGE_SLOTS, LARGE_PX, NULL, ids_large, st_large },
    { IPAP_BLUR, BLUR_SLOTS, BLUR_PX, NULL, ids_blur, st_blur },
};
#define N_CLASSES ((int)(sizeof classes / sizeof classes[0]))

/* Request queue, written by the UI thread and drained by the loader. */
struct request {
    uint32_t class_id;
    uint32_t art_id;
};
static struct request queue[QUEUE_MAX];
static int q_head, q_tail; /* count = head - tail, both monotonic mod QUEUE_MAX */
static struct mutex art_mutex;
static struct semaphore art_wake;
static unsigned int loader_id;
static bool loader_stop;
static volatile bool dirty;

static long art_stack[(DEFAULT_STACK_SIZE + 0x400) / sizeof(long)];
static const char art_thread_name[] = "art";

static struct cache_class *find_class(uint32_t id)
{
    for (int i = 0; i < N_CLASSES; i++)
        if (classes[i].id == id)
            return &classes[i];
    return NULL;
}

/* Caller holds art_mutex. */
static int find_slot(struct cache_class *c, uint32_t art_id)
{
    for (int i = 0; i < c->slots; i++)
        if (c->art_ids[i] == art_id)
            return i;
    return -1;
}

/* Caller holds art_mutex. Returns the slot to overwrite: a free one, else the oldest. */
static int victim_slot(struct cache_class *c)
{
    int oldest = 0;
    for (int i = 0; i < c->slots; i++) {
        if (c->art_ids[i] == IPDB_NONE)
            return i;
        if (c->stamp[i] < c->stamp[oldest])
            oldest = i;
    }
    return oldest;
}

static bool load_slot(struct cache_class *c, uint32_t art_id)
{
    uint64_t offset;
    uint32_t size;
    uint16_t w, h;
    if (ipap_slot(&pack, c->id, art_id, &offset, &size, &w, &h) != 0)
        return false;
    if ((int)(w * h) != c->pixels)
        return false;

    mutex_lock(&art_mutex);
    if (find_slot(c, art_id) >= 0) { /* already loaded while we were queued */
        mutex_unlock(&art_mutex);
        return true;
    }
    int slot = victim_slot(c);
    c->art_ids[slot] = IPDB_NONE; /* mark in-flight so readers do not see partial data */
    uint16_t *dst = c->store + (size_t)slot * c->pixels;
    mutex_unlock(&art_mutex);

    if (lseek(pack_fd, (off_t)offset, SEEK_SET) < 0)
        return false;
    if (read(pack_fd, dst, size) != (ssize_t)size)
        return false;

    mutex_lock(&art_mutex);
    c->art_ids[slot] = art_id;
    c->stamp[slot] = ++clock_tick;
    mutex_unlock(&art_mutex);
    dirty = true;
    return true;
}

static void art_thread(void)
{
    while (1) {
        semaphore_wait(&art_wake, TIMEOUT_BLOCK);
        if (loader_stop)
            break;
        for (;;) {
            struct request r;
            mutex_lock(&art_mutex);
            if (q_tail == q_head) {
                mutex_unlock(&art_mutex);
                break;
            }
            r = queue[q_tail % QUEUE_MAX];
            q_tail++;
            mutex_unlock(&art_mutex);

            struct cache_class *c = find_class(r.class_id);
            if (c)
                load_slot(c, r.art_id);
        }
    }
}

static void enqueue(uint32_t class_id, uint32_t art_id)
{
    if (!available || art_id == IPDB_NONE)
        return;
    mutex_lock(&art_mutex);
    /* Drop duplicates already waiting. */
    for (int i = q_tail; i < q_head; i++) {
        struct request *r = &queue[i % QUEUE_MAX];
        if (r->class_id == class_id && r->art_id == art_id) {
            mutex_unlock(&art_mutex);
            return;
        }
    }
    if (q_head - q_tail >= QUEUE_MAX)
        q_tail++; /* oldest request is the least likely to still be on screen */
    queue[q_head % QUEUE_MAX] = (struct request){ class_id, art_id };
    q_head++;
    mutex_unlock(&art_mutex);
    semaphore_release(&art_wake);
}

bool art_init(uint64_t db_generation)
{
    uint8_t head[IPAP_HEAD_MAX];
    available = false;

    pack_fd = open(ART_PATH, O_RDONLY);
    if (pack_fd < 0)
        return false;
    off_t len = ffilesize(pack_fd);
    ssize_t got = read(pack_fd, head, sizeof head);
    if (got < 0 || ipap_parse(&pack, head, (size_t)got, (uint64_t)len) != IPDB_OK) {
        close(pack_fd);
        pack_fd = -1;
        return false;
    }
    if (pack.generation != db_generation) {
        /* Art belongs to a different library build; drawing it would pair the wrong
         * cover with the wrong album. */
        close(pack_fd);
        pack_fd = -1;
        return false;
    }

    size_t total = 0;
    for (int i = 0; i < N_CLASSES; i++)
        total += (size_t)classes[i].slots * classes[i].pixels * 2;
    cache_handle = core_alloc(total);
    if (cache_handle <= 0) {
        close(pack_fd);
        pack_fd = -1;
        return false;
    }
    core_pin(cache_handle);
    cache_mem = core_get_data(cache_handle);

    uint16_t *p = cache_mem;
    for (int i = 0; i < N_CLASSES; i++) {
        classes[i].store = p;
        p += (size_t)classes[i].slots * classes[i].pixels;
        for (int s = 0; s < classes[i].slots; s++) {
            classes[i].art_ids[s] = IPDB_NONE;
            classes[i].stamp[s] = 0;
        }
    }

    mutex_init(&art_mutex);
    semaphore_init(&art_wake, QUEUE_MAX, 0);
    loader_stop = false;
    q_head = q_tail = 0;
    loader_id = create_thread(art_thread, art_stack, sizeof art_stack, 0,
                              art_thread_name IF_PRIO(, PRIORITY_BACKGROUND) IF_COP(, CPU));
    if (loader_id == 0) {
        core_free(cache_handle);
        cache_handle = -1;
        close(pack_fd);
        pack_fd = -1;
        return false;
    }
    available = true;
    return true;
}

void art_shutdown(void)
{
    if (!available)
        return;
    loader_stop = true;
    semaphore_release(&art_wake);
    thread_wait(loader_id);
    if (cache_handle > 0)
        core_free(cache_handle);
    cache_handle = -1;
    if (pack_fd >= 0)
        close(pack_fd);
    pack_fd = -1;
    available = false;
}

bool art_available(void)
{
    return available;
}

const uint16_t *art_get(uint32_t class_id, uint32_t art_id, int *w, int *h)
{
    if (!available || art_id == IPDB_NONE)
        return NULL;
    struct cache_class *c = find_class(class_id);
    if (!c)
        return NULL;

    mutex_lock(&art_mutex);
    int slot = find_slot(c, art_id);
    const uint16_t *px = NULL;
    if (slot >= 0) {
        c->stamp[slot] = ++clock_tick;
        px = c->store + (size_t)slot * c->pixels;
    }
    mutex_unlock(&art_mutex);

    if (px) {
        uint64_t off;
        uint32_t size;
        uint16_t cw, ch;
        if (ipap_slot(&pack, class_id, art_id, &off, &size, &cw, &ch) == 0) {
            if (w) *w = cw;
            if (h) *h = ch;
        }
        return px;
    }
    enqueue(class_id, art_id);
    return NULL;
}

void art_prefetch(uint32_t class_id, uint32_t art_id)
{
    if (!available || art_id == IPDB_NONE)
        return;
    struct cache_class *c = find_class(class_id);
    if (!c)
        return;
    mutex_lock(&art_mutex);
    bool resident = find_slot(c, art_id) >= 0;
    mutex_unlock(&art_mutex);
    if (!resident)
        enqueue(class_id, art_id);
}

void art_cancel_pending(void)
{
    if (!available)
        return;
    mutex_lock(&art_mutex);
    q_tail = q_head;
    mutex_unlock(&art_mutex);
}

bool art_take_dirty(void)
{
    bool d = dirty;
    dirty = false;
    return d;
}
