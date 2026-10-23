/*
 * QEMU RDP server (libvds frontend): internal declarations
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef UI_RDP_H
#define UI_RDP_H

#include <vds.h>

#include "ui/console.h"

/* rdp-core.c */
VdsServer *qemu_rdp_server(void);
uint32_t qemu_rdp_max_fps(void);
bool qemu_rdp_has_client(void);

/* rdp-display.c */
void rdp_display_init(VdsServer *server, Error **errp);
void rdp_display_fini(void);
QemuConsole *rdp_display_console(uint32_t head);
void rdp_display_layout(const VdsMonitor *monitors, uint32_t count);

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
