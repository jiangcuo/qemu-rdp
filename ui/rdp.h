/*
 * QEMU RDP server (libvds frontend): internal declarations
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef UI_RDP_H
#define UI_RDP_H

#include <vds.h>

#include "qapi/qapi-types-ui.h"
#include "ui/console.h"

/* rdp-core.c */
VdsServer *qemu_rdp_server(void);
uint32_t qemu_rdp_max_fps(void);
bool qemu_rdp_has_client(void);

/* rdp-display.c */

/* Damage rectangles kept before collapsing to a bounding box. */
#define RDP_MAX_RECTS 32

typedef struct RdpGl RdpGl;

typedef struct RdpDisplay {
    DisplayChangeListener dcl;
    VdsServer *server;
    VdsHead *head;
    uint32_t index;
    DisplaySurface *surface;
    /* x8r8g8b8 copy, only when the surface uses another format. */
    pixman_image_t *shadow;
    /* libvds reads the surface in place (vds_head_attach_shared). */
    bool shared;
    VdsRect rects[RDP_MAX_RECTS];
    uint32_t nrects;
    /* GL scanout state, when the console uses the RDP GL context (rdp-gl.c). */
    RdpGl *gl;
} RdpDisplay;

void rdp_display_init(VdsServer *server, Error **errp);
void rdp_display_fini(void);
QemuConsole *rdp_display_console(uint32_t head);
RdpDisplay *rdp_display_lookup(uint32_t head);
void rdp_display_layout(const VdsMonitor *monitors, uint32_t count);
void rdp_display_head_input(uint32_t head, bool cpu, const char *reason);
RdpDisplayInfoList *rdp_display_info(void);
/* The 2D listener callbacks, shared with the GL listener of rdp-gl.c. */
void rdp_display_refresh(DisplayChangeListener *dcl);
void rdp_display_gfx_update(DisplayChangeListener *dcl,
                            int x, int y, int w, int h);
void rdp_display_gfx_switch(DisplayChangeListener *dcl,
                            DisplaySurface *new_surface);
bool rdp_display_check_format(DisplayChangeListener *dcl,
                              pixman_format_code_t format);
void rdp_display_mouse_set(DisplayChangeListener *dcl, int x, int y, bool on);
void rdp_display_cursor_define(DisplayChangeListener *dcl, QEMUCursor *c);

/*
 * rdp-gl.c, the "ui-rdp-gl" module: GL consoles hand libvds dmabufs, or read
 * back into CPU pixels when libvds cannot take them. Registered by the module
 * when it is loaded (built in without --enable-modules).
 */
typedef struct RdpGlOps {
    /* Sets up EGL on the render node; false (with errp) leaves -rdp on 2D. */
    bool (*init)(const char *rendernode, Error **errp);
    /* Path of the render node EGL uses; free with g_free. */
    char *(*render_node)(void);
    /* Gives the console the RDP GL context; returns the listener ops. */
    const DisplayChangeListenerOps *(*display_init)(RdpDisplay *rd,
                                                    QemuConsole *con);
    void (*display_fini)(RdpDisplay *rd);
    /* A GL scanout is shown: the 2D surface is not what the guest displays. */
    bool (*scanout_active)(RdpDisplay *rd);
    /* A 2D surface replaced the GL scanout. */
    void (*scanout_end)(RdpDisplay *rd);
    /* libvds asks for CPU pixels (cpu) or takes dmabufs again. */
    void (*head_input)(RdpDisplay *rd, bool cpu, const char *reason);
    /* How the head is handed over: "dmabuf", "readback" or "off". */
    const char *(*input)(RdpDisplay *rd, const char **reason);
} RdpGlOps;

void rdp_gl_register(const RdpGlOps *ops);
const RdpGlOps *rdp_gl_ops(void);

/* rdp-input.c */
void rdp_input_init(void);
void rdp_input_key(uint8_t scancode, bool extended, bool down);
void rdp_input_lock_sync(bool scroll, bool num, bool caps);
void rdp_input_pointer_abs(uint32_t head, uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height);
void rdp_input_pointer_rel(int32_t dx, int32_t dy);
void rdp_input_button(uint32_t button, bool down);
void rdp_input_wheel(int32_t dx, int32_t dy);

/* rdp-audio.c */
void rdp_audio_capture_init(VdsServer *server, const char *audiodev,
                            Error **errp);

/* rdp-clipboard.c */
void rdp_clipboard_init(VdsServer *server);
void rdp_clipboard_client_grab(const uint32_t *types, uint32_t count);
void rdp_clipboard_client_release(void);
void rdp_clipboard_client_request(uint32_t type);
void rdp_clipboard_client_data(uint32_t type, const uint8_t *data, size_t len);

#endif /* UI_RDP_H */
