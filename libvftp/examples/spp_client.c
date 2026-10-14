/* vftp client over CCSDS Space Packets / UDP.
 *
 *   vftp_spp_client [-h host] [-p port] [-m mtu_tx] [-u mtu_rx] [-l loss] [-c chunk] [-w window]
 *                   [-t timeout_ms] [-g gap_ms] [-r retries] <burst|poll> <command> args...
 *   commands: upload <local> <remote> | download <remote> <local> | ls <dir> | rm <path> | regen <dir>
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "vftp/vftp.h"
#include "vftp_spp_udp.h"
#include "vftp_storage_posix.h"

static vftp_client_t s_client;

static double now_s( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static void on_event( void *ctx, const vftp_event_t *ev )
{
    (void) ctx;
    switch ( ev->type ) {
        case VFTP_EV_START:
            printf( "file %lu bytes, crc 0x%08lx, %lu chunks of %u\n", (unsigned long) ev->size,
                    (unsigned long) ev->crc32, (unsigned long) ev->total_chunks, (unsigned int) ev->chunk_size );
            break;
        case VFTP_EV_RESUME:
            printf( "resuming: %lu/%lu chunks already there\n", (unsigned long) ev->done_chunks,
                    (unsigned long) ev->total_chunks );
            break;
        case VFTP_EV_ROUND:
            printf( "  round %lu: %lu/%lu chunks, %lu missing\n", (unsigned long) ev->round,
                    (unsigned long) ev->done_chunks, (unsigned long) ev->total_chunks, (unsigned long) ev->missing );
            break;
        case VFTP_EV_CRC:
            printf( "crc remote 0x%08lx local 0x%08lx %s\n", (unsigned long) ev->remote_crc,
                    (unsigned long) ev->local_crc, ev->remote_crc == ev->local_crc ? "OK" : "MISMATCH" );
            break;
        case VFTP_EV_VERIFY:
            printf( "  server checksumming: %lu/%lu bytes\n", (unsigned long) ev->bytes_done, (unsigned long) ev->size );
            break;
        case VFTP_EV_PROGRESS:
        default:
            break;
    }
}

static void on_entry( void *ctx, uint32_t total, uint32_t index, const vftp_list_entry_t *e )
{
    (void) ctx;
    (void) total;
    char date[32] = "-";
    if ( e->mtime != 0u ) {
        vftp_civil_t t;
        vftp_unix_to_civil( e->mtime, &t );
        snprintf( date, sizeof( date ), "%04u-%02u-%02u_%02u:%02u", t.year, t.month, t.day, t.hour, t.minute );
    }
    printf( "%5lu %10lu %s %s%s%s\n", (unsigned long) index, (unsigned long) e->size, date, e->name,
            ( e->flags & VFTP_ENTRY_DIR ) ? "/" : "", ( e->flags & VFTP_ENTRY_NAME_CUT ) ? "~" : "" );
}

int main( int argc, char **argv )
{
    const char *host = "127.0.0.1";
    unsigned long port = 5100, mtu_tx = 150, mtu_rx = 222, loss = 0, chunk = 0, window = 64, timeout = 2000, gap = 300, retries = 5;
    int opt;
    while ( ( opt = getopt( argc, argv, "h:p:m:u:l:c:w:t:g:r:" ) ) != -1 ) {
        switch ( opt ) {
            case 'h': host = optarg; break;
            case 'p': port = strtoul( optarg, NULL, 0 ); break;
            case 'm': mtu_tx = strtoul( optarg, NULL, 0 ); break;
            case 'u': mtu_rx = strtoul( optarg, NULL, 0 ); break;
            case 'l': loss = strtoul( optarg, NULL, 0 ); break;
            case 'c': chunk = strtoul( optarg, NULL, 0 ); break;
            case 'w': window = strtoul( optarg, NULL, 0 ); break;
            case 't': timeout = strtoul( optarg, NULL, 0 ); break;
            case 'g': gap = strtoul( optarg, NULL, 0 ); break;
            case 'r': retries = strtoul( optarg, NULL, 0 ); break;
            default: return 2;
        }
    }
    if ( argc - optind < 2 ) {
        fprintf( stderr, "usage: %s [opts] <burst|poll> <upload|download|ls|rm|regen> args...\n", argv[0] );
        return 2;
    }
    const char *svc = argv[optind];
    const char *cmd = argv[optind + 1];
    char **a = argv + optind + 2;
    int na = argc - optind - 2;
    bool burst = strcmp( svc, "burst" ) == 0;

    static vftp_spp_udp_client_t link_ctx;
    static vftp_link_t link;
    if ( vftp_spp_udp_client_open( &link, &link_ctx, host, (uint16_t) port,
                                   burst ? VFTP_SPP_APID_BURST : VFTP_SPP_APID_POLL, (uint16_t) mtu_tx,
                                   (uint16_t) mtu_rx ) != 0 ) {
        perror( "udp" );
        return 1;
    }
    link_ctx.loss_tx = (uint16_t) loss;
    link_ctx.loss_rx = (uint16_t) loss;
    link_ctx.seed = 777u;

    static vftp_posix_fs_t fsctx;
    static vftp_storage_t fs;
    vftp_storage_posix_init( &fs, &fsctx, "" );

    vftp_client_cfg_t cfg;
    vftp_client_default_cfg( &cfg );
    cfg.link = &link;
    cfg.fs = &fs;
    cfg.chunk_size = (uint16_t) chunk;
    cfg.window = (uint32_t) window;
    cfg.timeout_ms = (uint32_t) timeout;
    cfg.gap_ms = (uint32_t) gap;
    cfg.retries = (uint8_t) retries;
    cfg.max_stall = 8;
    cfg.on_event = on_event;
    vftp_client_init( &s_client, &cfg );

    double t0 = now_s();
    vftp_ret_t st = VFTP_INVAL;
    uint32_t total = 0;
    char out[VFTP_MAX_PATH];
    if ( strcmp( cmd, "upload" ) == 0 && na == 2 ) {
        st = burst ? vftp_burst_upload( &s_client, a[0], a[1] ) : vftp_poll_upload( &s_client, a[0], a[1] );
    } else if ( strcmp( cmd, "download" ) == 0 && na == 2 ) {
        st = burst ? vftp_burst_download( &s_client, a[0], a[1] ) : vftp_poll_download( &s_client, a[0], a[1] );
    } else if ( strcmp( cmd, "ls" ) == 0 && na == 1 && burst ) {
        st = vftp_burst_list( &s_client, a[0], on_entry, NULL, &total );
        if ( st == VFTP_OK ) {
            printf( "%lu entries\n", (unsigned long) total );
        }
    } else if ( strcmp( cmd, "rm" ) == 0 && na == 1 && burst ) {
        st = vftp_burst_remove( &s_client, a[0] );
    } else if ( strcmp( cmd, "regen" ) == 0 && na == 1 && burst ) {
        st = vftp_burst_list_regen( &s_client, a[0], out );
        if ( st == VFTP_OK ) {
            printf( "server wrote %s\n", out );
        }
    } else {
        fprintf( stderr, "bad command\n" );
    }
    double dt = now_s() - t0;
    const vftp_client_stats_t *s = &s_client.stats;
    printf( "%s %s: %s", svc, cmd, vftp_strerror( st ) );
    if ( st == VFTP_REMOTE ) {
        printf( " (server: %s)", vftp_strerror( s_client.remote_ret ) );
    }
    printf( "  [%.2fs, rounds %lu, data tx %lu rx %lu dup %lu, retries %lu, link lost tx %lu rx %lu]\n", dt,
            (unsigned long) s->rounds, (unsigned long) s->data_tx, (unsigned long) s->data_rx,
            (unsigned long) s->duplicates, (unsigned long) s->retries, (unsigned long) link_ctx.dropped_tx,
            (unsigned long) link_ctx.dropped_rx );
    vftp_spp_udp_client_close( &link_ctx );
    return st == VFTP_OK ? 0 : 1;
}
