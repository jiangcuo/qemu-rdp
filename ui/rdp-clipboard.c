/*
 * QEMU RDP server (libvds frontend): clipboard
 *
 * Bridges the RDP client clipboard with the QEMU clipboard (guest side:
 * qemu-vdagent chardev + spice-vdagent). Text only, like the QEMU
 * clipboard itself.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "ui/clipboard.h"
#include "rdp.h"
#include "trace.h"

static struct {
    VdsServer *server;
    QemuClipboardPeer peer;
    /* Latest guest clipboard (owned by another peer). */
    QemuClipboardInfo *guest;
    /* The client asked for guest text not available yet. */
    bool client_waiting;
    /* Clipboard published on behalf of the client. */
    QemuClipboardInfo *client;
    /*
     * Loop breaker.  Announcing the guest clipboard to the client makes the
     * client send its own format list back; without this, that answer is
     * published to the guest again and the guest agent answers with another
     * grab, which fills the qemu-vdagent output buffer (ui/vdagent.c).
     */
    bool guest_text;
    bool client_regrabbed;
} rdp_cb;

static void rdp_clipboard_send_guest_text(QemuClipboardInfo *info)
{
    if (!info->types[QEMU_CLIPBOARD_TYPE_TEXT].data) {
        return;
    }
    rdp_cb.client_waiting = false;
    trace_rdp_clipboard_to_client(info->types[QEMU_CLIPBOARD_TYPE_TEXT].size);
    vds_clipboard_data(rdp_cb.server, VDS_CLIPBOARD_TEXT,
                       info->types[QEMU_CLIPBOARD_TYPE_TEXT].data,
                       info->types[QEMU_CLIPBOARD_TYPE_TEXT].size);
}

static void rdp_clipboard_update_info(QemuClipboardInfo *info)
{
    uint32_t text = VDS_CLIPBOARD_TEXT;

    if (info->owner == &rdp_cb.peer ||
        info->selection != QEMU_CLIPBOARD_SELECTION_CLIPBOARD) {
        return;
    }
    if (info != rdp_cb.guest) {
        bool available = info->types[QEMU_CLIPBOARD_TYPE_TEXT].available;

        qemu_clipboard_info_unref(rdp_cb.guest);
        rdp_cb.guest = qemu_clipboard_info_ref(info);
        if (available) {
            /*
             * The agent answers a host grab with a grab of its own.  If the
             * client already published a grab since we last announced the
             * guest clipboard, this is the echo of that announcement: do not
             * bounce it back to the client, it would loop forever.
             */
            if (!(rdp_cb.guest_text && rdp_cb.client_regrabbed)) {
                vds_clipboard_grab(rdp_cb.server, &text, 1);
            }
        } else {
            vds_clipboard_release(rdp_cb.server);
        }
        rdp_cb.guest_text = available;
        rdp_cb.client_regrabbed = false;
        return;
    }
    /* Same info: data we asked for may have arrived. */
    if (rdp_cb.client_waiting) {
        rdp_clipboard_send_guest_text(info);
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
    if (type == QEMU_CLIPBOARD_TYPE_TEXT) {
        vds_clipboard_request(rdp_cb.server, VDS_CLIPBOARD_TEXT);
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
    uint32_t i;
    bool text = false;

    if (!rdp_cb.server) {
        return;
    }
    for (i = 0; i < count; i++) {
        text |= types[i] == VDS_CLIPBOARD_TEXT;
    }
    trace_rdp_clipboard_client_grab(text);
    rdp_cb.client_regrabbed = true;
    info = qemu_clipboard_info_new(&rdp_cb.peer,
                                   QEMU_CLIPBOARD_SELECTION_CLIPBOARD);
    info->types[QEMU_CLIPBOARD_TYPE_TEXT].available = text;
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

    if (!rdp_cb.server || type != VDS_CLIPBOARD_TEXT || !info ||
        !info->types[QEMU_CLIPBOARD_TYPE_TEXT].available) {
        return;
    }
    if (info->types[QEMU_CLIPBOARD_TYPE_TEXT].data) {
        rdp_clipboard_send_guest_text(info);
        return;
    }
    rdp_cb.client_waiting = true;
    qemu_clipboard_request(info, QEMU_CLIPBOARD_TYPE_TEXT);
}

/* Client data for a guest paste. */
void rdp_clipboard_client_data(uint32_t type, const uint8_t *data, size_t len)
{
    if (!rdp_cb.server || type != VDS_CLIPBOARD_TEXT || !rdp_cb.client ||
        !qemu_clipboard_peer_owns(&rdp_cb.peer,
                                  QEMU_CLIPBOARD_SELECTION_CLIPBOARD)) {
        return;
    }
    trace_rdp_clipboard_from_client(len);
    qemu_clipboard_set_data(&rdp_cb.peer, rdp_cb.client,
                            QEMU_CLIPBOARD_TYPE_TEXT, len, data, true);
}
