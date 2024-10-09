/*
 * Twin - A Tiny Window System
 * Copyright (c) 2024 National Cheng Kung University, Taiwan
 * All rights reserved.
 */

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <twin.h>
#include <unistd.h>

#include "linux_input.h"
#include "twin_backend.h"
#include "twin_private.h"

#define FBDEV_NAME "FRAMEBUFFER"
#define FBDEV_DEFAULT "/dev/fb0"
#define SCREEN(x) ((twin_context_t *) x)->screen
#define PRIV(x) ((twin_fbdev_t *) ((twin_context_t *) x)->priv)
#define FBIO_CACHE_SYNC 0x4630

typedef struct {
    twin_screen_t *screen;

    /* Linux input system */
    void *input;

    /* Linux virtual terminal (VT) */
    int vt_fd;
    int vt_num;
    bool vt_active;

    /* Linux framebuffer */
    int fb_fd;
    struct fb_var_screeninfo fb_var;
    struct fb_fix_screeninfo fb_fix;
    uint16_t cmap[3][256];
    uint8_t *fb_base;
    size_t fb_len;
} twin_fbdev_t;

static void _twin_fbdev_put_span(twin_coord_t left,
                                 twin_coord_t top,
                                 twin_coord_t right,
                                 twin_argb32_t *pixels,
                                 void *closure)
{
    twin_screen_t *screen = SCREEN(closure);
    twin_fbdev_t *tx = PRIV(closure);

    if (tx->fb_base == MAP_FAILED)
        return;

    twin_coord_t width = right - left;
    off_t off = top * screen->width + left;
    uint32_t *dest =
        (uint32_t *) ((uintptr_t) tx->fb_base + (off * sizeof(uint32_t)));
    memcpy(dest, pixels, width * sizeof(uint32_t));
}

static void twin_fbdev_get_screen_size(twin_fbdev_t *tx,
                                       int *width,
                                       int *height)
{
    struct fb_var_screeninfo info;
    ioctl(tx->fb_fd, FBIOGET_VSCREENINFO, &info);
    *width = info.xres;
    *height = info.yres;
}

static void twin_fbdev_damage(twin_screen_t *screen, twin_fbdev_t *tx)
{
    int width, height;
    twin_fbdev_get_screen_size(tx, &width, &height);
    twin_screen_damage(tx->screen, 0, 0, width, height);
}

void sync_cache(int fb_fd, unsigned char *mem_start)
{
    if (fb_fd < 0)
        return;
    const unsigned int pitch = 4 * 854;
    const unsigned int y = 0;
    const unsigned int h = 480;
    const unsigned int w = 854;

    unsigned int args[2];
    unsigned int dirty_rect_vir_addr_begin = (unsigned int) (mem_start);
    unsigned int dirty_rect_vir_addr_end =
        (unsigned int) (mem_start + pitch * (y + h));
    args[0] = dirty_rect_vir_addr_begin;
    args[1] = dirty_rect_vir_addr_end;
    ioctl(fb_fd, FBIO_CACHE_SYNC, args);
}

/* seed msg to fb driver to call pan_disp */
void video_sync(int fb_fd, unsigned char *mem_start)
{
    struct fb_var_screeninfo var;
    ioctl(fb_fd, FBIOGET_VSCREENINFO, &var);

    sync_cache(fb_fd, mem_start);

    var.yoffset = 0;
    var.reserved[0] = 0;
    var.reserved[1] = 0;
    var.reserved[2] = 854;
    var.reserved[3] = 480;
    ioctl(fb_fd, FBIOPAN_DISPLAY, &var);
}

static bool twin_fbdev_work(void *closure)
{
    twin_screen_t *screen = SCREEN(closure);

    if (twin_screen_damaged(screen))
        twin_screen_update(screen);

    twin_context_t *ctx = (twin_context_t *) closure;
    twin_fbdev_t *tx = ctx->priv;
    video_sync(tx->fb_fd, tx->fb_base);
    return true;
}

static bool twin_fbdev_apply_config(twin_fbdev_t *tx)
{
    /* Read changable information of the framebuffer */
    if (ioctl(tx->fb_fd, FBIOGET_VSCREENINFO, &tx->fb_var) == -1) {
        log_error("Failed to get framebuffer information");
        return false;
    }

    /* Set the virtual screen size to be the same as the physical screen */
    tx->fb_var.xres_virtual = tx->fb_var.xres;
    tx->fb_var.yres_virtual = tx->fb_var.yres;
    tx->fb_var.bits_per_pixel = 32;
    if (ioctl(tx->fb_fd, FBIOPUT_VSCREENINFO, &tx->fb_var) < 0) {
        log_error("Failed to set framebuffer mode");
        return false;
    }

    /* Read changable information of the framebuffer again */
    if (ioctl(tx->fb_fd, FBIOGET_VSCREENINFO, &tx->fb_var) < 0) {
        log_error("Failed to get framebuffer information");
        return false;
    }

    /* Check bits per pixel */
    if (tx->fb_var.bits_per_pixel != 32) {
        log_error("Failed to set framebuffer bpp to 32");
        return false;
    }

    /* Read unchangable information of the framebuffer */
    ioctl(tx->fb_fd, FBIOGET_FSCREENINFO, &tx->fb_fix);

    /* Align the framebuffer memory address with the page size */
    off_t pgsize = getpagesize();
    off_t start = (off_t) tx->fb_fix.smem_start & (pgsize - 1);

    /* Round up the framebuffer memory size to match the page size */
    tx->fb_len = start + (size_t) tx->fb_fix.smem_len + (pgsize - 1);
    tx->fb_len &= ~(pgsize - 1);

    /* Map framebuffer device to the virtual memory */
    tx->fb_base = mmap(NULL, tx->fb_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                       tx->fb_fd, 0);
    if (tx->fb_base == MAP_FAILED) {
        log_error("Failed to mmap framebuffer");
        return false;
    }

    return true;
}

static int twin_vt_open(int vt_num)
{
    int fd;

    char vt_dev[30] = {0};
    snprintf(vt_dev, 30, "/dev/tty%d", vt_num);

    fd = open(vt_dev, O_RDWR);
    if (fd < 0) {
        log_error("Failed to open %s", vt_dev);
    }

    return fd;
}

static bool twin_vt_setup(twin_fbdev_t *tx)
{
    /* Open VT0 to inquire information */
    if ((tx->vt_fd = twin_vt_open(0)) < -1) {
        log_error("Failed to open VT0");
        return false;
    }

    /* Inquire for current VT number */
    struct vt_stat vt;
    if (ioctl(tx->vt_fd, VT_GETSTATE, &vt) == -1) {
        log_error("Failed to get VT number");
        return false;
    }
    tx->vt_num = vt.v_active;

    /* Open the VT */
    if ((tx->vt_fd = twin_vt_open(tx->vt_num)) < -1) {
        return false;
    }

    /* Set VT to graphics mode to inhibit command-line text */
    if (ioctl(tx->vt_fd, KDSETMODE, KD_GRAPHICS) < 0) {
        log_error("Failed to set KD_GRAPHICS mode");
        return false;
    }

    return true;
}

twin_context_t *twin_fbdev_init(int width, int height)
{
    char *fbdev_path = getenv(FBDEV_NAME);
    if (!fbdev_path) {
        log_info("Environment variable $FRAMEBUFFER not set, use %s by default",
                 FBDEV_DEFAULT);
        fbdev_path = FBDEV_DEFAULT;
    }

    twin_context_t *ctx = calloc(1, sizeof(twin_context_t));
    if (!ctx)
        return NULL;
    ctx->priv = calloc(1, sizeof(twin_fbdev_t));
    if (!ctx->priv)
        return NULL;

    twin_fbdev_t *tx = ctx->priv;

    /* Open the framebuffer device */
    tx->fb_fd = open(fbdev_path, O_RDWR);
    if (tx->fb_fd == -1) {
        log_error("Failed to open %s", fbdev_path);
        goto bail;
    }

    /* Set up virtual terminal environment */
    if (!twin_vt_setup(tx)) {
        goto bail_fb_fd;
    }

    /* Apply configurations to the framebuffer device */
    if (!twin_fbdev_apply_config(tx)) {
        log_error("Failed to apply configurations to the framebuffer device");
        goto bail_vt_fd;
    }

    /* Create TWIN screen */
    twin_fbdev_get_screen_size(tx, &width, &height);
    ctx->screen =
        twin_screen_create(width, height, NULL, _twin_fbdev_put_span, ctx);

    /* Create Linux input system object */
    tx->input = twin_linux_input_create(ctx->screen);
    if (!tx->input) {
        log_error("Failed to create Linux input system object");
        goto bail_screen;
    }

    /* Setup file handler and work functions */
    twin_set_work(twin_fbdev_work, TWIN_WORK_REDISPLAY, ctx);

    return ctx;

bail_screen:
    twin_screen_destroy(ctx->screen);
bail_vt_fd:
    close(tx->vt_fd);
bail_fb_fd:
    close(tx->fb_fd);
bail:
    free(ctx->priv);
    free(ctx);
    return NULL;
}

static void twin_fbdev_configure(twin_context_t *ctx)
{
    int width, height;
    twin_fbdev_t *tx = ctx->priv;
    twin_fbdev_get_screen_size(tx, &width, &height);
    twin_screen_resize(ctx->screen, width, height);
}

static void twin_fbdev_exit(twin_context_t *ctx)
{
    if (!ctx)
        return;

    twin_fbdev_t *tx = PRIV(ctx);
    ioctl(tx->vt_fd, KDSETMODE, KD_TEXT);
    munmap(tx->fb_base, tx->fb_len);
    twin_linux_input_destroy(tx->input);
    close(tx->vt_fd);
    close(tx->fb_fd);
    free(ctx->priv);
    free(ctx);
}

/* Register the Linux framebuffer backend */

const twin_backend_t g_twin_backend = {
    .init = twin_fbdev_init,
    .configure = twin_fbdev_configure,
    .exit = twin_fbdev_exit,
};
