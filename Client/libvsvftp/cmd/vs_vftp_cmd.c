/**
  ******************************************************************************
  * @file    lib/libvsvftp/cmd/vs_vftp_cmd.c
  * @brief   The vftp commands: `fburst` (burst service, CSP port 9) and `fpoll` (poll service, CSP port 8).
  *
  *          vcli --argv--> cmd_* --show_block (panel)--> setup --libvftp client--> link --> server
  *          libvftp events --on_event--> live bar (percent, chunks, speed, ETA) --report_ex--> summary line
  *
  *          The summary gives elapsed time, average throughput, NACK rounds, DATA packets (re)sent or received
  *          versus the chunks needed, control retries; a failure says whether it was local, the link, or the server
  *          refusing (with the server's reason). Ctrl-C stops a transfer; the same command again resumes it.
  *          Scripts parse the lines after the panel ("<n> bytes, crc32 ...", "... OK: ..."): keep their format.
  ******************************************************************************
  */
#define _GNU_SOURCE
#include <vs/vs_vftp.h>
#include <vs/vs_vftp_cmd.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vcli/vcli.h>
#include <vs/vs_csp.h>
#include <vs/vs_util.h>

#include "vftp/vftp.h"
#include "vftp/vftp_client.h"
#include "vftp_storage_posix.h"

typedef struct {
    uint8_t host;
    uint16_t chunk;
    bool rdp;
    bool crc32;
    uint32_t timeout_ms;
    uint32_t window;
    uint8_t retries;
    uint32_t gap_ms;
    uint32_t window_up; // packets per upload round: below the server's connection RX queue (16 packets on the OBC),
                        // or the round's STATUS_REQUEST is dropped with the overflow
} svc_settings_t;

static svc_settings_t s_burst = {0, 0, false, false, 10000, 64, 3, 2000, 12};
static svc_settings_t s_poll = {0, 0, false, false, 10000, 64, 3, 2000, 12};

static vftp_client_t s_client;  // holds a 1 MiB download bitmap (1 GB files): static, not on the stack
static vftp_posix_fs_t s_fsctx; // ground files: paths used as given
static vftp_storage_t s_fs;
static vs_vftp_csp_link_t s_linkctx;
static vftp_link_t s_link;

// ---- progress ----

#define COLOR_GOOD "\033[1;32m" // bright green: the part of the bar done, match, OK
#define COLOR_BAD "\033[1;31m"  // bright red: MISMATCH, FAILED
#define COLOR_OFF "\033[0m"     // the terminal's own text colour, readable on any background: the bar still to do

// Colours only on an interactive coloured console in text mode: scripts, cci programs and transcripts get the
// same text without escape sequences, and the bar keeps its # and . so it reads the same without colour.
static bool use_color(void)
{
    return vcli_console_color_active() && (vcli_console_get_mode() != VCLI_MODE_CCI);
}

// "OK", green on a coloured console.
static const char * word_ok(void)
{
    return use_color() ? COLOR_GOOD "OK" COLOR_OFF : "OK";
}

// "FAILED", red on a coloured console.
static const char * word_failed(void)
{
    return use_color() ? COLOR_BAD "FAILED" COLOR_OFF : "FAILED";
}

typedef struct {
    vcli_ctx_t * ctx;
    bool tty;
    uint32_t size;
    uint32_t total;
    uint32_t start_done;
    uint32_t start_bytes; // bytes an earlier attempt had already moved (resume)
    uint32_t done_now;    // chunks and bytes of the last progress event, to draw the bar again while waiting
    uint32_t bytes_now;
    uint32_t last_bytes;
    uint32_t rounds;
    unsigned int samples; // one-second speed samples taken
    bool verifying;
    bool started;    // START came: the transfer part is between two rules
    bool line_open;  // the bar (or the verify line) is drawn without its line end
    bool bar_active; // chunks are still moving: the bar may be drawn again while the link waits
    double bps;
    vs_timestamp_t t0;
    vs_timestamp_t t_last;
    vs_timestamp_t t_speed;
} progress_t;

// Seconds as hh:mm:ss, rounded up: time still to go never shows as 00:00:00.
static void hms(double s, char * out, size_t n)
{
    unsigned long t = (s > 0.0) ? (unsigned long) (s + 0.999) : 0ul;
    snprintf(out, n, "%02lu:%02lu:%02lu", t / 3600ul, (t / 60ul) % 60ul, t % 60ul);
}

// Updates the speed estimate and, on a terminal, draws the bar (at most every 0.2 s, and the last state).
static void draw(progress_t * p, uint32_t done, uint32_t bytes, bool last)
{
    vs_timestamp_t now;
    vs_clock_monotonic(&now);
    double since = vs_ts_diff(&p->t_speed, &now);
    if ((since >= 1.0) || last) {
        double inst = (since > 0.0) ? (double) (bytes - p->last_bytes) / since : 0.0;
        p->bps = (p->samples > 0u) ? (0.3 * inst + 0.7 * p->bps) : inst; // smoothed; falls while nothing arrives
        p->samples++;
        p->last_bytes = bytes;
        p->t_speed = now;
    }
    if (!p->tty || (!last && (vs_ts_diff(&p->t_last, &now) < 0.2))) {
        return;
    }
    p->t_last = now;
    p->line_open = true;
    double frac = p->total ? (double) done / (double) p->total : 1.0;
    enum { WIDTH = 30 };
    int fill = (int) (frac * WIDTH);
    fill = (fill < 0) ? 0 : (fill > WIDTH) ? WIDTH : fill;
    char done_part[WIDTH + 1];
    char todo_part[WIDTH + 1];
    memset(done_part, '#', (size_t) fill);
    done_part[fill] = '\0';
    memset(todo_part, '.', (size_t) (WIDTH - fill));
    todo_part[WIDTH - fill] = '\0';
    // the average since the start: the speed shown until the first one-second sample, and always the ETA's, as it
    // counts the waits between burst rounds and so does not jump with them
    double elapsed = vs_ts_diff(&p->t0, &now);
    double avg = (elapsed >= 0.2) ? (double) (bytes - p->start_bytes) / elapsed : 0.0;
    double speed = (p->samples > 0u) ? p->bps : avg;
    char eta[32] = "--:--:--";
    if ((avg > 1.0) && (bytes < p->size)) {
        hms((double) (p->size - bytes) / avg, eta, sizeof(eta));
    }
    const bool color = use_color();
    fprintf(p->ctx->out, "\r%5.1f%% [%s%s%s%s] %lu/%lu chunks %7.2f kB/s eta %s ", frac * 100.0,
            color ? COLOR_GOOD : "", done_part, color ? COLOR_OFF : "", todo_part, (unsigned long) done,
            (unsigned long) p->total, speed / 1024.0, eta);
    fflush(p->ctx->out);
}

// The link's cancel callback, polled at least every 200 ms while it waits for a packet: Ctrl-C stops the transfer,
// and while nothing arrives (the gap that ends a burst round, a slow link) the bar is drawn again, so its speed and
// ETA keep moving instead of standing still.
static bool poll_cancel(void * ctx)
{
    progress_t * p = ctx;
    if (p->bar_active) {
        draw(p, p->done_now, p->bytes_now, false);
    }
    return vcli_interrupted_cb(NULL);
}

// libvftp event callback: the transfer's lines and the bar.
static void on_event(void * ctx, const vftp_event_t * ev)
{
    progress_t * p = ctx;
    switch (ev->type) {
        case VFTP_EV_START:
            p->started = true;
            p->size = ev->size;
            p->total = ev->total_chunks;
            p->bar_active = (ev->total_chunks != 0u);
            vs_clock_monotonic(&p->t0);
            p->t_last = p->t0;
            p->t_speed = p->t0;
            if ((ev->crc32 == 0u) && (ev->size != 0u)) { // a large file: the server checksums it while sending
                fprintf(p->ctx->out, "%lu bytes, crc32 checked at the end, %lu chunks of %u bytes\r\n",
                        (unsigned long) ev->size, (unsigned long) ev->total_chunks, (unsigned int) ev->chunk_size);
            } else {
                fprintf(p->ctx->out, "%lu bytes, crc32 0x%08lx, %lu chunks of %u bytes\r\n", (unsigned long) ev->size,
                        (unsigned long) ev->crc32, (unsigned long) ev->total_chunks, (unsigned int) ev->chunk_size);
            }
            break;
        case VFTP_EV_RESUME:
            p->start_done = ev->done_chunks;
            p->start_bytes = ev->bytes_done;
            p->done_now = ev->done_chunks;
            p->bytes_now = ev->bytes_done;
            p->last_bytes = ev->bytes_done;
            fprintf(p->ctx->out, "resuming: %lu/%lu chunks already transferred by an earlier attempt\r\n",
                    (unsigned long) ev->done_chunks, (unsigned long) ev->total_chunks);
            break;
        case VFTP_EV_PROGRESS:
            p->done_now = ev->done_chunks;
            p->bytes_now = ev->bytes_done;
            draw(p, ev->done_chunks, ev->bytes_done, ev->done_chunks >= p->total);
            if (ev->done_chunks >= p->total) {
                p->bar_active = false; // the last state is drawn; what follows (crc32 check) has its own lines
            }
            break;
        case VFTP_EV_ROUND:
            p->rounds = ev->round;
            if (!p->tty) {
                fprintf(p->ctx->out, "round %lu: %lu/%lu chunks, %lu missing\r\n", (unsigned long) ev->round,
                        (unsigned long) ev->done_chunks, (unsigned long) ev->total_chunks,
                        (unsigned long) ev->missing);
            }
            break;
        case VFTP_EV_VERIFY:
            // a large file: the server checksums it a step per request (its CRC reads the card)
            p->bar_active = false; // the verify line takes the bar's place
            if (p->tty || !p->verifying) {
                fprintf(p->ctx->out, "%sserver verifying crc32: %lu/%lu bytes (%.1f%%)%s", p->tty ? "\r" : "",
                        (unsigned long) ev->bytes_done, (unsigned long) ev->size,
                        ev->size ? 100.0 * (double) ev->bytes_done / (double) ev->size : 100.0,
                        p->tty ? "   " : "\r\n");
                fflush(p->ctx->out);
                p->line_open = p->tty;
            }
            p->verifying = true;
            break;
        case VFTP_EV_CRC: {
            p->bar_active = false;
            const bool same = (ev->remote_crc == ev->local_crc);
            const bool color = use_color();
            fprintf(p->ctx->out, "%scrc32 remote 0x%08lx local 0x%08lx %s%s%s\r\n", p->line_open ? "\r\n" : "",
                    (unsigned long) ev->remote_crc, (unsigned long) ev->local_crc,
                    color ? (same ? COLOR_GOOD : COLOR_BAD) : "", same ? "match" : "MISMATCH", color ? COLOR_OFF : "");
            p->line_open = false;
            break;
        }
        default:
            break;
    }
}

// ---- running one call ----

// Prepares the client for one call: storage, CSP link (cancel: Ctrl-C, and the bar while waiting), settings, event
// callback.
static void setup(const svc_settings_t * s, uint8_t port, progress_t * p, vcli_ctx_t * ctx)
{
    memset(p, 0, sizeof(*p));
    p->ctx = ctx;
    p->tty = isatty(fileno(ctx->out));
    uint32_t opts = (s->rdp ? CSP_O_RDP : 0u) | (s->crc32 ? CSP_O_CRC32 : 0u);
    vftp_storage_posix_init(&s_fs, &s_fsctx, "");
    vs_vftp_csp_link_init(&s_link, &s_linkctx, s->host, port, opts, s->timeout_ms, poll_cancel, p);
    vftp_client_cfg_t cfg;
    vftp_client_default_cfg(&cfg);
    cfg.link = &s_link;
    cfg.fs = &s_fs;
    cfg.chunk_size = s->chunk;
    cfg.timeout_ms = s->timeout_ms;
    cfg.gap_ms = s->gap_ms;
    cfg.window = s->window;
    cfg.window_up = s->window_up;
    cfg.retries = s->retries;
    cfg.max_stall = 6;
    cfg.on_event = on_event;
    cfg.event_ctx = p;
    vftp_client_init(&s_client, &cfg);
}

// vftp result (or the server's reason for VFTP_REMOTE) as a vs_err_t.
static vs_err_t to_vs(vftp_ret_t st, uint8_t remote)
{
    int code = (st == VFTP_REMOTE) ? remote : (int) st;
    switch (code) {
        case VFTP_OK:
            return VS_OK;
        case VFTP_NOENT:
            return VS_ERR_NOT_FOUND;
        case VFTP_INVAL:
        case VFTP_BADNAME:
            return VS_ERR_ARG;
        case VFTP_NOSPC:
            return VS_ERR_FULL;
        case VFTP_IO:
            return VS_ERR_IO;
        case VFTP_EXISTS:
            return VS_ERR_EXIST;
        case VFTP_NOTSUP:
            return VS_ERR_NOT_SUPPORTED;
        case VFTP_NOMEM:
            return VS_ERR_NO_MEM;
        case VFTP_BUSY:
            return VS_ERR_BUSY;
        case VFTP_TIMEOUT:
            return VS_ERR_TIMEOUT;
        case VFTP_LINK:
            return VS_ERR_CONN_RESET;
        case VFTP_RANGE:
            return VS_ERR_RANGE;
        case VFTP_ISDIR:
            return VS_ERR_IS_DIR;
        default:
            return VS_ERR_DATA;
    }
}

// The result of a call: closes the bar and the transfer part, prints OK with statistics or FAILED
// with the reason; transfer: also the resume hint.
static vs_err_t report_ex(vcli_ctx_t * ctx, progress_t * p, vftp_ret_t st, const char * what, bool upload,
                          bool transfer)
{
    vs_timestamp_t now;
    vs_clock_monotonic(&now);
    const vftp_client_stats_t * s = &s_client.stats;
    if (p->line_open) {
        fprintf(ctx->out, "\r\n");
    }
    if (p->started) {
        vcli_panel_rule(ctx->out);
    }
    if ((st != VFTP_OK) && vcli_interrupted()) {
        fprintf(ctx->out, "%s %s: interrupted (Ctrl-C)\r\n", what, word_failed());
        if (transfer) {
            fprintf(ctx->out, "  run the same command again: completed chunks are kept and the transfer resumes\r\n");
        }
        return VS_ERR_INTERRUPTED;
    }
    if (st == VFTP_OK) {
        double t = (p->size != 0u) ? vs_ts_diff(&p->t0, &now) : 0.0;
        uint32_t moved = (p->total > p->start_done) ? (p->total - p->start_done) : 0u;
        uint32_t pkts = upload ? s->data_tx : s->data_rx;
        fprintf(ctx->out, "%s %s: %lu bytes in %.2f s (%.2f kB/s avg)", what, word_ok(), (unsigned long) p->size, t,
                (t > 0.0) ? (double) p->size / 1024.0 / t : 0.0);
        if (p->total != 0u) {
            fprintf(ctx->out, ", %lu round%s, %lu DATA packets for %lu chunks (%.0f%% extra), %lu retries",
                    (unsigned long) s->rounds, (s->rounds == 1u) ? "" : "s", (unsigned long) pkts,
                    (unsigned long) moved, moved ? 100.0 * ((double) pkts - (double) moved) / (double) moved : 0.0,
                    (unsigned long) s->retries);
        }
        fprintf(ctx->out, "\r\n");
    } else if (st == VFTP_REMOTE) {
        fprintf(ctx->out, "%s %s: server refused: %s%s\r\n", what, word_failed(), vftp_strerror(s_client.remote_ret),
                (s_client.remote_ret == VFTP_BADNAME) ? " (the OBC's FAT takes 8.3 names only: NAME.EXT, <= 8 + 3)"
                : (s_client.remote_ret == VFTP_IO) ? " (storage keeps failing on the OBC; completed chunks are kept)"
                                                   : "");
    } else {
        fprintf(ctx->out, "%s %s: %s%s\r\n", what, word_failed(), vftp_strerror(st),
                (st == VFTP_TIMEOUT)        ? " -- no reply: link down, wrong node/port, service busy, or too lossy"
                : (st == VFTP_LINK)         ? " -- connection lost (CSP/RDP gave up)"
                : (st == VFTP_NOENT)        ? " (local file)"
                : (st == VFTP_ISDIR)        ? " (local path: give a file name, e.g. ./NAME)"
                : (st == VFTP_CRC_MISMATCH) ? " -- resume state discarded: run the same command again for a clean copy"
                                            : "");
        if (transfer && ((st == VFTP_LINK) || (st == VFTP_TIMEOUT))) {
            fprintf(ctx->out, "  run the same command again: completed chunks are kept and the transfer resumes\r\n");
        }
    }
    return to_vs(st, s_client.remote_ret);
}

// report_ex() for a transfer.
static vs_err_t report(vcli_ctx_t * ctx, progress_t * p, vftp_ret_t st, const char * what, bool upload)
{
    return report_ex(ctx, p, st, what, upload, true);
}

// File name part of a path.
static const char * base_name(const char * path)
{
    const char * b = strrchr(path, '/');
    return b ? b + 1 : path;
}

// ---- settings commands ----

typedef enum { SHOW_SETTINGS, SHOW_DOWNLOAD, SHOW_UPLOAD } show_op_t;

// The settings a command runs with, as a vcli panel; a transfer adds its files and a rule before the transfer:
//
//   fburst download  [this node 1] << [node 3 port 9]
//     file   local [/tmp/abc.h] << remote [/tmp/xyz.c]
//     link   RDP     [on]        CRC32   [on]        timeout [10000 ms]  retries [3]
//     burst  chunk   [100 B]     window  [64]        gap     [2000 ms]
//   ----------------------------------------------------------------------------
//
// `server` / `opts` show the settings alone, with both burst windows.
static void show_block(vcli_ctx_t * ctx, const svc_settings_t * s, bool burst, show_op_t op, const char * remote,
                       const char * local)
{
    bool up = (op == SHOW_UPLOAD);
    const char * arrow = (op == SHOW_SETTINGS) ? VCLI_PANEL_WITH : up ? VCLI_PANEL_OUT : VCLI_PANEL_IN;
    char name[24];
    char self[24];
    char peer[32];
    snprintf(name, sizeof(name), "%s%s", burst ? "fburst" : "fpoll",
             (op == SHOW_SETTINGS) ? "" : up ? " upload" : " download");
    snprintf(self, sizeof(self), "this node %u", vs_csp_address());
    snprintf(peer, sizeof(peer), "node %u port %u", s->host, burst ? VFTP_BURST_CSP_PORT : VFTP_POLL_CSP_PORT);
    vcli_panel_title(ctx->out, name, self, arrow, peer);
    if (op != SHOW_SETTINGS) {
        vcli_panel_text(ctx->out, "file", "local [%s] %s remote [%s]", local, arrow, remote);
    }

    char timeout[24];
    char retries[8];
    snprintf(timeout, sizeof(timeout), "%lu ms", (unsigned long) s->timeout_ms);
    snprintf(retries, sizeof(retries), "%u", s->retries);
    vcli_panel_row(ctx->out, "link",
                   VCLI_CELLS({"RDP", s->rdp ? "on" : "off"}, {"CRC32", s->crc32 ? "on" : "off"},
                              {"timeout", timeout}, {"retries", retries}));

    char chunk[16];
    char window[16];
    char window_up[16];
    char gap[24];
    if (s->chunk) {
        snprintf(chunk, sizeof(chunk), "%u B", s->chunk);
    } else {
        snprintf(chunk, sizeof(chunk), "auto");
    }
    snprintf(window, sizeof(window), "%lu", (unsigned long) (up ? s->window_up : s->window));
    snprintf(window_up, sizeof(window_up), "%lu", (unsigned long) s->window_up);
    snprintf(gap, sizeof(gap), "%lu ms", (unsigned long) s->gap_ms);
    if (!burst) {
        vcli_panel_row(ctx->out, "poll", VCLI_CELLS({"chunk", chunk}, {"mode", "stop-and-wait"}));
    } else if (op == SHOW_SETTINGS) {
        vcli_panel_row(ctx->out, "burst",
                       VCLI_CELLS({"chunk", chunk}, {"window", window}, {"gap", gap}, {"upload", window_up}));
    } else {
        vcli_panel_row(ctx->out, "burst", VCLI_CELLS({"chunk", chunk}, {"window", window}, {"gap", gap}));
    }
    if (op != SHOW_SETTINGS) {
        vcli_panel_rule(ctx->out);
    }
}

// Panel of the settings alone (`server`, `opts`).
static void show(vcli_ctx_t * ctx, const svc_settings_t * s, bool burst)
{
    show_block(ctx, s, burst, SHOW_SETTINGS, NULL, NULL);
}

// server [node] [chunk|0] [rdp on|off] [crc32 on|off]: changes nothing unless every argument is valid.
static vs_err_t cmd_server(vcli_ctx_t * ctx, svc_settings_t * s, bool burst)
{
    svc_settings_t n = *s; // all or nothing
    if (ctx->argc > 1) {
        if (vs_str_to_u8(ctx->argv[1], &n.host) || (n.host > VS_CSP_MAX_NODE)) {
            return VS_ERR_ARG;
        }
    }
    if (ctx->argc > 2) {
        uint32_t c;
        if (vs_str_to_u32(ctx->argv[2], &c) || (c > 65535u)) {
            return VS_ERR_ARG;
        }
        n.chunk = (uint16_t) c;
    }
    if ((ctx->argc > 3) && vs_str_to_bool(ctx->argv[3], &n.rdp)) {
        return VS_ERR_ARG;
    }
    if ((ctx->argc > 4) && vs_str_to_bool(ctx->argv[4], &n.crc32)) {
        return VS_ERR_ARG;
    }
    *s = n;
    show(ctx, s, burst);
    return VS_OK;
}

// Positive number up to max.
static vs_err_t opt_u32(const char * a, uint32_t max, uint32_t * out)
{
    uint32_t v;
    if (vs_str_to_u32(a, &v) || (v == 0u) || (v > max)) {
        return VS_ERR_ARG;
    }
    *out = v;
    return VS_OK;
}

// opts [timeout_ms] [retries] [window] [gap_ms] [window_up]
static vs_err_t cmd_opts(vcli_ctx_t * ctx, svc_settings_t * s, bool burst)
{
    svc_settings_t n = *s;
    uint32_t retries = n.retries;
    if (((ctx->argc > 1) && opt_u32(ctx->argv[1], UINT32_MAX, &n.timeout_ms)) ||
        ((ctx->argc > 2) && opt_u32(ctx->argv[2], 255u, &retries)) ||
        ((ctx->argc > 3) && opt_u32(ctx->argv[3], UINT32_MAX, &n.window)) ||
        ((ctx->argc > 4) && opt_u32(ctx->argv[4], UINT32_MAX, &n.gap_ms)) ||
        ((ctx->argc > 5) && opt_u32(ctx->argv[5], UINT32_MAX, &n.window_up))) {
        return VS_ERR_ARG;
    }
    n.retries = (uint8_t) retries;
    *s = n;
    show(ctx, s, burst);
    return VS_OK;
}

// fburst server ...
static int cmd_burst_server(vcli_ctx_t * ctx)
{
    return cmd_server(ctx, &s_burst, true);
}
// fpoll server ...
static int cmd_poll_server(vcli_ctx_t * ctx)
{
    return cmd_server(ctx, &s_poll, false);
}
// fburst opts ...
static int cmd_burst_opts(vcli_ctx_t * ctx)
{
    return cmd_opts(ctx, &s_burst, true);
}
// fpoll opts ...
static int cmd_poll_opts(vcli_ctx_t * ctx)
{
    return cmd_opts(ctx, &s_poll, false);
}

// ---- transfer commands ----

// fburst upload <local file> <remote path>
static int cmd_burst_upload(vcli_ctx_t * ctx)
{
    progress_t p;
    show_block(ctx, &s_burst, true, SHOW_UPLOAD, ctx->argv[2], ctx->argv[1]);
    setup(&s_burst, VFTP_BURST_CSP_PORT, &p, ctx);
    return report(ctx, &p, vftp_burst_upload(&s_client, ctx->argv[1], ctx->argv[2]), "burst upload", true);
}

// fburst download <remote path> [local file]
static int cmd_burst_download(vcli_ctx_t * ctx)
{
    progress_t p;
    const char * local = (ctx->argc > 2) ? ctx->argv[2] : base_name(ctx->argv[1]);
    show_block(ctx, &s_burst, true, SHOW_DOWNLOAD, ctx->argv[1], local);
    setup(&s_burst, VFTP_BURST_CSP_PORT, &p, ctx);
    return report(ctx, &p, vftp_burst_download(&s_client, ctx->argv[1], local), "burst download", false);
}

// fpoll upload <local file> <remote path>
static int cmd_poll_upload(vcli_ctx_t * ctx)
{
    progress_t p;
    show_block(ctx, &s_poll, false, SHOW_UPLOAD, ctx->argv[2], ctx->argv[1]);
    setup(&s_poll, VFTP_POLL_CSP_PORT, &p, ctx);
    return report(ctx, &p, vftp_poll_upload(&s_client, ctx->argv[1], ctx->argv[2]), "poll upload", true);
}

// fpoll download <remote path> [local file]
static int cmd_poll_download(vcli_ctx_t * ctx)
{
    progress_t p;
    const char * local = (ctx->argc > 2) ? ctx->argv[2] : base_name(ctx->argv[1]);
    show_block(ctx, &s_poll, false, SHOW_DOWNLOAD, ctx->argv[1], local);
    setup(&s_poll, VFTP_POLL_CSP_PORT, &p, ctx);
    return report(ctx, &p, vftp_poll_download(&s_client, ctx->argv[1], local), "poll download", false);
}

// fburst rm <remote path>: also removes the server's resume state of that file.
static int cmd_burst_rm(vcli_ctx_t * ctx)
{
    progress_t p;
    setup(&s_burst, VFTP_BURST_CSP_PORT, &p, ctx);
    vftp_ret_t st = vftp_burst_remove(&s_client, ctx->argv[1]);
    if (st == VFTP_OK) {
        fprintf(ctx->out, "removed %s\r\n", ctx->argv[1]);
        return VS_OK;
    }
    return report_ex(ctx, &p, st, "rm", false, false);
}

// ---- listing: method 2 (ls) and method 1 (list_regen) ----

// mtime on the wire: seconds since 1970 (UTC), 0 = unknown.
static void entry_date(uint32_t t, char * out, size_t n)
{
    if (t == 0u) {
        snprintf(out, n, "%-16s", "-"); // this filesystem keeps no timestamps
        return;
    }
    vftp_civil_t c;
    vftp_unix_to_civil(t, &c);
    snprintf(out, n, "%04u-%02u-%02u %02u:%02u", (unsigned) c.year, (unsigned) c.month, (unsigned) c.day,
             (unsigned) c.hour, (unsigned) c.minute);
}

// vftp list callback: one directory entry.
static void ls_entry(void * ctx, uint32_t total, uint32_t index, const vftp_list_entry_t * e)
{
    vcli_ctx_t * c = ctx;
    char date[24];
    (void) total;
    entry_date(e->mtime, date, sizeof(date));
    // NAME/ = directory; NAME~ = only the start of a longer name (download it by its full path)
    fprintf(c->out, "%5lu %10lu  %s  %s%s%s\r\n", (unsigned long) index, (unsigned long) e->size, date, e->name,
            (e->flags & VFTP_ENTRY_DIR) ? "/" : "", (e->flags & VFTP_ENTRY_NAME_CUT) ? "~" : "");
}

// fburst ls <dir>: the listing, page by page (method 2).
static int cmd_burst_ls(vcli_ctx_t * ctx)
{
    progress_t p;
    uint32_t total = 0;
    setup(&s_burst, VFTP_BURST_CSP_PORT, &p, ctx);
    vftp_ret_t st = vftp_burst_list(&s_client, ctx->argv[1], ls_entry, ctx, &total);
    if (st == VFTP_OK) {
        fprintf(ctx->out, "%lu entr%s in %s\r\n", (unsigned long) total, (total == 1u) ? "y" : "ies",
                ctx->argv[1][0] ? ctx->argv[1] : "/");
        return VS_OK;
    }
    if ((st == VFTP_REMOTE) && (s_client.remote_ret == VFTP_NOENT)) {
        fprintf(ctx->out, "ls %s: no such directory\r\n", ctx->argv[1]); // an empty one lists 0 entries
        return VS_ERR_NOT_FOUND;
    }
    return report_ex(ctx, &p, st, "ls", false, false);
}

// true for "down" or "nodown", *down set accordingly.
static bool is_mode(const char * a, bool * down)
{
    if ((strcmp(a, "down") == 0) || (strcmp(a, "nodown") == 0)) {
        *down = (a[0] == 'd');
        return true;
    }
    return false;
}

// fburst list_regen <dir> [local] [down|nodown] -- default nodown: the server writes LIST.TXT in its root (one
// file, whatever directory) and only its location is printed; `down` also fetches and prints it.
static int cmd_burst_list_regen(vcli_ctx_t * ctx)
{
    progress_t p;
    char remote[VFTP_MAX_PATH];
    const char * local = "list.txt";
    bool down = false;
    for (int i = 2; i < ctx->argc; i++) {
        if (!is_mode(ctx->argv[i], &down)) {
            if (i != 2) {
                return VS_ERR_ARG;
            }
            local = ctx->argv[i];
        }
    }
    setup(&s_burst, VFTP_BURST_CSP_PORT, &p, ctx);
    vftp_ret_t st = vftp_burst_list_regen(&s_client, ctx->argv[1], remote);
    if (st != VFTP_OK) {
        return report_ex(ctx, &p, st, "list_regen", false, false);
    }
    if (!down) {
        fprintf(ctx->out, "server wrote %s (its root) listing %s; fetch it with: fburst download %s [local file]\r\n",
                remote, ctx->argv[1][0] ? ctx->argv[1] : "/", remote);
        return VS_OK;
    }
    fprintf(ctx->out, "server wrote %s, downloading it to %s\r\n", remote, local);
    show_block(ctx, &s_burst, true, SHOW_DOWNLOAD, remote, local);
    setup(&s_burst, VFTP_BURST_CSP_PORT, &p, ctx);
    st = vftp_burst_download(&s_client, remote, local);
    vs_err_t rc = report(ctx, &p, st, "LIST.TXT download", false);
    if (st == VFTP_OK) {
        FILE * f = fopen(local, "r");
        char line[128];
        unsigned long n = 0;
        while (f && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = '\0';
            fprintf(ctx->out, "%s\r\n", line);
            n += (line[0] != '#') ? 1u : 0u; // "# <dir>" first, "# <n> entries" last
        }
        if (f) {
            fclose(f);
        }
        fprintf(ctx->out,
                "%lu entries (size, date or '-' when the filesystem keeps none, name; NAME/ = directory, dates UTC)\r\n",
                n);
    }
    return rc;
}

// ---- listing method 3: count, download by index (both services) ----

// count <dir>: number of entries (method 3).
static vs_err_t index_count(vcli_ctx_t * ctx, bool burst)
{
    progress_t p;
    vftp_index_rep_t info;
    setup(burst ? &s_burst : &s_poll, burst ? VFTP_BURST_CSP_PORT : VFTP_POLL_CSP_PORT, &p, ctx);
    vftp_ret_t st = burst ? vftp_burst_index(&s_client, ctx->argv[1], VFTP_INDEX_COUNT_ONLY, &info)
                          : vftp_poll_index(&s_client, ctx->argv[1], VFTP_INDEX_COUNT_ONLY, &info);
    if (st != VFTP_OK) {
        return report_ex(ctx, &p, st, "count", false, false);
    }
    fprintf(ctx->out, "%lu entries in %s (index 0..%lu, same order as ls)\r\n", (unsigned long) info.total,
            ctx->argv[1][0] ? ctx->argv[1] : "/", (unsigned long) (info.total ? info.total - 1u : 0u));
    return VS_OK;
}

// download_index <dir> <index> [local file]: the n-th entry, no name needed (method 3).
static vs_err_t index_download(vcli_ctx_t * ctx, bool burst)
{
    progress_t p;
    uint32_t index;
    char remote[VFTP_MAX_PATH] = "";
    if (vs_str_to_u32(ctx->argv[2], &index)) {
        return VS_ERR_ARG;
    }
    const char * local = (ctx->argc > 3) ? ctx->argv[3] : NULL;
    char what[VFTP_MAX_PATH + 24];
    snprintf(what, sizeof(what), "%s entry %lu", ctx->argv[1][0] ? ctx->argv[1] : "/", (unsigned long) index);
    show_block(ctx, burst ? &s_burst : &s_poll, burst, SHOW_DOWNLOAD, what, local ? local : "(its own name)");
    setup(burst ? &s_burst : &s_poll, burst ? VFTP_BURST_CSP_PORT : VFTP_POLL_CSP_PORT, &p, ctx);
    vftp_ret_t st = burst ? vftp_burst_download_index(&s_client, ctx->argv[1], index, local, remote)
                          : vftp_poll_download_index(&s_client, ctx->argv[1], index, local, remote);
    if (remote[0] != '\0') {
        fprintf(ctx->out, "index %lu of %s = %s%s\r\n", (unsigned long) index, ctx->argv[1], remote,
                (st == VFTP_ISDIR) ? "/ (a directory)" : (st == VFTP_BADNAME) ? "~ (name cut)" : "");
    }
    if (st == VFTP_BADNAME) {
        fprintf(ctx->out,
                "the server's name is longer than %u characters and came cut: download it by its full path\r\n",
                (unsigned) VFTP_MAX_FILE_NAME);
        return VS_ERR_ARG;
    }
    if ((st == VFTP_REMOTE) && (s_client.remote_ret == VFTP_RANGE)) {
        fprintf(ctx->out, "index %lu out of range: use `%s count %s` to see how many entries there are\r\n",
                (unsigned long) index, burst ? "fburst" : "fpoll", ctx->argv[1]);
        return VS_ERR_RANGE;
    }
    return report_ex(ctx, &p, st, burst ? "burst download_index" : "poll download_index", false, st != VFTP_ISDIR);
}

// fburst count <dir>
static int cmd_burst_count(vcli_ctx_t * ctx)
{
    return index_count(ctx, true);
}
// fpoll count <dir>
static int cmd_poll_count(vcli_ctx_t * ctx)
{
    return index_count(ctx, false);
}
// fburst download_index <dir> <index> [local file]
static int cmd_burst_download_index(vcli_ctx_t * ctx)
{
    return index_download(ctx, true);
}
// fpoll download_index <dir> <index> [local file]
static int cmd_poll_download_index(vcli_ctx_t * ctx)
{
    return index_download(ctx, false);
}

// ---- completion: local paths ----

// Tab: local files and directories matching the word being typed.
static void complete_local_path(vcli_ctx_t * ctx)
{
    const char * word = ctx->argv[ctx->argc - 1];
    char dir[512];
    const char * slash = strrchr(word, '/');
    size_t dlen = slash ? (size_t) (slash - word) + 1u : 0u;
    if (dlen >= sizeof(dir)) {
        return;
    }
    memcpy(dir, word, dlen);
    dir[dlen] = '\0';
    DIR * d = opendir(dlen ? dir : ".");
    if (d == NULL) {
        return;
    }
    struct dirent * e;
    while ((e = readdir(d)) != NULL) {
        if ((strcmp(e->d_name, ".") == 0) || (strcmp(e->d_name, "..") == 0)) {
            continue;
        }
        char cand[1024];
        snprintf(cand, sizeof(cand), "%s%s", dir, e->d_name);
        struct stat st;
        if ((stat(cand, &st) == 0) && S_ISDIR(st.st_mode)) {
            strncat(cand, "/", sizeof(cand) - strlen(cand) - 1u);
        }
        vcli_complete_add(ctx, cand);
    }
    closedir(d);
}

// Tab on upload: a local file first.
static void complete_upload(vcli_ctx_t * ctx, int arg)
{
    if (arg == 1) {
        complete_local_path(ctx);
    }
}

// Tab on download: the local file second.
static void complete_download(vcli_ctx_t * ctx, int arg)
{
    if (arg == 2) {
        complete_local_path(ctx);
    }
}

// Tab on download_index: the local file third.
static void complete_download_index(vcli_ctx_t * ctx, int arg)
{
    if (arg == 3) {
        complete_local_path(ctx);
    }
}

// Tab on server: on, off for rdp and crc32.
static void complete_onoff_server(vcli_ctx_t * ctx, int arg)
{
    if ((arg == 3) || (arg == 4)) {
        vcli_complete_add(ctx, "on");
        vcli_complete_add(ctx, "off");
    }
}

// Tab on list_regen: a local file, down, nodown.
static void complete_list_regen(vcli_ctx_t * ctx, int arg)
{
    if (arg >= 2) {
        vcli_complete_add(ctx, "down");
        vcli_complete_add(ctx, "nodown");
        if (arg == 2) {
            complete_local_path(ctx);
        }
    }
}

// ---- tables ----

static const vcli_cmd_t burst_cmds[] = {
    {.name = "server", .help = "show/set node, chunk size, rdp, crc32",
     .usage = "[node] [chunk|0=auto] [rdp on|off] [crc32 on|off]", .handler = cmd_burst_server,
     .completer = complete_onoff_server, .opt_args = 4},
    {.name = "opts", .help = "show/set timeout, retries, window (download), burst gap, upload window",
     .usage = "[timeout_ms] [retries] [window] [gap_ms] [window_up]", .handler = cmd_burst_opts, .opt_args = 5},
    {.name = "upload", .help = "upload a file (resumable)", .usage = "<local file> <remote path>",
     .handler = cmd_burst_upload, .completer = complete_upload, .min_args = 2},
    {.name = "download", .help = "download a file (resumable)", .usage = "<remote path> [local file]",
     .handler = cmd_burst_download, .completer = complete_download, .min_args = 1, .opt_args = 1},
    {.name = "ls", .help = "list a directory page by page (method 2)", .usage = "<dir>", .handler = cmd_burst_ls,
     .min_args = 1},
    {.name = "list_regen",
     .help = "server writes LIST.TXT (its root) listing <dir>; down: also fetch and print it (method 1)",
     .usage = "<dir> [local file] [down|nodown, default nodown]", .handler = cmd_burst_list_regen,
     .completer = complete_list_regen, .min_args = 1, .opt_args = 2},
    {.name = "rm", .help = "remove a file (and its resume state)", .usage = "<remote path>", .handler = cmd_burst_rm,
     .min_args = 1},
    {.name = "count", .help = "number of entries in a directory (method 3)", .usage = "<dir>",
     .handler = cmd_burst_count, .min_args = 1},
    {.name = "download_index", .help = "download the n-th entry of a directory, no name needed (method 3)",
     .usage = "<dir> <index> [local file]", .handler = cmd_burst_download_index,
     .completer = complete_download_index, .min_args = 2, .opt_args = 1},
};

static const vcli_cmd_t poll_cmds[] = {
    {.name = "server", .help = "show/set node, chunk size, rdp, crc32",
     .usage = "[node] [chunk|0=auto] [rdp on|off] [crc32 on|off]", .handler = cmd_poll_server,
     .completer = complete_onoff_server, .opt_args = 4},
    {.name = "opts", .help = "show/set timeout, retries", .usage = "[timeout_ms] [retries]",
     .handler = cmd_poll_opts, .opt_args = 2},
    {.name = "upload", .help = "upload a file (stop-and-wait, resumable)", .usage = "<local file> <remote path>",
     .handler = cmd_poll_upload, .completer = complete_upload, .min_args = 2},
    {.name = "download", .help = "download a file (stop-and-wait, resumable)", .usage = "<remote path> [local file]",
     .handler = cmd_poll_download, .completer = complete_download, .min_args = 1, .opt_args = 1},
    {.name = "count", .help = "number of entries in a directory (method 3)", .usage = "<dir>",
     .handler = cmd_poll_count, .min_args = 1},
    {.name = "download_index", .help = "download the n-th entry of a directory, no name needed (method 3)",
     .usage = "<dir> <index> [local file]", .handler = cmd_poll_download_index,
     .completer = complete_download_index, .min_args = 2, .opt_args = 1},
};

static const vcli_cmd_t vftp_cmds[] = {
    {.name = "fburst", .help = "vftp protocol service, burst (CSP port 9): blast + NACK ranges, resumable",
     VCLI_SUB(burst_cmds)},
    {.name = "fpoll", .help = "vftp protocol service, poll (CSP port 8): stop-and-wait", VCLI_SUB(poll_cmds)},
};

void vs_vftp_register_commands(void)
{
    VCLI_REGISTER_IN(VCLI_CAT_SERVICES, vftp_cmds);
}
