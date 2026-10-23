/*
 * QEMU RDP server (libvds frontend): clipboard
 *
 * Bridges the RDP client clipboard with the QEMU clipboard (guest side:
 * qemu-vdagent chardev + spice-vdagent).  Text and PNG images.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "ui/clipboard.h"
#include "rdp.h"
#include "trace.h"

/* QEMU clipboard type <-> VDS_CLIPBOARD_* */
static const uint32_t rdp_cb_vds_type[QEMU_CLIPBOARD_TYPE__COUNT] = {
    [QEMU_CLIPBOARD_TYPE_TEXT] = VDS_CLIPBOARD_TEXT,
    [QEMU_CLIPBOARD_TYPE_PNG] = VDS_CLIPBOARD_PNG,
};

static struct {
    VdsServer *server;
    QemuClipboardPeer peer;
    /* Latest guest clipboard (owned by another peer). */
    QemuClipboardInfo *guest;
    /* Bit per QemuClipboardType: client asked for guest data not here yet. */
    uint32_t client_waiting;
    /* Clipboard published on behalf of the client. */
    QemuClipboardInfo *client;
    /*
     * Loop breaker.  Announcing the guest clipboard to the client makes the
     * client send its own format list back; without this, that answer is
     * published to the guest again and the guest agent answers with another
     * grab, which fills the qemu-vdagent output buffer (ui/vdagent.c).
     */
    bool guest_any;
    bool client_regrabbed;
} rdp_cb;

static bool rdp_clipboard_qemu_type(uint32_t vds, QemuClipboardType *type)
{
    QemuClipboardType t;

    for (t = 0; t < QEMU_CLIPBOARD_TYPE__COUNT; t++) {
        if (rdp_cb_vds_type[t] && rdp_cb_vds_type[t] == vds) {
            *type = t;
            return true;
        }
    }
    return false;
}

static void rdp_clipboard_send_guest_data(QemuClipboardInfo *info,
                                          QemuClipboardType type)
{
    if (!info->types[type].data) {
        return;
    }
    rdp_cb.client_waiting &= ~(1u << type);
    trace_rdp_clipboard_to_client(info->types[type].size);
    vds_clipboard_data(rdp_cb.server, rdp_cb_vds_type[type],
                       info->types[type].data, info->types[type].size);
}

static void rdp_clipboard_update_info(QemuClipboardInfo *info)
{
    uint32_t types[QEMU_CLIPBOARD_TYPE__COUNT];
    uint32_t count = 0;
    QemuClipboardType t;

    if (info->owner == &rdp_cb.peer ||
        info->selection != QEMU_CLIPBOARD_SELECTION_CLIPBOARD) {
        return;
    }
    if (info != rdp_cb.guest) {
        for (t = 0; t < QEMU_CLIPBOARD_TYPE__COUNT; t++) {
            if (rdp_cb_vds_type[t] && info->types[t].available) {
                types[count++] = rdp_cb_vds_type[t];
            }
        }
        qemu_clipboard_info_unref(rdp_cb.guest);
        rdp_cb.guest = qemu_clipboard_info_ref(info);
        rdp_cb.client_waiting = 0;
        if (count) {
            /*
             * The agent answers a host grab with a grab of its own.  If the
             * client already published a grab since we last announced the
             * guest clipboard, this is the echo of that announcement: do not
             * bounce it back to the client, it would loop forever.
             */
            if (!(rdp_cb.guest_any && rdp_cb.client_regrabbed)) {
                vds_clipboard_grab(rdp_cb.server, types, count);
            }
        } else {
            vds_clipboard_release(rdp_cb.server);
        }
        rdp_cb.guest_any = count > 0;
        rdp_cb.client_regrabbed = false;
        return;
    }
    /* Same info: data we asked for may have arrived. */
    for (t = 0; t < QEMU_CLIPBOARD_TYPE__COUNT; t++) {
        if (rdp_cb.client_waiting & (1u << t)) {
            rdp_clipboard_send_guest_data(info, t);
        }
    }
}

static void rdp_clipboard_notify(Notifier *notifier, void *data)
{
    QemuClipboardNotify *notify = data;

    switch (notify->type) {
    case QEMU_CLIPBOARD_UPDATE_INFO:
        rdp_clipboard_update_info(notify->info);
        break;
    case QEMU_CLIPBOARD_RESET_SERIAL:
        break;
    }
}

/* The guest pastes data the client owns. */
static void rdp_clipboard_request(QemuClipboardInfo *info,
                                  QemuClipboardType type)
{
    if (rdp_cb_vds_type[type]) {
        vds_clipboard_request(rdp_cb.server, rdp_cb_vds_type[type]);
    }
}

void rdp_clipboard_init(VdsServer *server)
{
    rdp_cb.server = server;
    rdp_cb.peer.name = "rdp";
    rdp_cb.peer.notifier.notify = rdp_clipboard_notify;
    rdp_cb.peer.request = rdp_clipboard_request;
    qemu_clipboard_peer_register(&rdp_cb.peer);
}

void rdp_clipboard_client_grab(const uint32_t *types, uint32_t count)
{
    QemuClipboardInfo *info;
    QemuClipboardType type;
    uint32_t i;

    if (!rdp_cb.server) {
        return;
    }
    rdp_cb.client_regrabbed = true;
    info = qemu_clipboard_info_new(&rdp_cb.peer,
                                   QEMU_CLIPBOARD_SELECTION_CLIPBOARD);
    for (i = 0; i < count; i++) {
        if (rdp_clipboard_qemu_type(types[i], &type)) {
            info->types[type].available = true;
        }
    }
    trace_rdp_clipboard_client_grab(
        info->types[QEMU_CLIPBOARD_TYPE_TEXT].available,
        info->types[QEMU_CLIPBOARD_TYPE_PNG].available);
    qemu_clipboard_info_unref(rdp_cb.client);
    rdp_cb.client = qemu_clipboard_info_ref(info);
    qemu_clipboard_update(info);
    qemu_clipboard_info_unref(info);
}

void rdp_clipboard_client_release(void)
{
    if (!rdp_cb.server) {
        return;
    }
    qemu_clipboard_peer_release(&rdp_cb.peer,
                                QEMU_CLIPBOARD_SELECTION_CLIPBOARD);
    qemu_clipboard_info_unref(rdp_cb.client);
    rdp_cb.client = NULL;
}

/* The client pastes data the guest owns. */
void rdp_clipboard_client_request(uint32_t type)
{
    QemuClipboardInfo *info = rdp_cb.guest;
    QemuClipboardType qtype;

    if (!rdp_cb.server || !rdp_clipboard_qemu_type(type, &qtype) || !info ||
        !info->types[qtype].available) {
        return;
    }
    if (info->types[qtype].data) {
        rdp_clipboard_send_guest_data(info, qtype);
        return;
    }
    rdp_cb.client_waiting |= 1u << qtype;
    qemu_clipboard_request(info, qtype);
}

/* Client data for a guest paste. */
void rdp_clipboard_client_data(uint32_t type, const uint8_t *data, size_t len)
{
    QemuClipboardType qtype;

    if (!rdp_cb.server || !rdp_clipboard_qemu_type(type, &qtype) ||
        !rdp_cb.client ||
        !qemu_clipboard_peer_owns(&rdp_cb.peer,
                                  QEMU_CLIPBOARD_SELECTION_CLIPBOARD)) {
        return;
    }
    trace_rdp_clipboard_from_client(len);
    qemu_clipboard_set_data(&rdp_cb.peer, rdp_cb.client,
                            qtype, len, data, true);
}
