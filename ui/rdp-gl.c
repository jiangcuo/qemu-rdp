/*
 * QEMU RDP server (libvds frontend): GL consoles ("-rdp gl=on")
 *
 * The RDP display owns the EGL context of GL consoles (virgl, vfio display)
 * instead of "-display egl-headless". A GL scanout is exported as a dmabuf
 * and handed to libvds, whose VA-API encoder reads it on the GPU: RDP needs
 * no glReadPixels.
 *
 * As egl-headless, the GL frames are read back into the console surface, so
 * the 2D listeners of the console (vnc...) show them too.
 *
 * Each scanout falls back step by step, so the picture never stops:
 *
 *   dmabuf    libvds encodes the GPU buffer (VA-API, same render node);
 *   readback  libvds cannot take it (no VA-API, openh264, a legacy client,
 *             another GPU...): libvds is handed the console surface the
 *             frame was read back into;
 *   off       even the read back failed: the client keeps the last frame
 *             until the next scanout tries again.
 *
 * libvds also asks for CPU pixels at any time (head_input), and takes
 * dmabufs again when nothing needs them. A head whose dmabufs are refused
 * RDP_GL_MAX_FAILURES scanouts in a row reads back for the rest of the run.
 *
 * The guest is not held while libvds reads a dmabuf: holding it until each
 * frame was encoded stalled the guest's rendering and input (dragging lagged
 * and jumped). A frame may tear, as with a shared 2D surface; the next one
 * repairs it.
 *
 * This file is the "ui-rdp-gl" module: it needs ui-opengl, which is a module
 * itself with --enable-modules. It registers itself with rdp-core.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/dmabuf.h"
#include "ui/egl-context.h"
#include "ui/egl-helpers.h"
#include "ui/surface.h"
#include "rdp.h"
#include "trace.h"

/* Refused dmabufs in a row before a head reads back for good. */
#define RDP_GL_MAX_FAILURES 3

typedef enum RdpGlInput {
    RDP_GL_DMABUF,
    RDP_GL_READBACK,
    RDP_GL_OFF,
} RdpGlInput;

static const char *const rdp_gl_input_names[] = {
    [RDP_GL_DMABUF] = "dmabuf",
    [RDP_GL_READBACK] = "readback",
    [RDP_GL_OFF] = "off",
};

struct RdpGl {
    DisplayGLCtx ctx;
    /* A GL scanout is shown (not the 2D surface). */
    bool active;
    RdpGlInput input;
    /* Why the scanout is not handed over as a dmabuf. */
    char *reason;

    /* The scanout: a texture, or a dmabuf from the device. */
    uint32_t texture;
    QemuDmaBuf *dmabuf;
    bool y0_top;
    uint32_t width;
    uint32_t height;
    /* The scanout is a part of its buffer. */
    bool partial;
    /* Layout of the last dmabuf exported, for traces. */
    uint32_t fourcc;
    uint64_t modifier;

    /* Read back into the console surface. */
    egl_fb guest_fb;
    egl_fb blit_fb;
    /* An update was not submitted (the client was busy): send it all next. */
    bool missed;

    /* libvds wants CPU pixels (head_input). */
    bool cpu_wanted;
    /* Refused dmabufs in a row; pinned = read back for good. */
    uint32_t failures;
    bool pinned;
};

static const DisplayChangeListenerOps rdp_gl_dcl_ops;

/*
 * rdp-gl's own GL objects (framebuffers) live in the render node context,
 * which shares textures with the guest's contexts; virgl makes its own
 * context current again before it renders.
 */
static void rdp_gl_make_current(void)
{
    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   qemu_egl_rn_ctx);
}

static void rdp_gl_set_reason(RdpDisplay *rd, const char *reason)
{
    RdpGl *gl = rd->gl;

    if (g_strcmp0(gl->reason, reason)) {
        if (reason) {
            trace_rdp_gl_readback_reason(rd->index, reason);
        }
        g_free(gl->reason);
        gl->reason = g_strdup(reason);
    }
}

/* Reading back failed too: the client keeps its last frame. */
static void rdp_gl_off(RdpDisplay *rd, const char *why)
{
    RdpGl *gl = rd->gl;

    if (gl->input != RDP_GL_OFF) {
        error_report("rdp: display %u: %s; the client keeps the last frame "
                     "until the next scanout", rd->index, why);
    }
    gl->input = RDP_GL_OFF;
    g_free(gl->reason);
    gl->reason = g_strdup(why);
}

/* Sets up the framebuffers that read the scanout back. */
static void rdp_gl_setup_fb(RdpDisplay *rd)
{
    RdpGl *gl = rd->gl;
    uint32_t texture = gl->texture;

    rdp_gl_make_current();
    if (gl->dmabuf) {
        egl_dmabuf_import_texture(gl->dmabuf);
        texture = qemu_dmabuf_get_texture(gl->dmabuf);
    }
    if (!texture) {
        egl_fb_destroy(&gl->guest_fb);
        return;
    }
    egl_fb_setup_for_tex(&gl->guest_fb, gl->width, gl->height, texture, false);
    if (gl->blit_fb.width != gl->width || gl->blit_fb.height != gl->height) {
        egl_fb_destroy(&gl->blit_fb);
        egl_fb_setup_new_tex(&gl->blit_fb, gl->width, gl->height);
    }
}

/*
 * Reads the frame back into the console surface, as egl-headless does (the
 * device sized it to the scanout); false if it could not.
 */
static bool rdp_gl_read(RdpDisplay *rd)
{
    RdpGl *gl = rd->gl;
    int64_t started = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    if (!gl->guest_fb.texture || !rd->surface ||
        surface_format(rd->surface) != PIXMAN_x8r8g8b8) {
        return false;
    }
    rdp_gl_make_current();
    while (glGetError() != GL_NO_ERROR) {
        /* Errors of somebody else's GL calls. */
    }
    egl_fb_blit(&gl->blit_fb, &gl->guest_fb, gl->y0_top);
    egl_fb_read(rd->surface, &gl->blit_fb);
    if (glGetError() != GL_NO_ERROR) {
        return false;
    }
    trace_rdp_gl_readback(rd->index, 1,
                          qemu_clock_get_us(QEMU_CLOCK_REALTIME) - started);
    return true;
}

/* Hands libvds the damaged rectangle of the console surface. */
static void rdp_gl_submit(RdpDisplay *rd, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h)
{
    DisplaySurface *ds = rd->surface;
    uint32_t width = surface_width(ds);
    uint32_t height = surface_height(ds);
    VdsRect r;

    r.x = MIN(x, width);
    r.y = MIN(y, height);
    r.w = MIN(w, width - r.x);
    r.h = MIN(h, height - r.y);
    vds_head_submit(rd->head, surface_data(ds),
                    (size_t)surface_stride(ds) * height,
                    surface_stride(ds), &r, 1);
}

static void rdp_gl_start_readback(RdpDisplay *rd)
{
    RdpGl *gl = rd->gl;

    if (!rdp_gl_read(rd)) {
        rdp_gl_off(rd, "reading the GL frame back failed");
        return;
    }
    gl->input = RDP_GL_READBACK;
    /* Libvds may have had a dmabuf: the whole picture is new to it. */
    rdp_gl_submit(rd, 0, 0, gl->width, gl->height);
}

/* Hands libvds the scanout as a dmabuf; false with the reason recorded. */
static bool rdp_gl_try_dmabuf(RdpDisplay *rd)
{
    RdpGl *gl = rd->gl;
    VdsDmabuf buf = { .fd = -1, .y0_top = gl->y0_top };
    g_autofree char *why = NULL;
    char *vds_err = NULL;
    bool own_fd = false;
    int32_t rc;

    if (gl->pinned) {
        return false;
    }
    if (gl->cpu_wanted) {
        /* Not a failure: libvds asked, and says when it is over. */
        return false;
    }
    if (gl->partial) {
        why = g_strdup("the scanout is a part of a larger buffer");
        goto refused;
    }
    buf.width = gl->width;
    buf.height = gl->height;
    if (gl->dmabuf) {
        buf.fd = qemu_dmabuf_get_fd(gl->dmabuf);
        buf.stride = qemu_dmabuf_get_stride(gl->dmabuf);
        buf.fourcc = qemu_dmabuf_get_fourcc(gl->dmabuf);
        buf.modifier = qemu_dmabuf_get_modifier(gl->dmabuf);
    } else {
        EGLint stride, fourcc;
        EGLuint64KHR modifier;

        rdp_gl_make_current();
        buf.fd = egl_get_fd_for_texture(gl->texture, &stride, &fourcc,
                                        &modifier);
        own_fd = true;
        buf.stride = stride;
        buf.fourcc = fourcc;
        buf.modifier = modifier;
    }
    if (buf.fd < 0) {
        why = g_strdup("cannot export the scanout as a dmabuf");
        goto refused;
    }
    gl->fourcc = buf.fourcc;
    gl->modifier = buf.modifier;
    /* No consumed callback: the guest is not held (see the top). */
    rc = vds_head_attach_dmabuf(rd->head, &buf, NULL, NULL, &vds_err);
    if (own_fd) {
        /* libvds keeps its own duplicate. */
        close(buf.fd);
    }
    if (rc == VDS_OK) {
        gl->failures = 0;
        if (gl->reason) {
            trace_rdp_gl_dmabuf(rd->index);
        }
        g_free(gl->reason);
        gl->reason = NULL;
        return true;
    }
    why = g_strdup(vds_err ? vds_err : "libvds refused the dmabuf");
    vds_free_string(vds_err);

refused:
    rdp_gl_set_reason(rd, why);
    if (++gl->failures >= RDP_GL_MAX_FAILURES) {
        gl->pinned = true;
        warn_report("rdp: display %u: dmabufs refused for %u scanouts in a "
                    "row (%s), GL frames are read back from now on",
                    rd->index, gl->failures, why);
    }
    return false;
}

/* A new GL scanout: dmabuf if possible, read back otherwise. */
static void rdp_gl_start(RdpDisplay *rd)
{
    RdpGl *gl = rd->gl;

    gl->active = true;
    gl->fourcc = 0;
    gl->modifier = 0;
    rdp_gl_setup_fb(rd);
    /* A part of a buffer is read back: the console surface has its size. */
    if (vds_head_resize(rd->head,
                        gl->partial && rd->surface ?
                        surface_width(rd->surface) : gl->width,
                        gl->partial && rd->surface ?
                        surface_height(rd->surface) : gl->height) != VDS_OK) {
        rdp_gl_off(rd, "libvds cannot use a display of this size");
        return;
    }
    if (rdp_gl_try_dmabuf(rd)) {
        gl->input = RDP_GL_DMABUF;
    } else {
        rdp_gl_start_readback(rd);
    }
    trace_rdp_gl_scanout(rd->index, gl->width, gl->height, gl->fourcc,
                         gl->modifier, gl->y0_top,
                         rdp_gl_input_names[gl->input]);
}

static void rdp_gl_scanout_texture(DisplayChangeListener *dcl,
                                   uint32_t backing_id, bool backing_y_0_top,
                                   uint32_t backing_width,
                                   uint32_t backing_height,
                                   uint32_t x, uint32_t y,
                                   uint32_t w, uint32_t h, void *d3d_tex2d)
{
    RdpDisplay *rd = container_of(dcl, RdpDisplay, dcl);
    RdpGl *gl = rd->gl;

    gl->texture = backing_id;
    gl->dmabuf = NULL;
    gl->y0_top = backing_y_0_top;
    gl->width = backing_width;
    gl->height = backing_height;
    gl->partial = x || y || w != backing_width || h != backing_height;
    rdp_gl_start(rd);
}

static void rdp_gl_scanout_dmabuf(DisplayChangeListener *dcl,
                                  QemuDmaBuf *dmabuf)
{
    RdpDisplay *rd = container_of(dcl, RdpDisplay, dcl);
    RdpGl *gl = rd->gl;
    uint32_t bw = qemu_dmabuf_get_backing_width(dmabuf);
    uint32_t bh = qemu_dmabuf_get_backing_height(dmabuf);
    uint32_t w = qemu_dmabuf_get_width(dmabuf);
    uint32_t h = qemu_dmabuf_get_height(dmabuf);

    gl->texture = 0;
    gl->dmabuf = dmabuf;
    gl->y0_top = qemu_dmabuf_get_y0_top(dmabuf);
    gl->width = bw ? bw : w;
    gl->height = bh ? bh : h;
    gl->partial = qemu_dmabuf_get_x(dmabuf) || qemu_dmabuf_get_y(dmabuf) ||
                  w != gl->width || h != gl->height;
    rdp_gl_start(rd);
}

/* The GL scanout is over: back to the 2D surface. */
static void rdp_gl_scanout_end(RdpDisplay *rd)
{
    RdpGl *gl = rd->gl;

    if (!gl->active) {
        return;
    }
    gl->active = false;
    rdp_gl_make_current();
    egl_fb_destroy(&gl->guest_fb);
    gl->texture = 0;
    gl->dmabuf = NULL;
    vds_head_detach(rd->head);
}

static void rdp_gl_scanout_disable(DisplayChangeListener *dcl)
{
    RdpDisplay *rd = container_of(dcl, RdpDisplay, dcl);

    rdp_gl_scanout_end(rd);
    if (rd->surface) {
        rdp_display_gfx_switch(dcl, rd->surface);
    }
}

static void rdp_gl_release_dmabuf(DisplayChangeListener *dcl,
                                  QemuDmaBuf *dmabuf)
{
    RdpDisplay *rd = container_of(dcl, RdpDisplay, dcl);

    if (rd->gl->dmabuf == dmabuf) {
        rdp_gl_scanout_disable(dcl);
    }
    egl_dmabuf_release_texture(dmabuf);
}

static void rdp_gl_update(DisplayChangeListener *dcl,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    RdpDisplay *rd = container_of(dcl, RdpDisplay, dcl);
    RdpGl *gl = rd->gl;
    VdsRect r = { x, y, w, h };
    bool read;

    if (!gl->active) {
        return;
    }
    /*
     * As egl-headless: the 2D listeners see the frame in the console surface.
     * The update reaches this listener too; dpy_refresh ignores it while the
     * GL scanout is shown.
     */
    read = rdp_gl_read(rd);
    if (read) {
        dpy_gfx_update(dcl->con, x, y, w, h);
    }
    switch (gl->input) {
    case RDP_GL_DMABUF:
        /* The guest's rendering must reach the buffer before VA-API reads. */
        glFlush();
        vds_head_damage(rd->head, &r, 1);
        break;
    case RDP_GL_READBACK:
        if (!read) {
            rdp_gl_off(rd, "reading the GL frame back failed");
        } else if (!vds_server_wants_frame(rd->server)) {
            gl->missed = true;
        } else if (gl->missed) {
            gl->missed = false;
            rdp_gl_submit(rd, 0, 0, gl->width, gl->height);
        } else {
            rdp_gl_submit(rd, x, y, w, h);
        }
        break;
    case RDP_GL_OFF:
        break;
    }
    trace_rdp_gl_update(rd->index, x, y, w, h,
                        rdp_gl_input_names[gl->input]);
}

static void rdp_gl_cursor_dmabuf(DisplayChangeListener *dcl,
                                 QemuDmaBuf *dmabuf, bool have_hot,
                                 uint32_t hot_x, uint32_t hot_y)
{
    RdpDisplay *rd = container_of(dcl, RdpDisplay, dcl);
    egl_fb cursor_fb = EGL_FB_INIT;
    DisplaySurface *ds;
    uint32_t texture, w, h;

    if (!dmabuf) {
        vds_cursor_set_visible(rd->server, false);
        return;
    }
    rdp_gl_make_current();
    egl_dmabuf_import_texture(dmabuf);
    texture = qemu_dmabuf_get_texture(dmabuf);
    if (!texture) {
        return;
    }
    w = qemu_dmabuf_get_width(dmabuf);
    h = qemu_dmabuf_get_height(dmabuf);
    /* A cursor is small: read it back and let the client draw it. */
    egl_fb_setup_for_tex(&cursor_fb, w, h, texture, false);
    ds = qemu_create_displaysurface(w, h);
    egl_fb_read(ds, &cursor_fb);
    egl_fb_destroy(&cursor_fb);
    if (vds_cursor_define(rd->server, w, h, have_hot ? hot_x : 0,
                          have_hot ? hot_y : 0,
                          (const uint32_t *)surface_data(ds)) != VDS_OK) {
        trace_rdp_display_cursor_rejected(w, h);
    }
    qemu_free_displaysurface(ds);
}

static void rdp_gl_cursor_position(DisplayChangeListener *dcl,
                                   uint32_t pos_x, uint32_t pos_y)
{
    RdpDisplay *rd = container_of(dcl, RdpDisplay, dcl);

    /* The client draws the cursor where its own pointer is. */
    vds_cursor_set_visible(rd->server, true);
}

static const DisplayChangeListenerOps rdp_gl_dcl_ops = {
    .dpy_name = "rdp-gl",
    .dpy_refresh = rdp_display_refresh,
    .dpy_gfx_update = rdp_display_gfx_update,
    .dpy_gfx_switch = rdp_display_gfx_switch,
    .dpy_gfx_check_format = rdp_display_check_format,
    .dpy_mouse_set = rdp_display_mouse_set,
    .dpy_cursor_define = rdp_display_cursor_define,

    .dpy_gl_scanout_disable = rdp_gl_scanout_disable,
    .dpy_gl_scanout_texture = rdp_gl_scanout_texture,
    .dpy_gl_scanout_dmabuf = rdp_gl_scanout_dmabuf,
    .dpy_gl_cursor_dmabuf = rdp_gl_cursor_dmabuf,
    .dpy_gl_release_dmabuf = rdp_gl_release_dmabuf,
    .dpy_gl_cursor_position = rdp_gl_cursor_position,
    .dpy_gl_update = rdp_gl_update,
};

static QEMUGLContext rdp_gl_ctx_create(DisplayGLCtx *dgc,
                                       QEMUGLParams *params)
{
    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   qemu_egl_rn_ctx);
    return qemu_egl_create_context(dgc, params);
}

static bool rdp_gl_ctx_is_compatible_dcl(DisplayGLCtx *dgc,
                                         DisplayChangeListener *dcl)
{
    /*
     * As egl-headless, compatible with all 2D listeners (vnc...): the GL
     * frames are read back into the console surface.
     */
    return !dcl->ops->dpy_gl_update || dcl->ops == &rdp_gl_dcl_ops;
}

static const DisplayGLCtxOps rdp_gl_ctx_ops = {
    .dpy_gl_ctx_is_compatible_dcl = rdp_gl_ctx_is_compatible_dcl,
    .dpy_gl_ctx_create = rdp_gl_ctx_create,
    .dpy_gl_ctx_destroy = qemu_egl_destroy_context,
    .dpy_gl_ctx_make_current = qemu_egl_make_context_current,
};

/* RdpGlOps */

static bool rdp_gl_init(const char *rendernode, Error **errp)
{
    return egl_init(rendernode, DISPLAY_GL_MODE_ON, errp);
}

static char *rdp_gl_render_node(void)
{
    g_autofree char *link = g_strdup_printf("/proc/self/fd/%d",
                                            qemu_egl_rn_fd);

    return qemu_egl_rn_fd >= 0 ? g_file_read_link(link, NULL) : NULL;
}

static const DisplayChangeListenerOps *rdp_gl_display_init(RdpDisplay *rd,
                                                           QemuConsole *con)
{
    RdpGl *gl = g_new0(RdpGl, 1);

    gl->ctx.ops = &rdp_gl_ctx_ops;
    rd->gl = gl;
    qemu_console_set_display_gl_ctx(con, &gl->ctx);
    return &rdp_gl_dcl_ops;
}

static void rdp_gl_display_fini(RdpDisplay *rd)
{
    RdpGl *gl = rd->gl;

    rdp_gl_make_current();
    egl_fb_destroy(&gl->guest_fb);
    egl_fb_destroy(&gl->blit_fb);
    g_free(gl->reason);
    g_free(gl);
    rd->gl = NULL;
}

static bool rdp_gl_scanout_active(RdpDisplay *rd)
{
    return rd->gl->active;
}

static void rdp_gl_head_input(RdpDisplay *rd, bool cpu, const char *reason)
{
    RdpGl *gl = rd->gl;

    gl->cpu_wanted = cpu;
    if (!gl->active) {
        return;
    }
    if (cpu && gl->input == RDP_GL_DMABUF) {
            rdp_gl_set_reason(rd, reason);
        rdp_gl_start_readback(rd);
    } else if (!cpu && gl->input == RDP_GL_READBACK && rdp_gl_try_dmabuf(rd)) {
        gl->input = RDP_GL_DMABUF;
    }
}

static const char *rdp_gl_input(RdpDisplay *rd, const char **reason)
{
    *reason = rd->gl->reason;
    return rdp_gl_input_names[rd->gl->input];
}

static const RdpGlOps rdp_gl = {
    .init = rdp_gl_init,
    .render_node = rdp_gl_render_node,
    .display_init = rdp_gl_display_init,
    .display_fini = rdp_gl_display_fini,
    .scanout_active = rdp_gl_scanout_active,
    .scanout_end = rdp_gl_scanout_end,
    .head_input = rdp_gl_head_input,
    .input = rdp_gl_input,
};

static void rdp_gl_register_ops(void)
{
    rdp_gl_register(&rdp_gl);
}

type_init(rdp_gl_register_ops);

module_dep("ui-opengl");
