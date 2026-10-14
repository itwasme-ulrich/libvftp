/* vftp server over CCSDS Space Packets / UDP, serving a POSIX directory.
 *
 *   vftp_spp_server [-p port] [-r root] [-m mtu_tx] [-u mtu_rx] [-l loss_permille] [-i idle_ms]
 *
 * Both services on one UDP port, chosen by APID (burst 0x109, poll 0x108).
 * -l drops that share of packets in BOTH directions on purpose. */
#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vftp/vftp.h"
#include "vftp_spp_udp.h"
#include "vftp_storage_posix.h"

static vftp_burst_server_t s_burst;
static vftp_poll_server_t s_poll;
static vftp_spp_udp_server_t s_srv;

static void on_signal( int sig )
{
    (void) sig;
    s_srv.stop = 1;
}

int main( int argc, char **argv )
{
    unsigned long port = 5100, mtu_tx = 222, mtu_rx = 150, loss = 0, idle = 10000;
    const char *root = ".";
    int opt;
    while ( ( opt = getopt( argc, argv, "p:r:m:u:l:i:" ) ) != -1 ) {
        switch ( opt ) {
            case 'p': port = strtoul( optarg, NULL, 0 ); break;
            case 'r': root = optarg; break;
            case 'm': mtu_tx = strtoul( optarg, NULL, 0 ); break;
            case 'u': mtu_rx = strtoul( optarg, NULL, 0 ); break;
            case 'l': loss = strtoul( optarg, NULL, 0 ); break;
            case 'i': idle = strtoul( optarg, NULL, 0 ); break;
            default:
                fprintf( stderr, "usage: %s [-p port] [-r root] [-m mtu_tx] [-u mtu_rx] [-l loss_permille] [-i idle_ms]\n",
                         argv[0] );
                return 2;
        }
    }
    static vftp_posix_fs_t fsctx;
    static vftp_storage_t fs;
    vftp_storage_posix_init( &fs, &fsctx, root );
    vftp_server_cfg_t cfg = { &fs, (uint16_t) mtu_tx, (uint16_t) mtu_rx };
    if ( vftp_burst_server_init( &s_burst, &cfg ) != VFTP_OK || vftp_poll_server_init( &s_poll, &cfg ) != VFTP_OK ) {
        fprintf( stderr, "bad mtu (max %u)\n", (unsigned int) VFTP_CFG_PACKET_MAX );
        return 2;
    }
    memset( &s_srv, 0, sizeof( s_srv ) );
    s_srv.burst = &s_burst;
    s_srv.poll = &s_poll;
    s_srv.apid_burst = VFTP_SPP_APID_BURST;
    s_srv.apid_poll = VFTP_SPP_APID_POLL;
    s_srv.idle_ms = (uint32_t) idle;
    s_srv.loss_tx = (uint16_t) loss;
    s_srv.loss_rx = (uint16_t) loss;
    s_srv.seed = 12345u;
    s_srv.verbose = 1;
    if ( vftp_spp_udp_server_open( &s_srv, (uint16_t) port ) != 0 ) {
        perror( "bind" );
        return 1;
    }
    signal( SIGINT, on_signal );
    signal( SIGTERM, on_signal );
    printf( "[spp-server] libvftp %s, CCSDS SPP over UDP :%lu, root %s, mtu tx %lu rx %lu, simulated loss %lu/1000\n",
            VFTP_VERSION_STRING, port, root, mtu_tx, mtu_rx, loss );
    fflush( stdout );
    vftp_spp_udp_server_run( &s_srv );
    printf( "[spp-server] stop: rx %lu tx %lu dropped tx %lu rx %lu malformed %lu\n", (unsigned long) s_srv.rx,
            (unsigned long) s_srv.tx, (unsigned long) s_srv.dropped_tx, (unsigned long) s_srv.dropped_rx,
            (unsigned long) s_srv.bad );
    return 0;
}
