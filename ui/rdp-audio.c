/*
 * QEMU RDP server (libvds frontend): audio capture
 *
 * Sends the guest's playback to the RDP client whatever audio backend the
 * sound card uses (none, pa, pipewire, spice...): the mixed output of that
 * backend is tapped with AUD_add_capture(), like the VNC audio extension.
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
#include "qapi/qapi-types-audio.h"
#include "audio/audio.h"
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

void rdp_audio_capture_init(VdsServer *server, const char *audiodev,
                            Error **errp)
{
    struct audio_capture_ops ops = {
        .notify = rdp_audio_notify,
        .capture = rdp_audio_captured,
        .destroy = rdp_audio_destroy,
    };
    /* What libvds sends over rdpsnd. */
    struct audsettings as = {
        .freq = 48000,
        .nchannels = 2,
        .fmt = AUDIO_FORMAT_S16,
        .endianness = AUDIO_HOST_ENDIANNESS,
    };
    AudioState *s;
    bool several;

    if (audiodev) {
        s = audio_state_by_name(audiodev, errp);
        if (!s) {
            return;
        }
    } else {
        s = audio_get_sole_audio_state(&several);
        if (!s) {
            /* No backend at all is fine: the VM has no sound card. */
            if (several) {
                warn_report("rdp: several audio backends, set audiodev= "
                            "to send audio to RDP clients");
            }
            return;
        }
    }
    if (audio_state_driver(s) == AUDIODEV_DRIVER_RDP) {
        return;
    }

    trace_rdp_audio_capture(audiodev ?: "(sole)",
                            AudiodevDriver_str(audio_state_driver(s)));
    rdp_audio.server = server;
    rdp_audio.cap = AUD_add_capture(s, &as, &ops, NULL);
    if (!rdp_audio.cap) {
        error_setg(errp, "rdp: cannot capture audio");
    }
}
