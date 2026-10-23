/*
 * QEMU RDP server (libvds frontend): audio capture
 *
 * Sends the guest's playback to the RDP client whatever audio backend the
 * sound card uses (none, pa, pipewire, spice...): the mixed output of that
 * backend is tapped with audio_be_add_capture(), like the VNC audio
 * extension.
 * A sound card on "-audiodev rdp" needs no capture, audio/rdpaudio.c
 * forwards it directly.
 *
 * Guest volume is already applied to the captured samples when the backend
 * has no volume control of its own (none, wav, spice...).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-audio.h"
#include "qemu/audio.h"
#include "qemu/audio-capture.h"
#include "rdp.h"
#include "trace.h"

static struct {
    VdsServer *server;
    CaptureVoiceOut *cap;
} rdp_audio;

static void rdp_audio_notify(void *opaque, audcnotification_e cmd)
{
}

static void rdp_audio_captured(void *opaque, const void *buf, int size)
{
    if (size > 0) {
        vds_audio_out(rdp_audio.server, buf, size / sizeof(int16_t));
    }
}

static void rdp_audio_destroy(void *opaque)
{
}

/*
 * The -audiodev to capture: @audiodev if set, else the VM's only one.
 * Returns false (with nothing to capture) when there is none, or several
 * and no choice.
 */
static bool rdp_audio_pick(const char *audiodev, char **id,
                           AudiodevDriver *driver, Error **errp)
{
    g_autoptr(AudiodevList) list = qmp_query_audiodevs(NULL);
    AudiodevList *e;
    unsigned count = 0;

    for (e = list; e; e = e->next) {
        count++;
        if (audiodev && strcmp(e->value->id, audiodev) == 0) {
            *id = g_strdup(e->value->id);
            *driver = e->value->driver;
            return true;
        }
    }
    if (audiodev) {
        error_setg(errp, "audiodev '%s' not found", audiodev);
        return false;
    }
    if (count > 1) {
        warn_report("rdp: several audio backends, set audiodev= "
                    "to send audio to RDP clients");
    }
    if (count != 1) {
        /* No backend at all is fine: the VM has no sound card. */
        return false;
    }
    *id = g_strdup(list->value->id);
    *driver = list->value->driver;
    return true;
}

void rdp_audio_capture_init(VdsServer *server, const char *audiodev,
                            Error **errp)
{
    static const struct audio_capture_ops ops = {
        .notify = rdp_audio_notify,
        .capture = rdp_audio_captured,
        .destroy = rdp_audio_destroy,
    };
    /* What libvds sends over rdpsnd. */
    struct audsettings as = {
        .freq = 48000,
        .nchannels = 2,
        .fmt = AUDIO_FORMAT_S16,
        .big_endian = HOST_BIG_ENDIAN,
    };
    g_autofree char *id = NULL;
    AudiodevDriver driver;
    AudioBackend *be;

    if (!rdp_audio_pick(audiodev, &id, &driver, errp)) {
        return;
    }
    if (driver == AUDIODEV_DRIVER_RDP) {
        return;
    }
    be = audio_be_by_name(id, errp);
    if (!be) {
        return;
    }

    trace_rdp_audio_capture(id, AudiodevDriver_str(driver));
    rdp_audio.server = server;
    rdp_audio.cap = audio_be_add_capture(be, &as, &ops, NULL);
    if (!rdp_audio.cap) {
        error_setg(errp, "rdp: cannot capture audio");
    }
}
