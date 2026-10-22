/*
 * QEMU RDP server (libvds frontend): hooks used outside ui/rdp-*.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_RDP_H
#define QEMU_RDP_H

#include "qapi/qapi-types-ui.h"

/* Starts the RDP server if -rdp was given. */
void qemu_rdp_init(Error **errp);

/* QMP set_password / expire_password, protocol "rdp". */
void qemu_rdp_set_password(const char *password, SetPasswordAction connected,
                           SetPasswordOptionsRdp *opts, Error **errp);
void qemu_rdp_set_password_expiry(time_t when, Error **errp);

#endif /* QEMU_RDP_H */
