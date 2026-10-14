/* libvftp: vftp_poll server engine -- stop-and-wait, one chunk per round trip.
 *
 * Upload is strictly sequential, so its resume state is a single watermark:
 * chunks 0..committed-1 are on storage, and the sidecar ("<dir>/XXXXXXXX.VFI")
 * says so. A DATA_ACK is only sent after the chunk's write returned, and the
 * watermark only advances after that, so "acked" never runs ahead of "stored".
 * A repeated DATA for the chunk just acknowledged (its ACK got lost) is acked
 * again without rewriting -- that is what makes the client's retransmission
 * safe.
 *
 * Sidecar ("XXXXXXXX.VFI" in the root): magic:4 version:2 chunk_size:2 size:4
 * crc32:4 committed:4 path_crc:4. A resume with another chunk_size keeps the
 * bytes below the watermark (rounded down to the new chunking).
 *
 * File CRC: as in burst, never more than VFTP_CFG_CRC_STEP_BYTES of reading per
 * request. A download is checksummed from the GET_CHUNK reads themselves (they
 * come in order), so CRC_REQUEST normally finds it done; an upload's CRC is
 * read back from storage, VFTP_AGAIN until finished.
 */
#include <string.h>

#include "vftp/vftp_crc32.h"
#include "vftp/vftp_server.h"
#include "vftp_server_internal.h"

enum { ST_IDLE = 0, ST_DOWNLOAD = 1, ST_UPLOAD = 2 };

#define IDX_MAGIC 0x49465456u /* "VTFI" */
#define IDX_VERSION 1u
#define IDX_SIZE 24u

static void send_tx( vftp_poll_server_t *s, vftp_send_fn send, void *ctx )
{
    uint16_t n = vftp_encode( &s->tx, s->pkt, s->cfg.mtu_tx );
    if ( n != 0u ) {
        (void) send( ctx, s->pkt, n );
    }
}

/* Next session byte: 1..255, never 0 (0 = no session). */
static uint8_t new_session( vftp_poll_server_t *s )
{
    s->last_session = (uint8_t) ( s->last_session == 255u ? 1u : s->last_session + 1u );
    return s->last_session;
}

static void session_reset( vftp_poll_server_t *s )
{
    s->state = ST_IDLE;
    s->session = 0u;
    s->path[0] = '\0';
    s->idx_path[0] = '\0';
    s->size = 0;
    s->crc32 = 0;
    s->total = 0;
    s->committed = 0;
    s->chunk_size = 0;
    vftp_crcjob_init( &s->crc, 0u );
    s->io_streak = 0;
}

static void idx_encode( const vftp_poll_server_t *s, uint32_t committed, uint8_t *h )
{
    vftp_put_u32( h + 0, IDX_MAGIC );
    vftp_put_u16( h + 4, IDX_VERSION );
    vftp_put_u16( h + 6, s->chunk_size );
    vftp_put_u32( h + 8, s->size );
    vftp_put_u32( h + 12, s->crc32 );
    vftp_put_u32( h + 16, committed );
    vftp_put_u32( h + 20, vftp_crc32( s->path, strlen( s->path ) ) );
}

static uint32_t idx_load( const vftp_poll_server_t *s )
{
    uint8_t want[IDX_SIZE];
    uint8_t got_b[IDX_SIZE];
    uint16_t got = 0;
    if ( s->cfg.fs->read( s->cfg.fs->ctx, s->idx_path, 0, got_b, (uint16_t) IDX_SIZE, &got ) != VFTP_OK ||
         got != IDX_SIZE ) {
        return 0u;
    }
    uint32_t committed = vftp_get_u32( got_b + 16 );
    uint16_t old_cs = vftp_get_u16( got_b + 6 );
    idx_encode( s, committed, want );
    if ( memcmp( want, got_b, 6u ) != 0 || memcmp( want + 8, got_b + 8, 8u ) != 0 ||
         memcmp( want + 20, got_b + 20, 4u ) != 0 || old_cs == 0u ||
         committed > vftp_total_chunks( s->size, old_cs ) ) {
        return 0u; /* a different transfer's state: start over */
    }
    if ( old_cs != s->chunk_size ) {
        /* Bytes [0, committed * old_cs) are stored; keep the whole new chunks inside them. */
        uint32_t bytes = committed * (uint32_t) old_cs;
        if ( bytes >= s->size ) {
            return s->total;
        }
        committed = bytes / s->chunk_size;
        VFTP_LOG_INFO( "vftp_poll: %s chunk size %u -> %u, resumes at chunk %lu", s->path, (unsigned int) old_cs,
                       (unsigned int) s->chunk_size, (unsigned long) committed );
    }
    return committed;
}

static vftp_ret_t idx_store( const vftp_poll_server_t *s )
{
    uint8_t h[IDX_SIZE];
    idx_encode( s, s->committed, h );
    return s->cfg.fs->write( s->cfg.fs->ctx, s->idx_path, 0, h, (uint16_t) IDX_SIZE );
}

static void on_download_request( vftp_poll_server_t *s, vftp_send_fn send, void *ctx )
{
    const char *path = s->rx.u.down_req.path;
    uint16_t cs = s->rx.u.down_req.chunk_size;
    vftp_ret_t st = VFTP_OK;
    uint32_t size = 0;

    s->tx.type = VFTP_MSG_DOWNLOAD_REPLY;
    if ( s->state != ST_IDLE ) {
        if ( s->state == ST_DOWNLOAD && cs == s->chunk_size && vftp_str_eq( path, s->path ) ) {
            s->activity++;
            size = s->size; /* repeated request: same answer */
        } else {
            st = VFTP_BUSY;
        }
    } else if ( cs == 0u || (uint32_t) cs + VFTP_DATA_HEADER > s->cfg.mtu_tx ) {
        st = VFTP_INVAL;
    } else if ( ( st = vftp_client_path_ok( s->cfg.fs, path ) ) == VFTP_OK ) {
        st = s->cfg.fs->stat( s->cfg.fs->ctx, path, &size );
        if ( st == VFTP_OK ) {
            session_reset( s );
            vftp_crcjob_init( &s->crc, size );
            st = vftp_crcjob_step( &s->crc, s->cfg.fs, path, s->buf, (uint32_t) sizeof( s->buf ),
                                   s->crc_step );
        }
        if ( st != VFTP_OK ) {
            session_reset( s );
        } else {
            s->state = ST_DOWNLOAD;
            s->session = new_session( s );
            memcpy( s->path, path, VFTP_MAX_PATH );
            s->size = size;
            s->chunk_size = cs;
            s->total = vftp_total_chunks( size, cs );
            VFTP_LOG_INFO( "vftp_poll: download %s, %lu bytes", s->path, (unsigned long) size );
        }
    }
    bool valid = st == VFTP_OK && s->crc.done;
    s->tx.session = 0u; /* a refusal opens no session */
    if ( st == VFTP_OK ) {
        s->tx.session = s->session;
    }
    s->tx.u.down_rep.ret = (uint8_t) st;
    s->tx.u.down_rep.size = st == VFTP_OK ? size : 0u;
    s->tx.u.down_rep.crc32 = valid ? s->crc.value : 0u;
    s->tx.u.down_rep.flags = valid ? VFTP_DL_CRC_VALID : 0u;
    send_tx( s, send, ctx );
}

static void on_get_chunk( vftp_poll_server_t *s, vftp_send_fn send, void *ctx )
{
    uint32_t chunk = s->rx.u.get_chunk.chunk;
    if ( s->state != ST_DOWNLOAD || chunk >= s->total ) {
        return; /* no reply defined: the client times out */
    }
    s->activity++;
    uint16_t want = vftp_chunk_len( s->size, s->chunk_size, chunk );
    uint16_t got = 0;
    if ( s->cfg.fs->read( s->cfg.fs->ctx, s->path, chunk * (uint32_t) s->chunk_size, s->pkt + VFTP_DATA_HEADER, want,
                          &got ) != VFTP_OK ||
         got != want ) {
        return;
    }
    vftp_crcjob_feed( &s->crc, chunk * (uint32_t) s->chunk_size, s->pkt + VFTP_DATA_HEADER, got );
    s->tx.type = VFTP_MSG_DATA;
    s->tx.session = s->session;
    s->tx.u.data.chunk = chunk;
    s->tx.u.data.bytes = s->pkt + VFTP_DATA_HEADER;
    s->tx.u.data.len = got;
    send_tx( s, send, ctx );
}

static void on_upload_request( vftp_poll_server_t *s, vftp_send_fn send, void *ctx )
{
    vftp_ret_t st = VFTP_OK;
    uint16_t cs = s->rx.u.up_req.chunk_size;

    if ( s->state != ST_IDLE ) {
        bool same = s->state == ST_UPLOAD && s->size == s->rx.u.up_req.size && s->crc32 == s->rx.u.up_req.crc32 &&
                    s->chunk_size == cs && vftp_str_eq( s->path, s->rx.u.up_req.path );
        st = same ? VFTP_OK : VFTP_BUSY;
        if ( same ) {
            s->activity++;
        }
    } else if ( cs == 0u || (uint32_t) cs + VFTP_DATA_HEADER > s->cfg.mtu_rx ) {
        st = VFTP_INVAL;
    } else if ( ( st = vftp_upload_target_ok( s->cfg.fs, s->rx.u.up_req.path ) ) != VFTP_OK ) {
        /* refused before any data is sent */
    } else {
        session_reset( s );
        s->state = ST_UPLOAD;
        s->session = new_session( s );
        memcpy( s->path, s->rx.u.up_req.path, VFTP_MAX_PATH );
        s->size = s->rx.u.up_req.size;
        s->crc32 = s->rx.u.up_req.crc32;
        s->chunk_size = cs;
        s->total = vftp_total_chunks( s->size, cs );
        if ( !vftp_sidecar_path( s->path, "VFI", true, s->idx_path, sizeof( s->idx_path ) ) ) {
            st = VFTP_INVAL;
        } else {
            s->committed = idx_load( s );
            if ( s->committed == 0u ) {
                st = s->cfg.fs->truncate( s->cfg.fs->ctx, s->path );
            }
            if ( st == VFTP_OK ) {
                st = idx_store( s );
            }
        }
        if ( st != VFTP_OK ) {
            session_reset( s );
        } else {
            vftp_crcjob_init( &s->crc, s->size );
            VFTP_LOG_INFO( "vftp_poll: upload %s, %lu bytes, from chunk %lu", s->path, (unsigned long) s->size,
                           (unsigned long) s->committed );
        }
    }
    s->tx.type = VFTP_MSG_UPLOAD_REPLY;
    s->tx.session = 0u; /* a refusal opens no session */
    if ( st == VFTP_OK ) {
        s->tx.session = s->session;
    }
    s->tx.u.up_rep.ret = (uint8_t) st;
    s->tx.u.up_rep.resume_from = st == VFTP_OK ? s->committed : 0u;
    send_tx( s, send, ctx );
}

static void on_data( vftp_poll_server_t *s, vftp_send_fn send, void *ctx )
{
    const vftp_data_t *d = &s->rx.u.data;
    vftp_ret_t st = VFTP_OK;
    uint32_t ack_chunk = d->chunk;

    if ( s->state != ST_UPLOAD ) {
        st = VFTP_INVAL;
    } else if ( ++s->activity == 0u ) {
        /* counter wrapped: nothing to do, it only has to move */
    } else if ( s->committed > 0u && d->chunk == s->committed - 1u ) {
        st = VFTP_OK; /* our previous ACK got lost: acknowledge again, do not rewrite */
    } else if ( d->chunk != s->committed || d->chunk >= s->total ) {
        st = VFTP_RANGE; /* tell the client where we really are */
        ack_chunk = s->committed;
    } else if ( d->len != vftp_chunk_len( s->size, s->chunk_size, d->chunk ) ) {
        st = VFTP_INVAL;
        ack_chunk = s->committed;
    } else {
        st = s->cfg.fs->write( s->cfg.fs->ctx, s->path, d->chunk * (uint32_t) s->chunk_size, d->bytes, d->len );
        if ( st == VFTP_OK ) {
            s->committed++;
            s->io_streak = 0;
            (void) idx_store( s ); /* a lost watermark only costs a resend after a reboot */
        } else {
            ack_chunk = s->committed;
            if ( s->io_streak < UINT8_MAX ) {
                s->io_streak++;
            }
            if ( s->io_streak < VFTP_CFG_IO_FAIL_LIMIT ) {
                st = VFTP_RANGE; /* transient: "resend from committed", the client retries */
            }
        }
    }
    s->tx.type = VFTP_MSG_DATA_ACK;
    s->tx.session = s->rx.session; /* the open session's (stale ones never get here), or the asker's when idle */
    s->tx.u.data_ack.chunk = ack_chunk;
    s->tx.u.data_ack.ret = (uint8_t) st;
    send_tx( s, send, ctx );
}

static void on_crc_request( vftp_poll_server_t *s, vftp_send_fn send, void *ctx )
{
    vftp_ret_t st = VFTP_OK;
    if ( s->state != ST_IDLE ) {
        s->activity++;
    }
    if ( s->state == ST_UPLOAD ) {
        if ( s->committed < s->total ) {
            st = VFTP_RANGE;
        }
    } else if ( s->state != ST_DOWNLOAD ) {
        st = VFTP_INVAL;
    }
    if ( st == VFTP_OK ) {
        st = vftp_crcjob_step( &s->crc, s->cfg.fs, s->path, s->buf, (uint32_t) sizeof( s->buf ),
                               s->crc_step );
    }
    if ( st == VFTP_OK && !s->crc.done ) {
        st = VFTP_AGAIN;
    }
    s->tx.type = VFTP_MSG_CRC_REPLY;
    s->tx.session = s->rx.session;
    s->tx.u.crc_rep.ret = (uint8_t) st;
    s->tx.u.crc_rep.crc32 = st == VFTP_OK ? s->crc.value : 0u;
    s->tx.u.crc_rep.done = s->crc.off;
    send_tx( s, send, ctx );
}

static vftp_srv_action_t on_index( vftp_poll_server_t *s, vftp_send_fn send, void *ctx )
{
    vftp_index_req_t q = s->rx.u.idx_req;
    s->tx.type = VFTP_MSG_INDEX_REPLY;
    s->tx.session = 0u;
    vftp_index_lookup( s->cfg.fs, &q, &s->tx.u.idx_rep );
    send_tx( s, send, ctx );
    return s->state == ST_IDLE ? VFTP_SRV_CLOSE : VFTP_SRV_KEEP;
}

static void finish( vftp_poll_server_t *s )
{
    if ( s->state == ST_UPLOAD && s->committed >= s->total ) {
        (void) s->cfg.fs->remove( s->cfg.fs->ctx, s->idx_path );
    }
    if ( s->state != ST_IDLE ) {
        VFTP_LOG_INFO( "vftp_poll: %s closed", s->path );
    }
    session_reset( s );
}

vftp_ret_t vftp_poll_server_init( vftp_poll_server_t *s, const vftp_server_cfg_t *cfg )
{
    if ( s == NULL || cfg == NULL || cfg->fs == NULL || cfg->mtu_tx > VFTP_CFG_PACKET_MAX ||
         cfg->mtu_rx > VFTP_CFG_PACKET_MAX || cfg->mtu_tx <= VFTP_DATA_HEADER || cfg->mtu_rx <= VFTP_DATA_HEADER ) {
        return VFTP_INVAL;
    }
    s->cfg = *cfg;
    s->crc_step = VFTP_CFG_CRC_STEP_BYTES;
    s->last_session = 0u;
    session_reset( s );
    return VFTP_OK;
}

vftp_srv_action_t vftp_poll_server_handle( vftp_poll_server_t *s, const uint8_t *pkt, uint16_t len, vftp_send_fn send,
                                           void *send_ctx )
{
    if ( vftp_decode( pkt, len, &s->rx ) != VFTP_OK ) {
        return VFTP_SRV_KEEP;
    }
    if ( s->state != ST_IDLE && vftp_msg_has_session( s->rx.type ) && s->rx.session != s->session ) {
        return VFTP_SRV_KEEP; /* a late packet of an earlier transfer: dropped, see the burst server */
    }
    switch ( s->rx.type ) {
        case VFTP_MSG_DOWNLOAD_REQUEST:
            on_download_request( s, send, send_ctx );
            return VFTP_SRV_KEEP;
        case VFTP_MSG_GET_CHUNK:
            on_get_chunk( s, send, send_ctx );
            return VFTP_SRV_KEEP;
        case VFTP_MSG_UPLOAD_REQUEST:
            on_upload_request( s, send, send_ctx );
            return VFTP_SRV_KEEP;
        case VFTP_MSG_DATA:
            on_data( s, send, send_ctx );
            return VFTP_SRV_KEEP;
        case VFTP_MSG_CRC_REQUEST:
            on_crc_request( s, send, send_ctx );
            return VFTP_SRV_KEEP;
        case VFTP_MSG_DONE:
        case VFTP_MSG_ABORT:
            finish( s );
            return VFTP_SRV_CLOSE;
        case VFTP_MSG_INDEX_REQUEST:
            return on_index( s, send, send_ctx );
        default:
            return VFTP_SRV_KEEP;
    }
}

void vftp_poll_server_abort( vftp_poll_server_t *s )
{
    finish( s );
}

bool vftp_poll_server_busy( const vftp_poll_server_t *s )
{
    return s->state != ST_IDLE;
}
