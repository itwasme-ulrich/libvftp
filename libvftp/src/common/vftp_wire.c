/* libvftp: byte-exact message encode/decode -- the only place that touches wire bytes.
 * Layouts: include/vftp/vftp_wire.h and docs/protocol.md. */
#include <string.h>

#include "vftp/vftp_wire.h"
#include "vftp_internal.h"

/* ---- bounded writer / reader ------------------------------------------- */

typedef struct {
    uint8_t *p;
    uint16_t cap;
    uint16_t len;
    bool ok;
} wbuf_t;

typedef struct {
    const uint8_t *p;
    uint16_t len;
    uint16_t pos;
    bool ok;
} rbuf_t;

static uint8_t *w_take( wbuf_t *w, uint16_t n )
{
    if ( !w->ok || (uint32_t) w->len + n > w->cap ) {
        w->ok = false;
        return NULL;
    }
    uint8_t *at = w->p + w->len;
    w->len = (uint16_t) ( w->len + n );
    return at;
}

static void w_u8( wbuf_t *w, uint8_t v )
{
    uint8_t *at = w_take( w, 1u );
    if ( at ) {
        at[0] = v;
    }
}

static void w_u16( wbuf_t *w, uint16_t v )
{
    uint8_t *at = w_take( w, 2u );
    if ( at ) {
        vftp_put_u16( at, v );
    }
}

static void w_u32( wbuf_t *w, uint32_t v )
{
    uint8_t *at = w_take( w, 4u );
    if ( at ) {
        vftp_put_u32( at, v );
    }
}

/* str = len:1 + len bytes, no NUL. A path never exceeds VFTP_MAX_PATH - 1 characters. */
static void w_str( wbuf_t *w, const char *str, size_t max )
{
    size_t n = vftp_str_len( str, max + 1u );
    if ( n > max || n > 255u ) {
        w->ok = false; /* too long: refuse rather than truncate */
        return;
    }
    w_u8( w, (uint8_t) n );
    uint8_t *at = w_take( w, (uint16_t) n );
    if ( at && n != 0u ) {
        memcpy( at, str, n );
    }
}

static void w_bytes( wbuf_t *w, const uint8_t *src, uint16_t n )
{
    uint8_t *at = w_take( w, n );
    if ( at && n != 0u ) {
        memmove( at, src, n ); /* src may already sit at `at` (payload read in place) */
    }
}

static void w_ranges( wbuf_t *w, const vftp_ranges_t *r )
{
    if ( r->entries > VFTP_WIRE_MAX_RANGES ) {
        w->ok = false;
        return;
    }
    w_u16( w, r->entries );
    for ( uint16_t i = 0; i < r->entries; i++ ) {
        w_u32( w, r->range[i].next );
        w_u32( w, r->range[i].count );
    }
}

static const uint8_t *r_take( rbuf_t *r, uint16_t n )
{
    if ( !r->ok || (uint32_t) r->pos + n > r->len ) {
        r->ok = false;
        return NULL;
    }
    const uint8_t *at = r->p + r->pos;
    r->pos = (uint16_t) ( r->pos + n );
    return at;
}

static uint8_t r_u8( rbuf_t *r )
{
    const uint8_t *at = r_take( r, 1u );
    if ( at == NULL ) {
        return 0u;
    }
    return at[0];
}

static uint16_t r_u16( rbuf_t *r )
{
    const uint8_t *at = r_take( r, 2u );
    if ( at == NULL ) {
        return 0u;
    }
    return vftp_get_u16( at );
}

static uint32_t r_u32( rbuf_t *r )
{
    const uint8_t *at = r_take( r, 4u );
    if ( at == NULL ) {
        return 0u;
    }
    return vftp_get_u32( at );
}

/* A path: refused (whole message invalid) if longer than the buffer or holding a NUL. */
static void r_path( rbuf_t *r, char *dst )
{
    uint8_t n = r_u8( r );
    const uint8_t *at = r_take( r, n );
    dst[0] = '\0';
    if ( at == NULL ) {
        return;
    }
    if ( n > VFTP_MAX_PATH - 1u || memchr( at, 0, n ) != NULL ) {
        r->ok = false;
        return;
    }
    memcpy( dst, at, n );
    dst[n] = '\0';
}

/* An entry name: a longer name than this build keeps is cut and flagged, never refused. */
static void r_name( rbuf_t *r, char *dst, uint8_t *flags )
{
    uint8_t n = r_u8( r );
    const uint8_t *at = r_take( r, n );
    dst[0] = '\0';
    if ( at == NULL ) {
        return;
    }
    if ( memchr( at, 0, n ) != NULL ) {
        r->ok = false;
        return;
    }
    uint8_t keep = n;
    if ( keep > VFTP_MAX_FILE_NAME ) {
        keep = (uint8_t) VFTP_MAX_FILE_NAME;
        *flags = (uint8_t) ( *flags | VFTP_ENTRY_NAME_CUT );
    }
    memcpy( dst, at, keep );
    dst[keep] = '\0';
}

static void r_ranges( rbuf_t *r, vftp_ranges_t *out )
{
    out->entries = r_u16( r );
    if ( out->entries > VFTP_WIRE_MAX_RANGES ) {
        r->ok = false;
        out->entries = 0;
        return;
    }
    for ( uint16_t i = 0; i < out->entries; i++ ) {
        out->range[i].next = r_u32( r );
        out->range[i].count = r_u32( r );
    }
}

static void r_rest( rbuf_t *r, vftp_data_t *d )
{
    d->bytes = r->p + r->pos;
    d->len = (uint16_t) ( r->len - r->pos );
    r->pos = r->len;
}

bool vftp_path_set( char dst[VFTP_MAX_PATH], const char *src )
{
    size_t n = src ? vftp_str_len( src, VFTP_MAX_PATH ) : VFTP_MAX_PATH;
    if ( n >= VFTP_MAX_PATH ) {
        dst[0] = '\0';
        return false;
    }
    memset( dst, 0, VFTP_MAX_PATH );
    memcpy( dst, src, n );
    return true;
}

bool vftp_msg_has_session( uint8_t type )
{
    switch ( type ) {
        case VFTP_MSG_DOWNLOAD_REPLY:
        case VFTP_MSG_UPLOAD_REPLY:
        case VFTP_MSG_DATA:
        case VFTP_MSG_STATUS_REQUEST:
        case VFTP_MSG_STATUS_REPLY:
        case VFTP_MSG_CRC_REQUEST:
        case VFTP_MSG_CRC_REPLY:
        case VFTP_MSG_DONE:
        case VFTP_MSG_ABORT:
        case VFTP_MSG_WANT:
        case VFTP_MSG_GET_CHUNK:
        case VFTP_MSG_DATA_ACK:
            return true;
        default:
            return false;
    }
}

/* ---- decode -------------------------------------------------------------- */

vftp_ret_t vftp_decode( const uint8_t *buf, uint16_t len, vftp_msg_t *m )
{
    if ( buf == NULL || m == NULL || len < 1u ) {
        return VFTP_INVAL;
    }
    rbuf_t r = { buf, len, 1u, true };
    m->type = buf[0];
    m->session = 0u;
    if ( vftp_msg_has_session( m->type ) ) {
        m->session = r_u8( &r );
    }
    switch ( m->type ) {
        case VFTP_MSG_DOWNLOAD_REQUEST:
            m->u.down_req.chunk_size = r_u16( &r );
            r_path( &r, m->u.down_req.path );
            break;
        case VFTP_MSG_DOWNLOAD_REPLY:
            m->u.down_rep.ret = r_u8( &r );
            m->u.down_rep.size = r_u32( &r );
            m->u.down_rep.crc32 = r_u32( &r );
            m->u.down_rep.flags = r_u8( &r );
            break;
        case VFTP_MSG_UPLOAD_REQUEST:
            m->u.up_req.size = r_u32( &r );
            m->u.up_req.crc32 = r_u32( &r );
            m->u.up_req.chunk_size = r_u16( &r );
            r_path( &r, m->u.up_req.path );
            break;
        case VFTP_MSG_UPLOAD_REPLY:
            m->u.up_rep.ret = r_u8( &r );
            m->u.up_rep.resume_from = r_u32( &r );
            break;
        case VFTP_MSG_DATA:
            m->u.data.chunk = r_u32( &r );
            r_rest( &r, &m->u.data );
            break;
        case VFTP_MSG_STATUS_REQUEST:
        case VFTP_MSG_CRC_REQUEST:
        case VFTP_MSG_DONE:
        case VFTP_MSG_ABORT:
            break;
        case VFTP_MSG_STATUS_REPLY:
            m->u.status.ret = r_u8( &r );
            m->u.status.total = r_u32( &r );
            m->u.status.complete = r_u32( &r );
            r_ranges( &r, &m->u.status.missing );
            break;
        case VFTP_MSG_WANT:
            r_ranges( &r, &m->u.want );
            break;
        case VFTP_MSG_CRC_REPLY:
            m->u.crc_rep.ret = r_u8( &r );
            m->u.crc_rep.crc32 = r_u32( &r );
            m->u.crc_rep.done = r_u32( &r );
            break;
        case VFTP_MSG_LIST_REQUEST:
            m->u.list_req.offset = r_u32( &r );
            m->u.list_req.limit = r_u16( &r );
            r_path( &r, m->u.list_req.path );
            break;
        case VFTP_MSG_LIST_REPLY:
            m->u.list_rep.ret = r_u8( &r );
            m->u.list_rep.total_files = r_u32( &r );
            m->u.list_rep.first_file = r_u32( &r );
            m->u.list_rep.packet_files = r_u16( &r );
            if ( m->u.list_rep.packet_files > VFTP_WIRE_MAX_ENTRIES ) {
                return VFTP_INVAL;
            }
            for ( uint16_t i = 0; i < m->u.list_rep.packet_files && r.ok; i++ ) {
                vftp_list_entry_t *e = &m->u.list_rep.entry[i];
                e->flags = r_u8( &r );
                e->size = r_u32( &r );
                e->mtime = r_u32( &r );
                r_name( &r, e->name, &e->flags );
            }
            break;
        case VFTP_MSG_DELETE_REQUEST:
        case VFTP_MSG_LIST_REGEN_REQUEST:
            r_path( &r, m->u.path_req.path );
            break;
        case VFTP_MSG_DELETE_REPLY:
        case VFTP_MSG_LIST_REGEN_REPLY:
            m->u.ret_rep.ret = r_u8( &r );
            break;
        case VFTP_MSG_INDEX_REQUEST:
            m->u.idx_req.index = r_u32( &r );
            r_path( &r, m->u.idx_req.path );
            break;
        case VFTP_MSG_INDEX_REPLY:
            m->u.idx_rep.ret = r_u8( &r );
            m->u.idx_rep.total = r_u32( &r );
            m->u.idx_rep.index = r_u32( &r );
            m->u.idx_rep.flags = r_u8( &r );
            m->u.idx_rep.size = r_u32( &r );
            m->u.idx_rep.mtime = r_u32( &r );
            r_name( &r, m->u.idx_rep.name, &m->u.idx_rep.flags );
            break;
        case VFTP_MSG_GET_CHUNK:
            m->u.get_chunk.chunk = r_u32( &r );
            break;
        case VFTP_MSG_DATA_ACK:
            m->u.data_ack.chunk = r_u32( &r );
            m->u.data_ack.ret = r_u8( &r );
            break;
        default:
            return VFTP_INVAL;
    }
    return r.ok ? VFTP_OK : VFTP_INVAL;
}

/* ---- encode -------------------------------------------------------------- */

uint16_t vftp_encode( const vftp_msg_t *m, uint8_t *buf, uint16_t cap )
{
    if ( m == NULL || buf == NULL ) {
        return 0u;
    }
    wbuf_t w = { buf, cap, 0u, true };
    w_u8( &w, m->type );
    if ( vftp_msg_has_session( m->type ) ) {
        w_u8( &w, m->session );
    }
    switch ( m->type ) {
        case VFTP_MSG_DOWNLOAD_REQUEST:
            w_u16( &w, m->u.down_req.chunk_size );
            w_str( &w, m->u.down_req.path, VFTP_MAX_PATH - 1u );
            break;
        case VFTP_MSG_DOWNLOAD_REPLY:
            w_u8( &w, m->u.down_rep.ret );
            w_u32( &w, m->u.down_rep.size );
            w_u32( &w, m->u.down_rep.crc32 );
            w_u8( &w, m->u.down_rep.flags );
            break;
        case VFTP_MSG_UPLOAD_REQUEST:
            w_u32( &w, m->u.up_req.size );
            w_u32( &w, m->u.up_req.crc32 );
            w_u16( &w, m->u.up_req.chunk_size );
            w_str( &w, m->u.up_req.path, VFTP_MAX_PATH - 1u );
            break;
        case VFTP_MSG_UPLOAD_REPLY:
            w_u8( &w, m->u.up_rep.ret );
            w_u32( &w, m->u.up_rep.resume_from );
            break;
        case VFTP_MSG_DATA:
            w_u32( &w, m->u.data.chunk );
            w_bytes( &w, m->u.data.bytes, m->u.data.len );
            break;
        case VFTP_MSG_STATUS_REQUEST:
        case VFTP_MSG_CRC_REQUEST:
        case VFTP_MSG_DONE:
        case VFTP_MSG_ABORT:
            break;
        case VFTP_MSG_STATUS_REPLY:
            w_u8( &w, m->u.status.ret );
            w_u32( &w, m->u.status.total );
            w_u32( &w, m->u.status.complete );
            w_ranges( &w, &m->u.status.missing );
            break;
        case VFTP_MSG_WANT:
            w_ranges( &w, &m->u.want );
            break;
        case VFTP_MSG_CRC_REPLY:
            w_u8( &w, m->u.crc_rep.ret );
            w_u32( &w, m->u.crc_rep.crc32 );
            w_u32( &w, m->u.crc_rep.done );
            break;
        case VFTP_MSG_LIST_REQUEST:
            w_u32( &w, m->u.list_req.offset );
            w_u16( &w, m->u.list_req.limit );
            w_str( &w, m->u.list_req.path, VFTP_MAX_PATH - 1u );
            break;
        case VFTP_MSG_LIST_REPLY:
            if ( m->u.list_rep.packet_files > VFTP_WIRE_MAX_ENTRIES ) {
                return 0u;
            }
            w_u8( &w, m->u.list_rep.ret );
            w_u32( &w, m->u.list_rep.total_files );
            w_u32( &w, m->u.list_rep.first_file );
            w_u16( &w, m->u.list_rep.packet_files );
            for ( uint16_t i = 0; i < m->u.list_rep.packet_files; i++ ) {
                const vftp_list_entry_t *e = &m->u.list_rep.entry[i];
                w_u8( &w, e->flags );
                w_u32( &w, e->size );
                w_u32( &w, e->mtime );
                w_str( &w, e->name, VFTP_MAX_FILE_NAME );
            }
            break;
        case VFTP_MSG_DELETE_REQUEST:
        case VFTP_MSG_LIST_REGEN_REQUEST:
            w_str( &w, m->u.path_req.path, VFTP_MAX_PATH - 1u );
            break;
        case VFTP_MSG_DELETE_REPLY:
        case VFTP_MSG_LIST_REGEN_REPLY:
            w_u8( &w, m->u.ret_rep.ret );
            break;
        case VFTP_MSG_INDEX_REQUEST:
            w_u32( &w, m->u.idx_req.index );
            w_str( &w, m->u.idx_req.path, VFTP_MAX_PATH - 1u );
            break;
        case VFTP_MSG_INDEX_REPLY:
            w_u8( &w, m->u.idx_rep.ret );
            w_u32( &w, m->u.idx_rep.total );
            w_u32( &w, m->u.idx_rep.index );
            w_u8( &w, m->u.idx_rep.flags );
            w_u32( &w, m->u.idx_rep.size );
            w_u32( &w, m->u.idx_rep.mtime );
            w_str( &w, m->u.idx_rep.name, VFTP_MAX_FILE_NAME );
            break;
        case VFTP_MSG_GET_CHUNK:
            w_u32( &w, m->u.get_chunk.chunk );
            break;
        case VFTP_MSG_DATA_ACK:
            w_u32( &w, m->u.data_ack.chunk );
            w_u8( &w, m->u.data_ack.ret );
            break;
        default:
            return 0u;
    }
    if ( !w.ok ) {
        return 0u;
    }
    return w.len;
}
