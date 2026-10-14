/* vftp over CCSDS Space Packets in UDP datagrams -- see the header. */
#define _POSIX_C_SOURCE 200809L

#include "vftp_spp_udp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint32_t rnd( uint32_t *seed )
{
    uint32_t x = *seed ? *seed : 0x2545F491u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *seed = x;
    return x;
}

static bool lose( uint32_t *seed, uint16_t per_mille )
{
    return per_mille != 0u && ( rnd( seed ) % 1000u ) < per_mille;
}

static uint64_t now_ms( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

/* ---- client link --------------------------------------------------------- */

static int c_send( void *ctx, const uint8_t *pkt, uint16_t len )
{
    vftp_spp_udp_client_t *c = (vftp_spp_udp_client_t *) ctx;
    uint16_t n = vftp_spp_wrap( c->frame, (uint16_t) sizeof( c->frame ), c->apid, true, c->seq++, pkt, len );
    if ( n == 0u ) {
        return -1;
    }
    c->sent++;
    if ( lose( &c->seed, c->loss_tx ) ) {
        c->dropped_tx++;
        return 0;
    }
    if ( send( c->fd, c->frame, n, 0 ) < 0 && errno != ECONNREFUSED ) {
        return -1;
    }
    return 0;
}

static int c_recv( void *ctx, uint8_t *buf, uint16_t cap, uint32_t timeout_ms )
{
    vftp_spp_udp_client_t *c = (vftp_spp_udp_client_t *) ctx;
    uint64_t deadline = now_ms() + timeout_ms;
    for ( ;; ) {
        uint64_t t = now_ms();
        if ( t >= deadline ) {
            return 0;
        }
        struct pollfd pfd = { c->fd, POLLIN, 0 };
        int r = poll( &pfd, 1, (int) ( deadline - t ) );
        if ( r < 0 ) {
            return errno == EINTR ? 0 : -1;
        }
        if ( r == 0 ) {
            return 0;
        }
        ssize_t n = recv( c->fd, c->frame, sizeof( c->frame ), 0 );
        if ( n < 0 ) {
            if ( errno == ECONNREFUSED ) {
                continue; /* no server yet: same as silence */
            }
            return -1;
        }
        vftp_spp_t sp;
        if ( !vftp_spp_unwrap( c->frame, (uint16_t) n, &sp ) || sp.telecommand || sp.apid != c->apid ||
             sp.len > cap ) {
            continue;
        }
        c->received++;
        if ( lose( &c->seed, c->loss_rx ) ) {
            c->dropped_rx++;
            continue;
        }
        memcpy( buf, sp.data, sp.len );
        return (int) sp.len;
    }
}

static void c_sleep( void *ctx, uint32_t ms )
{
    (void) ctx;
    struct timespec ts = { (time_t) ( ms / 1000u ), (long) ( ms % 1000u ) * 1000000L };
    nanosleep( &ts, NULL );
}

int vftp_spp_udp_client_open( vftp_link_t *link, vftp_spp_udp_client_t *c, const char *host, uint16_t port,
                              uint16_t apid, uint16_t mtu_tx, uint16_t mtu_rx )
{
    struct addrinfo hints;
    struct addrinfo *ai = NULL;
    char port_s[8];
    memset( &hints, 0, sizeof( hints ) );
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf( port_s, sizeof( port_s ), "%u", (unsigned int) port );
    if ( getaddrinfo( host, port_s, &hints, &ai ) != 0 || ai == NULL ) {
        return -1;
    }
    c->fd = socket( ai->ai_family, ai->ai_socktype, ai->ai_protocol );
    if ( c->fd < 0 || connect( c->fd, ai->ai_addr, ai->ai_addrlen ) != 0 ) {
        freeaddrinfo( ai );
        return -1;
    }
    freeaddrinfo( ai );
    c->apid = apid;
    c->seq = 0;
    c->sent = c->received = c->dropped_tx = c->dropped_rx = 0;
    memset( link, 0, sizeof( *link ) );
    link->send = c_send;
    link->recv = c_recv;
    link->sleep_ms = c_sleep;
    link->ctx = c;
    link->mtu_tx = mtu_tx;
    link->mtu_rx = mtu_rx;
    return 0;
}

void vftp_spp_udp_client_close( vftp_spp_udp_client_t *c )
{
    if ( c->fd >= 0 ) {
        close( c->fd );
        c->fd = -1;
    }
}

/* ---- server -------------------------------------------------------------- */

static int s_send( void *ctx, const uint8_t *pkt, uint16_t len )
{
    vftp_spp_udp_server_t *s = (vftp_spp_udp_server_t *) ctx;
    uint16_t n = vftp_spp_wrap( s->frame, (uint16_t) sizeof( s->frame ), s->reply_apid, false, s->seq++, pkt, len );
    if ( n == 0u ) {
        return -1;
    }
    s->tx++;
    if ( lose( &s->seed, s->loss_tx ) ) {
        s->dropped_tx++;
        return 0;
    }
    /* A full socket buffer drops the datagram, exactly what a radio does: the
     * client's NACK round asks for it again. */
    (void) sendto( s->fd, s->frame, n, 0, (const struct sockaddr *) &s->peer, s->peer_len );
    return 0;
}

int vftp_spp_udp_server_open( vftp_spp_udp_server_t *s, uint16_t port )
{
    struct sockaddr_in a;
    s->fd = socket( AF_INET, SOCK_DGRAM, 0 );
    if ( s->fd < 0 ) {
        return -1;
    }
    memset( &a, 0, sizeof( a ) );
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl( INADDR_ANY );
    a.sin_port = htons( port );
    if ( bind( s->fd, (const struct sockaddr *) &a, sizeof( a ) ) != 0 ) {
        close( s->fd );
        s->fd = -1;
        return -1;
    }
    s->stop = 0;
    s->rx = s->tx = s->dropped_tx = s->dropped_rx = s->bad = 0;
    return 0;
}

/* Rule 3 of vftp_server.h for a connectionless transport: a busy service whose
 * session has not seen one packet of its own for idle_ms is abandoned. */
typedef struct {
    uint32_t activity;
    uint64_t since;
} idle_t;

static void idle_check( vftp_spp_udp_server_t *s, idle_t *b, idle_t *p, uint64_t now )
{
    if ( s->burst ) {
        if ( !vftp_burst_server_busy( s->burst ) || s->burst->activity != b->activity ) {
            b->activity = s->burst->activity;
            b->since = now;
        } else if ( now - b->since >= s->idle_ms ) {
            vftp_burst_server_abort( s->burst );
            if ( s->verbose ) {
                printf( "[spp-server] burst session idle %lu ms: aborted (resume state kept)\n",
                        (unsigned long) s->idle_ms );
            }
        }
    }
    if ( s->poll ) {
        if ( !vftp_poll_server_busy( s->poll ) || s->poll->activity != p->activity ) {
            p->activity = s->poll->activity;
            p->since = now;
        } else if ( now - p->since >= s->idle_ms ) {
            vftp_poll_server_abort( s->poll );
            if ( s->verbose ) {
                printf( "[spp-server] poll session idle %lu ms: aborted (resume state kept)\n",
                        (unsigned long) s->idle_ms );
            }
        }
    }
}

void vftp_spp_udp_server_run( vftp_spp_udp_server_t *s )
{
    uint8_t in[VFTP_CFG_PACKET_MAX + VFTP_SPP_HEADER];
    idle_t ib = { 0u, now_ms() };
    idle_t ip = { 0u, now_ms() };
    while ( !s->stop ) {
        struct pollfd pfd = { s->fd, POLLIN, 0 };
        int r = poll( &pfd, 1, 100 );
        idle_check( s, &ib, &ip, now_ms() );
        if ( r <= 0 ) {
            continue;
        }
        struct sockaddr_storage from;
        socklen_t from_len = sizeof( from );
        ssize_t n = recvfrom( s->fd, in, sizeof( in ), 0, (struct sockaddr *) &from, &from_len );
        if ( n <= 0 ) {
            continue;
        }
        vftp_spp_t sp;
        if ( !vftp_spp_unwrap( in, (uint16_t) n, &sp ) || !sp.telecommand ) {
            s->bad++;
            continue;
        }
        s->rx++;
        if ( lose( &s->seed, s->loss_rx ) ) {
            s->dropped_rx++;
            continue;
        }
        memcpy( &s->peer, &from, from_len );
        s->peer_len = from_len;
        s->reply_apid = sp.apid;
        if ( sp.apid == s->apid_burst && s->burst ) {
            bool was = vftp_burst_server_busy( s->burst );
            (void) vftp_burst_server_handle( s->burst, sp.data, sp.len, s_send, s );
            if ( s->verbose && was != vftp_burst_server_busy( s->burst ) ) {
                printf( "[spp-server] burst session %s (rx %lu tx %lu, simulated loss tx %lu rx %lu)\n",
                        was ? "closed" : "opened", (unsigned long) s->rx, (unsigned long) s->tx,
                        (unsigned long) s->dropped_tx, (unsigned long) s->dropped_rx );
            }
        } else if ( sp.apid == s->apid_poll && s->poll ) {
            bool was = vftp_poll_server_busy( s->poll );
            (void) vftp_poll_server_handle( s->poll, sp.data, sp.len, s_send, s );
            if ( s->verbose && was != vftp_poll_server_busy( s->poll ) ) {
                printf( "[spp-server] poll session %s (rx %lu tx %lu)\n", was ? "closed" : "opened",
                        (unsigned long) s->rx, (unsigned long) s->tx );
            }
        } else {
            s->bad++;
        }
        fflush( stdout );
    }
}
