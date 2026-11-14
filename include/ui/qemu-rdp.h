/*
 * QEMU RDP server (libvds frontend): hooks used outside ui/rdp-*.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_RDP_H
#define QEMU_RDP_H

#include "qapi/qapi-types-ui.h"

/*
 * Sets up the GL context of -rdp gl=on; before the devices are created,
 * which need it. A failure only leaves RDP without GL.
 */
void qemu_rdp_early_init(void);

/* Starts the RDP server if -rdp was given. */
void qemu_rdp_init(Error **errp);

/*
 * -rdp gl=on owns the GL context of the consoles, as egl-headless: RDP must
 * start before the displays that show them (vnc, spice).
 */
bool qemu_rdp_owns_gl(void);

/* QMP set_password / expire_password, protocol "rdp". */
void qemu_rdp_set_password(const char *password, SetPasswordAction connected,
                           SetPasswordOptionsRdp *opts, Error **errp);
void qemu_rdp_set_password_expiry(time_t when, Error **errp);

#endif /* QEMU_RDP_H */
