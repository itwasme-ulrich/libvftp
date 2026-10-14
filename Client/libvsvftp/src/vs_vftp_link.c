/**
  ******************************************************************************
  * @file    lib/libvsvftp/src/vs_vftp_link.c
  * @brief   libvftp link over CSP (header version 1): one connection per vftp call.
  *
  *          vftp client --l_send--> csp_send          csp_read --l_recv (200 ms slices)--> vftp client
  ******************************************************************************
  */
#include <vs/vs_vftp.h>

#include <string.h>
#include <unistd.h>

#include <vs/vs_csp.h>
#include <vs/vs_os.h>

// true when the link's cancel callback asks to stop.
static bool cancelled(const vs_vftp_csp_link_t * c)
{
    return (c->cancel != NULL) && c->cancel(c->cancel_ctx);
}

// vftp_link_t open: connects to host:port with the link's options. 0, or -1 (failed or cancelled).
static int l_open(void * ctx)
{
    vs_vftp_csp_link_t * c = ctx;
    if (cancelled(c)) {
        return -1;
    }
    vs_csp_lock();
    c->conn = csp_connect(CSP_PRIO_NORM, c->host, c->port, c->timeout_ms, c->opts);
    vs_csp_unlock();
    return c->conn ? 0 : -1;
}

// vftp_link_t close: closes the connection, if open.
static void l_close(void * ctx)
{
    vs_vftp_csp_link_t * c = ctx;
    if (c->conn) {
        vs_csp_lock();
        csp_close(c->conn);
        vs_csp_unlock();
        c->conn = NULL;
    }
}

// vftp_link_t send: one packet; -1 when cancelled, too long, out of buffers, or the RDP peer is gone.
static int l_send(void * ctx, const uint8_t * pkt, uint16_t len)
{
    vs_vftp_csp_link_t * c = ctx;
    if (cancelled(c) || (c->conn == NULL) || (len > CSP_BUFFER_SIZE)) {
        return -1;
    }
    vs_csp_lock();
    csp_packet_t * p = csp_buffer_get(len);
    if (p == NULL) {
        vs_csp_unlock();
        return -1;
    }
    memcpy(p->data, pkt, len);
    p->length = len;
    // libcsp 2.1: csp_send() takes the packet whatever happens. With RDP it waits for window space (the CSP lock
    // is released meanwhile); a connection that stopped being active means the peer is gone.
    csp_send(c->conn, p);
    bool alive = ((c->opts & CSP_O_RDP) == 0) || csp_conn_is_active(c->conn);
    vs_csp_unlock();
    return alive ? 0 : -1;
}

// vftp_link_t recv: one packet into buf; 0 on timeout, -1 when cancelled or the RDP peer is gone.
static int l_recv(void * ctx, uint8_t * buf, uint16_t cap, uint32_t timeout_ms)
{
    vs_vftp_csp_link_t * c = ctx;
    if (cancelled(c) || (c->conn == NULL)) {
        return -1;
    }
    // wait in slices so that a cancel stops a transfer within 200 ms
    uint32_t left = timeout_ms;
    for (;;) {
        uint32_t slice = (left > 200u) ? 200u : left;
        vs_csp_lock();
        csp_packet_t * p = csp_read(c->conn, slice);
        bool active = ((c->opts & CSP_O_RDP) == 0) || csp_conn_is_active(c->conn);
        vs_csp_unlock();
        if (p) {
            int n = 0;
            if (p->length <= cap) {
                memcpy(buf, p->data, p->length);
                n = p->length;
            }
            vs_csp_lock();
            csp_buffer_free(p);
            vs_csp_unlock();
            return n;
        }
        if (!active || cancelled(c)) {
            return -1;
        }
        if (left <= slice) {
            return 0;
        }
        left -= slice;
    }
}

// vftp_link_t sleep_ms: the pause between burst rounds.
static void l_sleep(void * ctx, uint32_t ms)
{
    const vs_vftp_csp_link_t * c = ctx;
    // in steps, so that a cancel ends the wait within 50 ms
    while ((ms > 0u) && !cancelled(c)) {
        uint32_t step = (ms > 50u) ? 50u : ms;
        vs_sleep_ms(step);
        ms -= step;
    }
}

void vs_vftp_csp_link_init(vftp_link_t * link, vs_vftp_csp_link_t * ctx, uint8_t host, uint8_t port, uint32_t opts,
                           uint32_t timeout_ms, bool (*cancel)(void * ctx), void * cancel_ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->host = host;
    ctx->port = port;
    ctx->opts = opts;
    ctx->timeout_ms = timeout_ms;
    ctx->cancel = cancel;
    ctx->cancel_ctx = cancel_ctx;
    link->open = l_open;
    link->close = l_close;
    link->send = l_send;
    link->recv = l_recv;
    link->sleep_ms = l_sleep;
    link->ctx = ctx;
    link->mtu_tx = VS_VFTP_MTU_UP;
    link->mtu_rx = VS_VFTP_MTU_DOWN;
}
