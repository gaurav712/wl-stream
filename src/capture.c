#define _GNU_SOURCE
#include "capture.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include <glib-unix.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

static int shm_alloc(size_t size)
{
    char name[64];
    snprintf(name, sizeof(name), "/stream-shm-%d", (int)getpid());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    shm_unlink(name);
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
    return fd;
}

struct CaptureState {
    struct wl_display  *display;
    struct wl_registry *registry;
    struct wl_shm      *shm;
    struct wl_output   *output;
    struct zwlr_screencopy_manager_v1 *screencopy_mgr;

    int      buf_width, buf_height, buf_stride;
    uint32_t buf_format;
    void    *buf_data;
    int      buf_fd;
    size_t   buf_size;
    struct wl_shm_pool *pool;
    struct wl_buffer   *wl_buf;

    int out_width, out_height;
    struct zwlr_screencopy_frame_v1 *frame;

    FrameCallback  on_frame;
    void          *userdata;
    GMainLoop     *loop;
    guint          watch_id;
};

static void request_frame(CaptureState *cs);

static void buf_release(void *data, struct wl_buffer *buf) { (void)data; (void)buf; }
static const struct wl_buffer_listener buf_listener = { .release = buf_release };

static void ensure_buffer(CaptureState *cs, uint32_t fmt, int w, int h, int stride)
{
    size_t need = (size_t)stride * (size_t)h;
    if (cs->wl_buf && cs->buf_width == w && cs->buf_height == h &&
        cs->buf_stride == stride && cs->buf_format == fmt)
        return;

    if (cs->wl_buf)   { wl_buffer_destroy(cs->wl_buf);    cs->wl_buf  = NULL; }
    if (cs->pool)     { wl_shm_pool_destroy(cs->pool);     cs->pool    = NULL; }
    if (cs->buf_data) { munmap(cs->buf_data, cs->buf_size); cs->buf_data = NULL; }
    if (cs->buf_fd >= 0) { close(cs->buf_fd); cs->buf_fd = -1; }

    cs->buf_fd = shm_alloc(need);
    if (cs->buf_fd < 0) { perror("shm_alloc"); return; }
    cs->buf_data = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, cs->buf_fd, 0);
    if (cs->buf_data == MAP_FAILED) { perror("mmap"); close(cs->buf_fd); cs->buf_fd = -1; return; }

    cs->buf_size = need; cs->buf_width = w; cs->buf_height = h;
    cs->buf_stride = stride; cs->buf_format = fmt;
    cs->pool   = wl_shm_create_pool(cs->shm, cs->buf_fd, (int32_t)need);
    cs->wl_buf = wl_shm_pool_create_buffer(cs->pool, 0, w, h, stride, fmt);
    wl_buffer_add_listener(cs->wl_buf, &buf_listener, cs);
}

static void frame_ev_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
                             uint32_t fmt, uint32_t w, uint32_t h, uint32_t stride)
{
    (void)frame;
    CaptureState *cs = data;
    ensure_buffer(cs, fmt, (int)w, (int)h, (int)stride);
    if (!cs->out_width) { cs->out_width = (int)w; cs->out_height = (int)h; }
}

static void frame_ev_flags(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t fl)
{ (void)data; (void)f; (void)fl; }

static void frame_ev_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
                            uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
    CaptureState *cs = data;
    zwlr_screencopy_frame_v1_destroy(frame);
    cs->frame = NULL;
    if (cs->on_frame && cs->buf_data) {
        uint64_t ts = ((uint64_t)tv_sec_hi << 32 | tv_sec_lo) * 1000000000ULL + tv_nsec;
        cs->on_frame(cs->userdata, cs->buf_data, cs->buf_width, cs->buf_height,
                     cs->buf_stride, cs->buf_format, ts);
    }
    request_frame(cs);
}

static void frame_ev_failed(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
    CaptureState *cs = data;
    zwlr_screencopy_frame_v1_destroy(frame);
    cs->frame = NULL;
    request_frame(cs);
}

static void frame_ev_damage(void *data, struct zwlr_screencopy_frame_v1 *f,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{ (void)data; (void)f; (void)x; (void)y; (void)w; (void)h; }

static void frame_ev_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *f,
                                   uint32_t fmt, uint32_t w, uint32_t h)
{ (void)data; (void)f; (void)fmt; (void)w; (void)h; }

static void frame_ev_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
    CaptureState *cs = data;
    if (cs->wl_buf) {
        zwlr_screencopy_frame_v1_copy(frame, cs->wl_buf);
        wl_display_flush(cs->display);
    } else {
        zwlr_screencopy_frame_v1_destroy(frame);
        cs->frame = NULL;
        request_frame(cs);
    }
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer      = frame_ev_buffer,
    .flags       = frame_ev_flags,
    .ready       = frame_ev_ready,
    .failed      = frame_ev_failed,
    .damage      = frame_ev_damage,
    .linux_dmabuf = frame_ev_linux_dmabuf,
    .buffer_done = frame_ev_buffer_done,
};

static void output_ev_geometry(void *d, struct wl_output *o,
    int32_t x, int32_t y, int32_t pw, int32_t ph,
    int32_t sub, const char *make, const char *model, int32_t tr)
{ (void)d;(void)o;(void)x;(void)y;(void)pw;(void)ph;(void)sub;(void)make;(void)model;(void)tr; }

static void output_ev_mode(void *data, struct wl_output *o,
                            uint32_t flags, int32_t w, int32_t h, int32_t refresh)
{
    (void)o; (void)refresh;
    CaptureState *cs = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) { cs->out_width = w; cs->out_height = h; }
}

static void output_ev_done(void *d, struct wl_output *o)  { (void)d; (void)o; }
static void output_ev_scale(void *d, struct wl_output *o, int32_t f) { (void)d;(void)o;(void)f; }
static void output_ev_name(void *d, struct wl_output *o, const char *n) { (void)d;(void)o;(void)n; }
static void output_ev_description(void *d, struct wl_output *o, const char *desc) { (void)d;(void)o;(void)desc; }

static const struct wl_output_listener output_listener = {
    .geometry    = output_ev_geometry,
    .mode        = output_ev_mode,
    .done        = output_ev_done,
    .scale       = output_ev_scale,
    .name        = output_ev_name,
    .description = output_ev_description,
};

static void registry_global(void *data, struct wl_registry *reg,
                             uint32_t name, const char *iface, uint32_t version)
{
    CaptureState *cs = data;
    if (strcmp(iface, wl_shm_interface.name) == 0) {
        cs->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, wl_output_interface.name) == 0 && !cs->output) {
        cs->output = wl_registry_bind(reg, name, &wl_output_interface,
                                      version < 4 ? version : 4);
        wl_output_add_listener(cs->output, &output_listener, cs);
    } else if (strcmp(iface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        cs->screencopy_mgr = wl_registry_bind(reg, name,
            &zwlr_screencopy_manager_v1_interface, version < 3 ? version : 3);
    }
}

static void registry_global_remove(void *d, struct wl_registry *r, uint32_t n)
{ (void)d; (void)r; (void)n; }

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

static void request_frame(CaptureState *cs)
{
    if (!cs->screencopy_mgr || !cs->output) return;
    cs->frame = zwlr_screencopy_manager_v1_capture_output(
        cs->screencopy_mgr, 1, cs->output);
    if (!cs->frame) return;
    zwlr_screencopy_frame_v1_add_listener(cs->frame, &frame_listener, cs);
    wl_display_flush(cs->display);
}

static gboolean wayland_dispatch(gint fd, GIOCondition cond, gpointer data)
{
    (void)fd; (void)cond;
    CaptureState *cs = data;
    if (wl_display_dispatch(cs->display) < 0) {
        fprintf(stderr, "Wayland display dispatch error\n");
        g_main_loop_quit(cs->loop);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

CaptureState *capture_init(void)
{
    CaptureState *cs = calloc(1, sizeof(*cs));
    if (!cs) return NULL;
    cs->buf_fd = -1;

    cs->display = wl_display_connect(NULL);
    if (!cs->display) { fprintf(stderr, "Cannot connect to Wayland display\n"); free(cs); return NULL; }

    cs->registry = wl_display_get_registry(cs->display);
    wl_registry_add_listener(cs->registry, &registry_listener, cs);
    wl_display_roundtrip(cs->display);
    wl_display_roundtrip(cs->display);

    if (!cs->shm || !cs->output || !cs->screencopy_mgr) {
        fprintf(stderr, "Missing Wayland globals (wl_shm=%p wl_output=%p screencopy=%p)\n",
                (void *)cs->shm, (void *)cs->output, (void *)cs->screencopy_mgr);
        capture_destroy(cs);
        return NULL;
    }
    return cs;
}

void capture_set_callback(CaptureState *cs, FrameCallback cb, void *userdata)
{
    cs->on_frame = cb;
    cs->userdata = userdata;
}

void capture_start(CaptureState *cs, GMainLoop *loop)
{
    cs->loop = loop;
    cs->watch_id = g_unix_fd_add(wl_display_get_fd(cs->display),
                                  G_IO_IN, wayland_dispatch, cs);
    request_frame(cs);
    wl_display_flush(cs->display);
}

int capture_width(const CaptureState *cs)  { return cs->out_width; }
int capture_height(const CaptureState *cs) { return cs->out_height; }

void capture_destroy(CaptureState *cs)
{
    if (!cs) return;
    if (cs->watch_id)       g_source_remove(cs->watch_id);
    if (cs->frame)          zwlr_screencopy_frame_v1_destroy(cs->frame);
    if (cs->wl_buf)         wl_buffer_destroy(cs->wl_buf);
    if (cs->pool)           wl_shm_pool_destroy(cs->pool);
    if (cs->buf_data)       munmap(cs->buf_data, cs->buf_size);
    if (cs->buf_fd >= 0)    close(cs->buf_fd);
    if (cs->screencopy_mgr) zwlr_screencopy_manager_v1_destroy(cs->screencopy_mgr);
    if (cs->output)         wl_output_destroy(cs->output);
    if (cs->shm)            wl_shm_destroy(cs->shm);
    if (cs->registry)       wl_registry_destroy(cs->registry);
    if (cs->display)        wl_display_disconnect(cs->display);
    free(cs);
}
