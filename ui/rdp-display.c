/*
 * QEMU RDP server (libvds frontend): display
 *
 * One 2D DisplayChangeListener per graphic console. Damage reported by
 * dpy_gfx_update is accumulated and handed to libvds in dpy_refresh.
 *
 * A surface in shareable memory (virtio-gpu resources) is attached to libvds,
 * which reads it in place from its encoder thread: the main thread only
 * reports damage. Any other surface is copied by vds_head_submit, which
 * returns once the damaged rectangles are copied. GL consoles (virgl, vfio
 * display) either use "-rdp gl=on" (rdp-gl.c: dmabufs for libvds, or the
 * frame read back when libvds cannot take them) or "-display egl-headless".
 * Both read the GL frames back into the 2D surface.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/qemu-pixman.h"
#include "ui/surface.h"
#include "rdp.h"
#include "trace.h"

/* Refresh interval while no client consumes frames. */
#define RDP_IDLE_INTERVAL_MS 100

static GPtrArray *rdp_displays;

/*
 * Surface images libvds gave back. libvds releases them from its own threads,
 * but pixman reference counts are not atomic: the unref happens here, in a
 * bottom half on the main thread.
 */
static struct {
    QemuMutex lock;
    GSList *images;
    QEMUBH *bh;
} rdp_release;

static void rdp_release_bh(void *opaque)
{
    GSList *images, *l;

    qemu_mutex_lock(&rdp_release.lock);
    images = rdp_release.images;
    rdp_release.images = NULL;
    qemu_mutex_unlock(&rdp_release.lock);
    for (l = images; l; l = l->next) {
        pixman_image_unref(l->data);
    }
    g_slist_free(images);
}

/* VdsReleaseFn: any thread. */
static void rdp_release_image(void *opaque)
{
    qemu_mutex_lock(&rdp_release.lock);
    rdp_release.images = g_slist_prepend(rdp_release.images, opaque);
    qemu_mutex_unlock(&rdp_release.lock);
    qemu_bh_schedule(rdp_release.bh);
}

static bool rdp_rect_contains(const VdsRect *r, int x, int y, int w, int h)
{
    return x >= (int)r->x && y >= (int)r->y &&
           x + w <= (int)(r->x + r->w) && y + h <= (int)(r->y + r->h);
}

static void rdp_damage_add(RdpDisplay *rd, int x, int y, int w, int h)
{
    VdsRect *r;
    uint32_t i;
    int x1, y1;

    if (w <= 0 || h <= 0) {
        return;
    }
    /*
     * A surface switch damages everything and the flush that follows it
     * reports the same area again: do not hand libvds the same pixels twice.
     */
    for (i = 0; i < rd->nrects; i++) {
        if (rdp_rect_contains(&rd->rects[i], x, y, w, h)) {
            return;
        }
    }
    if (rd->nrects == RDP_MAX_RECTS) {
        /* Collapse everything into the first rectangle. */
        r = &rd->rects[0];
        x1 = MAX(r->x + r->w, (uint32_t)(x + w));
        y1 = MAX(r->y + r->h, (uint32_t)(y + h));
        for (i = 1; i < rd->nrects; i++) {
            x1 = MAX((uint32_t)x1, rd->rects[i].x + rd->rects[i].w);
            y1 = MAX((uint32_t)y1, rd->rects[i].y + rd->rects[i].h);
            r->x = MIN(r->x, rd->rects[i].x);
            r->y = MIN(r->y, rd->rects[i].y);
        }
        r->x = MIN(r->x, (uint32_t)x);
        r->y = MIN(r->y, (uint32_t)y);
        r->w = x1 - r->x;
        r->h = y1 - r->y;
        rd->nrects = 1;
        return;
    }
    rd->rects[rd->nrects++] = (VdsRect) { x, y, w, h };
}

static void rdp_damage_full(RdpDisplay *rd)
{
    rd->nrects = 0;
    if (rd->surface) {
        rdp_damage_add(rd, 0, 0, surface_width(rd->surface),
                       surface_height(rd->surface));
    }
}

static bool rdp_format_native(pixman_format_code_t format)
{
    return format == PIXMAN_x8r8g8b8 || format == PIXMAN_a8r8g8b8;
}

void rdp_display_gfx_update(DisplayChangeListener *dcl,
                            int x, int y, int w, int h)
{
    RdpDisplay *rd = container_of(dcl, RdpDisplay, dcl);

    rdp_damage_add(rd, x, y, w, h);
}

void rdp_display_gfx_switch(DisplayChangeListener *dcl,
                            DisplaySurface *new_surface)
{
    RdpDisplay *rd = container_of(dcl, RdpDisplay, dcl);
    const RdpGlOps *gl = rdp_gl_ops();
    int w, h;

    if (rd->gl && gl && gl->scanout_active(rd)) {
        gl->scanout_end(rd);
    }
    rd->surface = new_surface;
    qemu_pixman_image_unref(rd->shadow);
    rd->shadow = NULL;
    if (!new_surface) {
        return;
    }
    w = surface_width(new_surface);
    h = surface_height(new_surface);
    if (!rdp_format_native(surface_format(new_surface))) {
        rd->shadow = pixman_image_create_bits(PIXMAN_x8r8g8b8, w, h, NULL, 0);
    }
    if (vds_head_resize(rd->head, w, h) != VDS_OK) {
        warn_report("rdp: cannot use a %dx%d display", w, h);
    }
    rd->shared = false;
    if (!rd->shadow && new_surface->share_handle != SHAREABLE_NONE) {
        /* The reference keeps the memory mapped until libvds lets it go. */
        pixman_image_t *image = pixman_image_ref(new_surface->image);
        const uint8_t *data = (const uint8_t *)pixman_image_get_data(image);
        int stride = pixman_image_get_stride(image);

        if (vds_head_attach_shared(rd->head, data, (size_t)stride * h, stride,
                                   rdp_release_image, image) == VDS_OK) {
            rd->shared = true;
        } else {
            pixman_image_unref(image);
        }
    }
    trace_rdp_display_switch(rd->index, w, h, surface_format(new_surface),
                             rd->shared);
    rdp_damage_full(rd);
}

void rdp_display_refresh(DisplayChangeListener *dcl)
{
    RdpDisplay *rd = container_of(dcl, RdpDisplay, dcl);
    const RdpGlOps *gl = rdp_gl_ops();
    bool wanted = vds_server_wants_frame(rd->server);
    bool active = qemu_rdp_has_client();
    uint64_t interval = active ? 1000 / MAX(qemu_rdp_max_fps(), 1)
                               : RDP_IDLE_INTERVAL_MS;
    int64_t started = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    pixman_image_t *src;
    uint32_t i;

    trace_rdp_display_refresh(rd->index, active, wanted, interval);
    qemu_console_listener_set_refresh(dcl, interval);
    qemu_console_hw_update(dcl->con);
    if (rd->gl && gl && gl->scanout_active(rd)) {
        /* rdp-gl.c hands libvds the GL scanout itself. */
        rd->nrects = 0;
        return;
    }
    if (!wanted || !rd->surface || !rd->nrects) {
        return;
    }

    if (rd->shared) {
        /* libvds reads the surface itself, from its encoder thread. */
        vds_head_damage(rd->head, rd->rects, rd->nrects);
    } else {
        src = rd->surface->image;
        if (rd->shadow) {
            for (i = 0; i < rd->nrects; i++) {
                pixman_image_composite(PIXMAN_OP_SRC, src, NULL, rd->shadow,
                                       rd->rects[i].x, rd->rects[i].y, 0, 0,
                                       rd->rects[i].x, rd->rects[i].y,
                                       rd->rects[i].w, rd->rects[i].h);
            }
            src = rd->shadow;
        }
        vds_head_submit(rd->head, (const uint8_t *)pixman_image_get_data(src),
                        (size_t)pixman_image_get_stride(src) *
                        pixman_image_get_height(src),
                        pixman_image_get_stride(src), rd->rects, rd->nrects);
    }
    /*
     * T-V2: how long this refresh cost QEMU, everything before libvds sees the
     * damage: the dirty-page scan in qemu_console_hw_update, the shadow copy and the
     * submit (or only the damage report for a shared surface). libvds times
     * the rest of the path (docs/spec/logging.md 5).
     */
    trace_rdp_display_submit(rd->index, rd->nrects,
                             qemu_clock_get_us(QEMU_CLOCK_REALTIME) - started);
    rd->nrects = 0;
}

bool rdp_display_check_format(DisplayChangeListener *dcl,
                              pixman_format_code_t format)
{
    /* Other formats work through the shadow image, but cost a copy. */
    return rdp_format_native(format);
}

void rdp_display_mouse_set(DisplayChangeListener *dcl, int x, int y, bool on)
{
    RdpDisplay *rd = container_of(dcl, RdpDisplay, dcl);

    vds_cursor_set_visible(rd->server, on);
}

void rdp_display_cursor_define(DisplayChangeListener *dcl, QEMUCursor *c)
{
    RdpDisplay *rd = container_of(dcl, RdpDisplay, dcl);

    if (vds_cursor_define(rd->server, c->width, c->height,
                          MAX(c->hot_x, 0), MAX(c->hot_y, 0),
                          c->data) != VDS_OK) {
        trace_rdp_display_cursor_rejected(c->width, c->height);
    }
}

static const DisplayChangeListenerOps rdp_dcl_ops = {
    .dpy_name = "rdp",
    .dpy_refresh = rdp_display_refresh,
    .dpy_gfx_update = rdp_display_gfx_update,
    .dpy_gfx_switch = rdp_display_gfx_switch,
    .dpy_gfx_check_format = rdp_display_check_format,
    .dpy_mouse_set = rdp_display_mouse_set,
    .dpy_cursor_define = rdp_display_cursor_define,
};

void rdp_display_init(VdsServer *server, Error **errp)
{
    QemuConsole *con;
    uint32_t i;

    qemu_mutex_init(&rdp_release.lock);
    rdp_release.bh = qemu_bh_new(rdp_release_bh, NULL);
    rdp_displays = g_ptr_array_new();
    for (i = 0; (con = qemu_console_lookup_by_index(i)); i++) {
        RdpDisplay *rd;
        char *vds_err = NULL;

        if (!qemu_console_is_graphic(con)) {
            continue;
        }
        rd = g_new0(RdpDisplay, 1);
        rd->server = server;
        rd->index = qemu_console_get_index(con);
        rd->head = vds_head_add(server, rd->index,
                                qemu_console_get_width(con, 640),
                                qemu_console_get_height(con, 480), &vds_err);
        if (!rd->head) {
            error_setg(errp, "rdp: cannot add display %u: %s", rd->index,
                       vds_err ? vds_err : "unknown error");
            vds_free_string(vds_err);
            g_free(rd);
            return;
        }
        g_ptr_array_add(rdp_displays, rd);
        qemu_console_register_listener(con, &rd->dcl,
                                       rdp_gl_ops() ?
                                       rdp_gl_ops()->display_init(rd, con) :
                                       &rdp_dcl_ops);
    }
    if (!rdp_displays->len) {
        warn_report("rdp: the machine has no graphic console");
    }
}

void rdp_display_fini(void)
{
    uint32_t i;

    if (!rdp_displays) {
        return;
    }
    for (i = 0; i < rdp_displays->len; i++) {
        RdpDisplay *rd = g_ptr_array_index(rdp_displays, i);

        qemu_console_unregister_listener(&rd->dcl);
        if (rd->gl && rdp_gl_ops()) {
            rdp_gl_ops()->display_fini(rd);
        }
        vds_head_remove(rd->head);
        qemu_pixman_image_unref(rd->shadow);
        g_free(rd);
    }
    g_ptr_array_free(rdp_displays, true);
    rdp_displays = NULL;
}

RdpDisplay *rdp_display_lookup(uint32_t head)
{
    uint32_t i;

    for (i = 0; rdp_displays && i < rdp_displays->len; i++) {
        RdpDisplay *rd = g_ptr_array_index(rdp_displays, i);

        if (rd->index == head) {
            return rd;
        }
    }
    return NULL;
}

QemuConsole *rdp_display_console(uint32_t head)
{
    RdpDisplay *rd = rdp_display_lookup(head);

    return rd ? rd->dcl.con : NULL;
}

void rdp_display_head_input(uint32_t head, bool cpu, const char *reason)
{
    RdpDisplay *rd = rdp_display_lookup(head);

    trace_rdp_display_head_input(head, cpu, reason);
    if (rd && rd->gl && rdp_gl_ops()) {
        rdp_gl_ops()->head_input(rd, cpu, reason);
    }
}

RdpDisplayInfoList *rdp_display_info(void)
{
    RdpDisplayInfoList *list = NULL, **tail = &list;
    uint32_t i;

    for (i = 0; rdp_displays && i < rdp_displays->len; i++) {
        RdpDisplay *rd = g_ptr_array_index(rdp_displays, i);
        RdpDisplayInfo *info = g_new0(RdpDisplayInfo, 1);
        const char *reason = NULL;

        info->head = rd->index;
        if (rd->gl && rdp_gl_ops() && rdp_gl_ops()->scanout_active(rd)) {
            info->input = g_strdup(rdp_gl_ops()->input(rd, &reason));
            info->reason = g_strdup(reason);
        } else {
            info->input = g_strdup(rd->shared ? "shared" : "copy");
        }
        QAPI_LIST_APPEND(tail, info);
    }
    return list;
}

void rdp_display_layout(const VdsMonitor *monitors, uint32_t count)
{
    uint32_t i;

    /* Monitor i of the client drives graphic console i of the guest. */
    for (i = 0; rdp_displays && i < count && i < rdp_displays->len; i++) {
        RdpDisplay *rd = g_ptr_array_index(rdp_displays, i);
        QemuConsole *con = rd->dcl.con;
        QemuUIInfo info;

        if (!qemu_console_ui_info_supported(con)) {
            trace_rdp_display_layout_unsupported(rd->index);
            continue;
        }
        info = *qemu_console_get_ui_info(con);
        info.xoff = monitors[i].left;
        info.yoff = monitors[i].top;
        info.width = monitors[i].width;
        info.height = monitors[i].height;
        info.width_mm = monitors[i].width_mm;
        info.height_mm = monitors[i].height_mm;
        trace_rdp_display_layout(rd->index, info.width, info.height);
        qemu_console_set_ui_info(con, &info, false);
    }
}
