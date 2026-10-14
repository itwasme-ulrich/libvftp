/* In-process impaired loopback link -- see the header. */
#include "vftp_link_loopback.h"

#include <string.h>

static uint32_t rnd( vftp_loopback_t *lb )
{
    uint32_t x = lb->seed ? lb->seed : 0x9E3779B9u; /* xorshift32 */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    lb->seed = x;
    return x;
}

static bool hit( vftp_loopback_t *lb, uint16_t per_mille )
{
    return per_mille != 0u && ( rnd( lb ) % 1000u ) < per_mille;
}

static void push( vftp_loopback_t *lb, const uint8_t *pkt, uint16_t len )
{
    if ( lb->q_count >= VFTP_LOOPBACK_QUEUE ) {
        lb->down_overflow++; /* receive buffer full: dropped, like a real one */
        return;
    }
    uint32_t slot = ( lb->q_head + lb->q_count ) % VFTP_LOOPBACK_QUEUE;
    memcpy( lb->q[slot], pkt, len );
    lb->q_len[slot] = len;
    lb->q_count++;
}

static int server_send( void *ctx, const uint8_t *pkt, uint16_t len )
{
    vftp_loopback_t *lb = (vftp_loopback_t *) ctx;
    lb->down_sent++;
    if ( hit( lb, lb->loss_down ) ) {
        lb->down_lost++;
        return 0; /* lost in flight: the sender cannot know */
    }
    push( lb, pkt, len );
    if ( hit( lb, lb->dup_down ) ) {
        push( lb, pkt, len );
    }
    return 0;
}

static void server_abort( vftp_loopback_t *lb )
{
    if ( lb->burst ) {
        vftp_burst_server_abort( lb->burst );
    } else if ( lb->poll ) {
        vftp_poll_server_abort( lb->poll );
    }
}

static int lb_open( void *ctx )
{
    vftp_loopback_t *lb = (vftp_loopback_t *) ctx;
    lb->q_head = 0;
    lb->q_count = 0;
    lb->closed = false;
    return 0;
}

static void lb_close( void *ctx )
{
    vftp_loopback_t *lb = (vftp_loopback_t *) ctx;
    lb->q_count = 0;
    if ( !lb->silent_close ) {
        server_abort( lb );
    }
}

static int lb_send( void *ctx, const uint8_t *pkt, uint16_t len )
{
    vftp_loopback_t *lb = (vftp_loopback_t *) ctx;
    lb->tx_count++;
    if ( lb->die_after_tx != 0u && lb->tx_count >= lb->die_after_tx ) {
        return -1;
    }
    lb->up_sent++;
    if ( hit( lb, lb->loss_up ) ) {
        lb->up_lost++;
        return 0;
    }
    if ( lb->closed ) {
        return 0; /* connection already closed by the server: the packet goes nowhere */
    }
    vftp_srv_action_t a = VFTP_SRV_KEEP;
    if ( lb->burst ) {
        a = vftp_burst_server_handle( lb->burst, pkt, len, server_send, lb );
    } else if ( lb->poll ) {
        a = vftp_poll_server_handle( lb->poll, pkt, len, server_send, lb );
    }
    if ( a == VFTP_SRV_CLOSE ) {
        lb->closed = true;
        server_abort( lb ); /* rule 3: a conversation that ends always aborts */
    }
    return 0;
}

static int lb_recv( void *ctx, uint8_t *buf, uint16_t cap, uint32_t timeout_ms )
{
    vftp_loopback_t *lb = (vftp_loopback_t *) ctx;
    (void) timeout_ms;
    if ( lb->tx_count != 0u && lb->die_after_tx != 0u && lb->tx_count >= lb->die_after_tx ) {
        return -1;
    }
    if ( lb->q_count == 0u ) {
        return 0; /* nothing in flight: the wait would time out */
    }
    uint32_t pick = lb->reorder ? rnd( lb ) % lb->q_count : 0u;
    uint32_t slot = ( lb->q_head + pick ) % VFTP_LOOPBACK_QUEUE;
    uint16_t len = lb->q_len[slot];
    if ( len <= cap ) {
        memcpy( buf, lb->q[slot], len );
    }
    /* remove `pick` by moving the head element into its place */
    uint32_t head = lb->q_head % VFTP_LOOPBACK_QUEUE;
    if ( slot != head ) {
        memcpy( lb->q[slot], lb->q[head], lb->q_len[head] );
        lb->q_len[slot] = lb->q_len[head];
    }
    lb->q_head = ( lb->q_head + 1u ) % VFTP_LOOPBACK_QUEUE;
    lb->q_count--;
    return len <= cap ? (int) len : 0;
}

void vftp_loopback_init( vftp_link_t *link, vftp_loopback_t *lb, uint16_t mtu_tx, uint16_t mtu_rx )
{
    lb->tx_count = 0;
    lb->up_sent = lb->up_lost = lb->down_sent = lb->down_lost = lb->down_overflow = 0;
    lb->q_head = 0;
    lb->q_count = 0;
    lb->closed = false;
    link->open = lb_open;
    link->close = lb_close;
    link->send = lb_send;
    link->recv = lb_recv;
    link->sleep_ms = NULL;
    link->ctx = lb;
    link->mtu_tx = mtu_tx;
    link->mtu_rx = mtu_rx;
}
