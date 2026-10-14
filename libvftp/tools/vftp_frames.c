/* vftp_frames
 * The vftp bytes come from the library's own encoder, so they are exactly what goes on the wire.
 * The CSP/KISS wrapping is re-implemented here from the libcsp sources (deliberately independent
 * of libcsp) and was checked by feeding the frames to v-client's real CSP 1.x KISS decoder.
 * docs/protocol.md is generated from this output; run `make frames` after any wire change. */
#include <stdio.h>
#include <string.h>

#include "vftp/vftp.h"

static void hex( const char *label, const uint8_t *p, unsigned n )
{
    printf( "%-26s (%3u B):", label, n );
    for ( unsigned i = 0; i < n; i++ ) {
        printf( "%s%02X", ( i % 24 == 0 && i != 0 ) ? "\n                                  " : " ", p[i] );
    }
    printf( "\n" );
}

static uint16_t enc( vftp_msg_t *m, uint8_t *out )
{
    return vftp_encode( m, out, VFTP_CFG_PACKET_MAX );
}

/* A zeroed message of `type`, in session `sess` (ignored by types without a session byte). */
static vftp_msg_t *msg( vftp_msg_t *m, uint8_t type, uint8_t sess )
{
    memset( m, 0, sizeof( *m ) );
    m->type = type;
    m->session = sess;
    return m;
}

/* ---- CSP v1 / RDP / KISS, as libcsp builds them ---------------------------------------- */

/* CRC-32C (Castagnoli): reflected 0x82F63B78, init and final xor 0xFFFFFFFF -- libcsp's
 * csp_crc32_memory(). NOT the CRC-32/IEEE vftp uses for file content. */
static uint32_t crc32c( const uint8_t *p, unsigned n )
{
    uint32_t c = 0xFFFFFFFFu;
    for ( unsigned i = 0; i < n; i++ ) {
        c ^= p[i];
        for ( int k = 0; k < 8; k++ ) {
            c = ( c >> 1 ) ^ ( ( c & 1u ) ? 0x82F63B78u : 0u );
        }
    }
    return c ^ 0xFFFFFFFFu;
}

#define F_CRC32 0x01u
#define F_RDP 0x02u
#define RDP_ACK 0x04u

/* One KISS frame: C0 00 <escaped: CSP header(4, big-endian) | data | KISS CRC32-C(4)> C0.
 * data = vftp bytes [+ RDP trailer flags,seq,ack (5)] [+ CSP CRC32-C over data (4)]. */
static unsigned kiss_frame( uint8_t *out, unsigned prio, unsigned src, unsigned dst, unsigned dport, unsigned sport,
                            unsigned flags, const uint8_t *vftp, unsigned len, uint16_t seq, uint16_t ack, uint8_t *raw,
                            unsigned *raw_len )
{
    uint8_t d[300];
    unsigned n = 0;
    uint32_t id = ( (uint32_t) prio << 30 ) | ( (uint32_t) src << 25 ) | ( (uint32_t) dst << 20 ) |
                  ( (uint32_t) dport << 14 ) | ( (uint32_t) sport << 8 ) | flags;
    d[n++] = (uint8_t) ( id >> 24 );
    d[n++] = (uint8_t) ( id >> 16 );
    d[n++] = (uint8_t) ( id >> 8 );
    d[n++] = (uint8_t) id;
    memcpy( d + n, vftp, len );
    n += len;
    if ( flags & F_RDP ) {
        d[n++] = RDP_ACK;
        d[n++] = (uint8_t) ( seq >> 8 );
        d[n++] = (uint8_t) seq;
        d[n++] = (uint8_t) ( ack >> 8 );
        d[n++] = (uint8_t) ack;
    }
    if ( flags & F_CRC32 ) { /* over data only (the 4 header bytes excluded) */
        uint32_t c = crc32c( d + 4, n - 4 );
        d[n++] = (uint8_t) ( c >> 24 );
        d[n++] = (uint8_t) ( c >> 16 );
        d[n++] = (uint8_t) ( c >> 8 );
        d[n++] = (uint8_t) c;
    }
    uint32_t k = crc32c( d + 4, n - 4 ); /* KISS frame CRC: also data only, always present */
    d[n++] = (uint8_t) ( k >> 24 );
    d[n++] = (uint8_t) ( k >> 16 );
    d[n++] = (uint8_t) ( k >> 8 );
    d[n++] = (uint8_t) k;
    memcpy( raw, d, n );
    *raw_len = n;
    unsigned o = 0;
    out[o++] = 0xC0;
    out[o++] = 0x00;
    for ( unsigned i = 0; i < n; i++ ) {
        if ( d[i] == 0xC0 ) {
            out[o++] = 0xDB;
            out[o++] = 0xDC;
        } else if ( d[i] == 0xDB ) {
            out[o++] = 0xDB;
            out[o++] = 0xDD;
        } else {
            out[o++] = d[i];
        }
    }
    out[o++] = 0xC0;
    return o;
}

int main( void )
{
    uint8_t b[VFTP_CFG_PACKET_MAX];
    static const uint8_t payload[8] = { 0x48, 0x45, 0x4C, 0x4C, 0x4F, 0xC0, 0xDB, 0x21 }; /* "HELLO", C0, DB, '!' */
    const uint8_t S = 0x2A; /* the session the server assigned in its DOWNLOAD_REPLY / UPLOAD_REPLY */
    vftp_msg_t m;
    uint16_t n;

    printf( "== vftp_burst (CSP port 9) ==\n" );
    msg( &m, VFTP_MSG_DOWNLOAD_REQUEST, 0 );
    m.u.down_req.chunk_size = 205;
    vftp_path_set( m.u.down_req.path, "VT/B.BIN" );
    n = enc( &m, b );
    hex( "DOWNLOAD_REQUEST", b, n );
    uint8_t dl_req[VFTP_CFG_PACKET_MAX];
    uint16_t dl_req_n = n;
    memcpy( dl_req, b, n );

    msg( &m, VFTP_MSG_DOWNLOAD_REPLY, S );
    m.u.down_rep.ret = VFTP_OK;
    m.u.down_rep.size = 102400;
    m.u.down_rep.crc32 = 0xC4DF7001u;
    m.u.down_rep.flags = VFTP_DL_CRC_VALID;
    hex( "DOWNLOAD_REPLY", b, enc( &m, b ) );

    msg( &m, VFTP_MSG_WANT, S ); /* client -> server: "send me these ranges" */
    m.u.want.entries = 2;
    m.u.want.range[0].next = 0;
    m.u.want.range[0].count = 40;
    m.u.want.range[1].next = 100;
    m.u.want.range[1].count = 24;
    hex( "WANT", b, enc( &m, b ) );

    msg( &m, VFTP_MSG_DATA, S );
    m.u.data.chunk = 3;
    m.u.data.bytes = payload;
    m.u.data.len = sizeof( payload );
    n = enc( &m, b );
    hex( "DATA (8-byte chunk)", b, n );
    uint8_t data_pkt[VFTP_CFG_PACKET_MAX];
    uint16_t data_n = n;
    memcpy( data_pkt, b, n );

    msg( &m, VFTP_MSG_UPLOAD_REQUEST, 0 );
    m.u.up_req.size = 102400;
    m.u.up_req.crc32 = 0xC4DF7001u;
    m.u.up_req.chunk_size = 144;
    vftp_path_set( m.u.up_req.path, "VT/B.BIN" );
    hex( "UPLOAD_REQUEST", b, enc( &m, b ) );

    msg( &m, VFTP_MSG_UPLOAD_REPLY, S );
    m.u.up_rep.ret = VFTP_OK;
    hex( "UPLOAD_REPLY", b, enc( &m, b ) );
    msg( &m, VFTP_MSG_UPLOAD_REPLY, 0 ); /* a refusal opens no session */
    m.u.up_rep.ret = VFTP_BADNAME;
    hex( "UPLOAD_REPLY (BADNAME)", b, enc( &m, b ) );

    hex( "STATUS_REQUEST", b, enc( msg( &m, VFTP_MSG_STATUS_REQUEST, S ), b ) );

    msg( &m, VFTP_MSG_STATUS_REPLY, S ); /* server -> client during an upload */
    m.u.status.ret = VFTP_OK;
    m.u.status.total = 712;
    m.u.status.complete = 705;
    m.u.status.missing.entries = 1;
    m.u.status.missing.range[0].next = 698;
    m.u.status.missing.range[0].count = 7;
    hex( "STATUS_REPLY", b, enc( &m, b ) );

    hex( "CRC_REQUEST", b, enc( msg( &m, VFTP_MSG_CRC_REQUEST, S ), b ) );

    msg( &m, VFTP_MSG_CRC_REPLY, S );
    m.u.crc_rep.ret = VFTP_AGAIN;
    m.u.crc_rep.done = 524288;
    hex( "CRC_REPLY (AGAIN)", b, enc( &m, b ) );
    m.u.crc_rep.ret = VFTP_OK;
    m.u.crc_rep.crc32 = 0xC4DF7001u;
    m.u.crc_rep.done = 102400;
    hex( "CRC_REPLY (OK)", b, enc( &m, b ) );

    hex( "DONE", b, enc( msg( &m, VFTP_MSG_DONE, S ), b ) );
    hex( "ABORT", b, enc( msg( &m, VFTP_MSG_ABORT, S ), b ) );

    msg( &m, VFTP_MSG_LIST_REQUEST, 0 );
    vftp_path_set( m.u.list_req.path, "VOBC" );
    m.u.list_req.offset = 0;
    m.u.list_req.limit = 0; /* as many as fit */
    hex( "LIST_REQUEST", b, enc( &m, b ) );

    msg( &m, VFTP_MSG_LIST_REPLY, 0 );
    m.u.list_rep.ret = VFTP_OK;
    m.u.list_rep.total_files = 11;
    m.u.list_rep.first_file = 0;
    m.u.list_rep.packet_files = 2;
    m.u.list_rep.entry[0].flags = VFTP_ENTRY_DIR;
    m.u.list_rep.entry[0].mtime = vftp_unix_time( 2026, 10, 6, 14, 47, 0 );
    memcpy( m.u.list_rep.entry[0].name, "IMG", 4 );
    m.u.list_rep.entry[1].size = 3000;
    m.u.list_rep.entry[1].mtime = vftp_unix_time( 2026, 10, 6, 14, 47, 0 );
    memcpy( m.u.list_rep.entry[1].name, "LOG1.TXT", 9 );
    hex( "LIST_REPLY (2 entries)", b, enc( &m, b ) );

    msg( &m, VFTP_MSG_DELETE_REQUEST, 0 );
    vftp_path_set( m.u.path_req.path, "VT/B.BIN" );
    hex( "DELETE_REQUEST", b, enc( &m, b ) );
    msg( &m, VFTP_MSG_DELETE_REPLY, 0 );
    m.u.ret_rep.ret = VFTP_NOENT;
    hex( "DELETE_REPLY (NOENT)", b, enc( &m, b ) );
    msg( &m, VFTP_MSG_LIST_REGEN_REQUEST, 0 );
    vftp_path_set( m.u.path_req.path, "VOBC" );
    hex( "LIST_REGEN_REQUEST", b, enc( &m, b ) );
    msg( &m, VFTP_MSG_LIST_REGEN_REPLY, 0 );
    m.u.ret_rep.ret = VFTP_OK;
    hex( "LIST_REGEN_REPLY", b, enc( &m, b ) );

    msg( &m, VFTP_MSG_INDEX_REQUEST, 0 );
    vftp_path_set( m.u.idx_req.path, "VOBC" );
    m.u.idx_req.index = VFTP_INDEX_COUNT_ONLY;
    hex( "INDEX_REQUEST (count)", b, enc( &m, b ) );
    m.u.idx_req.index = 2;
    hex( "INDEX_REQUEST (n=2)", b, enc( &m, b ) );
    msg( &m, VFTP_MSG_INDEX_REPLY, 0 );
    m.u.idx_rep.ret = VFTP_OK;
    m.u.idx_rep.total = 11;
    m.u.idx_rep.index = 2;
    m.u.idx_rep.flags = 0;
    m.u.idx_rep.size = 3000;
    m.u.idx_rep.mtime = vftp_unix_time( 2026, 10, 6, 14, 47, 0 );
    memcpy( m.u.idx_rep.name, "LOG1.TXT", 9 );
    hex( "INDEX_REPLY", b, enc( &m, b ) );

    printf( "\n== vftp_poll (CSP port 8) ==\n" );
    const uint8_t P = 0x07;
    msg( &m, VFTP_MSG_DOWNLOAD_REQUEST, 0 );
    m.u.down_req.chunk_size = 205;
    vftp_path_set( m.u.down_req.path, "VT/P.BIN" );
    hex( "DOWNLOAD_REQUEST", b, enc( &m, b ) );
    msg( &m, VFTP_MSG_DOWNLOAD_REPLY, P );
    m.u.down_rep.ret = VFTP_OK;
    m.u.down_rep.size = 3000000;
    m.u.down_rep.flags = 0; /* large file: CRC not known yet */
    hex( "DOWNLOAD_REPLY (no CRC yet)", b, enc( &m, b ) );
    msg( &m, VFTP_MSG_GET_CHUNK, P );
    m.u.get_chunk.chunk = 5;
    hex( "GET_CHUNK", b, enc( &m, b ) );
    msg( &m, VFTP_MSG_DATA, P );
    m.u.data.chunk = 5;
    m.u.data.bytes = payload;
    m.u.data.len = sizeof( payload );
    hex( "DATA", b, enc( &m, b ) );
    msg( &m, VFTP_MSG_UPLOAD_REQUEST, 0 );
    m.u.up_req.size = 32768;
    m.u.up_req.crc32 = 0x5F2D3679u;
    m.u.up_req.chunk_size = 144;
    vftp_path_set( m.u.up_req.path, "VT/P.BIN" );
    hex( "UPLOAD_REQUEST", b, enc( &m, b ) );
    msg( &m, VFTP_MSG_UPLOAD_REPLY, P );
    m.u.up_rep.ret = VFTP_OK;
    m.u.up_rep.resume_from = 98;
    hex( "UPLOAD_REPLY (resume 98)", b, enc( &m, b ) );
    msg( &m, VFTP_MSG_DATA_ACK, P );
    m.u.data_ack.chunk = 98;
    m.u.data_ack.ret = VFTP_OK;
    hex( "DATA_ACK", b, enc( &m, b ) );
    hex( "CRC_REQUEST", b, enc( msg( &m, VFTP_MSG_CRC_REQUEST, P ), b ) );
    msg( &m, VFTP_MSG_CRC_REPLY, P );
    m.u.crc_rep.ret = VFTP_OK;
    m.u.crc_rep.crc32 = 0x5F2D3679u;
    m.u.crc_rep.done = 32768;
    hex( "CRC_REPLY", b, enc( &m, b ) );
    hex( "DONE", b, enc( msg( &m, VFTP_MSG_DONE, P ), b ) );

    printf( "\n== on the link: CSP v1 + KISS (ground node 1 sport 40 <-> OBC node 2 port 9) ==\n" );
    uint8_t f[600];
    uint8_t raw[300];
    unsigned rn = 0;
    unsigned fn = kiss_frame( f, 2, 1, 2, 9, 40, 0, dl_req, dl_req_n, 0, 0, raw, &rn );
    hex( "csp (plain) DOWNLOAD_REQUEST", raw, rn );
    hex( "kiss frame", f, fn );
    fn = kiss_frame( f, 2, 2, 1, 40, 9, F_CRC32, data_pkt, data_n, 0, 0, raw, &rn );
    hex( "csp (CRC32) DATA", raw, rn );
    hex( "kiss frame", f, fn );
    fn = kiss_frame( f, 2, 2, 1, 40, 9, F_RDP | F_CRC32, data_pkt, data_n, 0x1234, 0x5678, raw, &rn );
    hex( "csp (RDP+CRC32) DATA", raw, rn );
    hex( "kiss frame", f, fn );
    return 0;
}
