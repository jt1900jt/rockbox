/* Phase 2 hardware measurements: LCD push, storage throughput, DB load/validate, list frame
 * time, boot timing. Results go to the screen and to /.ipodos/bench.log.
 * Shown on the home menu only when /.ipodos/bench/scale.ipdb exists. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_alloc.h"
#include "dir.h"
#include "file.h"
#include "font.h"
#include "kernel.h"
#include "lcd.h"
#include "system.h"
#include "button.h"

#include "shell.h"
#include "shell_gfx.h"

#define BENCH_DIR   SHELL_DIR "/bench"
#define BENCH_DB    BENCH_DIR "/scale.ipdb"
#define BENCH_TMP   BENCH_DIR "/tmp.bin"
#define BENCH_LOG   SHELL_DIR "/bench.log"
#define TMP_BYTES   (16u * 1024 * 1024)
#define CHUNK       (64u * 1024)
#define BIG_CHUNK   (512u * 1024)

long shell_boot_tick;

static int log_fd = -1;
static int line_y;

static unsigned long now_us(void)
{
#ifdef USEC_TIMER
    return USEC_TIMER;
#else
    return (unsigned long)current_tick * (1000000 / HZ);
#endif
}

static void out(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (log_fd >= 0) {
        write(log_fd, buf, strlen(buf));
        write(log_fd, "\n", 1);
    }
    lcd_setfont(FONT_SYSFIXED);
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(LCD_RGBPACK(244, 244, 242));
    lcd_set_background(LCD_RGBPACK(11, 11, 12));
    if (line_y > LCD_HEIGHT - 10) {
        lcd_clear_display();
        line_y = 0;
    }
    lcd_putsxy(2, line_y, (const unsigned char *)buf);
    lcd_update_rect(0, line_y, LCD_WIDTH, 9);
    line_y += 9;
}

static unsigned long kbps(unsigned long bytes, unsigned long us)
{
    return us ? (unsigned long)((unsigned long long)bytes * 1000000 / 1024 / us) : 0;
}

static void bench_lcd(const char *tag)
{
    const int full_n = 60, row_n = 200;
    lcd_set_drawmode(DRMODE_SOLID);
    unsigned long t0 = now_us();
    for (int i = 0; i < full_n; i++) {
        lcd_set_foreground(i & 1 ? LCD_RGBPACK(40, 40, 44) : LCD_RGBPACK(60, 60, 64));
        lcd_fillrect(0, 0, LCD_WIDTH, LCD_HEIGHT);
        lcd_update();
    }
    unsigned long full = (now_us() - t0) / full_n;
    t0 = now_us();
    for (int i = 0; i < row_n; i++)
        lcd_update_rect(0, 22 + (i % 6) * 36, LCD_WIDTH, 36);
    unsigned long row = (now_us() - t0) / row_n;
    lcd_clear_display();
    line_y = 0;
    out("lcd %s: full %lu us (%lu fps), row 320x36 %lu us", tag, full, full ? 1000000 / full : 0, row);
}

static void bench_list_frame(const char *tag)
{
    struct view v = { .kind = V_SONGS };
    const int n = 40;
    if (shell_db.n_tracks == 0)
        return;

    /* full repaint: what a view change costs */
    unsigned long t0 = now_us();
    for (int i = 0; i < n; i++) {
        v.sel = i % draw_rows_visible();
        draw_list(&v, true);
        gfx_flush();
    }
    unsigned long full = (now_us() - t0) / n;

    /* selection move: the common case, two rows instead of the screen */
    draw_list(&v, true);
    gfx_flush();
    t0 = now_us();
    for (int i = 0; i < n; i++) {
        int old = v.sel;
        v.sel = (v.sel + 1) % draw_rows_visible();
        draw_list_rows(&v, old, v.sel);
        gfx_flush();
    }
    unsigned long partial = (now_us() - t0) / n;

    lcd_clear_display();
    line_y = 0;
    out("list %s: full %lu us, move selection %lu us", tag, full, partial);
}

static void bench_storage(void *buf)
{
    int fd = open(BENCH_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        out("storage: cannot create " BENCH_TMP);
        return;
    }
    memset(buf, 0x5A, BIG_CHUNK);
    unsigned long t0 = now_us();
    for (unsigned long done = 0; done < TMP_BYTES; done += CHUNK)
        write(fd, buf, CHUNK);
    close(fd);
    unsigned long wus = now_us() - t0;
    out("write 16MB/64KB: %lu KB/s", kbps(TMP_BYTES, wus));

    for (int pass = 0; pass < 2; pass++) {
        unsigned chunk = pass ? BIG_CHUNK : CHUNK;
        fd = open(BENCH_TMP, O_RDONLY);
        t0 = now_us();
        unsigned long total = 0;
        ssize_t got;
        while ((got = read(fd, buf, chunk)) > 0)
            total += (unsigned long)got;
        unsigned long rus = now_us() - t0;
        close(fd);
        out("read seq %uKB chunks: %lu KB/s", chunk / 1024, kbps(total, rus));
    }

    fd = open(BENCH_TMP, O_RDONLY);
    uint32_t seed = 12345;
    const int n = 400;
    t0 = now_us();
    for (int i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        off_t off = (off_t)((seed >> 8) % (TMP_BYTES / 4096)) * 4096;
        lseek(fd, off, SEEK_SET);
        read(fd, buf, 4096);
    }
    unsigned long rand_us = (now_us() - t0) / n;
    close(fd);
    out("read random 4KB: %lu us avg", rand_us);
    remove(BENCH_TMP);
}

static void bench_db(const char *tag)
{
    int fd = open(BENCH_DB, O_RDONLY);
    if (fd < 0) {
        out("db: " BENCH_DB " missing");
        return;
    }
    off_t size = ffilesize(fd);
    int h = core_alloc((size_t)size);
    if (h <= 0) {
        close(fd);
        out("db: cannot allocate %ld bytes", (long)size);
        return;
    }
    core_pin(h);
    void *buf = core_get_data(h);
    unsigned long t0 = now_us();
    ssize_t got = read(fd, buf, (size_t)size);
    unsigned long t_read = now_us() - t0;
    close(fd);
    if (got == size) {
        t0 = now_us();
        volatile uint32_t crc = ipdb_crc32(0, (const uint8_t *)buf + 64, (size_t)size - 64);
        unsigned long t_crc = now_us() - t0;
        (void)crc;
        ipdb_db db;
        t0 = now_us();
        int err = ipdb_open(&db, buf, (size_t)size);
        unsigned long t_open = now_us() - t0;
        out("db %s: %ld KB, %lu tracks", tag, (long)size / 1024, (unsigned long)db.n_tracks);
        out("  read %lu ms, crc %lu ms, open+validate %lu ms%s", t_read / 1000, t_crc / 1000, t_open / 1000,
            err ? " (FAILED)" : "");
    }
    core_free(h);
}

void shell_bench_run(void)
{
    mkdir(SHELL_DIR);
    log_fd = open(BENCH_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
    lcd_set_viewport(NULL);
    lcd_set_background(LCD_RGBPACK(11, 11, 12));
    lcd_clear_display();
    line_y = 0;

    out("=== iPod OS bench: %s ===", MODEL_NAME);
    out("boot to shell: %ld ms; core free %lu KB", shell_boot_tick * (1000 / HZ),
        (unsigned long)(core_available() / 1024));
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    out("cpu %ld MHz (normal)", cpu_frequency / 1000000);
#endif
    bench_lcd("normal");
    bench_list_frame("normal");

    int h = core_alloc(BIG_CHUNK);
    if (h > 0) {
        core_pin(h);
        bench_storage(core_get_data(h));
        core_free(h);
    }
    bench_db("normal");

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    cpu_boost(true);
    out("cpu %ld MHz (boosted)", cpu_frequency / 1000000);
    bench_lcd("boost");
    bench_list_frame("boost");
    bench_db("boost");
    cpu_boost(false);
#endif

    out("done. log: " BENCH_LOG);
    out("press any button");
    if (log_fd >= 0) {
        write(log_fd, "\n", 1);
        close(log_fd);
        log_fd = -1;
    }
#ifdef SIMULATOR
    if (getenv("IPODOS_SCRIPT"))
        return;
#endif
    button_clear_queue();
    while (button_get(true) & BUTTON_REL)
        ;
}

bool shell_bench_available(void)
{
    return file_exists(BENCH_DB);
}
