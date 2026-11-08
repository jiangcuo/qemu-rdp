/*
 * QEMU RDP server (libvds frontend): options, server lifecycle, QMP
 *
 * libvds runs its own threads. It wakes the main loop through the
 * "wakeup" callback (any thread) which only schedules a bottom half; the
 * bottom half calls vds_server_dispatch(), so every other callback runs in
 * the main thread with the BQL held.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/config-file.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/notify.h"
#include "qemu/option.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-ui.h"
#include "qapi/qapi-events-ui.h"
#include "qom/object.h"
#include "crypto/tlscreds.h"
#include "crypto/tlscredsx509.h"
#include "crypto/secret_common.h"
#include "monitor/monitor.h"
#include "monitor/hmp.h"
#include "monitor/qmp-helpers.h"
#include "system/system.h"
#include "ui/qemu-rdp.h"
#include "rdp.h"
#include "trace.h"

typedef struct RdpState {
    VdsServer *server;
    QEMUBH *dispatch_bh;
    Notifier exit_notifier;
    char *username;
    char *host;
    int port;
    const char *auth;
    uint32_t max_fps;
    uint64_t active_client_sid;
    char *debug_dir;
    uint32_t debug_flags;
} RdpState;

static RdpState rdp;

static QemuOptsList qemu_rdp_opts = {
    .name = "rdp",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_rdp_opts.head),
    .merge_lists = true,
    .desc = {
        { .name = "addr", .type = QEMU_OPT_STRING },
        { .name = "port", .type = QEMU_OPT_NUMBER },
        { .name = "tls-creds", .type = QEMU_OPT_STRING },
        { .name = "x509-cert-file", .type = QEMU_OPT_STRING },
        { .name = "x509-key-file", .type = QEMU_OPT_STRING },
        { .name = "auth", .type = QEMU_OPT_STRING },
        { .name = "username", .type = QEMU_OPT_STRING },
        { .name = "password-secret", .type = QEMU_OPT_STRING },
        { .name = "codecs", .type = QEMU_OPT_STRING },
        { .name = "max-fps", .type = QEMU_OPT_NUMBER },
        { .name = "bitrate", .type = QEMU_OPT_NUMBER },
        { .name = "encoder-threads", .type = QEMU_OPT_NUMBER },
        { .name = "encoder", .type = QEMU_OPT_STRING },
        { .name = "rendernode", .type = QEMU_OPT_STRING },
        { .name = "qp-max", .type = QEMU_OPT_NUMBER },
        { .name = "refine-qp", .type = QEMU_OPT_NUMBER },
        { .name = "refine-delay", .type = QEMU_OPT_NUMBER },
        { .name = "egfx-mixed", .type = QEMU_OPT_BOOL },
        { .name = "audio", .type = QEMU_OPT_BOOL },
        { .name = "audiodev", .type = QEMU_OPT_STRING },
        { .name = "clipboard", .type = QEMU_OPT_BOOL },
        { .name = "resize-on-connect", .type = QEMU_OPT_BOOL },
        { .name = "sas-hotkey", .type = QEMU_OPT_STRING },
        { .name = "debug-dir", .type = QEMU_OPT_STRING },
        { .name = "debug-input", .type = QEMU_OPT_BOOL },
        { .name = "debug-video", .type = QEMU_OPT_BOOL },
        { .name = "log", .type = QEMU_OPT_STRING },
        { .name = "log-format", .type = QEMU_OPT_STRING },
        { /* end of list */ }
    },
};

static void rdp_register_config(void)
{
    qemu_add_opts(&qemu_rdp_opts);
}
opts_init(rdp_register_config);

VdsServer *qemu_rdp_server(void)
{
    return rdp.server;
}

uint32_t qemu_rdp_max_fps(void)
{
    return rdp.max_fps;
}

bool qemu_rdp_has_client(void)
{
    return rdp.active_client_sid != 0;
}

/* Takes ownership of a libvds error string. */
static void rdp_error_take(Error **errp, char *vds_err, const char *what)
{
    error_setg(errp, "rdp: %s: %s", what, vds_err ? vds_err : "unknown error");
    vds_free_string(vds_err);
}

/* Callbacks, see VdsCallbacks in vds.h. */

static void rdp_cb_wakeup(void *opaque)
{
    /* Any thread: only schedule the bottom half. */
    qemu_bh_schedule(rdp.dispatch_bh);
}

static void rdp_dispatch_bh(void *opaque)
{
    if (rdp.server) {
        vds_server_dispatch(rdp.server);
    }
}

static void rdp_cb_key(void *opaque, uint8_t scancode, bool extended,
                       bool down)
{
    rdp_input_key(scancode, extended, down);
}

static void rdp_cb_unicode(void *opaque, uint16_t code, bool down)
{
    /* No guest keymap: unicode key events cannot be injected. */
    trace_rdp_input_unicode_ignored(down);
}

static void rdp_cb_lock_sync(void *opaque, bool scroll, bool num, bool caps)
{
    rdp_input_lock_sync(scroll, num, caps);
}

static void rdp_cb_pointer_abs(void *opaque, uint32_t head, uint32_t x,
                               uint32_t y, uint32_t width, uint32_t height)
{
    rdp_input_pointer_abs(head, x, y, width, height);
}

static void rdp_cb_pointer_rel(void *opaque, int32_t dx, int32_t dy)
{
    rdp_input_pointer_rel(dx, dy);
}

static void rdp_cb_button(void *opaque, uint32_t button, bool down)
{
    rdp_input_button(button, down);
}

static void rdp_cb_wheel(void *opaque, int32_t dx, int32_t dy)
{
    rdp_input_wheel(dx, dy);
}

static void rdp_cb_layout(void *opaque, const VdsMonitor *monitors,
                          uint32_t count)
{
    rdp_display_layout(monitors, count);
}

static void rdp_cb_clipboard_grab(void *opaque, const uint32_t *types,
                                  uint32_t count)
{
    rdp_clipboard_client_grab(types, count);
}

static void rdp_cb_clipboard_release(void *opaque)
{
    rdp_clipboard_client_release();
}

static void rdp_cb_clipboard_request(void *opaque, uint32_t type)
{
    rdp_clipboard_client_request(type);
}

static void rdp_cb_clipboard_data(void *opaque, uint32_t type,
                                  const uint8_t *data, size_t len)
{
    rdp_clipboard_client_data(type, data, len);
}

static void rdp_cb_client_state(void *opaque, const VdsClientInfo *info,
                                uint32_t state, const char *reason)
{
    RdpEventClient client = {
        .sid = info->sid,
        .peer = (char *)info->peer,
        .user = info->user[0] ? (char *)info->user : NULL,
    };

    trace_rdp_client_state(info->sid, info->peer, state);
    switch (state) {
    case VDS_CLIENT_CONNECTED:
        qapi_event_send_rdp_connected(&client);
        break;
    case VDS_CLIENT_INITIALIZED:
        rdp.active_client_sid = info->sid;
        qapi_event_send_rdp_initialized(&client);
        break;
    case VDS_CLIENT_DISCONNECTED:
        if (rdp.active_client_sid == info->sid) {
            rdp.active_client_sid = 0;
        }
        qapi_event_send_rdp_disconnected(&client, reason ? reason : "");
        break;
    default:
        break;
    }
}

static const VdsCallbacks rdp_callbacks = {
    .wakeup = rdp_cb_wakeup,
    .key = rdp_cb_key,
    .unicode = rdp_cb_unicode,
    .lock_sync = rdp_cb_lock_sync,
    .pointer_abs = rdp_cb_pointer_abs,
    .pointer_rel = rdp_cb_pointer_rel,
    .button = rdp_cb_button,
    .wheel = rdp_cb_wheel,
    .layout = rdp_cb_layout,
    .clipboard_grab = rdp_cb_clipboard_grab,
    .clipboard_release = rdp_cb_clipboard_release,
    .clipboard_request = rdp_cb_clipboard_request,
    .clipboard_data = rdp_cb_clipboard_data,
    .client_state = rdp_cb_client_state,
};

/* Option parsing */

static bool rdp_parse_codecs(const char *list, uint32_t *bits, Error **errp)
{
    g_auto(GStrv) names = g_strsplit(list, ":", -1);
    int i;

    *bits = 0;
    for (i = 0; names[i]; i++) {
        if (!strcmp(names[i], "avc420")) {
            *bits |= VDS_CODEC_AVC420;
        } else if (!strcmp(names[i], "remotefx")) {
            *bits |= VDS_CODEC_REMOTEFX;
        } else if (!strcmp(names[i], "bitmap")) {
            *bits |= VDS_CODEC_BITMAP;
        } else if (names[i][0]) {
            error_setg(errp, "rdp: unknown codec '%s'", names[i]);
            return false;
        }
    }
    if (!*bits) {
        error_setg(errp, "rdp: no codec allowed");
        return false;
    }
    return true;
}

static bool rdp_parse_auth(const char *auth, uint32_t *mode, Error **errp)
{
    if (!auth || !strcmp(auth, "nla")) {
        *mode = VDS_AUTH_NLA;
        rdp.auth = "nla";
    } else if (!strcmp(auth, "tls")) {
        *mode = VDS_AUTH_TLS;
        rdp.auth = "tls";
    } else if (!strcmp(auth, "none")) {
        *mode = VDS_AUTH_NONE;
        rdp.auth = "none";
    } else {
        error_setg(errp, "rdp: auth must be 'none', 'tls' or 'nla'");
        return false;
    }
    return true;
}

/*
 * Resolves sas-hotkey: what the client's Ctrl+Alt+End turns into for the
 * guest. Clients cannot send Ctrl+Alt+Del themselves, so they send
 * Ctrl+Alt+End and expect the server to inject the secure attention
 * sequence -- mstsc's behaviour, and the default here.
 */
static bool rdp_parse_sas(const char *sas, uint32_t *mode, Error **errp)
{
    if (!sas || !strcmp(sas, "ctrl-alt-end")) {
        *mode = VDS_SAS_CTRL_ALT_END;
    } else if (!strcmp(sas, "off")) {
        *mode = VDS_SAS_OFF;
    } else {
        error_setg(errp, "rdp: sas-hotkey must be 'ctrl-alt-end' or 'off'");
        return false;
    }
    return true;
}

/*
 * Resolves debug-dir: "on" (and the usual boolean spellings) means the
 * platform's temporary directory, %TEMP%\vds or /tmp/vds.
 */
static char *rdp_debug_dir(const char *value)
{
    if (!value) {
        return NULL;
    }
    if (!strcmp(value, "on") || !strcmp(value, "yes") ||
        !strcmp(value, "true")) {
        return g_build_filename(g_get_tmp_dir(), "vds", NULL);
    }
    return g_strdup(value);
}

static uint32_t rdp_debug_flags(QemuOpts *opts)
{
    uint32_t flags = 0;

    if (qemu_opt_get_bool(opts, "debug-input", false)) {
        flags |= VDS_DEBUG_INPUT;
    }
    if (qemu_opt_get_bool(opts, "debug-video", false)) {
        flags |= VDS_DEBUG_VIDEO;
    }
    return flags;
}

static bool rdp_load_tls_files(const char *cert_path, const char *key_path,
                               Error **errp)
{
    g_autofree char *cert = NULL;
    g_autofree char *key = NULL;
    g_autoptr(GError) gerr = NULL;
    char *vds_err = NULL;

    if (!g_file_get_contents(cert_path, &cert, NULL, &gerr) ||
        !g_file_get_contents(key_path, &key, NULL, &gerr)) {
        error_setg(errp, "rdp: cannot read TLS credentials: %s",
                   gerr->message);
        return false;
    }
    if (vds_server_set_tls(rdp.server, cert, key, &vds_err) != VDS_OK) {
        rdp_error_take(errp, vds_err, "invalid TLS credentials");
        return false;
    }
    return true;
}

static bool rdp_load_tls_creds(const char *id, Error **errp)
{
    Object *obj;
    QCryptoTLSCreds *creds;
    g_autofree char *dir = NULL;
    g_autofree char *cert_path = NULL;
    g_autofree char *key_path = NULL;

    obj = object_resolve_path_component(object_get_objects_root(), id);
    if (!obj || !object_dynamic_cast(obj, TYPE_QCRYPTO_TLS_CREDS_X509)) {
        error_setg(errp, "rdp: no tls-creds-x509 object with id '%s'", id);
        return false;
    }
    creds = QCRYPTO_TLS_CREDS(obj);
    if (!qcrypto_tls_creds_check_endpoint(creds,
                                          QCRYPTO_TLS_CREDS_ENDPOINT_SERVER,
                                          errp)) {
        return false;
    }
    dir = object_property_get_str(obj, "dir", errp);
    if (!dir) {
        return false;
    }
    cert_path = g_build_filename(dir, QCRYPTO_TLS_CREDS_X509_SERVER_CERT, NULL);
    key_path = g_build_filename(dir, QCRYPTO_TLS_CREDS_X509_SERVER_KEY, NULL);
    return rdp_load_tls_files(cert_path, key_path, errp);
}

static void rdp_exit_notify(Notifier *n, void *data)
{
    /*
     * Exit notifiers are run from an atexit() handler.  glibc destroys
     * thread-local storage before running atexit handlers, and libvds is
     * Rust: waking a tokio task or emitting a tracing event from here
     * panics with "cannot access a Thread Local Storage value during or
     * after destruction", which aborts the process.
     *
     * The process is exiting either way, so do not call back into libvds.
     * Leaking the server and the display heads is harmless: the OS reclaims
     * them (and stops the RDP thread) at exit.
     */
    rdp.server = NULL;
}

void qemu_rdp_init(Error **errp)
{
    ERRP_GUARD();
    QemuOpts *opts = qemu_opts_find(qemu_find_opts("rdp"), NULL);
    VdsConfig cfg;
    VdsLogConfig log_cfg = { .struct_size = sizeof(VdsLogConfig) };
    const char *str;
    const char *cert_file;
    const char *key_file;
    char *vds_err = NULL;
    uint32_t version = vds_abi_version();
    uint64_t port;

    if (!opts) {
        return;
    }
    /* Signed comparison: VDS_ABI_MINOR may be 0. */
    if ((version >> 16) != VDS_ABI_MAJOR ||
        (int)(version & 0xffff) < (int)VDS_ABI_MINOR) {
        error_setg(errp, "rdp: libvds ABI %u.%u is incompatible with %u.%u",
                   version >> 16, version & 0xffff,
                   VDS_ABI_MAJOR, VDS_ABI_MINOR);
        return;
    }

    log_cfg.filter = qemu_opt_get(opts, "log");
    str = qemu_opt_get(opts, "log-format");
    if (!str) {
        log_cfg.format = VDS_LOG_FORMAT_DEFAULT;
    } else if (!strcmp(str, "text")) {
        log_cfg.format = VDS_LOG_FORMAT_TEXT;
    } else if (!strcmp(str, "json")) {
        log_cfg.format = VDS_LOG_FORMAT_JSON;
    } else {
        error_setg(errp, "rdp: log-format must be 'text' or 'json'");
        return;
    }
    if (vds_log_init(&log_cfg, &vds_err) != VDS_OK) {
        rdp_error_take(errp, vds_err, "cannot initialize logging");
        return;
    }

    vds_config_init(&cfg);
    if (!rdp_parse_auth(qemu_opt_get(opts, "auth"), &cfg.auth, errp)) {
        return;
    }
    str = qemu_opt_get(opts, "codecs");
    if (str && !rdp_parse_codecs(str, &cfg.codecs, errp)) {
        return;
    }
    cfg.max_fps = qemu_opt_get_number(opts, "max-fps", cfg.max_fps);
    cfg.bitrate_kbps = qemu_opt_get_number(opts, "bitrate", 0);
    cfg.encoder_threads = qemu_opt_get_number(opts, "encoder-threads", 0);
    cfg.encoder = qemu_opt_get(opts, "encoder");
    cfg.render_node = qemu_opt_get(opts, "rendernode");
    cfg.qp_max = qemu_opt_get_number(opts, "qp-max", cfg.qp_max);
    cfg.refine_qp = qemu_opt_get_number(opts, "refine-qp", cfg.refine_qp);
    cfg.refine_delay_ms = qemu_opt_get_number(opts, "refine-delay",
                                              cfg.refine_delay_ms);
    cfg.egfx_mixed = qemu_opt_get_bool(opts, "egfx-mixed", cfg.egfx_mixed);
    cfg.audio = qemu_opt_get_bool(opts, "audio", true);
    cfg.clipboard = qemu_opt_get_bool(opts, "clipboard", true);
    cfg.resize_on_connect = qemu_opt_get_bool(opts, "resize-on-connect",
                                              true);
    if (!rdp_parse_sas(qemu_opt_get(opts, "sas-hotkey"), &cfg.sas_hotkey,
                       errp)) {
        return;
    }
    cfg.vm_name = qemu_name;
    rdp.debug_dir = rdp_debug_dir(qemu_opt_get(opts, "debug-dir"));
    cfg.debug_dir = rdp.debug_dir;
    cfg.debug_flags = rdp_debug_flags(opts);
    rdp.debug_flags = cfg.debug_flags;
    rdp.max_fps = cfg.max_fps;

    rdp.dispatch_bh = qemu_bh_new(rdp_dispatch_bh, NULL);
    rdp.server = vds_server_new(&cfg, &rdp_callbacks, NULL, &vds_err);
    if (!rdp.server) {
        rdp_error_take(errp, vds_err, "invalid configuration");
        return;
    }

    str = qemu_opt_get(opts, "tls-creds");
    cert_file = qemu_opt_get(opts, "x509-cert-file");
    key_file = qemu_opt_get(opts, "x509-key-file");
    if (!cert_file != !key_file) {
        error_setg(errp, "rdp: x509-cert-file and x509-key-file go together");
        return;
    }
    if (str && cert_file) {
        error_setg(errp, "rdp: tls-creds and x509-cert-file exclude each other");
        return;
    }
    if (str && !rdp_load_tls_creds(str, errp)) {
        return;
    }
    if (cert_file && !rdp_load_tls_files(cert_file, key_file, errp)) {
        return;
    }

    rdp.username = g_strdup(qemu_opt_get(opts, "username") ?: "");
    str = qemu_opt_get(opts, "password-secret");
    if (str) {
        g_autofree char *password = qcrypto_secret_lookup_as_utf8(str, errp);

        if (!password) {
            return;
        }
        if (vds_set_password(rdp.server, rdp.username, password, NULL,
                             VDS_CONNECTED_KEEP, false, &vds_err) != VDS_OK) {
            rdp_error_take(errp, vds_err, "cannot set the password");
            return;
        }
    } else if (cfg.auth != VDS_AUTH_NONE) {
        warn_report("rdp: no password-secret, clients are refused until "
                    "set_password is used");
    }

    rdp_input_init();
    rdp_display_init(rdp.server, errp);
    if (*errp) {
        return;
    }
    if (cfg.clipboard) {
        rdp_clipboard_init(rdp.server);
    }
    if (cfg.audio) {
        rdp_audio_capture_init(rdp.server, qemu_opt_get(opts, "audiodev"),
                               errp);
        if (*errp) {
            return;
        }
    }

    rdp.exit_notifier.notify = rdp_exit_notify;
    qemu_add_exit_notifier(&rdp.exit_notifier);

    port = qemu_opt_get_number(opts, "port", 0);
    if (qemu_opt_get(opts, "port")) {
        uint16_t bound = 0;

        if (port > 65535) {
            error_setg(errp, "rdp: invalid port %" PRIu64, port);
            return;
        }
        rdp.host = g_strdup(qemu_opt_get(opts, "addr") ?: "127.0.0.1");
        if (vds_server_listen(rdp.server, rdp.host, port, &bound,
                              &vds_err) != VDS_OK) {
            rdp_error_take(errp, vds_err, "cannot listen");
            return;
        }
        rdp.port = bound;
        trace_rdp_listen(rdp.host, bound);
    }
}

void qemu_rdp_set_password(const char *password, SetPasswordAction connected,
                           SetPasswordOptionsRdp *opts, Error **errp)
{
    char *vds_err = NULL;
    uint32_t action;

    if (!rdp.server) {
        error_setg(errp, "rdp: RDP is not enabled");
        return;
    }
    switch (connected) {
    case SET_PASSWORD_ACTION_FAIL:
        action = VDS_CONNECTED_FAIL;
        break;
    case SET_PASSWORD_ACTION_DISCONNECT:
        action = VDS_CONNECTED_DISCONNECT;
        break;
    default:
        action = VDS_CONNECTED_KEEP;
        break;
    }
    if (vds_set_password(rdp.server,
                         opts->username ? opts->username : rdp.username,
                         password, opts->domain, action,
                         opts->has_one_time && opts->one_time,
                         &vds_err) != VDS_OK) {
        rdp_error_take(errp, vds_err, "could not set password");
    }
}

void qmp_rdp_set_debug(bool has_input, bool input, bool has_video, bool video,
                       bool has_pdu, bool pdu, Error **errp)
{
    uint32_t flags;
    char *vds_err = NULL;

    if (!rdp.server) {
        error_setg(errp, "rdp: RDP is not enabled");
        return;
    }
    if (!rdp.debug_dir) {
        error_setg(errp, "rdp: no debug directory, start QEMU with "
                   "-rdp debug-dir=DIR");
        return;
    }
    flags = rdp.debug_flags;
    if (has_input) {
        flags = input ? flags | VDS_DEBUG_INPUT : flags & ~VDS_DEBUG_INPUT;
    }
    if (has_video) {
        flags = video ? flags | VDS_DEBUG_VIDEO : flags & ~VDS_DEBUG_VIDEO;
    }
    if (has_pdu) {
        flags = pdu ? flags | VDS_DEBUG_PDU : flags & ~VDS_DEBUG_PDU;
    }
    if (vds_server_set_debug(rdp.server, flags, &vds_err) != VDS_OK) {
        rdp_error_take(errp, vds_err, "cannot change the debug flags");
        return;
    }
    rdp.debug_flags = flags;
}

void qemu_rdp_set_password_expiry(time_t when, Error **errp)
{
    int64_t at = when == TIME_MAX ? VDS_EXPIRE_NEVER : (int64_t)when;

    if (!rdp.server) {
        error_setg(errp, "rdp: RDP is not enabled");
        return;
    }
    if (vds_set_password_expiry(rdp.server, at) != VDS_OK) {
        error_setg(errp, "rdp: could not set password expire time");
    }
}

static RdpClientInfoList *rdp_client_list(void)
{
    RdpClientInfoList *list = NULL;
    VdsClientInfo info[16];
    uint32_t n, i;

    n = vds_server_clients(rdp.server, info, ARRAY_SIZE(info));
    n = MIN(n, ARRAY_SIZE(info));
    for (i = 0; i < n; i++) {
        RdpClientInfo *c = g_new0(RdpClientInfo, 1);

        c->sid = info[i].sid;
        c->peer = g_strdup(info[i].peer);
        c->user = info[i].user[0] ? g_strdup(info[i].user) : NULL;
        c->codec = info[i].codec[0] ? g_strdup(info[i].codec) : NULL;
        c->connected_at = info[i].connected_at;
        c->fps = info[i].fps;
        c->bitrate_kbps = info[i].bitrate_kbps;
        c->has_rtt_ms = info[i].rtt_ms != VDS_RTT_UNKNOWN;
        c->rtt_ms = info[i].rtt_ms;
        QAPI_LIST_PREPEND(list, c);
    }
    return list;
}

/* Returns a g_malloc'ed copy of the certificate fingerprint, or NULL. */
static char *rdp_tls_fingerprint(void)
{
    char *vds_err = NULL;
    char *fp = vds_server_tls_fingerprint(rdp.server, &vds_err);
    char *ret;

    if (!fp) {
        warn_report("rdp: cannot get the TLS fingerprint: %s",
                    vds_err ? vds_err : "unknown error");
        vds_free_string(vds_err);
        return NULL;
    }
    ret = g_strdup(fp);
    vds_free_string(fp);
    return ret;
}

RdpInfo *qmp_query_rdp(Error **errp)
{
    RdpInfo *info = g_new0(RdpInfo, 1);

    info->enabled = rdp.server != NULL;
    if (!rdp.server) {
        return info;
    }
    if (rdp.host) {
        info->host = g_strdup(rdp.host);
        info->has_port = true;
        info->port = rdp.port;
    }
    info->auth = g_strdup(rdp.auth);
    info->fingerprint = rdp_tls_fingerprint();
    info->clients = rdp_client_list();
    if (rdp.debug_dir) {
        /* A string member has no has_ companion: NULL means "absent". */
        info->debug_dir = g_strdup(rdp.debug_dir);
        info->has_debug_input = true;
        info->debug_input = (rdp.debug_flags & VDS_DEBUG_INPUT) != 0;
        info->has_debug_video = true;
        info->debug_video = (rdp.debug_flags & VDS_DEBUG_VIDEO) != 0;
    }
    return info;
}

void hmp_info_rdp(Monitor *mon, const QDict *qdict)
{
    g_autoptr(RdpInfo) info = qmp_query_rdp(NULL);
    RdpClientInfoList *c;

    if (!info->enabled) {
        monitor_printf(mon, "Server: disabled\n");
        return;
    }
    if (info->host) {
        monitor_printf(mon, "Server: %s:%" PRId64 " (auth %s)\n",
                       info->host, info->port, info->auth);
    } else {
        monitor_printf(mon, "Server: not listening (auth %s)\n", info->auth);
    }
    if (info->fingerprint) {
        monitor_printf(mon, "Fingerprint: %s\n", info->fingerprint);
    }
    if (rdp.debug_dir) {
        monitor_printf(mon, "Debug: %s (input=%s video=%s)\n", rdp.debug_dir,
                       rdp.debug_flags & VDS_DEBUG_INPUT ? "on" : "off",
                       rdp.debug_flags & VDS_DEBUG_VIDEO ? "on" : "off");
    }
    if (!info->clients) {
        monitor_printf(mon, "Clients: none\n");
    }
    for (c = info->clients; c; c = c->next) {
        monitor_printf(mon, "Client %" PRId64 ": %s user=%s codec=%s "
                       "fps=%" PRId64 " bitrate=%" PRId64 "kbit/s",
                       c->value->sid, c->value->peer,
                       c->value->user ?: "-", c->value->codec ?: "-",
                       c->value->fps, c->value->bitrate_kbps);
        if (c->value->has_rtt_ms) {
            monitor_printf(mon, " rtt=%" PRId64 "ms", c->value->rtt_ms);
        }
        monitor_printf(mon, "\n");
    }
}

bool qmp_add_client_rdp(int fd, bool has_skipauth, bool skipauth,
                        bool has_tls, bool tls, Error **errp)
{
    intptr_t sock;

    if (!rdp.server) {
        error_setg(errp, "rdp: RDP is not enabled");
        return false;
    }
#ifdef _WIN32
    sock = (intptr_t)_get_osfhandle(fd);
#else
    sock = fd;
#endif
    if (vds_server_add_client(rdp.server, sock,
                              has_skipauth && skipauth) != VDS_OK) {
        error_setg(errp, "rdp: failed to add client");
        return false;
    }
#ifdef _WIN32
    /* libvds owns the SOCKET now: release the CRT descriptor only. */
    qemu_close_socket_osfhandle(fd);
#endif
    return true;
}
