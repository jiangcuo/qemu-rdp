/*
 * QEMU RDP server (libvds frontend): audiodev driver "rdp"
 *
 * Playback is forwarded to the RDP client (rdpsnd, 48 kHz s16 stereo).
 * Recording is not supported by libvds yet: capture returns silence.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "ui/rdp.h"

#define AUDIO_CAP "rdp"
#include "audio.h"
#include "audio_int.h"

typedef struct RdpVoiceOut {
    HWVoiceOut hw;
    RateCtl rate;
} RdpVoiceOut;

typedef struct RdpVoiceIn {
    HWVoiceIn hw;
    RateCtl rate;
} RdpVoiceIn;

static void rdp_audio_settings(struct audsettings *as)
{
    as->freq = 48000;
    as->nchannels = 2;
    as->fmt = AUDIO_FORMAT_S16;
    as->endianness = AUDIO_HOST_ENDIANNESS;
}

static void *rdp_audio_init(Audiodev *dev, Error **errp)
{
    return &rdp_audio_init;
}

static void rdp_audio_fini(void *opaque)
{
}

static int rdp_init_out(HWVoiceOut *hw, struct audsettings *as,
                        void *drv_opaque)
{
    RdpVoiceOut *out = container_of(hw, RdpVoiceOut, hw);
    struct audsettings settings;

    rdp_audio_settings(&settings);
    audio_pcm_init_info(&hw->info, &settings);
    hw->samples = 480 * 4;
    audio_rate_start(&out->rate);
    return 0;
}

static void rdp_fini_out(HWVoiceOut *hw)
{
}

static size_t rdp_write(HWVoiceOut *hw, void *buf, size_t len)
{
    RdpVoiceOut *out = container_of(hw, RdpVoiceOut, hw);
    size_t bytes = audio_rate_get_bytes(&out->rate, &hw->info, len);
    VdsServer *server = qemu_rdp_server();

    if (server && bytes) {
        vds_audio_out(server, buf, bytes / sizeof(int16_t));
    }
    return bytes;
}

static void rdp_enable_out(HWVoiceOut *hw, bool enable)
{
    RdpVoiceOut *out = container_of(hw, RdpVoiceOut, hw);

    if (enable) {
        audio_rate_start(&out->rate);
    }
}

static void rdp_volume_out(HWVoiceOut *hw, Volume *vol)
{
    VdsServer *server = qemu_rdp_server();

    if (server && vol->channels == 2) {
        vds_audio_volume(server, vol->vol[0] * 257, vol->vol[1] * 257,
                         vol->mute);
    }
}

static int rdp_init_in(HWVoiceIn *hw, struct audsettings *as,
                       void *drv_opaque)
{
    RdpVoiceIn *in = container_of(hw, RdpVoiceIn, hw);
    struct audsettings settings;

    rdp_audio_settings(&settings);
    audio_pcm_init_info(&hw->info, &settings);
    hw->samples = 480 * 4;
    audio_rate_start(&in->rate);
    return 0;
}

static void rdp_fini_in(HWVoiceIn *hw)
{
}

static size_t rdp_read(HWVoiceIn *hw, void *buf, size_t size)
{
    RdpVoiceIn *in = container_of(hw, RdpVoiceIn, hw);
    size_t bytes = audio_rate_get_bytes(&in->rate, &hw->info, size);

    audio_pcm_info_clear_buf(&hw->info, buf, bytes / hw->info.bytes_per_frame);
    return bytes;
}

static void rdp_enable_in(HWVoiceIn *hw, bool enable)
{
    RdpVoiceIn *in = container_of(hw, RdpVoiceIn, hw);

    if (enable) {
        audio_rate_start(&in->rate);
    }
}

static struct audio_pcm_ops rdp_pcm_ops = {
    .init_out = rdp_init_out,
    .fini_out = rdp_fini_out,
    .write = rdp_write,
    .buffer_get_free = audio_generic_buffer_get_free,
    .run_buffer_out = audio_generic_run_buffer_out,
    .enable_out = rdp_enable_out,
    .volume_out = rdp_volume_out,

    .init_in = rdp_init_in,
    .fini_in = rdp_fini_in,
    .read = rdp_read,
    .run_buffer_in = audio_generic_run_buffer_in,
    .enable_in = rdp_enable_in,
};

static struct audio_driver rdp_audio_driver = {
    .name = "rdp",
    .descr = "RDP audio (libvds)",
    .init = rdp_audio_init,
    .fini = rdp_audio_fini,
    .pcm_ops = &rdp_pcm_ops,
    .max_voices_out = 1,
    .max_voices_in = 1,
    .voice_size_out = sizeof(RdpVoiceOut),
    .voice_size_in = sizeof(RdpVoiceIn),
};

static void register_audio_rdp(void)
{
    audio_driver_register(&rdp_audio_driver);
}
type_init(register_audio_rdp);
