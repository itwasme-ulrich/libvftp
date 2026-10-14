/* libvftp: client side of vftp_burst and vftp_poll. See vftp_client.h for the
 * robustness rules every call follows. */
#include <string.h>

#include "vftp/vftp_client.h"
#include "vftp/vftp_crc32.h"
#include "../common/vftp_internal.h"

#define VMAP_MAGIC 0x44465456u /* "VTFD": burst download resume sidecar (bitmap), next to the local file */
#define WMAP_MAGIC 0x50465456u /* "VTFP": poll download resume sidecar (watermark), next to the local file */
#define WMAP_SIZE 24u
#define WMAP_EVERY 16u /* chunks between watermark saves: a crash re-fetches at most this many */
#define VMAP_VERSION 1u
#define VMAP_HEADER 24u

/* Packets skipped while waiting for one reply, per attempt, before the
 * attempt counts as lost (bounds the wait when stale DATA keeps arriving). */
#define SKIP_LIMIT 512u

typedef bool ( *b_match_fn )( const vftp_client_t *c, uint32_t arg );
typedef bool ( *p_match_fn )( const vftp_client_t *c, uint32_t arg );

static vftp_ret_t p_transact( vftp_client_t *c, uint8_t expect, p_match_fn match, uint32_t arg );
static bool p_any( const vftp_client_t *c, uint32_t arg );

/* ---- generic ------------------------------------------------------------- */

/* CRC-32 of the first `size` bytes of a local file, read through `buf`. VFTP_IO if the file is shorter. */
static vftp_ret_t vftp_fs_crc( const vftp_storage_t *fs, const char *path, uint32_t size, uint8_t *buf,
                               uint32_t buf_size, uint32_t *crc )
{
    uint32_t c = VFTP_CRC32_INIT;
    uint32_t off = 0;
    while ( off < size ) {
        uint32_t want = size - off;
        if ( want > buf_size ) {
            want = buf_size;
        }
        uint32_t got = 0;
        vftp_ret_t st = vftp_fs_read_all( fs, path, off, buf, want, &got );
        if ( st != VFTP_OK ) {
            return st;
        }
        if ( got != want ) {
            return VFTP_IO; /* file shorter than the size it was opened with */
        }
        c = vftp_crc32_update( c, buf, got );
        off += got;
    }
    *crc = vftp_crc32_final( c );
    return VFTP_OK;
}

void vftp_client_default_cfg( vftp_client_cfg_t *cfg )
{
    memset( cfg, 0, sizeof( *cfg ) );
    cfg->timeout_ms = 10000u; /* 1200 bit/s: a full round trip can take seconds */
    cfg->gap_ms = 1000u;
    cfg->window = 64u;
    cfg->retries = 3u;
    cfg->max_stall = 4u;
}

vftp_ret_t vftp_client_init( vftp_client_t *c, const vftp_client_cfg_t *cfg )
{
    if ( c == NULL || cfg == NULL ) {
        return VFTP_INVAL;
    }
    memset( &c->stats, 0, sizeof( c->stats ) );
    c->cfg = *cfg;
    c->remote_ret = VFTP_OK;
    return VFTP_OK;
}

static void emit( vftp_client_t *c, vftp_event_t *ev )
{
    if ( c->cfg.on_event ) {
        c->cfg.on_event( c->cfg.event_ctx, ev );
    }
}

static void emit_simple( vftp_client_t *c, vftp_event_type_t type, uint32_t size, uint32_t crc, uint16_t cs,
                         uint32_t total, uint32_t done )
{
    vftp_event_t ev;
    memset( &ev, 0, sizeof( ev ) );
    ev.type = type;
    ev.size = size;
    ev.crc32 = crc;
    ev.chunk_size = cs;
    ev.total_chunks = total;
    ev.done_chunks = done;
    uint32_t bytes = done * (uint32_t) cs;
    ev.bytes_done = ( cs == 0u || bytes > size ) ? size : bytes;
    emit( c, &ev );
}

static vftp_ret_t check_cfg( vftp_client_t *c )
{
    const vftp_link_t *l = c->cfg.link;
    if ( l == NULL || l->send == NULL || l->recv == NULL || c->cfg.fs == NULL ||
         l->mtu_tx <= VFTP_DATA_HEADER || l->mtu_rx <= VFTP_DATA_HEADER || l->mtu_tx > VFTP_CFG_PACKET_MAX ||
         l->mtu_rx > VFTP_CFG_PACKET_MAX ) {
        return VFTP_INVAL;
    }
    memset( &c->stats, 0, sizeof( c->stats ) );
    c->remote_ret = VFTP_OK;
    c->session = 0u; /* every call is its own transfer */
    return VFTP_OK;
}

static uint16_t pick_chunk( uint16_t want, uint16_t mtu )
{
    uint16_t max = (uint16_t) ( mtu - VFTP_DATA_HEADER );
    if ( want == 0u || want > max ) {
        return max;
    }
    return want;
}

static uint8_t attempts( const vftp_client_t *c )
{
    if ( c->cfg.retries == 0u ) {
        return 1u;
    }
    return c->cfg.retries;
}

static vftp_ret_t link_open( vftp_client_t *c )
{
    const vftp_link_t *l = c->cfg.link;
    return ( l->open == NULL || l->open( l->ctx ) == 0 ) ? VFTP_OK : VFTP_LINK;
}

static void link_close( vftp_client_t *c )
{
    const vftp_link_t *l = c->cfg.link;
    if ( l->close ) {
        l->close( l->ctx );
    }
}

static vftp_ret_t link_send( vftp_client_t *c, uint16_t len )
{
    const vftp_link_t *l = c->cfg.link;
    return l->send( l->ctx, c->tx, len ) < 0 ? VFTP_LINK : VFTP_OK;
}

/* > 0 length, 0 timeout, < 0 link broken */
static int link_recv( vftp_client_t *c, uint32_t timeout_ms )
{
    const vftp_link_t *l = c->cfg.link;
    uint16_t cap = l->mtu_rx;
    if ( cap > sizeof( c->rx ) ) {
        cap = (uint16_t) sizeof( c->rx );
    }
    return l->recv( l->ctx, c->rx, cap, timeout_ms );
}

/* Encodes c->msg; a message of the open transfer gets the call's session byte. */
static uint16_t encode( vftp_client_t *c, uint8_t *buf, uint16_t cap )
{
    c->msg.session = 0u;
    if ( vftp_msg_has_session( c->msg.type ) ) {
        c->msg.session = c->session;
    }
    return vftp_encode( &c->msg, buf, cap );
}

/* The decoded c->msg belongs to another (older) transfer: skipped like any other stale packet.
 * Until the session is known (c->session 0) nothing is stale -- the reply that opens the transfer
 * is where the session byte comes from. */
static bool stale( const vftp_client_t *c )
{
    return c->session != 0u && vftp_msg_has_session( c->msg.type ) && c->msg.session != c->session;
}

/* ---- burst control round trip ------------------------------------------- */

static bool b_any( const vftp_client_t *c, uint32_t arg )
{
    (void) c;
    (void) arg;
    return true;
}

/* Sends c->msg (already filled in), waits for a reply of `expect` that also
 * satisfies `match`, retrying the whole exchange. The reply lands in c->msg. */
static vftp_ret_t b_transact( vftp_client_t *c, uint8_t expect, b_match_fn match, uint32_t arg )
{
    uint16_t n = encode( c, c->tx, c->cfg.link->mtu_tx );
    if ( n == 0u ) {
        return VFTP_INVAL;
    }
    for ( uint8_t a = 0; a < attempts( c ); a++ ) {
        if ( a != 0u ) {
            c->stats.retries++;
        }
        vftp_ret_t st = link_send( c, n );
        if ( st != VFTP_OK ) {
            return st;
        }
        for ( uint32_t skipped = 0; skipped < SKIP_LIMIT; skipped++ ) {
            int len = link_recv( c, c->cfg.timeout_ms );
            if ( len < 0 ) {
                return VFTP_LINK;
            }
            if ( len == 0 ) {
                break; /* this attempt is lost */
            }
            if ( vftp_decode( c->rx, (uint16_t) len, &c->msg ) == VFTP_OK && !stale( c ) && c->msg.type == expect &&
                 match( c, arg ) ) {
                return VFTP_OK;
            }
            /* a late DATA, a duplicate, a reply to an earlier attempt: skip it */
        }
    }
    return VFTP_TIMEOUT;
}

/* DONE has no reply, so a lost DONE would leave the server's session open (and
 * the service BUSY) until its idle timeout. It is idempotent -- a DONE with no
 * session open is a no-op -- so it is simply sent `retries` times. */
static void send_done( vftp_client_t *c, uint16_t n, vftp_ret_t st )
{
    if ( n == 0u || st == VFTP_LINK ) {
        return;
    }
    for ( uint8_t i = 0; i < attempts( c ); i++ ) {
        if ( link_send( c, n ) != VFTP_OK ) {
            return;
        }
    }
}

/* A failure on this side after the server opened the session (local file cannot be created, too
 * many chunks...): tell it with ABORT, sent like DONE (no reply, idempotent), so the service is free
 * at once instead of waiting out its idle timeout -- every request until then would time out. */
static vftp_ret_t abandon( vftp_client_t *c, vftp_ret_t st )
{
    c->msg.type = VFTP_MSG_ABORT;
    uint16_t n = encode( c, c->tx, c->cfg.link->mtu_tx );
    send_done( c, n, st );
    link_close( c );
    return st;
}

static vftp_ret_t remote( vftp_client_t *c, uint8_t ret )
{
    c->remote_ret = ret;
    return VFTP_REMOTE;
}

/* Asks for the file CRC until the server has it. A large file is checksummed a step per request
 * (reply VFTP_AGAIN + bytes done so far); this keeps asking while that number moves and gives up
 * after max_stall replies in a row without progress. */
static vftp_ret_t fetch_crc( vftp_client_t *c, bool burst, uint32_t size, uint32_t *crc )
{
    uint32_t last = 0;
    uint32_t stall = 0;
    uint32_t limit = c->cfg.max_stall == 0u ? 1u : c->cfg.max_stall;
    for ( ;; ) {
        vftp_ret_t st;
        uint8_t ret;
        uint32_t done;
        if ( burst ) {
            c->msg.type = VFTP_MSG_CRC_REQUEST;
            st = b_transact( c, VFTP_MSG_CRC_REPLY, b_any, 0u );
            ret = c->msg.u.crc_rep.ret;
            done = c->msg.u.crc_rep.done;
            *crc = c->msg.u.crc_rep.crc32;
        } else {
            c->msg.type = VFTP_MSG_CRC_REQUEST;
            st = p_transact( c, VFTP_MSG_CRC_REPLY, p_any, 0u );
            ret = c->msg.u.crc_rep.ret;
            done = c->msg.u.crc_rep.done;
            *crc = c->msg.u.crc_rep.crc32;
        }
        if ( st != VFTP_OK ) {
            return st;
        }
        if ( ret != VFTP_AGAIN ) {
            return ret == VFTP_OK ? VFTP_OK : remote( c, ret );
        }
        vftp_event_t ev;
        memset( &ev, 0, sizeof( ev ) );
        ev.type = VFTP_EV_VERIFY;
        ev.size = size;
        ev.bytes_done = done;
        emit( c, &ev );
        if ( done > last ) {
            stall = 0;
        } else if ( ++stall >= limit ) {
            return VFTP_TIMEOUT;
        }
        last = done;
    }
}

/* ---- burst download ------------------------------------------------------ */

static void vmap_header( uint8_t *h, uint16_t cs, uint32_t size, uint32_t crc, uint32_t total, const char *remote_path )
{
    vftp_put_u32( h + 0, VMAP_MAGIC );
    vftp_put_u16( h + 4, VMAP_VERSION );
    vftp_put_u16( h + 6, cs );
    vftp_put_u32( h + 8, size );
    vftp_put_u32( h + 12, crc );
    vftp_put_u32( h + 16, total );
    vftp_put_u32( h + 20, vftp_crc32( remote_path, strlen( remote_path ) ) );
}

static vftp_ret_t bm_store( vftp_client_t *c, const char *vmap, uint32_t lo, uint32_t hi )
{
    return vftp_fs_write_all( c->cfg.fs, vmap, VMAP_HEADER + lo, c->bitmap + lo, hi - lo + 1u );
}

static vftp_ret_t vmap_create( vftp_client_t *c, const char *vmap, const uint8_t *hdr, uint32_t nbytes )
{
    const vftp_storage_t *fs = c->cfg.fs;
    vftp_ret_t st = fs->truncate( fs->ctx, vmap );
    if ( st == VFTP_OK ) {
        st = fs->write( fs->ctx, vmap, 0, hdr, (uint16_t) VMAP_HEADER );
    }
    if ( st == VFTP_OK && nbytes != 0u ) {
        st = bm_store( c, vmap, 0u, nbytes - 1u );
    }
    return st;
}

/* Loads the local sidecar into c->bitmap if it belongs to this remote file (same path, size, CRC).
 * Saved with another chunk_size (the user changed it between attempts), the chunks already on
 * disk are carried over to the new chunking instead of fetched again. */
static bool vmap_load( vftp_client_t *c, const char *vmap, const uint8_t *want, uint32_t size, uint16_t cs,
                       uint32_t total )
{
    const vftp_storage_t *fs = c->cfg.fs;
    uint8_t h[VMAP_HEADER];
    uint16_t got = 0;
    if ( fs->read( fs->ctx, vmap, 0, h, (uint16_t) VMAP_HEADER, &got ) != VFTP_OK || got != VMAP_HEADER ||
         memcmp( h, want, 6u ) != 0 || memcmp( h + 8, want + 8, 8u ) != 0 || memcmp( h + 20, want + 20, 4u ) != 0 ) {
        return false;
    }
    uint16_t old_cs = vftp_get_u16( h + 6 );
    uint32_t old_total = vftp_get_u32( h + 16 );
    uint32_t nbytes = ( total + 7u ) / 8u;
    if ( old_cs == cs ) {
        uint32_t n = 0;
        return old_total == total && vftp_fs_read_all( fs, vmap, VMAP_HEADER, c->bitmap, nbytes, &n ) == VFTP_OK &&
               n == nbytes;
    }
    uint32_t held = 0;
    return old_cs != 0u && old_total == vftp_total_chunks( size, old_cs ) &&
           vftp_rechunk( fs, vmap, VMAP_HEADER, old_cs, old_total, cs, total, size, c->bitmap, c->buf,
                         (uint32_t) sizeof( c->buf ), &held ) == VFTP_OK &&
           vmap_create( c, vmap, want, nbytes ) == VFTP_OK;
}

/* The local file must still hold every chunk the sidecar marks done: chunks land at their own
 * offsets, so it is at least as long as the end of the last one. A partial file deleted or cut
 * short while its .VFD stayed (by hand, or a disk clean-up) would otherwise "resume" over holes
 * and end in a CRC mismatch. */
static bool local_holds_bitmap( const uint8_t *bitmap, uint32_t total, uint32_t size, uint16_t cs,
                                uint32_t local_size )
{
    for ( uint32_t i = total; i-- > 0u; ) {
        if ( vftp_bit_get( bitmap, i ) ) {
            return local_size >= i * (uint32_t) cs + vftp_chunk_len( size, cs, i );
        }
    }
    return true;
}

vftp_ret_t vftp_burst_download( vftp_client_t *c, const char *remote_path, const char *local_path )
{
    vftp_ret_t st = check_cfg( c );
    if ( st != VFTP_OK ) {
        return st;
    }
    const vftp_storage_t *fs = c->cfg.fs;
    char vmap[VFTP_SIDECAR_PATH_MAX];
    if ( !vftp_path_set( c->msg.u.down_req.path, remote_path ) ||
         !vftp_sidecar_path( local_path, "VFD", false, vmap, sizeof( vmap ) ) ) {
        return VFTP_INVAL;
    }
    uint32_t local_size = 0;
    vftp_ret_t local_st = fs->stat( fs->ctx, local_path, &local_size );
    if ( local_st == VFTP_ISDIR ) {
        return VFTP_ISDIR; /* the destination is a local directory: refused before the server is asked */
    }
    if ( local_st != VFTP_OK ) {
        local_size = 0; /* no local file: nothing to resume from */
    }
    uint16_t cs = pick_chunk( c->cfg.chunk_size, c->cfg.link->mtu_rx );

    st = link_open( c );
    if ( st != VFTP_OK ) {
        return st;
    }
    c->msg.type = VFTP_MSG_DOWNLOAD_REQUEST;
    c->msg.u.down_req.chunk_size = cs;
    st = b_transact( c, VFTP_MSG_DOWNLOAD_REPLY, b_any, 0u );
    if ( st == VFTP_OK && c->msg.u.down_rep.ret != VFTP_OK ) {
        st = remote( c, c->msg.u.down_rep.ret );
    }
    if ( st != VFTP_OK ) {
        link_close( c );
        return st;
    }
    c->session = c->msg.session; /* every later message of this transfer carries it */
    uint32_t size = c->msg.u.down_rep.size;
    /* A large file's CRC is not known yet (the server computes it while serving): 0 here, and the
     * end-of-transfer CRC_REQUEST is then the only reference. */
    bool rcrc_valid = ( c->msg.u.down_rep.flags & VFTP_DL_CRC_VALID ) != 0u;
    uint32_t rcrc = rcrc_valid ? c->msg.u.down_rep.crc32 : 0u;
    uint32_t total = vftp_total_chunks( size, cs );
    emit_simple( c, VFTP_EV_START, size, rcrc, cs, total, 0u );
    if ( total > VFTP_CFG_CLIENT_MAX_CHUNKS ) {
        return abandon( c, VFTP_NOSPC );
    }

    /* Local resume: only if the sidecar describes this remote file (any chunking). */
    uint8_t hdr[VMAP_HEADER];
    vmap_header( hdr, cs, size, rcrc, total, remote_path );
    uint32_t nbytes = ( total + 7u ) / 8u;
    uint32_t done = 0;
    if ( vmap_load( c, vmap, hdr, size, cs, total ) && local_holds_bitmap( c->bitmap, total, size, cs, local_size ) ) {
        done = vftp_bitmap_fix_count( c->bitmap, total );
        if ( done != 0u ) {
            emit_simple( c, VFTP_EV_RESUME, size, rcrc, cs, total, done );
        }
    } else {
        memset( c->bitmap, 0, nbytes );
        st = fs->truncate( fs->ctx, local_path );
        if ( st == VFTP_OK ) {
            st = vmap_create( c, vmap, hdr, nbytes );
        }
        if ( st != VFTP_OK ) {
            return abandon( c, st );
        }
    }

    uint32_t max_ranges = VFTP_WANT_RANGES_FOR( (uint32_t) c->cfg.link->mtu_tx );
    if ( max_ranges > VFTP_WIRE_MAX_RANGES ) {
        max_ranges = VFTP_WIRE_MAX_RANGES;
    }
    uint32_t window = c->cfg.window == 0u ? 64u : c->cfg.window;
    uint32_t stall = 0;
    while ( st == VFTP_OK && done < total ) {
        c->stats.rounds++;
        /* WANT: the next `window` missing chunks. */
        c->msg.type = VFTP_MSG_WANT;
        c->msg.u.want.entries =
            vftp_bitmap_missing( c->bitmap, total, c->msg.u.want.range, (uint16_t) max_ranges, window );
        uint32_t expected = 0;
        for ( uint16_t i = 0; i < c->msg.u.want.entries; i++ ) {
            expected += c->msg.u.want.range[i].count;
        }
        uint16_t n = encode( c, c->tx, c->cfg.link->mtu_tx );
        st = n == 0u ? VFTP_INVAL : link_send( c, n );
        if ( st != VFTP_OK ) {
            break;
        }
        /* Take whatever arrives until the burst goes quiet. */
        uint32_t fresh = 0;
        uint32_t lo = UINT32_MAX;
        uint32_t hi = 0;
        for ( uint32_t seen = 0; seen < expected; ) {
            int len = link_recv( c, seen == 0u && fresh == 0u ? c->cfg.timeout_ms : c->cfg.gap_ms );
            if ( len < 0 ) {
                st = VFTP_LINK;
                break;
            }
            if ( len == 0 ) {
                break;
            }
            if ( vftp_decode( c->rx, (uint16_t) len, &c->msg ) != VFTP_OK || c->msg.type != VFTP_MSG_DATA ||
                 stale( c ) ) {
                continue; /* not data of this transfer */
            }
            seen++;
            c->stats.data_rx++;
            const vftp_data_t *d = &c->msg.u.data;
            if ( d->chunk >= total || d->len != vftp_chunk_len( size, cs, d->chunk ) ) {
                continue;
            }
            if ( vftp_bit_get( c->bitmap, d->chunk ) ) {
                c->stats.duplicates++;
                continue;
            }
            st = fs->write( fs->ctx, local_path, d->chunk * (uint32_t) cs, d->bytes, d->len );
            if ( st != VFTP_OK ) {
                break;
            }
            vftp_bit_set( c->bitmap, d->chunk );
            uint32_t byte = d->chunk >> 3;
            lo = byte < lo ? byte : lo;
            hi = byte > hi ? byte : hi;
            done++;
            fresh++;
            emit_simple( c, VFTP_EV_PROGRESS, size, rcrc, cs, total, done );
        }
        if ( fresh != 0u ) {
            (void) bm_store( c, vmap, lo, hi ); /* losing this only costs a re-fetch after a crash */
        }
        vftp_event_t ev;
        memset( &ev, 0, sizeof( ev ) );
        ev.type = VFTP_EV_ROUND;
        ev.round = c->stats.rounds;
        ev.total_chunks = total;
        ev.done_chunks = done;
        ev.missing = total - done;
        emit( c, &ev );
        if ( st == VFTP_OK && fresh == 0u && ++stall >= ( c->cfg.max_stall == 0u ? 1u : c->cfg.max_stall ) ) {
            st = VFTP_TIMEOUT;
        } else if ( fresh != 0u ) {
            stall = 0;
        }
    }

    uint32_t remote_crc = 0;
    if ( st == VFTP_OK ) {
        st = fetch_crc( c, true, size, &remote_crc );
    }
    if ( st == VFTP_OK ) {
        uint32_t local_crc = 0;
        st = vftp_fs_crc( fs, local_path, size, c->buf, (uint32_t) sizeof( c->buf ), &local_crc );
        vftp_event_t ev;
        memset( &ev, 0, sizeof( ev ) );
        ev.type = VFTP_EV_CRC;
        ev.remote_crc = remote_crc;
        ev.local_crc = local_crc;
        emit( c, &ev );
        if ( st == VFTP_OK && ( local_crc != remote_crc || ( rcrc_valid && local_crc != rcrc ) ) ) {
            st = VFTP_CRC_MISMATCH;
        }
        if ( st == VFTP_OK || st == VFTP_CRC_MISMATCH ) {
            /* Done, or the bytes on disk cannot be trusted chunk by chunk: start over next time. */
            (void) fs->remove( fs->ctx, vmap );
        }
    }
    c->msg.type = VFTP_MSG_DONE;
    uint16_t n = encode( c, c->tx, c->cfg.link->mtu_tx );
    send_done( c, n, st );
    link_close( c );
    return st;
}

/* ---- burst upload -------------------------------------------------------- */

static vftp_ret_t local_file( vftp_client_t *c, const char *path, uint32_t *size, uint32_t *crc )
{
    const vftp_storage_t *fs = c->cfg.fs;
    vftp_ret_t st = fs->stat( fs->ctx, path, size );
    if ( st == VFTP_OK ) {
        st = vftp_fs_crc( fs, path, *size, c->buf, (uint32_t) sizeof( c->buf ), crc );
    }
    return st;
}

vftp_ret_t vftp_burst_upload( vftp_client_t *c, const char *local_path, const char *remote_path )
{
    vftp_ret_t st = check_cfg( c );
    if ( st != VFTP_OK ) {
        return st;
    }
    const vftp_storage_t *fs = c->cfg.fs;
    uint32_t size = 0;
    uint32_t crc = 0;
    if ( !vftp_path_set( c->msg.u.up_req.path, remote_path ) ) {
        return VFTP_INVAL;
    }
    st = local_file( c, local_path, &size, &crc );
    if ( st != VFTP_OK ) {
        return st;
    }
    uint16_t cs = pick_chunk( c->cfg.chunk_size, c->cfg.link->mtu_tx );
    uint32_t total = vftp_total_chunks( size, cs );

    st = link_open( c );
    if ( st != VFTP_OK ) {
        return st;
    }
    c->msg.type = VFTP_MSG_UPLOAD_REQUEST;
    c->msg.u.up_req.size = size;
    c->msg.u.up_req.crc32 = crc;
    c->msg.u.up_req.chunk_size = cs;
    st = b_transact( c, VFTP_MSG_UPLOAD_REPLY, b_any, 0u );
    if ( st == VFTP_OK && c->msg.u.up_rep.ret != VFTP_OK ) {
        st = remote( c, c->msg.u.up_rep.ret );
    }
    if ( st == VFTP_OK ) {
        c->session = c->msg.session;
        emit_simple( c, VFTP_EV_START, size, crc, cs, total, 0u );
        c->msg.type = VFTP_MSG_STATUS_REQUEST;
        st = b_transact( c, VFTP_MSG_STATUS_REPLY, b_any, 0u );
        if ( st == VFTP_OK && c->msg.u.status.ret != VFTP_OK ) {
            st = remote( c, c->msg.u.status.ret );
        }
    }
    if ( st == VFTP_OK && c->msg.u.status.total != total ) {
        st = VFTP_PROTO;
    }
    if ( st == VFTP_OK && c->msg.u.status.complete != 0u ) {
        emit_simple( c, VFTP_EV_RESUME, size, crc, cs, total, c->msg.u.status.complete );
    }

    uint32_t window = c->cfg.window_up != 0u ? c->cfg.window_up : ( c->cfg.window == 0u ? 64u : c->cfg.window );
    uint32_t last = st == VFTP_OK ? c->msg.u.status.complete : 0u;
    uint32_t stall = 0;
    vftp_range_t ranges[VFTP_WIRE_MAX_RANGES];
    while ( st == VFTP_OK && c->msg.u.status.complete < c->msg.u.status.total ) {
        c->stats.rounds++;
        uint16_t nr = c->msg.u.status.missing.entries;
        memcpy( ranges, c->msg.u.status.missing.range, nr * sizeof( vftp_range_t ) );
        uint32_t sent = 0;
        for ( uint16_t i = 0; i < nr && sent < window && st == VFTP_OK; i++ ) {
            for ( uint32_t j = 0; j < ranges[i].count && sent < window; j++ ) {
                uint32_t chunk = ranges[i].next + j;
                if ( chunk >= total ) {
                    break;
                }
                uint16_t want = vftp_chunk_len( size, cs, chunk );
                uint16_t got = 0;
                st = fs->read( fs->ctx, local_path, chunk * (uint32_t) cs, c->tx + VFTP_DATA_HEADER, want, &got );
                if ( st == VFTP_OK && got != want ) {
                    st = VFTP_IO; /* local file changed under us */
                }
                if ( st != VFTP_OK ) {
                    break;
                }
                c->msg.type = VFTP_MSG_DATA;
                c->msg.u.data.chunk = chunk;
                c->msg.u.data.bytes = c->tx + VFTP_DATA_HEADER;
                c->msg.u.data.len = want;
                uint16_t n = encode( c, c->tx, c->cfg.link->mtu_tx );
                st = n == 0u ? VFTP_INVAL : link_send( c, n );
                if ( st != VFTP_OK ) {
                    break; /* a dead link stops the round at once instead of blasting into nothing */
                }
                sent++;
                c->stats.data_tx++;
                if ( c->cfg.tx_gap_ms != 0u && c->cfg.link->sleep_ms ) {
                    c->cfg.link->sleep_ms( c->cfg.link->ctx, c->cfg.tx_gap_ms );
                }
            }
        }
        if ( st != VFTP_OK ) {
            break;
        }
        /* The STATUS round trip doubles as flow control: it is answered after the
         * server has dealt with every DATA sent before it. */
        c->msg.type = VFTP_MSG_STATUS_REQUEST;
        st = b_transact( c, VFTP_MSG_STATUS_REPLY, b_any, 0u );
        if ( st == VFTP_OK && c->msg.u.status.ret != VFTP_OK ) {
            st = remote( c, c->msg.u.status.ret );
        }
        if ( st != VFTP_OK ) {
            break;
        }
        uint32_t now = c->msg.u.status.complete;
        vftp_event_t ev;
        memset( &ev, 0, sizeof( ev ) );
        ev.type = VFTP_EV_ROUND;
        ev.round = c->stats.rounds;
        ev.total_chunks = total;
        ev.done_chunks = now;
        ev.missing = total - now;
        emit( c, &ev );
        emit_simple( c, VFTP_EV_PROGRESS, size, crc, cs, total, now );
        if ( now > last ) {
            stall = 0;
        } else if ( ++stall >= ( c->cfg.max_stall == 0u ? 1u : c->cfg.max_stall ) ) {
            st = VFTP_TIMEOUT;
        }
        last = now;
    }

    if ( st == VFTP_OK ) {
        uint32_t rc = 0;
        st = fetch_crc( c, true, size, &rc );
        if ( st == VFTP_OK ) {
            vftp_event_t ev;
            memset( &ev, 0, sizeof( ev ) );
            ev.type = VFTP_EV_CRC;
            ev.remote_crc = rc;
            ev.local_crc = crc;
            emit( c, &ev );
            if ( rc != crc ) {
                st = VFTP_CRC_MISMATCH;
            }
        }
    }
    c->msg.type = VFTP_MSG_DONE;
    uint16_t n = encode( c, c->tx, c->cfg.link->mtu_tx );
    send_done( c, n, st );
    link_close( c );
    return st;
}

/* ---- burst: delete, list, list.txt ---------------------------------------- */

static vftp_ret_t one_shot( vftp_client_t *c, uint8_t type, uint8_t reply, const char *path )
{
    vftp_ret_t st = check_cfg( c );
    if ( st != VFTP_OK ) {
        return st;
    }
    if ( !vftp_path_set( c->msg.u.path_req.path, path ) ) {
        return VFTP_INVAL;
    }
    st = link_open( c );
    if ( st != VFTP_OK ) {
        return st;
    }
    c->msg.type = type;
    st = b_transact( c, reply, b_any, 0u );
    if ( st == VFTP_OK && c->msg.u.ret_rep.ret != VFTP_OK ) {
        st = remote( c, c->msg.u.ret_rep.ret );
    }
    link_close( c );
    return st;
}

vftp_ret_t vftp_burst_remove( vftp_client_t *c, const char *remote_path )
{
    return one_shot( c, VFTP_MSG_DELETE_REQUEST, VFTP_MSG_DELETE_REPLY, remote_path );
}

vftp_ret_t vftp_burst_list_regen( vftp_client_t *c, const char *dir, char *out_path )
{
    vftp_ret_t st = one_shot( c, VFTP_MSG_LIST_REGEN_REQUEST, VFTP_MSG_LIST_REGEN_REPLY, dir );
    if ( st == VFTP_OK && out_path != NULL ) {
        memcpy( out_path, VFTP_LIST_TXT, sizeof( VFTP_LIST_TXT ) );
    }
    return st;
}

static bool list_page_is( const vftp_client_t *c, uint32_t offset )
{
    return c->msg.u.list_rep.first_file == offset || c->msg.u.list_rep.ret != VFTP_OK;
}

vftp_ret_t vftp_burst_list( vftp_client_t *c, const char *dir, vftp_list_fn cb, void *cb_ctx, uint32_t *total_out )
{
    vftp_ret_t st = check_cfg( c );
    if ( st != VFTP_OK ) {
        return st;
    }
    char path[VFTP_MAX_PATH];
    if ( !vftp_path_set( path, dir ) ) {
        return VFTP_INVAL;
    }
    uint32_t limit = VFTP_LIST_ENTRIES_MAX_FOR( (uint32_t) c->cfg.link->mtu_rx );
    if ( limit > VFTP_WIRE_MAX_ENTRIES ) {
        limit = VFTP_WIRE_MAX_ENTRIES;
    }
    if ( limit == 0u ) {
        return VFTP_INVAL;
    }
    st = link_open( c );
    if ( st != VFTP_OK ) {
        return st;
    }
    uint32_t offset = 0;
    uint32_t total = 0;
    for ( ;; ) {
        c->msg.type = VFTP_MSG_LIST_REQUEST;
        memcpy( c->msg.u.list_req.path, path, VFTP_MAX_PATH );
        c->msg.u.list_req.offset = offset;
        c->msg.u.list_req.limit = (uint16_t) limit;
        st = b_transact( c, VFTP_MSG_LIST_REPLY, list_page_is, offset );
        if ( st == VFTP_OK && c->msg.u.list_rep.ret != VFTP_OK ) {
            st = remote( c, c->msg.u.list_rep.ret );
        }
        if ( st != VFTP_OK ) {
            break;
        }
        total = c->msg.u.list_rep.total_files;
        uint32_t got = c->msg.u.list_rep.packet_files;
        for ( uint32_t i = 0; i < got && cb; i++ ) {
            cb( cb_ctx, total, offset + i, &c->msg.u.list_rep.entry[i] );
        }
        offset += got;
        if ( got == 0u || offset >= total ) {
            break;
        }
    }
    if ( total_out ) {
        *total_out = total;
    }
    link_close( c );
    return st;
}

/* ---- poll ---------------------------------------------------------------- */

static bool p_any( const vftp_client_t *c, uint32_t arg )
{
    (void) c;
    (void) arg;
    return true;
}

static bool p_data_is( const vftp_client_t *c, uint32_t chunk )
{
    return c->msg.u.data.chunk == chunk;
}

static bool p_ack_is( const vftp_client_t *c, uint32_t chunk )
{
    return c->msg.u.data_ack.chunk == chunk || c->msg.u.data_ack.ret != VFTP_OK;
}

static vftp_ret_t p_transact( vftp_client_t *c, uint8_t expect, p_match_fn match, uint32_t arg )
{
    uint16_t n = encode( c, c->tx, c->cfg.link->mtu_tx );
    if ( n == 0u ) {
        return VFTP_INVAL;
    }
    for ( uint8_t a = 0; a < attempts( c ); a++ ) {
        if ( a != 0u ) {
            c->stats.retries++;
        }
        vftp_ret_t st = link_send( c, n );
        if ( st != VFTP_OK ) {
            return st;
        }
        for ( uint32_t skipped = 0; skipped < SKIP_LIMIT; skipped++ ) {
            int len = link_recv( c, c->cfg.timeout_ms );
            if ( len < 0 ) {
                return VFTP_LINK;
            }
            if ( len == 0 ) {
                break;
            }
            if ( vftp_decode( c->rx, (uint16_t) len, &c->msg ) == VFTP_OK && !stale( c ) && c->msg.type == expect &&
                 match( c, arg ) ) {
                return VFTP_OK;
            }
        }
    }
    return VFTP_TIMEOUT;
}

/* ---- poll download resume: a watermark sidecar ("XXXXXXXX.VFP") next to the local file --------
 * magic:4 version:2 chunk_size:2 size:4 crc32:4 committed:4 path_crc:4, little-endian, like the
 * server's .VFI. `committed` only ever counts chunks already written to the local file. */

static void wmap_encode( uint8_t *h, uint16_t cs, uint32_t size, uint32_t crc, uint32_t committed,
                         const char *remote_path )
{
    vftp_put_u32( h + 0, WMAP_MAGIC );
    vftp_put_u16( h + 4, VMAP_VERSION );
    vftp_put_u16( h + 6, cs );
    vftp_put_u32( h + 8, size );
    vftp_put_u32( h + 12, crc );
    vftp_put_u32( h + 16, committed );
    vftp_put_u32( h + 20, vftp_crc32( remote_path, strlen( remote_path ) ) );
}

static vftp_ret_t wmap_store( vftp_client_t *c, const char *wmap, uint16_t cs, uint32_t size, uint32_t crc,
                              uint32_t committed, const char *remote_path )
{
    uint8_t h[WMAP_SIZE];
    wmap_encode( h, cs, size, crc, committed, remote_path );
    return c->cfg.fs->write( c->cfg.fs->ctx, wmap, 0u, h, (uint16_t) WMAP_SIZE );
}

/* Chunks of this remote file already in `local` from an earlier attempt (0: start over). Saved
 * with another chunk size, the whole new chunks below the old watermark are kept. */
static uint32_t wmap_load( vftp_client_t *c, const char *wmap, const char *local, uint16_t cs, uint32_t size,
                           uint32_t crc, uint32_t total, const char *remote_path )
{
    const vftp_storage_t *fs = c->cfg.fs;
    uint8_t want[WMAP_SIZE];
    uint8_t h[WMAP_SIZE];
    uint16_t got = 0;
    wmap_encode( want, cs, size, crc, 0u, remote_path );
    if ( fs->read( fs->ctx, wmap, 0u, h, (uint16_t) WMAP_SIZE, &got ) != VFTP_OK || got != WMAP_SIZE ||
         memcmp( h, want, 4u ) != 0 || memcmp( h + 4, want + 4, 2u ) != 0 || memcmp( h + 8, want + 8, 8u ) != 0 ||
         memcmp( h + 20, want + 20, 4u ) != 0 ) {
        return 0u; /* none, or another file's */
    }
    uint16_t old_cs = vftp_get_u16( h + 6 );
    uint32_t old_committed = vftp_get_u32( h + 16 );
    if ( old_cs == 0u || old_committed > vftp_total_chunks( size, old_cs ) ) {
        return 0u;
    }
    uint32_t bytes = old_committed * (uint32_t) old_cs;
    uint32_t committed = bytes >= size ? total : bytes / cs;
    uint32_t need = committed * (uint32_t) cs > size ? size : committed * (uint32_t) cs;
    uint32_t have = 0;
    if ( fs->stat( fs->ctx, local, &have ) != VFTP_OK || have < need ) {
        return 0u; /* the local file lost what the watermark claims */
    }
    return committed;
}

/* expect: download -- the CRC the DOWNLOAD_REPLY announced, if expect_valid; upload -- the local CRC.
 * wmap: a poll download's watermark sidecar, removed once the result is final (success or bad CRC). */
static vftp_ret_t p_finish( vftp_client_t *c, vftp_ret_t st, uint32_t expect, bool expect_valid, bool download,
                            uint32_t size, const char *local_path, const char *wmap )
{
    uint32_t rc = 0;
    uint32_t local_crc = download ? 0u : expect;
    if ( st == VFTP_OK ) {
        st = fetch_crc( c, false, size, &rc );
    }
    if ( st == VFTP_OK ) {
        if ( download ) {
            st = vftp_fs_crc( c->cfg.fs, local_path, size, c->buf, (uint32_t) sizeof( c->buf ), &local_crc );
        }
        vftp_event_t ev;
        memset( &ev, 0, sizeof( ev ) );
        ev.type = VFTP_EV_CRC;
        ev.remote_crc = rc;
        ev.local_crc = local_crc;
        emit( c, &ev );
        if ( st == VFTP_OK && ( rc != local_crc || ( expect_valid && expect != local_crc ) ) ) {
            st = VFTP_CRC_MISMATCH;
        }
        if ( wmap != NULL && ( st == VFTP_OK || st == VFTP_CRC_MISMATCH ) ) {
            (void) c->cfg.fs->remove( c->cfg.fs->ctx, wmap ); /* done, or the local bytes cannot be trusted */
        }
    }
    c->msg.type = VFTP_MSG_DONE;
    uint16_t n = encode( c, c->tx, c->cfg.link->mtu_tx );
    send_done( c, n, st );
    link_close( c );
    return st;
}

vftp_ret_t vftp_poll_download( vftp_client_t *c, const char *remote_path, const char *local_path )
{
    vftp_ret_t st = check_cfg( c );
    if ( st != VFTP_OK ) {
        return st;
    }
    const vftp_storage_t *fs = c->cfg.fs;
    char wmap[VFTP_SIDECAR_PATH_MAX];
    if ( !vftp_path_set( c->msg.u.down_req.path, remote_path ) ||
         !vftp_sidecar_path( local_path, "VFP", false, wmap, sizeof( wmap ) ) ) {
        return VFTP_INVAL;
    }
    uint32_t local_size = 0;
    if ( fs->stat( fs->ctx, local_path, &local_size ) == VFTP_ISDIR ) {
        return VFTP_ISDIR; /* the destination is a local directory: refused before the server is asked */
    }
    uint16_t cs = pick_chunk( c->cfg.chunk_size, c->cfg.link->mtu_rx );
    st = link_open( c );
    if ( st != VFTP_OK ) {
        return st;
    }
    c->msg.type = VFTP_MSG_DOWNLOAD_REQUEST;
    c->msg.u.down_req.chunk_size = cs;
    st = p_transact( c, VFTP_MSG_DOWNLOAD_REPLY, p_any, 0u );
    if ( st == VFTP_OK && c->msg.u.down_rep.ret != VFTP_OK ) {
        st = remote( c, c->msg.u.down_rep.ret );
    }
    if ( st != VFTP_OK ) {
        link_close( c );
        return st;
    }
    c->session = c->msg.session; /* every later message of this transfer carries it */
    uint32_t size = c->msg.u.down_rep.size;
    bool rcrc_valid = ( c->msg.u.down_rep.flags & VFTP_DL_CRC_VALID ) != 0u;
    uint32_t rcrc = rcrc_valid ? c->msg.u.down_rep.crc32 : 0u;
    uint32_t total = vftp_total_chunks( size, cs );
    emit_simple( c, VFTP_EV_START, size, rcrc, cs, total, 0u );

    /* Resume from the watermark of an earlier attempt at this same remote file, if the local file
     * still holds those chunks; otherwise start clean. */
    uint32_t start = wmap_load( c, wmap, local_path, cs, size, rcrc, total, remote_path );
    if ( start != 0u ) {
        emit_simple( c, VFTP_EV_RESUME, size, rcrc, cs, total, start );
    } else {
        st = fs->truncate( fs->ctx, local_path );
        if ( st == VFTP_OK ) {
            st = wmap_store( c, wmap, cs, size, rcrc, 0u, remote_path );
        }
    }
    uint32_t held = start;
    for ( uint32_t chunk = start; st == VFTP_OK && chunk < total; chunk++ ) {
        c->stats.rounds++;
        c->msg.type = VFTP_MSG_GET_CHUNK;
        c->msg.u.get_chunk.chunk = chunk;
        st = p_transact( c, VFTP_MSG_DATA, p_data_is, chunk );
        if ( st != VFTP_OK ) {
            break;
        }
        c->stats.data_rx++;
        if ( c->msg.u.data.len != vftp_chunk_len( size, cs, chunk ) ) {
            st = VFTP_PROTO;
            break;
        }
        st = fs->write( fs->ctx, local_path, chunk * (uint32_t) cs, c->msg.u.data.bytes, c->msg.u.data.len );
        if ( st == VFTP_OK ) {
            held = chunk + 1u;
            if ( held % WMAP_EVERY == 0u ) {
                (void) wmap_store( c, wmap, cs, size, rcrc, held, remote_path ); /* a lost save only costs a re-fetch */
            }
            emit_simple( c, VFTP_EV_PROGRESS, size, rcrc, cs, total, held );
        }
    }
    if ( st != VFTP_OK && held != start ) {
        (void) wmap_store( c, wmap, cs, size, rcrc, held, remote_path ); /* the next attempt goes on from here */
    }
    return p_finish( c, st, rcrc, rcrc_valid, true, size, local_path, wmap );
}

vftp_ret_t vftp_poll_upload( vftp_client_t *c, const char *local_path, const char *remote_path )
{
    vftp_ret_t st = check_cfg( c );
    if ( st != VFTP_OK ) {
        return st;
    }
    const vftp_storage_t *fs = c->cfg.fs;
    uint32_t size = 0;
    uint32_t crc = 0;
    if ( !vftp_path_set( c->msg.u.up_req.path, remote_path ) ) {
        return VFTP_INVAL;
    }
    st = local_file( c, local_path, &size, &crc );
    if ( st != VFTP_OK ) {
        return st;
    }
    uint16_t cs = pick_chunk( c->cfg.chunk_size, c->cfg.link->mtu_tx );
    uint32_t total = vftp_total_chunks( size, cs );
    st = link_open( c );
    if ( st != VFTP_OK ) {
        return st;
    }
    c->msg.type = VFTP_MSG_UPLOAD_REQUEST;
    c->msg.u.up_req.size = size;
    c->msg.u.up_req.crc32 = crc;
    c->msg.u.up_req.chunk_size = cs;
    st = p_transact( c, VFTP_MSG_UPLOAD_REPLY, p_any, 0u );
    if ( st == VFTP_OK && c->msg.u.up_rep.ret != VFTP_OK ) {
        st = remote( c, c->msg.u.up_rep.ret );
    }
    if ( st != VFTP_OK ) {
        link_close( c );
        return st;
    }
    c->session = c->msg.session;
    uint32_t chunk = c->msg.u.up_rep.resume_from;
    emit_simple( c, VFTP_EV_START, size, crc, cs, total, 0u );
    if ( chunk > total ) {
        st = VFTP_PROTO;
    } else if ( chunk != 0u ) {
        emit_simple( c, VFTP_EV_RESUME, size, crc, cs, total, chunk );
    }
    uint32_t resyncs = 0;
    while ( st == VFTP_OK && chunk < total ) {
        c->stats.rounds++;
        uint16_t want = vftp_chunk_len( size, cs, chunk );
        uint16_t got = 0;
        st = fs->read( fs->ctx, local_path, chunk * (uint32_t) cs, c->tx + VFTP_DATA_HEADER, want, &got );
        if ( st == VFTP_OK && got != want ) {
            st = VFTP_IO;
        }
        if ( st != VFTP_OK ) {
            break;
        }
        c->msg.type = VFTP_MSG_DATA;
        c->msg.u.data.chunk = chunk;
        c->msg.u.data.bytes = c->tx + VFTP_DATA_HEADER;
        c->msg.u.data.len = want;
        st = p_transact( c, VFTP_MSG_DATA_ACK, p_ack_is, chunk );
        if ( st != VFTP_OK ) {
            break;
        }
        c->stats.data_tx++;
        uint8_t ret = c->msg.u.data_ack.ret;
        if ( ret == VFTP_OK ) {
            chunk++;
            resyncs = 0;
            emit_simple( c, VFTP_EV_PROGRESS, size, crc, cs, total, chunk );
        } else if ( ret == VFTP_RANGE && c->msg.u.data_ack.chunk <= total && ++resyncs <= 4u * attempts( c ) ) {
            chunk = c->msg.u.data_ack.chunk; /* the server's watermark is the truth */
        } else {
            st = ret == VFTP_RANGE ? VFTP_PROTO : remote( c, ret );
        }
    }
    return p_finish( c, st, crc, false, false, size, local_path, NULL );
}

/* ---- listing method 3: by index (both services) --------------------------- */

static bool b_index_is( const vftp_client_t *c, uint32_t index )
{
    return c->msg.u.idx_rep.index == index;
}

static bool p_index_is( const vftp_client_t *c, uint32_t index )
{
    return c->msg.u.idx_rep.index == index;
}

static vftp_ret_t index_query( vftp_client_t *c, bool burst, const char *dir, uint32_t index, vftp_index_rep_t *info )
{
    vftp_ret_t st = check_cfg( c );
    if ( st != VFTP_OK ) {
        return st;
    }
    vftp_index_req_t *q = burst ? &c->msg.u.idx_req : &c->msg.u.idx_req;
    if ( !vftp_path_set( q->path, dir ) ) {
        return VFTP_INVAL;
    }
    q->index = index;
    st = link_open( c );
    if ( st != VFTP_OK ) {
        return st;
    }
    if ( burst ) {
        c->msg.type = VFTP_MSG_INDEX_REQUEST;
        st = b_transact( c, VFTP_MSG_INDEX_REPLY, b_index_is, index );
    } else {
        c->msg.type = VFTP_MSG_INDEX_REQUEST;
        st = p_transact( c, VFTP_MSG_INDEX_REPLY, p_index_is, index );
    }
    link_close( c );
    if ( st != VFTP_OK ) {
        return st;
    }
    *info = burst ? c->msg.u.idx_rep : c->msg.u.idx_rep;
    return info->ret == VFTP_OK ? VFTP_OK : remote( c, info->ret );
}

vftp_ret_t vftp_burst_index( vftp_client_t *c, const char *dir, uint32_t index, vftp_index_rep_t *info )
{
    return index_query( c, true, dir, index, info );
}

vftp_ret_t vftp_poll_index( vftp_client_t *c, const char *dir, uint32_t index, vftp_index_rep_t *info )
{
    return index_query( c, false, dir, index, info );
}

/* "<dir>/<name>", no doubled slash, "" dir -> name alone. */
static bool join_remote( const char *dir, const char *name, char *out )
{
    size_t dl = vftp_str_len( dir, VFTP_MAX_PATH );
    size_t nl = vftp_str_len( name, VFTP_MAX_FILE_NAME + 1u );
    bool slash = dl > 0u && dir[dl - 1u] != '/';
    if ( dl + ( slash ? 1u : 0u ) + nl + 1u > VFTP_MAX_PATH ) {
        return false;
    }
    memcpy( out, dir, dl );
    if ( slash ) {
        out[dl++] = '/';
    }
    memcpy( out + dl, name, nl );
    out[dl + nl] = '\0';
    return true;
}

static vftp_ret_t download_index( vftp_client_t *c, bool burst, const char *dir, uint32_t index, const char *local,
                                  char *remote_out )
{
    vftp_index_rep_t info;
    char remote_path[VFTP_MAX_PATH];
    if ( index == VFTP_INDEX_COUNT_ONLY ) {
        return VFTP_INVAL;
    }
    vftp_ret_t st = index_query( c, burst, dir, index, &info );
    if ( st != VFTP_OK ) {
        return st;
    }
    if ( !join_remote( dir, info.name, remote_path ) ) {
        return VFTP_INVAL;
    }
    if ( remote_out != NULL ) {
        memcpy( remote_out, remote_path, VFTP_MAX_PATH );
    }
    if ( info.flags & VFTP_ENTRY_DIR ) {
        return VFTP_ISDIR; /* never "download" a directory as an empty file */
    }
    if ( info.flags & VFTP_ENTRY_NAME_CUT ) {
        return VFTP_BADNAME; /* only the start of a longer name came: the path would be wrong */
    }
    const char *dst = local != NULL ? local : info.name;
    return burst ? vftp_burst_download( c, remote_path, dst ) : vftp_poll_download( c, remote_path, dst );
}

vftp_ret_t vftp_burst_download_index( vftp_client_t *c, const char *dir, uint32_t index, const char *local,
                                      char *remote_out )
{
    return download_index( c, true, dir, index, local, remote_out );
}

vftp_ret_t vftp_poll_download_index( vftp_client_t *c, const char *dir, uint32_t index, const char *local,
                                     char *remote_out )
{
    return download_index( c, false, dir, index, local, remote_out );
}
