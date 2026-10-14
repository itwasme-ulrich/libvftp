/* libvftp test suite (Unity).
 *
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "vftp/vftp.h"
#include "vftp_link_loopback.h"
#include "vftp_spp.h"
#include "vftp_storage_posix.h"
#include "vftp_storage_ram.h"
#include "vftp_internal.h" /* vftp_rechunk, checked against a brute-force reference */

#define POOL ( 4u << 20 )
#define MTU_UP 150u   /* ground -> sat, the real CSP uplink budget */
#define MTU_DOWN 222u /* sat -> ground */
#define TEST_CRC_STEP 20000u

static uint8_t s_srv_pool[POOL];
static uint8_t s_cli_pool[POOL];
static vftp_ramfs_t s_srv_ram;
static vftp_ramfs_t s_cli_ram;
static vftp_storage_t s_srv_fs;
static vftp_storage_t s_cli_fs;
static vftp_burst_server_t s_burst;
static vftp_poll_server_t s_poll;
static vftp_loopback_t s_lb;
static vftp_link_t s_link;
static vftp_client_t s_cli;
static uint8_t s_buf[300000];
static uint8_t s_buf2[300000];

typedef struct {
    int starts;
    int resumes;
    uint32_t resume_done;
    int crcs;
    int verifies; /* CRC_REPLY AGAIN: the server still checksumming a large file */
    uint32_t last_done;
} events_t;
static events_t s_ev;

static void on_event( void *ctx, const vftp_event_t *ev )
{
    events_t *e = (events_t *) ctx;
    switch ( ev->type ) {
        case VFTP_EV_START:
            e->starts++;
            break;
        case VFTP_EV_RESUME:
            e->resumes++;
            e->resume_done = ev->done_chunks;
            break;
        case VFTP_EV_CRC:
            e->crcs++;
            break;
        case VFTP_EV_VERIFY:
            e->verifies++;
            break;
        case VFTP_EV_PROGRESS:
        case VFTP_EV_ROUND:
        default:
            e->last_done = ev->done_chunks;
            break;
    }
}

static void servers_init( uint16_t mtu_tx, uint16_t mtu_rx )
{
    vftp_server_cfg_t cfg = { &s_srv_fs, mtu_tx, mtu_rx };
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_server_init( &s_burst, &cfg ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_poll_server_init( &s_poll, &cfg ) );
    /* Test files are at most a few hundred kB: a small CRC step makes most of them "large" (CRC
     * fed while serving, finished over several CRC_REQUEST/AGAIN), so every test runs that path. */
    s_burst.crc_step = TEST_CRC_STEP;
    s_poll.crc_step = TEST_CRC_STEP;
}

static void link_init( bool burst, uint16_t up, uint16_t down )
{
    memset( &s_lb, 0, sizeof( s_lb ) );
    s_lb.burst = burst ? &s_burst : NULL;
    s_lb.poll = burst ? NULL : &s_poll;
    s_lb.seed = 0xC0FFEEu;
    vftp_loopback_init( &s_link, &s_lb, up, down );
}

void setUp( void )
{
    vftp_storage_ram_init( &s_srv_fs, &s_srv_ram, s_srv_pool, POOL );
    vftp_storage_ram_init( &s_cli_fs, &s_cli_ram, s_cli_pool, POOL );
    servers_init( MTU_DOWN, MTU_UP );
    link_init( true, MTU_UP, MTU_DOWN );
    vftp_client_cfg_t cfg;
    vftp_client_default_cfg( &cfg );
    cfg.link = &s_link;
    cfg.fs = &s_cli_fs;
    cfg.on_event = on_event;
    cfg.event_ctx = &s_ev;
    cfg.retries = 10;
    cfg.max_stall = 12;
    vftp_client_init( &s_cli, &cfg );
    memset( &s_ev, 0, sizeof( s_ev ) );
}

void tearDown( void )
{
}

static void fill( uint8_t *p, uint32_t n, uint32_t seed )
{
    uint32_t x = seed * 2654435761u + 1u;
    for ( uint32_t i = 0; i < n; i++ ) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        p[i] = (uint8_t) x;
    }
}

static void local_file( const char *path, uint32_t n, uint32_t seed )
{
    fill( s_buf, n, seed );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_ramfs_put( &s_cli_ram, path, s_buf, n ) );
}

static void remote_file( const char *path, uint32_t n, uint32_t seed )
{
    fill( s_buf, n, seed );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_ramfs_put( &s_srv_ram, path, s_buf, n ) );
}

static void assert_same( vftp_ramfs_t *a, const char *pa, vftp_ramfs_t *b, const char *pb )
{
    uint32_t la = 0, lb = 0;
    const uint8_t *da = vftp_ramfs_get( a, pa, &la );
    const uint8_t *db = vftp_ramfs_get( b, pb, &lb );
    TEST_ASSERT_NOT_NULL_MESSAGE( da, pa );
    TEST_ASSERT_NOT_NULL_MESSAGE( db, pb );
    TEST_ASSERT_EQUAL_UINT32( la, lb );
    if ( la != 0u ) {
        TEST_ASSERT_EQUAL_MEMORY( da, db, la );
    }
}

static void use_poll( void )
{
    s_lb.burst = NULL;
    s_lb.poll = &s_poll;
}

/* ---- primitives ---------------------------------------------------------- */

static void test_crc32_reference_vector( void )
{
    TEST_ASSERT_EQUAL_HEX32( 0xCBF43926u, vftp_crc32( "123456789", 9 ) );
    uint32_t c = VFTP_CRC32_INIT;
    c = vftp_crc32_update( c, "1234", 4 );
    c = vftp_crc32_update( c, "56789", 5 );
    TEST_ASSERT_EQUAL_HEX32( 0xCBF43926u, vftp_crc32_final( c ) );
    TEST_ASSERT_EQUAL_HEX32( 0x00000000u, vftp_crc32( "", 0 ) );
}

/* The wire is little-endian byte for byte, whatever the host is. */
static void test_wire_golden_bytes( void )
{
    vftp_msg_t m;
    uint8_t b[128];
    memset( &m, 0, sizeof( m ) );
    m.type = VFTP_MSG_UPLOAD_REQUEST;
    m.u.up_req.size = 0x01020304u;
    m.u.up_req.crc32 = 0xA1B2C3D4u;
    m.u.up_req.chunk_size = 144u;
    TEST_ASSERT_TRUE( vftp_path_set( m.u.up_req.path, "/IOBC/X.BIN" ) );
    static const uint8_t up[] = { 0x02, 0x04, 0x03, 0x02, 0x01, 0xD4, 0xC3, 0xB2, 0xA1, 144, 0, 11,
                                  '/',  'I',  'O',  'B',  'C',  '/',  'X',  '.',  'B',  'I', 'N' };
    TEST_ASSERT_EQUAL_UINT32( sizeof( up ), vftp_encode( &m, b, sizeof( b ) ) );
    TEST_ASSERT_EQUAL_HEX8_ARRAY( up, b, sizeof( up ) );

    memset( &m, 0, sizeof( m ) );
    m.type = VFTP_MSG_STATUS_REPLY;
    m.session = 0x5A;
    m.u.status.ret = 0;
    m.u.status.total = 10;
    m.u.status.complete = 7;
    m.u.status.missing.entries = 1;
    m.u.status.missing.range[0].next = 3;
    m.u.status.missing.range[0].count = 2;
    static const uint8_t st[] = { 0x06, 0x5A, 0, 10, 0, 0, 0, 7, 0, 0, 0, 1, 0, 3, 0, 0, 0, 2, 0, 0, 0 };
    TEST_ASSERT_EQUAL_UINT32( sizeof( st ), vftp_encode( &m, b, sizeof( b ) ) );
    TEST_ASSERT_EQUAL_HEX8_ARRAY( st, b, sizeof( st ) );
    vftp_msg_t d;
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_decode( b, sizeof( st ), &d ) );
    TEST_ASSERT_EQUAL_UINT8( 0x5Au, d.session );
    TEST_ASSERT_EQUAL_UINT32( 7u, d.u.status.complete );
    TEST_ASSERT_EQUAL_UINT32( 2u, d.u.status.missing.range[0].count );

    memset( &m, 0, sizeof( m ) );
    m.type = VFTP_MSG_WANT;
    m.session = 0x07;
    m.u.want.entries = 1;
    m.u.want.range[0].next = 0x100;
    m.u.want.range[0].count = 5;
    static const uint8_t want[] = { 0x13, 0x07, 1, 0, 0x00, 0x01, 0, 0, 5, 0, 0, 0 };
    TEST_ASSERT_EQUAL_UINT32( sizeof( want ), vftp_encode( &m, b, sizeof( b ) ) );
    TEST_ASSERT_EQUAL_HEX8_ARRAY( want, b, sizeof( want ) );

    memset( &m, 0, sizeof( m ) );
    m.type = VFTP_MSG_DATA;
    m.session = 0x07;
    m.u.data.chunk = 0x0102;
    m.u.data.bytes = (const uint8_t *) "hi";
    m.u.data.len = 2;
    static const uint8_t data[] = { 0x04, 0x07, 0x02, 0x01, 0, 0, 'h', 'i' };
    TEST_ASSERT_EQUAL_UINT32( VFTP_DATA_HEADER + 2u, vftp_encode( &m, b, sizeof( b ) ) );
    TEST_ASSERT_EQUAL_HEX8_ARRAY( data, b, sizeof( data ) );

    /* One directory "VT", modified 2026-10-06 14:47:00 UTC. */
    memset( &m, 0, sizeof( m ) );
    m.type = VFTP_MSG_LIST_REPLY;
    m.u.list_rep.total_files = 3;
    m.u.list_rep.packet_files = 1;
    m.u.list_rep.entry[0].flags = VFTP_ENTRY_DIR;
    m.u.list_rep.entry[0].mtime = vftp_unix_time( 2026, 10, 6, 14, 47, 0 );
    memcpy( m.u.list_rep.entry[0].name, "VT", 3 );
    TEST_ASSERT_EQUAL_UINT32( 1791298020u, m.u.list_rep.entry[0].mtime );
    static const uint8_t ls[] = { 0x0C, 0, 3, 0, 0, 0, 0, 0, 0, 0, 1, 0,
                                  0x01, 0, 0, 0, 0, 0xE4, 0x09, 0xC5, 0x6A, 2, 'V', 'T' };
    TEST_ASSERT_EQUAL_UINT32( sizeof( ls ), vftp_encode( &m, b, sizeof( b ) ) );
    TEST_ASSERT_EQUAL_HEX8_ARRAY( ls, b, sizeof( ls ) );
}

/* Smallest packet of every type (all numbers 0, empty strings): pins each layout's length, and
 * every shorter cut of it must be refused. */
static void test_wire_min_length_of_every_type( void )
{
    static const uint8_t min_len[] = {
        4,  /* 00 DOWNLOAD_REQUEST: type chunk_size:2 len:1 */
        12, /* 01 DOWNLOAD_REPLY */
        12, /* 02 UPLOAD_REQUEST */
        7,  /* 03 UPLOAD_REPLY */
        6,  /* 04 DATA (no payload) */
        2,  /* 05 STATUS_REQUEST */
        13, /* 06 STATUS_REPLY */
        2,  /* 07 CRC_REQUEST */
        11, /* 08 CRC_REPLY */
        2,  /* 09 DONE */
        2,  /* 0A ABORT */
        8,  /* 0B LIST_REQUEST */
        12, /* 0C LIST_REPLY */
        2,  /* 0D DELETE_REQUEST */
        2,  /* 0E DELETE_REPLY */
        2,  /* 0F LIST_REGEN_REQUEST */
        2,  /* 10 LIST_REGEN_REPLY */
        6,  /* 11 INDEX_REQUEST */
        20, /* 12 INDEX_REPLY */
        4,  /* 13 WANT */
        6,  /* 14 GET_CHUNK */
        7,  /* 15 DATA_ACK */
    };
    TEST_ASSERT_EQUAL_UINT32( VFTP_MSG_DATA_ACK + 1u, sizeof( min_len ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_STATUS_HEADER, min_len[VFTP_MSG_STATUS_REPLY] );
    TEST_ASSERT_EQUAL_UINT8( VFTP_WANT_HEADER, min_len[VFTP_MSG_WANT] );
    TEST_ASSERT_EQUAL_UINT8( VFTP_LIST_HEADER, min_len[VFTP_MSG_LIST_REPLY] );
    TEST_ASSERT_EQUAL_UINT8( VFTP_DATA_HEADER, min_len[VFTP_MSG_DATA] );
    uint8_t b[VFTP_CFG_PACKET_MAX];
    for ( uint8_t t = 0; t <= VFTP_MSG_DATA_ACK; t++ ) {
        vftp_msg_t m, d;
        memset( &m, 0, sizeof( m ) );
        m.type = t;
        uint16_t n = vftp_encode( &m, b, sizeof( b ) );
        TEST_ASSERT_EQUAL_UINT16_MESSAGE( min_len[t], n, "min length" );
        TEST_ASSERT_EQUAL_UINT16( 0u, vftp_encode( &m, b, (uint16_t) ( n - 1u ) ) ); /* no room: refused */
        TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_decode( b, n, &d ) );
        TEST_ASSERT_EQUAL_UINT8( t, d.type );
        for ( uint16_t cut = 1; cut < n; cut++ ) {
            if ( t == VFTP_MSG_DATA && cut >= VFTP_DATA_HEADER ) {
                continue;
            }
            TEST_ASSERT_EQUAL_INT_MESSAGE( VFTP_INVAL, vftp_decode( b, cut, &d ), "truncation accepted" );
        }
    }
    vftp_msg_t m;
    memset( &m, 0, sizeof( m ) );
    m.type = VFTP_MSG_DATA_ACK + 1u;
    TEST_ASSERT_EQUAL_UINT16( 0u, vftp_encode( &m, b, sizeof( b ) ) );
}

static void test_wire_roundtrip_every_type( void )
{
    uint8_t b[VFTP_CFG_PACKET_MAX];
    for ( uint8_t t = 0; t <= VFTP_MSG_DATA_ACK; t++ ) {
        vftp_msg_t m, d;
        memset( &m, 0, sizeof( m ) );
        m.type = t;
        m.session = vftp_msg_has_session( t ) ? 0x42u : 0u;
        switch ( t ) {
            case VFTP_MSG_DOWNLOAD_REQUEST:
                vftp_path_set( m.u.down_req.path, "dir/file.bin" );
                m.u.down_req.chunk_size = 205;
                break;
            case VFTP_MSG_UPLOAD_REQUEST:
                vftp_path_set( m.u.up_req.path, "dir/file.bin" );
                break;
            case VFTP_MSG_LIST_REQUEST:
                vftp_path_set( m.u.list_req.path, "dir/file.bin" );
                break;
            case VFTP_MSG_DELETE_REQUEST:
            case VFTP_MSG_LIST_REGEN_REQUEST:
                vftp_path_set( m.u.path_req.path, "dir/file.bin" );
                break;
            case VFTP_MSG_INDEX_REQUEST:
                vftp_path_set( m.u.idx_req.path, "dir" );
                m.u.idx_req.index = 7;
                break;
            case VFTP_MSG_INDEX_REPLY:
                m.u.idx_rep.total = 9;
                m.u.idx_rep.flags = VFTP_ENTRY_DIR;
                m.u.idx_rep.mtime = 1791298020u;
                memcpy( m.u.idx_rep.name, "SUBDIR", 7 );
                break;
            case VFTP_MSG_DATA:
                m.u.data.chunk = 77;
                m.u.data.bytes = (const uint8_t *) "abc";
                m.u.data.len = 3;
                break;
            case VFTP_MSG_LIST_REPLY:
                m.u.list_rep.packet_files = 2;
                m.u.list_rep.total_files = 9;
                memcpy( m.u.list_rep.entry[0].name, "A", 2 );
                m.u.list_rep.entry[0].flags = VFTP_ENTRY_DIR;
                memcpy( m.u.list_rep.entry[1].name, "ABCDEFGH.XYZ", 13 );
                m.u.list_rep.entry[1].size = 1234;
                m.u.list_rep.entry[1].mtime = 99;
                break;
            case VFTP_MSG_WANT:
                m.u.want.entries = 2;
                m.u.want.range[1].next = 40;
                m.u.want.range[1].count = 3;
                break;
            case VFTP_MSG_DATA_ACK:
                m.u.data_ack.chunk = 5;
                m.u.data_ack.ret = VFTP_RANGE;
                break;
            default:
                break;
        }
        uint16_t n = vftp_encode( &m, b, sizeof( b ) );
        TEST_ASSERT_TRUE_MESSAGE( n != 0u, "encode" );
        TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_decode( b, n, &d ) );
        TEST_ASSERT_EQUAL_UINT8( t, d.type );
        TEST_ASSERT_EQUAL_UINT8( m.session, d.session );
        if ( t == VFTP_MSG_DOWNLOAD_REQUEST ) {
            TEST_ASSERT_EQUAL_UINT16( 205u, d.u.down_req.chunk_size );
            TEST_ASSERT_EQUAL_STRING( "dir/file.bin", d.u.down_req.path );
            TEST_ASSERT_EQUAL_UINT32( 16u, n );
        }
        if ( t == VFTP_MSG_DATA ) {
            TEST_ASSERT_EQUAL_UINT32( 77u, d.u.data.chunk );
            TEST_ASSERT_EQUAL_UINT16( 3u, d.u.data.len );
            TEST_ASSERT_EQUAL_MEMORY( "abc", d.u.data.bytes, 3 );
        }
        if ( t == VFTP_MSG_LIST_REPLY ) {
            TEST_ASSERT_EQUAL_STRING( "A", d.u.list_rep.entry[0].name );
            TEST_ASSERT_EQUAL_UINT8( VFTP_ENTRY_DIR, d.u.list_rep.entry[0].flags );
            TEST_ASSERT_EQUAL_STRING( "ABCDEFGH.XYZ", d.u.list_rep.entry[1].name );
            TEST_ASSERT_EQUAL_UINT8( 0u, d.u.list_rep.entry[1].flags );
            TEST_ASSERT_EQUAL_UINT32( 1234u, d.u.list_rep.entry[1].size );
            TEST_ASSERT_EQUAL_UINT32( 99u, d.u.list_rep.entry[1].mtime );
            TEST_ASSERT_EQUAL_UINT32( VFTP_LIST_HEADER + 2u * VFTP_LIST_ENTRY_FIXED + 1u + 12u, n );
        }
        if ( t == VFTP_MSG_DELETE_REQUEST ) {
            TEST_ASSERT_EQUAL_STRING( "dir/file.bin", d.u.path_req.path );
        }
        if ( t == VFTP_MSG_INDEX_REQUEST ) {
            TEST_ASSERT_EQUAL_UINT32( 7u, d.u.idx_req.index );
            TEST_ASSERT_EQUAL_STRING( "dir", d.u.idx_req.path );
            TEST_ASSERT_EQUAL_UINT32( 9u, n );
        }
        if ( t == VFTP_MSG_INDEX_REPLY ) {
            TEST_ASSERT_EQUAL_STRING( "SUBDIR", d.u.idx_rep.name );
            TEST_ASSERT_EQUAL_UINT8( VFTP_ENTRY_DIR, d.u.idx_rep.flags );
            TEST_ASSERT_EQUAL_UINT32( 1791298020u, d.u.idx_rep.mtime );
            TEST_ASSERT_EQUAL_UINT32( 26u, n );
        }
        if ( t == VFTP_MSG_WANT ) {
            TEST_ASSERT_EQUAL_UINT16( 2u, d.u.want.entries );
            TEST_ASSERT_EQUAL_UINT32( 40u, d.u.want.range[1].next );
            TEST_ASSERT_EQUAL_UINT32( 3u, d.u.want.range[1].count );
        }
        if ( t == VFTP_MSG_DATA_ACK ) {
            TEST_ASSERT_EQUAL_UINT32( 5u, d.u.data_ack.chunk );
            TEST_ASSERT_EQUAL_UINT8( VFTP_RANGE, d.u.data_ack.ret );
        }
    }
}

static void test_wire_rejects_malformed( void )
{
    vftp_msg_t m, d;
    uint8_t b[VFTP_CFG_PACKET_MAX];

    /* Paths: at most 55 characters, no NUL, inside the packet. */
    b[0] = VFTP_MSG_DELETE_REQUEST;
    b[1] = 55;
    memset( b + 2, 'p', 56 );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_decode( b, 2 + 55, &d ) );
    TEST_ASSERT_EQUAL_UINT32( 55u, strlen( d.u.path_req.path ) );
    b[1] = 56;
    TEST_ASSERT_EQUAL_INT( VFTP_INVAL, vftp_decode( b, 2 + 56, &d ) );
    b[1] = 55;
    TEST_ASSERT_EQUAL_INT( VFTP_INVAL, vftp_decode( b, 2 + 54, &d ) ); /* length past the end */
    uint8_t nul[] = { VFTP_MSG_DELETE_REQUEST, 3, 'a', 0, 'b' };
    TEST_ASSERT_EQUAL_INT( VFTP_INVAL, vftp_decode( nul, sizeof( nul ), &d ) );
    memset( &m, 0, sizeof( m ) );
    m.type = VFTP_MSG_DELETE_REQUEST;
    memset( m.u.path_req.path, 'p', VFTP_MAX_PATH ); /* no terminator in the buffer */
    TEST_ASSERT_EQUAL_UINT16( 0u, vftp_encode( &m, b, sizeof( b ) ) );

    /* Item counts must fit both the packet and the host array. */
    uint8_t st[] = { VFTP_MSG_STATUS_REPLY, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 200, 0 }; /* 200 ranges, none present */
    TEST_ASSERT_EQUAL_INT( VFTP_INVAL, vftp_decode( st, sizeof( st ), &d ) );
    uint8_t want[] = { VFTP_MSG_WANT, 1, 2, 0, 1, 0, 0, 0, 1, 0, 0, 0 }; /* claims 2 ranges, has 1 */
    TEST_ASSERT_EQUAL_INT( VFTP_INVAL, vftp_decode( want, sizeof( want ), &d ) );
    want[2] = 1;
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_decode( want, sizeof( want ), &d ) );
    uint8_t lr[] = { VFTP_MSG_LIST_REPLY, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0 }; /* claims 1 entry, has 0 */
    TEST_ASSERT_EQUAL_INT( VFTP_INVAL, vftp_decode( lr, sizeof( lr ), &d ) );
    uint8_t lr_cut[] = { VFTP_MSG_LIST_REPLY, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0,
                         0, 1, 0, 0, 0, 0, 0, 0, 0, 5, 'A', 'B' }; /* name says 5, has 2 */
    TEST_ASSERT_EQUAL_INT( VFTP_INVAL, vftp_decode( lr_cut, sizeof( lr_cut ), &d ) );
    uint8_t lr_nul[] = { VFTP_MSG_LIST_REPLY, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0,
                         0, 1, 0, 0, 0, 0, 0, 0, 0, 2, 'A', 0 };
    TEST_ASSERT_EQUAL_INT( VFTP_INVAL, vftp_decode( lr_nul, sizeof( lr_nul ), &d ) );

    /* A name longer than this build keeps (a peer with longer names) is cut and flagged. */
    uint8_t lr_long[] = { VFTP_MSG_LIST_REPLY, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0,
                          VFTP_ENTRY_DIR, 0, 0, 0, 0, 0, 0, 0, 0, 20,
                          'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j',
                          'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't' };
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_decode( lr_long, sizeof( lr_long ), &d ) );
    TEST_ASSERT_EQUAL_STRING( "abcdefghijklm", d.u.list_rep.entry[0].name );
    TEST_ASSERT_EQUAL_UINT8( VFTP_ENTRY_DIR | VFTP_ENTRY_NAME_CUT, d.u.list_rep.entry[0].flags );

    /* Unknown types; trailing bytes are ignored (room for later fields). */
    uint8_t unknown[] = { 200, 1, 2, 3 };
    TEST_ASSERT_EQUAL_INT( VFTP_INVAL, vftp_decode( unknown, sizeof( unknown ), &d ) );
    unknown[0] = VFTP_MSG_DATA_ACK + 1u;
    TEST_ASSERT_EQUAL_INT( VFTP_INVAL, vftp_decode( unknown, sizeof( unknown ), &d ) );
    uint8_t done[] = { VFTP_MSG_DONE, 9, 0xEE, 0xEE };
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_decode( done, sizeof( done ), &d ) );
    TEST_ASSERT_EQUAL_UINT8( 9u, d.session );
    char p56[VFTP_MAX_PATH + 1];
    memset( p56, 'p', VFTP_MAX_PATH );
    p56[VFTP_MAX_PATH] = '\0';
    char field[VFTP_MAX_PATH];
    TEST_ASSERT_FALSE( vftp_path_set( field, p56 ) );     /* 56 chars: no room for the NUL */
    TEST_ASSERT_TRUE( vftp_path_set( field, p56 + 1 ) ); /* 55 chars: fits */
    memset( &m, 0, sizeof( m ) );
    m.type = VFTP_MSG_DATA;
    m.u.data.bytes = b;
    m.u.data.len = 300;
    TEST_ASSERT_EQUAL_UINT16( 0u, vftp_encode( &m, b, 100 ) ); /* does not fit cap: refused, not truncated */
}

static int discard( void *ctx, const uint8_t *pkt, uint16_t len )
{
    (void) ctx;
    (void) pkt;
    (void) len;
    return 0;
}

/* 40000 random packets into both servers: no crash (ASan/UBSan watch), and they still work after. */
static void test_fuzz_servers_survive_garbage( void )
{
    uint8_t p[300];
    uint32_t x = 1;
    remote_file( "f.bin", 5000, 1 );
    for ( int i = 0; i < 40000; i++ ) {
        x = x * 1103515245u + 12345u;
        uint16_t len = (uint16_t) ( ( x >> 8 ) % sizeof( p ) );
        for ( uint16_t k = 0; k < len; k++ ) {
            x = x * 1103515245u + 12345u;
            p[k] = (uint8_t) ( x >> 16 );
        }
        if ( len > 0u ) {
            p[0] = (uint8_t) ( p[0] % 20u ); /* mostly valid types, so handlers get exercised */
        }
        if ( len > 2u && ( x & 1u ) ) {
            memcpy( p + 1, "\x10\x00f.bin", 7 < len - 1 ? 7 : (size_t) ( len - 1 ) );
        }
        (void) vftp_burst_server_handle( &s_burst, p, len, discard, NULL );
        (void) vftp_poll_server_handle( &s_poll, p, len, discard, NULL );
        if ( ( i % 1000 ) == 0 ) {
            vftp_burst_server_abort( &s_burst );
            vftp_poll_server_abort( &s_poll );
        }
    }
    vftp_burst_server_abort( &s_burst );
    vftp_poll_server_abort( &s_poll );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_download( &s_cli, "f.bin", "f.back" ) );
    assert_same( &s_srv_ram, "f.bin", &s_cli_ram, "f.back" );
}

/* ---- burst transfers ------------------------------------------------------ */

static const uint32_t s_sizes[] = { 0u, 1u, 143u, 144u, 145u, 1443u, 32768u, 32769u, 200000u };

static void burst_roundtrip( uint32_t size, uint16_t loss, bool reorder, uint16_t dup )
{
    char msg[96];
    snprintf( msg, sizeof( msg ), "burst size %lu loss %u", (unsigned long) size, (unsigned int) loss );
    setUp();
    s_lb.loss_up = loss;
    s_lb.loss_down = loss;
    s_lb.reorder = reorder;
    s_lb.dup_down = dup;
    local_file( "src.bin", size, size + 7u );
    TEST_ASSERT_EQUAL_INT_MESSAGE( VFTP_OK, vftp_burst_upload( &s_cli, "src.bin", "up/dst.bin" ), msg );
    assert_same( &s_cli_ram, "src.bin", &s_srv_ram, "up/dst.bin" );
    TEST_ASSERT_FALSE_MESSAGE( vftp_burst_server_busy( &s_burst ), msg );
    TEST_ASSERT_EQUAL_INT_MESSAGE( VFTP_OK, vftp_burst_download( &s_cli, "up/dst.bin", "back.bin" ), msg );
    assert_same( &s_cli_ram, "src.bin", &s_cli_ram, "back.bin" );
    /* no resume state left behind on either side */
    char side[VFTP_SIDECAR_PATH_MAX];
    vftp_sidecar_path( "up/dst.bin", "VFM", true, side, sizeof( side ) );
    TEST_ASSERT_FALSE_MESSAGE( vftp_ramfs_exists( &s_srv_ram, side ), msg );
    vftp_sidecar_path( "back.bin", "VFD", false, side, sizeof( side ) );
    TEST_ASSERT_FALSE_MESSAGE( vftp_ramfs_exists( &s_cli_ram, side ), msg );
}

static void test_burst_sizes_without_loss( void )
{
    for ( size_t i = 0; i < sizeof( s_sizes ) / sizeof( s_sizes[0] ); i++ ) {
        burst_roundtrip( s_sizes[i], 0, false, 0 );
    }
}

static void test_burst_sizes_with_10_percent_loss( void )
{
    for ( size_t i = 0; i < sizeof( s_sizes ) / sizeof( s_sizes[0] ); i++ ) {
        burst_roundtrip( s_sizes[i], 100, false, 0 );
    }
}

static void test_burst_30_percent_loss_reorder_and_duplicates( void )
{
    burst_roundtrip( 32769u, 300, true, 100 );
    burst_roundtrip( 120000u, 300, true, 100 );
}

/* A window far bigger than the receive queue: most of every burst overflows and
 * is lost, leaving many scattered holes -- more than one STATUS reply can list.
 * The transfer must still converge (progress-based termination). */
static void test_burst_window_overflowing_receive_queue( void )
{
    setUp();
    s_cli.cfg.window = 1000;
    s_lb.loss_down = 50;
    local_file( "w.bin", 200000, 3 );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_upload( &s_cli, "w.bin", "w.bin" ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_download( &s_cli, "w.bin", "w2.bin" ) );
    assert_same( &s_cli_ram, "w.bin", &s_cli_ram, "w2.bin" );
    TEST_ASSERT_GREATER_THAN_UINT32( 0u, s_lb.down_overflow );
}

/* CCSDS-sized frames: the same engines with a 1 kB link instead of CSP's 150/222 B. */
static void test_burst_big_mtu_link( void )
{
    uint16_t mtu = VFTP_CFG_PACKET_MAX < 1100u ? (uint16_t) VFTP_CFG_PACKET_MAX : 1100u;
    setUp();
    servers_init( mtu, mtu );
    link_init( true, mtu, mtu );
    s_lb.loss_down = 100;
    s_lb.loss_up = 100;
    local_file( "big.bin", 250000, 9 );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_upload( &s_cli, "big.bin", "big.bin" ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_download( &s_cli, "big.bin", "big2.bin" ) );
    assert_same( &s_cli_ram, "big.bin", &s_cli_ram, "big2.bin" );
}

/* The link dies mid-upload, the satellite reboots (server RAM gone), the upload
 * resumes from the sidecar instead of starting over. */
static void test_burst_upload_resumes_after_link_loss_and_server_reboot( void )
{
    setUp();
    local_file( "r.bin", 200000, 11 ); /* > staging buffer: write-behind + sidecar */
    s_lb.die_after_tx = 600;
    TEST_ASSERT_EQUAL_INT( VFTP_LINK, vftp_burst_upload( &s_cli, "r.bin", "r.bin" ) );
    char side[VFTP_SIDECAR_PATH_MAX];
    vftp_sidecar_path( "r.bin", "VFM", true, side, sizeof( side ) );
    TEST_ASSERT_TRUE( vftp_ramfs_exists( &s_srv_ram, side ) );

    servers_init( MTU_DOWN, MTU_UP ); /* reboot */
    link_init( true, MTU_UP, MTU_DOWN );
    memset( &s_ev, 0, sizeof( s_ev ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_upload( &s_cli, "r.bin", "r.bin" ) );
    TEST_ASSERT_EQUAL_INT( 1, s_ev.resumes );
    TEST_ASSERT_GREATER_THAN_UINT32( 400u, s_ev.resume_done );
    uint32_t total = ( 200000u + 143u ) / 144u;
    TEST_ASSERT_LESS_THAN_UINT32( total - 400u, s_cli.stats.data_tx ); /* resent only what was missing */
    assert_same( &s_cli_ram, "r.bin", &s_srv_ram, "r.bin" );
    TEST_ASSERT_FALSE( vftp_ramfs_exists( &s_srv_ram, side ) );
}

/* Which uploads are staged (no sidecar, restart from 0 if cut) and which resume: staged only when
 * size <= VFTP_CFG_STAGE_MAX_BYTES AND chunks <= VFTP_CFG_STAGE_MAX_CHUNKS. Until 2026-10-08 any
 * file up to the 32 KiB buffer was staged: ~5 minutes lost per interruption on a 1200 bit/s radio,
 * and a 22 KB upload at 1-byte chunks (22055 packets) never resumed on the bench. */
static void cut_upload_then_retry( const char *name, uint32_t size, uint16_t chunk, bool expect_resume )
{
    setUp();
    s_cli.cfg.chunk_size = chunk;
    local_file( name, size, size );
    s_lb.die_after_tx = 20; /* request + status + one full round of 12 + status + a few more */
    TEST_ASSERT_EQUAL_INT_MESSAGE( VFTP_LINK, vftp_burst_upload( &s_cli, name, name ), name );
    char side[VFTP_SIDECAR_PATH_MAX];
    vftp_sidecar_path( name, "VFM", true, side, sizeof( side ) );
    TEST_ASSERT_EQUAL_MESSAGE( expect_resume, vftp_ramfs_exists( &s_srv_ram, side ), name );
    link_init( true, MTU_UP, MTU_DOWN );
    memset( &s_ev, 0, sizeof( s_ev ) );
    TEST_ASSERT_EQUAL_INT_MESSAGE( VFTP_OK, vftp_burst_upload( &s_cli, name, name ), name );
    TEST_ASSERT_EQUAL_INT_MESSAGE( expect_resume ? 1 : 0, s_ev.resumes, name );
    assert_same( &s_cli_ram, name, &s_srv_ram, name );
    TEST_ASSERT_FALSE_MESSAGE( vftp_ramfs_exists( &s_srv_ram, side ), name ); /* no sidecar left behind */
}

static void test_burst_upload_staging_bounds( void )
{
    TEST_ASSERT_EQUAL_UINT32( 4096u, VFTP_CFG_STAGE_MAX_BYTES );
    TEST_ASSERT_EQUAL_UINT32( 32u, VFTP_CFG_STAGE_MAX_CHUNKS );
    cut_upload_then_retry( "s4096.bin", 4096u, 144u, false ); /* 29 chunks, 4 KiB: staged, sent again */
    cut_upload_then_retry( "s4097.bin", 4097u, 144u, true );  /* one byte over: write-behind, resumes */
    cut_upload_then_retry( "c47.bin", 3000u, 64u, true );     /* small but 47 chunks: resumes */
    cut_upload_then_retry( "k20.bin", 20000u, 144u, true );   /* staged under the old 32 KiB rule */
}

static void test_burst_download_resumes_with_local_sidecar( void )
{
    setUp();
    remote_file( "d.bin", 150000, 21 );
    s_lb.die_after_tx = 8;
    s_cli.cfg.window = 128;
    TEST_ASSERT_EQUAL_INT( VFTP_LINK, vftp_burst_download( &s_cli, "d.bin", "d.local" ) );
    link_init( true, MTU_UP, MTU_DOWN );
    memset( &s_ev, 0, sizeof( s_ev ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_download( &s_cli, "d.bin", "d.local" ) );
    TEST_ASSERT_EQUAL_INT( 1, s_ev.resumes );
    uint32_t total = ( 150000u + 215u ) / 216u;
    TEST_ASSERT_LESS_THAN_UINT32( total, s_cli.stats.data_rx );
    assert_same( &s_srv_ram, "d.bin", &s_cli_ram, "d.local" );
}

/* A .VFD whose local file was deleted or cut short (by hand, a disk clean-up) is not resumed over
 * holes: a clean start, no CRC mismatch (QEMU 2026-10-08: rm of the partial file, .VFD kept). */
static void test_burst_download_resume_needs_the_local_file( void )
{
    for ( int cut = 0; cut < 2; cut++ ) {
        setUp();
        remote_file( "h.bin", 150000, 23 );
        s_lb.die_after_tx = 8;
        s_cli.cfg.window = 128;
        TEST_ASSERT_EQUAL_INT( VFTP_LINK, vftp_burst_download( &s_cli, "h.bin", "h.local" ) );
        if ( cut == 0 ) {
            TEST_ASSERT_EQUAL_INT( VFTP_OK, s_cli_fs.remove( s_cli_fs.ctx, "h.local" ) );
        } else {
            uint32_t n = 0;
            const uint8_t *d = vftp_ramfs_get( &s_cli_ram, "h.local", &n );
            TEST_ASSERT_NOT_NULL( d );
            memcpy( s_buf2, d, n / 2u );
            TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_ramfs_put( &s_cli_ram, "h.local", s_buf2, n / 2u ) );
        }
        link_init( true, MTU_UP, MTU_DOWN );
        memset( &s_ev, 0, sizeof( s_ev ) );
        TEST_ASSERT_EQUAL_INT_MESSAGE( VFTP_OK, vftp_burst_download( &s_cli, "h.bin", "h.local" ),
                                       cut ? "local file cut short" : "local file deleted" );
        TEST_ASSERT_EQUAL_INT( 0, s_ev.resumes );
        assert_same( &s_srv_ram, "h.bin", &s_cli_ram, "h.local" );
    }
}

/* Resume state is tied to the exact remote content: a changed file restarts cleanly. */
static void test_burst_download_resume_ignores_stale_sidecar( void )
{
    setUp();
    s_burst.crc_step = 0; /* small file: CRC in the DOWNLOAD_REPLY */
    remote_file( "c.bin", 100000, 31 );
    s_lb.die_after_tx = 6;
    TEST_ASSERT_EQUAL_INT( VFTP_LINK, vftp_burst_download( &s_cli, "c.bin", "c.local" ) );
    remote_file( "c.bin", 100000, 32 ); /* same size, new content */
    link_init( true, MTU_UP, MTU_DOWN );
    memset( &s_ev, 0, sizeof( s_ev ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_download( &s_cli, "c.bin", "c.local" ) );
    TEST_ASSERT_EQUAL_INT( 0, s_ev.resumes );
    assert_same( &s_srv_ram, "c.bin", &s_cli_ram, "c.local" );

    /* A large file (CRC not known up front) changed the same way: the resume cannot tell, the
     * end-to-end CRC does -- MISMATCH, resume state dropped, the next attempt is clean. */
    setUp();
    remote_file( "c.bin", 100000, 33 );
    s_lb.die_after_tx = 4;
    TEST_ASSERT_EQUAL_INT( VFTP_LINK, vftp_burst_download( &s_cli, "c.bin", "c.local" ) );
    remote_file( "c.bin", 100000, 34 );
    link_init( true, MTU_UP, MTU_DOWN );
    TEST_ASSERT_EQUAL_INT( VFTP_CRC_MISMATCH, vftp_burst_download( &s_cli, "c.bin", "c.local" ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_download( &s_cli, "c.bin", "c.local" ) );
    assert_same( &s_srv_ram, "c.bin", &s_cli_ram, "c.local" );
}

/* Uploading over a longer existing file must not leave its tail behind (both write paths). */
static void test_burst_upload_replaces_longer_file( void )
{
    setUp();
    remote_file( "o.bin", 90000, 1 );
    local_file( "s.bin", 1000, 2 );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_upload( &s_cli, "s.bin", "o.bin" ) );
    assert_same( &s_cli_ram, "s.bin", &s_srv_ram, "o.bin" );
    remote_file( "o.bin", 200000, 3 );
    local_file( "m.bin", 50000, 4 );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_upload( &s_cli, "m.bin", "o.bin" ) );
    assert_same( &s_cli_ram, "m.bin", &s_srv_ram, "o.bin" );
}

static void test_burst_upload_of_empty_file_creates_it( void )
{
    setUp();
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_ramfs_put( &s_cli_ram, "e.bin", s_buf, 0 ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_upload( &s_cli, "e.bin", "e.bin" ) );
    TEST_ASSERT_TRUE( vftp_ramfs_exists( &s_srv_ram, "e.bin" ) );
}

/* window_up bounds an upload round independently of the download window. */
static void test_burst_upload_window_up( void )
{
    setUp();
    s_cli.cfg.window = 200;
    s_cli.cfg.window_up = 12;
    local_file( "u.bin", 20000, 15 ); /* 139 chunks of 144 B */
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_upload( &s_cli, "u.bin", "u.bin" ) );
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32( ( 138u + 11u ) / 12u, s_cli.stats.rounds );
    assert_same( &s_cli_ram, "u.bin", &s_srv_ram, "u.bin" );
    s_cli.cfg.window_up = 0; /* 0: falls back to window, one round */
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_upload( &s_cli, "u.bin", "u2.bin" ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, s_cli.stats.rounds );
}

/* Write-behind: a 200 kB upload in 144 B chunks must not cost two storage writes per chunk. */
static void test_burst_write_behind_batches_storage_writes( void )
{
    setUp();
    local_file( "b.bin", 200000, 5 );
    s_cli.cfg.window = 256;
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_upload( &s_cli, "b.bin", "b.bin" ) );
    uint32_t chunks = ( 200000u + 143u ) / 144u;
    TEST_ASSERT_LESS_THAN_UINT32( chunks / 8u, s_srv_ram.n_writes );
    assert_same( &s_cli_ram, "b.bin", &s_srv_ram, "b.bin" );
}

/* A storage write that fails once: those chunks are dropped from the bitmap, re-requested,
 * and the transfer still completes. */
static void test_burst_survives_transient_storage_failure( void )
{
    setUp();
    local_file( "t.bin", 100000, 6 );
    s_srv_ram.fail_write_at = 6;
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_upload( &s_cli, "t.bin", "t.bin" ) );
    assert_same( &s_cli_ram, "t.bin", &s_srv_ram, "t.bin" );
}

/* A client filesystem that silently flips one bit in whatever it writes at
 * offset 0: the transfer "works", only the end-to-end CRC can notice. */
static vftp_storage_t s_bad_fs;
static vftp_ret_t bad_write( void *ctx, const char *path, uint32_t off, const uint8_t *data, uint16_t len )
{
    uint8_t tmp[1200];
    memcpy( tmp, data, len );
    if ( off == 0u && len != 0u ) {
        tmp[0] ^= 0x01u;
    }
    return s_cli_fs.write( ctx, path, off, tmp, len );
}

/* A card that keeps refusing writes: the client is told (VFTP_IO) and stops, instead of resending
 * the same chunks until the stall limit -- all three write paths. */
static void test_persistent_storage_failure_is_reported( void )
{
    setUp();
    local_file( "f.bin", 100000, 81 ); /* write-behind */
    s_srv_ram.fail_write_from = 3;     /* sidecar header + bitmap succeed, then the card dies */
    TEST_ASSERT_EQUAL_INT( VFTP_REMOTE, vftp_burst_upload( &s_cli, "f.bin", "f.bin" ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_IO, s_cli.remote_ret );
    TEST_ASSERT_LESS_THAN_UINT32( 2u * 690u, s_cli.stats.data_tx ); /* about one pass, not max_stall of them */
    TEST_ASSERT_FALSE( vftp_burst_server_busy( &s_burst ) );

    setUp();
    local_file( "g.bin", 1000, 82 ); /* staged */
    s_srv_ram.fail_write_from = 1;
    TEST_ASSERT_EQUAL_INT( VFTP_REMOTE, vftp_burst_upload( &s_cli, "g.bin", "g.bin" ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_IO, s_cli.remote_ret );

    setUp();
    use_poll();
    local_file( "h.bin", 5000, 83 );
    s_srv_ram.fail_write_from = 3;
    TEST_ASSERT_EQUAL_INT( VFTP_REMOTE, vftp_poll_upload( &s_cli, "h.bin", "h.bin" ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_IO, s_cli.remote_ret );
    TEST_ASSERT_FALSE( vftp_poll_server_busy( &s_poll ) );

    setUp(); /* poll, one failed write: retried silently */
    use_poll();
    local_file( "h.bin", 5000, 84 );
    s_srv_ram.fail_write_at = 6;
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_poll_upload( &s_cli, "h.bin", "h.bin" ) );
    assert_same( &s_cli_ram, "h.bin", &s_srv_ram, "h.bin" );
}

/* A name the server's filesystem cannot hold (FAT 8.3 on the OBC) is refused at the request,
 * before any data crosses the link -- not after the whole upload. */
static void test_upload_with_bad_name_is_refused_up_front( void )
{
    setUp();
    s_srv_ram.fat83 = true;
    local_file( "n.bin", 50000, 91 );
    TEST_ASSERT_EQUAL_INT( VFTP_REMOTE, vftp_burst_upload( &s_cli, "n.bin", "DATA/VERYLONGNAME.BIN" ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_BADNAME, s_cli.remote_ret );
    TEST_ASSERT_EQUAL_UINT32( 0u, s_cli.stats.data_tx );
    TEST_ASSERT_FALSE( vftp_ramfs_exists( &s_srv_ram, "DATA/VERYLONGNAME.BIN" ) );
    TEST_ASSERT_FALSE( vftp_burst_server_busy( &s_burst ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_upload( &s_cli, "n.bin", "DATA/00000001.BIN" ) );
    use_poll();
    TEST_ASSERT_EQUAL_INT( VFTP_REMOTE, vftp_poll_upload( &s_cli, "n.bin", "a.tar.gz" ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_BADNAME, s_cli.remote_ret );
    TEST_ASSERT_FALSE( vftp_poll_server_busy( &s_poll ) );
}

/* ---- large files: CRC in steps, resume across a chunk_size change --------------- */

/* No request ever makes the server read more than crc_step bytes for a CRC: a large file is
 * checksummed while served and finished over several CRC_REQUESTs (AGAIN), on every path. */
static void test_large_file_crc_in_steps( void )
{
    setUp();
    s_burst.crc_step = 4096;
    s_poll.crc_step = 4096;
    local_file( "L.bin", 200000, 61 );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_upload( &s_cli, "L.bin", "L.bin" ) ); /* read back from storage */
    TEST_ASSERT_GREATER_OR_EQUAL_INT( 40, s_ev.verifies );
    assert_same( &s_cli_ram, "L.bin", &s_srv_ram, "L.bin" );
    memset( &s_ev, 0, sizeof( s_ev ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_download( &s_cli, "L.bin", "L2.bin" ) ); /* fed while served */
    TEST_ASSERT_EQUAL_INT( 1, s_ev.crcs );
    assert_same( &s_cli_ram, "L.bin", &s_cli_ram, "L2.bin" );

    link_init( true, MTU_UP, MTU_DOWN ); /* resumed download: holes the feed never saw */
    s_lb.die_after_tx = 4;
    TEST_ASSERT_EQUAL_INT( VFTP_LINK, vftp_burst_download( &s_cli, "L.bin", "L3.bin" ) );
    link_init( true, MTU_UP, MTU_DOWN );
    memset( &s_ev, 0, sizeof( s_ev ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_download( &s_cli, "L.bin", "L3.bin" ) );
    TEST_ASSERT_EQUAL_INT( 1, s_ev.resumes );
    TEST_ASSERT_GREATER_THAN_INT( 0, s_ev.verifies );
    assert_same( &s_cli_ram, "L.bin", &s_cli_ram, "L3.bin" );

    use_poll();
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_poll_upload( &s_cli, "L.bin", "P.bin" ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_poll_download( &s_cli, "P.bin", "P2.bin" ) );
    assert_same( &s_cli_ram, "L.bin", &s_cli_ram, "P2.bin" );

    s_bad_fs = s_cli_fs; /* and corruption is still caught when the CRC came in steps */
    s_bad_fs.write = bad_write;
    s_cli.cfg.fs = &s_bad_fs;
    s_lb.poll = NULL;
    s_lb.burst = &s_burst;
    TEST_ASSERT_EQUAL_INT( VFTP_CRC_MISMATCH, vftp_burst_download( &s_cli, "L.bin", "L4.bin" ) );
    use_poll();
    TEST_ASSERT_EQUAL_INT( VFTP_CRC_MISMATCH, vftp_poll_download( &s_cli, "L.bin", "L4.bin" ) );
}

/* vftp_rechunk on scattered holes: new chunk j is held iff every byte of it lies in a held old
 * chunk -- checked byte by byte for many size/chunk combinations, with a scratch window smaller
 * than the old bitmap so the windowed reads are exercised too. */
static void test_rechunk_matches_byte_reference( void )
{
    static const uint16_t sizes_cs[] = { 1u, 7u, 64u, 100u, 144u, 216u };
    static uint8_t old_bm[4096];
    static uint8_t new_bm[8192];
    uint8_t scratch[5];
    for ( uint32_t trial = 0; trial < 6u * 6u * 3u; trial++ ) {
        uint16_t old_cs = sizes_cs[trial % 6u];
        uint16_t new_cs = sizes_cs[( trial / 6u ) % 6u];
        uint32_t size = 3000u + trial * 37u + ( trial % 3u == 0u ? 0u : old_cs - 1u );
        uint32_t old_total = vftp_total_chunks( size, old_cs );
        uint32_t new_total = vftp_total_chunks( size, new_cs );
        fill( old_bm, ( old_total + 7u ) / 8u, trial + 1u );
        for ( uint32_t k = 0; k < old_total; k++ ) { /* ~3/4 held, in runs and holes */
            if ( ( k / 5u ) % 4u == 3u ) {
                old_bm[k >> 3] = (uint8_t) ( old_bm[k >> 3] & ~( 1u << ( k & 7u ) ) );
            }
        }
        TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_ramfs_put( &s_srv_ram, "bm", old_bm, ( old_total + 7u ) / 8u ) );
        uint32_t held = 0;
        TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_rechunk( &s_srv_fs, "bm", 0u, old_cs, old_total, new_cs, new_total, size,
                                                      new_bm, scratch, sizeof( scratch ), &held ) );
        uint32_t want_held = 0;
        for ( uint32_t j = 0; j < new_total; j++ ) {
            bool all = true;
            for ( uint32_t byte = j * new_cs; byte < size && byte < ( j + 1u ) * new_cs; byte++ ) {
                uint32_t k = byte / old_cs;
                all = all && ( old_bm[k >> 3] & ( 1u << ( k & 7u ) ) ) != 0u;
            }
            want_held += all ? 1u : 0u;
            TEST_ASSERT_EQUAL_INT( all ? 1 : 0, ( new_bm[j >> 3] >> ( j & 7u ) ) & 1u );
        }
        TEST_ASSERT_EQUAL_UINT32( want_held, held );
    }
}

/* Interrupted upload, then resumed with another chunk size (smaller, then larger): the bytes
 * already on the satellite are kept, only the rest crosses the link. The sidecar lives in the
 * root, so the data directory never shows it. */
static void test_burst_upload_resume_with_new_chunk_size( void )
{
    setUp();
    local_file( "r.bin", 200000, 71 );
    s_lb.die_after_tx = 600;
    TEST_ASSERT_EQUAL_INT( VFTP_LINK, vftp_burst_upload( &s_cli, "r.bin", "DIR/r.bin" ) );
    char side[VFTP_SIDECAR_PATH_MAX];
    vftp_sidecar_path( "DIR/r.bin", "VFM", true, side, sizeof( side ) );
    TEST_ASSERT_NULL( strchr( side, '/' ) );
    TEST_ASSERT_TRUE( vftp_ramfs_exists( &s_srv_ram, side ) );
    link_init( true, MTU_UP, MTU_DOWN );
    vftp_index_rep_t info;
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_index( &s_cli, "DIR", VFTP_INDEX_COUNT_ONLY, &info ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, info.total );

    servers_init( MTU_DOWN, MTU_UP ); /* reboot */
    link_init( true, MTU_UP, MTU_DOWN );
    s_lb.die_after_tx = 300;
    s_cli.cfg.chunk_size = 100; /* 144 -> 100 */
    memset( &s_ev, 0, sizeof( s_ev ) );
    TEST_ASSERT_EQUAL_INT( VFTP_LINK, vftp_burst_upload( &s_cli, "r.bin", "DIR/r.bin" ) );
    TEST_ASSERT_EQUAL_INT( 1, s_ev.resumes );
    TEST_ASSERT_GREATER_THAN_UINT32( 800u, s_ev.resume_done ); /* ~588 chunks of 144 B = ~846 of 100 B */

    servers_init( MTU_DOWN, MTU_UP );
    link_init( true, MTU_UP, MTU_DOWN );
    s_cli.cfg.chunk_size = 0; /* 100 -> 144 */
    memset( &s_ev, 0, sizeof( s_ev ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_upload( &s_cli, "r.bin", "DIR/r.bin" ) );
    TEST_ASSERT_EQUAL_INT( 1, s_ev.resumes );
    uint32_t total = ( 200000u + 143u ) / 144u;
    TEST_ASSERT_GREATER_THAN_UINT32( 700u, s_ev.resume_done );
    TEST_ASSERT_LESS_THAN_UINT32( total - 700u, s_cli.stats.data_tx );
    assert_same( &s_cli_ram, "r.bin", &s_srv_ram, "DIR/r.bin" );
    TEST_ASSERT_FALSE( vftp_ramfs_exists( &s_srv_ram, side ) );
}

static void test_burst_download_resume_with_new_chunk_size( void )
{
    setUp();
    remote_file( "d.bin", 150000, 22 );
    s_cli.cfg.window = 128;
    s_lb.die_after_tx = 4; /* request + 2 rounds; 96 of each 128 fit the receive queue: 192 chunks of 216 B */
    TEST_ASSERT_EQUAL_INT( VFTP_LINK, vftp_burst_download( &s_cli, "d.bin", "d.local" ) );
    link_init( true, MTU_UP, MTU_DOWN );
    s_cli.cfg.chunk_size = 100;
    memset( &s_ev, 0, sizeof( s_ev ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_download( &s_cli, "d.bin", "d.local" ) );
    TEST_ASSERT_EQUAL_INT( 1, s_ev.resumes );
    TEST_ASSERT_EQUAL_UINT32( 192u * 216u / 100u, s_ev.resume_done ); /* every whole 100 B chunk kept */
    TEST_ASSERT_LESS_THAN_UINT32( 1500u - 400u, s_cli.stats.data_rx );
    assert_same( &s_srv_ram, "d.bin", &s_cli_ram, "d.local" );
}

static void test_poll_upload_resume_with_new_chunk_size( void )
{
    setUp();
    use_poll();
    local_file( "w.bin", 30000, 13 );
    s_lb.die_after_tx = 100;
    TEST_ASSERT_EQUAL_INT( VFTP_LINK, vftp_poll_upload( &s_cli, "w.bin", "w.bin" ) );
    servers_init( MTU_DOWN, MTU_UP );
    link_init( false, MTU_UP, MTU_DOWN );
    s_lb.die_after_tx = 100;
    s_cli.cfg.chunk_size = 64; /* 144 -> 64: watermark 98 * 144 B = 220 chunks of 64 */
    memset( &s_ev, 0, sizeof( s_ev ) );
    TEST_ASSERT_EQUAL_INT( VFTP_LINK, vftp_poll_upload( &s_cli, "w.bin", "w.bin" ) );
    TEST_ASSERT_EQUAL_INT( 1, s_ev.resumes );
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32( 90u * 144u / 64u, s_ev.resume_done );
    servers_init( MTU_DOWN, MTU_UP );
    link_init( false, MTU_UP, MTU_DOWN );
    s_cli.cfg.chunk_size = 0; /* 64 -> 144 */
    memset( &s_ev, 0, sizeof( s_ev ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_poll_upload( &s_cli, "w.bin", "w.bin" ) );
    TEST_ASSERT_EQUAL_INT( 1, s_ev.resumes );
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32( 135u, s_ev.resume_done ); /* ~320 chunks of 64 B = ~142 of 144 B */
    assert_same( &s_cli_ram, "w.bin", &s_srv_ram, "w.bin" );
}

/* Idempotent request vs BUSY, and abort() frees the service (no stuck session). */
static void test_burst_busy_idempotent_and_abort( void )
{
    setUp();
    uint8_t b[VFTP_CFG_PACKET_MAX];
    vftp_msg_t m;
    memset( &m, 0, sizeof( m ) );
    m.type = VFTP_MSG_UPLOAD_REQUEST;
    m.u.up_req.size = 100000;
    m.u.up_req.crc32 = 1;
    m.u.up_req.chunk_size = 100;
    vftp_path_set( m.u.up_req.path, "x.bin" );
    uint16_t n = vftp_encode( &m, b, sizeof( b ) );
    s_lb.q_count = 0;
    (void) vftp_burst_server_handle( &s_burst, b, n, discard, NULL );
    TEST_ASSERT_TRUE( vftp_burst_server_busy( &s_burst ) );
    /* same request again: accepted; a different one: BUSY */
    vftp_msg_t r;
    uint8_t got[VFTP_CFG_PACKET_MAX];
    int len;
    link_init( true, MTU_UP, MTU_DOWN );
    s_link.send( s_link.ctx, b, n );
    len = s_link.recv( s_link.ctx, got, sizeof( got ), 0 );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_decode( got, (uint16_t) len, &r ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_OK, r.u.ret_rep.ret );
    vftp_path_set( m.u.up_req.path, "other.bin" );
    n = vftp_encode( &m, b, sizeof( b ) );
    s_link.send( s_link.ctx, b, n );
    len = s_link.recv( s_link.ctx, got, sizeof( got ), 0 );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_decode( got, (uint16_t) len, &r ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_BUSY, r.u.ret_rep.ret );
    vftp_burst_server_abort( &s_burst );
    TEST_ASSERT_FALSE( vftp_burst_server_busy( &s_burst ) );
}

/* DATA with a wrong length must never be written. */
static void test_burst_malformed_data_is_ignored( void )
{
    setUp();
    uint8_t b[VFTP_CFG_PACKET_MAX];
    vftp_msg_t m;
    memset( &m, 0, sizeof( m ) );
    m.type = VFTP_MSG_UPLOAD_REQUEST;
    m.u.up_req.size = 300;
    m.u.up_req.chunk_size = 100;
    vftp_path_set( m.u.up_req.path, "x.bin" );
    (void) vftp_burst_server_handle( &s_burst, b, vftp_encode( &m, b, sizeof( b ) ), discard, NULL );
    memset( &m, 0, sizeof( m ) );
    m.type = VFTP_MSG_DATA;
    m.u.data.chunk = 0;
    m.u.data.bytes = s_buf;
    m.u.data.len = 99; /* chunk 0 must be exactly 100 bytes */
    (void) vftp_burst_server_handle( &s_burst, b, vftp_encode( &m, b, sizeof( b ) ), discard, NULL );
    TEST_ASSERT_EQUAL_UINT32( 0u, s_burst.complete );
    m.u.data.chunk = 3; /* out of range */
    m.u.data.len = 100;
    (void) vftp_burst_server_handle( &s_burst, b, vftp_encode( &m, b, sizeof( b ) ), discard, NULL );
    TEST_ASSERT_EQUAL_UINT32( 0u, s_burst.complete );
}

static void test_end_to_end_crc_catches_corruption( void )
{
    setUp();
    remote_file( "z.bin", 5000, 41 );
    s_bad_fs = s_cli_fs;
    s_bad_fs.write = bad_write;
    s_cli.cfg.fs = &s_bad_fs;
    TEST_ASSERT_EQUAL_INT( VFTP_CRC_MISMATCH, vftp_burst_download( &s_cli, "z.bin", "z.local" ) );
    char side[VFTP_SIDECAR_PATH_MAX];
    vftp_sidecar_path( "z.local", "VFD", false, side, sizeof( side ) );
    TEST_ASSERT_FALSE( vftp_ramfs_exists( &s_cli_ram, side ) ); /* untrusted bytes: next attempt starts over */
    use_poll();
    TEST_ASSERT_EQUAL_INT( VFTP_CRC_MISMATCH, vftp_poll_download( &s_cli, "z.bin", "z.local" ) );
    s_lb.poll = NULL;
    s_lb.burst = &s_burst;
    s_cli.cfg.fs = &s_cli_fs;
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_download( &s_cli, "z.bin", "z.local" ) );
    assert_same( &s_srv_ram, "z.bin", &s_cli_ram, "z.local" );
}

/* ---- listing (methods 1 and 2), delete, errors ---------------------------- */

typedef struct {
    uint32_t count;
    uint32_t total;
    char names[40][VFTP_MAX_FILE_NAME + 1u];
} list_seen_t;

static void on_entry( void *ctx, uint32_t total, uint32_t index, const vftp_list_entry_t *e )
{
    list_seen_t *s = (list_seen_t *) ctx;
    s->total = total;
    if ( index < 40u ) {
        memcpy( s->names[index], e->name, sizeof( e->name ) );
    }
    s->count++;
}

static void test_list_pages_through_a_large_directory( void )
{
    setUp();
    for ( int i = 0; i < 13; i++ ) {
        char p[32];
        snprintf( p, sizeof( p ), "dir/%08d.bin", i );
        remote_file( p, (uint32_t) ( 10 + i ), (uint32_t) i );
    }
    remote_file( "other/x.bin", 5, 1 );
    static list_seen_t seen;
    memset( &seen, 0, sizeof( seen ) );
    uint32_t total = 0;
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_list( &s_cli, "dir", on_entry, &seen, &total ) );
    TEST_ASSERT_EQUAL_UINT32( 13u, total ); /* > 9 per 222-byte reply: needs two pages on one conversation */
    TEST_ASSERT_EQUAL_UINT32( 13u, seen.count );
    for ( uint32_t i = 0; i < 13u; i++ ) {
        for ( uint32_t j = i + 1u; j < 13u; j++ ) {
            TEST_ASSERT_TRUE( strcmp( seen.names[i], seen.names[j] ) != 0 );
        }
    }
    s_lb.loss_up = 300;
    s_lb.loss_down = 300;
    memset( &seen, 0, sizeof( seen ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_list( &s_cli, "/dir/", on_entry, &seen, &total ) );
    TEST_ASSERT_EQUAL_UINT32( 13u, seen.count );
}

static void test_list_of_missing_directory_is_an_error( void )
{
    setUp();
    TEST_ASSERT_EQUAL_INT( VFTP_REMOTE, vftp_burst_list( &s_cli, "nope", NULL, NULL, NULL ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_NOENT, s_cli.remote_ret );
}

static void test_list_regen_then_download_list_txt( void )
{
    setUp();
    remote_file( "logs/A.BIN", 100, 1 );
    remote_file( "logs/B.BIN", 2000, 2 );
    remote_file( "logs/C.BIN", 30000, 3 );
    char out[VFTP_MAX_PATH];
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_list_regen( &s_cli, "logs", out ) );
    TEST_ASSERT_EQUAL_STRING( "LIST.TXT", out ); /* in the root, whatever directory was listed */
    TEST_ASSERT_FALSE( vftp_ramfs_exists( &s_srv_ram, "logs/LIST.TXT" ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_list_regen( &s_cli, "logs", out ) ); /* again: replaced */
    TEST_ASSERT_FALSE( vftp_ramfs_exists( &s_srv_ram, "LISTTMP.TXT" ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_download( &s_cli, out, "list.txt" ) );
    uint32_t len = 0;
    const uint8_t *txt = vftp_ramfs_get( &s_cli_ram, "list.txt", &len );
    TEST_ASSERT_NOT_NULL( txt );
    memcpy( s_buf2, txt, len );
    s_buf2[len] = 0;
    const char *t = (const char *) s_buf2;
    TEST_ASSERT_NOT_NULL( strstr( t, "       100 - A.BIN\n" ) );
    TEST_ASSERT_NOT_NULL( strstr( t, "      2000 - B.BIN\n" ) );
    TEST_ASSERT_NOT_NULL( strstr( t, "     30000 - C.BIN\n" ) );
    TEST_ASSERT_EQUAL_MEMORY( "# logs\n", t, 7 );
    TEST_ASSERT_NOT_NULL( strstr( t, "\n# 3 entries\n" ) );
    TEST_ASSERT_EQUAL_INT( '\n', t[len - 1u] );
}

/* Listing the root, where LIST.TXT itself lives: it never lists itself or its temporary, and a
 * failed regen leaves the previous list whole. */
static void test_list_regen_of_root_is_atomic( void )
{
    setUp();
    remote_file( "A.BIN", 10, 1 );
    remote_file( "B.BIN", 20, 2 );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_list_regen( &s_cli, "/", NULL ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_list_regen( &s_cli, "", NULL ) );
    uint32_t len = 0;
    const uint8_t *txt = vftp_ramfs_get( &s_srv_ram, "LIST.TXT", &len );
    TEST_ASSERT_NOT_NULL( txt );
    memcpy( s_buf2, txt, len );
    s_buf2[len] = 0;
    const char *t = (const char *) s_buf2;
    TEST_ASSERT_NOT_NULL( strstr( t, " A.BIN\n" ) );
    TEST_ASSERT_NOT_NULL( strstr( t, " B.BIN\n" ) );
    TEST_ASSERT_NULL( strstr( t, "LIST" ) );
    TEST_ASSERT_NOT_NULL( strstr( t, "# 2 entries\n" ) );
    TEST_ASSERT_FALSE( vftp_ramfs_exists( &s_srv_ram, "LISTTMP.TXT" ) );

    s_srv_ram.fail_write_from = s_srv_ram.n_writes + 2u; /* card dies half way through the next regen */
    TEST_ASSERT_EQUAL_INT( VFTP_REMOTE, vftp_burst_list_regen( &s_cli, "", NULL ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_IO, s_cli.remote_ret );
    s_srv_ram.fail_write_from = 0;
    uint32_t len2 = 0;
    const uint8_t *txt2 = vftp_ramfs_get( &s_srv_ram, "LIST.TXT", &len2 );
    TEST_ASSERT_EQUAL_UINT32( len, len2 );
    TEST_ASSERT_EQUAL_MEMORY( s_buf2, txt2, len ); /* the previous list, untouched */
    TEST_ASSERT_FALSE( vftp_ramfs_exists( &s_srv_ram, "LISTTMP.TXT" ) );
}

/* The server ends a listing's conversation with its last page (a held-open conversation is what
 * kept the service busy, and the next `ls` waiting, for seconds). */
static void test_list_ends_conversation_after_last_page( void )
{
    setUp();
    for ( int i = 0; i < 15; i++ ) {
        char p[32];
        snprintf( p, sizeof( p ), "e/%08d.bin", i );
        remote_file( p, 10, (uint32_t) i );
    }
    static list_seen_t seen;
    memset( &seen, 0, sizeof( seen ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_list( &s_cli, "e", on_entry, &seen, NULL ) );
    TEST_ASSERT_EQUAL_UINT32( 15u, seen.count ); /* two pages: the first must not have closed it */
    TEST_ASSERT_TRUE( s_lb.closed );
    memset( &seen, 0, sizeof( seen ) );
    TEST_ASSERT_EQUAL_INT( VFTP_REMOTE, vftp_burst_list( &s_cli, "nodir", on_entry, &seen, NULL ) );
    TEST_ASSERT_TRUE( s_lb.closed );
    s_lb.loss_down = 300; /* a lost last page is asked again, and still answered */
    s_lb.loss_up = 300;
    memset( &seen, 0, sizeof( seen ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_list( &s_cli, "e", on_entry, &seen, NULL ) );
    TEST_ASSERT_EQUAL_UINT32( 15u, seen.count );
}

static void test_remove_and_remote_error_codes( void )
{
    setUp();
    remote_file( "k.bin", 10, 1 );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_remove( &s_cli, "k.bin" ) );
    TEST_ASSERT_FALSE( vftp_ramfs_exists( &s_srv_ram, "k.bin" ) );
    TEST_ASSERT_EQUAL_INT( VFTP_REMOTE, vftp_burst_remove( &s_cli, "k.bin" ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_NOENT, s_cli.remote_ret );
    TEST_ASSERT_EQUAL_INT( VFTP_REMOTE, vftp_burst_download( &s_cli, "missing.bin", "m" ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_NOENT, s_cli.remote_ret );
    TEST_ASSERT_EQUAL_INT( VFTP_NOENT, vftp_burst_upload( &s_cli, "no-local-file", "x" ) );
    char p56[VFTP_MAX_PATH + 1];
    memset( p56, 'p', VFTP_MAX_PATH );
    p56[VFTP_MAX_PATH] = '\0';
    local_file( "l.bin", 10, 1 );
    TEST_ASSERT_EQUAL_INT( VFTP_INVAL, vftp_burst_upload( &s_cli, "l.bin", p56 ) ); /* never silently truncated */
    TEST_ASSERT_FALSE( vftp_burst_server_busy( &s_burst ) );
}


/* ---- listing method 3: by index, both services ------------------------------ */

static void index_both_services( bool burst )
{
    setUp();
    if ( !burst ) {
        use_poll();
    }
    for ( int i = 0; i < 5; i++ ) {
        char p[32];
        snprintf( p, sizeof( p ), "data/%08d.bin", i );
        remote_file( p, (uint32_t) ( 1000 + 300 * i ), (uint32_t) ( 50 + i ) );
    }
    vftp_index_rep_t info;
    TEST_ASSERT_EQUAL_INT( VFTP_OK, burst ? vftp_burst_index( &s_cli, "data", VFTP_INDEX_COUNT_ONLY, &info )
                                          : vftp_poll_index( &s_cli, "data", VFTP_INDEX_COUNT_ONLY, &info ) );
    TEST_ASSERT_EQUAL_UINT32( 5u, info.total );
    for ( uint32_t i = 0; i < 5u; i++ ) {
        char remote[VFTP_MAX_PATH];
        char local[32];
        snprintf( local, sizeof( local ), "got%lu.bin", (unsigned long) i );
        TEST_ASSERT_EQUAL_INT( VFTP_OK, burst ? vftp_burst_download_index( &s_cli, "/data/", i, local, remote )
                                              : vftp_poll_download_index( &s_cli, "/data/", i, local, remote ) );
        TEST_ASSERT_EQUAL_MEMORY( "/data/", remote, 6 );    /* resolved name comes back */
        assert_same( &s_srv_ram, remote, &s_cli_ram, local ); /* and it is that file's content */
    }
    TEST_ASSERT_EQUAL_INT( VFTP_REMOTE, burst ? vftp_burst_download_index( &s_cli, "data", 5u, NULL, NULL )
                                              : vftp_poll_download_index( &s_cli, "data", 5u, NULL, NULL ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_RANGE, s_cli.remote_ret );
    TEST_ASSERT_EQUAL_INT( VFTP_REMOTE, burst ? vftp_burst_index( &s_cli, "nodir", 0u, &info )
                                              : vftp_poll_index( &s_cli, "nodir", 0u, &info ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_NOENT, s_cli.remote_ret );
    TEST_ASSERT_FALSE( burst ? vftp_burst_server_busy( &s_burst ) : vftp_poll_server_busy( &s_poll ) );
}

static void test_index_count_and_download_burst( void )
{
    index_both_services( true );
}

static void test_index_count_and_download_poll( void )
{
    index_both_services( false );
}

/* A listing walks the directory once per page, never once per entry (the old FsIo port rescanned
 * from the start for every entry -- O(n^3) card reads, which timed out on the real OBC). */
static void test_list_walks_once_per_page( void )
{
    setUp();
    for ( int i = 0; i < 15; i++ ) {
        char p[32];
        snprintf( p, sizeof( p ), "w/%08d.bin", i );
        remote_file( p, 10, (uint32_t) i );
    }
    s_srv_ram.n_walk = 0;
    s_srv_ram.n_dirent = 0;
    static list_seen_t seen;
    memset( &seen, 0, sizeof( seen ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_list( &s_cli, "w", on_entry, &seen, NULL ) );
    TEST_ASSERT_EQUAL_UINT32( 15u, seen.count );
    TEST_ASSERT_EQUAL_UINT32( 2u, s_srv_ram.n_walk ); /* two pages, one walk each */
    TEST_ASSERT_EQUAL_UINT32( 0u, s_srv_ram.n_dirent );
}

/* A port without walk() (dirent() only) still gets every listing feature. */
static vftp_storage_t s_nowalk_fs;
static void test_listing_without_walk_port( void )
{
    setUp();
    s_nowalk_fs = s_srv_fs;
    s_nowalk_fs.walk = NULL;
    servers_init( MTU_DOWN, MTU_UP );
    s_burst.cfg.fs = &s_nowalk_fs;
    for ( int i = 0; i < 12; i++ ) {
        char p[32];
        snprintf( p, sizeof( p ), "n/%08d.bin", i );
        remote_file( p, 20, (uint32_t) i );
    }
    static list_seen_t seen;
    memset( &seen, 0, sizeof( seen ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_list( &s_cli, "n", on_entry, &seen, NULL ) );
    TEST_ASSERT_EQUAL_UINT32( 12u, seen.count );
    vftp_index_rep_t info;
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_index( &s_cli, "n", 11u, &info ) );
    TEST_ASSERT_EQUAL_STRING( seen.names[11], info.name );
    char out[VFTP_MAX_PATH];
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_list_regen( &s_cli, "n", out ) );
    uint32_t len = 0;
    TEST_ASSERT_NOT_NULL( vftp_ramfs_get( &s_srv_ram, "LIST.TXT", &len ) );
    /* "# n\n", 12 lines of "%10lu - 00000000.bin\n" (10+3+12+1), "# 12 entries\n" */
    TEST_ASSERT_EQUAL_UINT32( 4u + 12u * 26u + 13u, len );
}

/* ---- poll ------------------------------------------------------------------ */

static void poll_roundtrip( uint32_t size, uint16_t loss )
{
    char msg[64];
    snprintf( msg, sizeof( msg ), "poll size %lu loss %u", (unsigned long) size, (unsigned int) loss );
    setUp();
    use_poll();
    s_lb.loss_up = loss;
    s_lb.loss_down = loss;
    local_file( "p.bin", size, size + 1u );
    TEST_ASSERT_EQUAL_INT_MESSAGE( VFTP_OK, vftp_poll_upload( &s_cli, "p.bin", "q/p.bin" ), msg );
    assert_same( &s_cli_ram, "p.bin", &s_srv_ram, "q/p.bin" );
    TEST_ASSERT_EQUAL_INT_MESSAGE( VFTP_OK, vftp_poll_download( &s_cli, "q/p.bin", "p.back" ), msg );
    assert_same( &s_cli_ram, "p.bin", &s_cli_ram, "p.back" );
    TEST_ASSERT_FALSE_MESSAGE( vftp_poll_server_busy( &s_poll ), msg );
}

static void test_poll_sizes_and_loss( void )
{
    static const uint32_t sizes[] = { 0u, 1u, 144u, 145u, 1000u, 20000u };
    static const uint16_t losses[] = { 0u, 100u, 300u };
    for ( size_t l = 0; l < 3u; l++ ) {
        for ( size_t i = 0; i < sizeof( sizes ) / sizeof( sizes[0] ); i++ ) {
            poll_roundtrip( sizes[i], losses[l] );
        }
    }
}

static void test_poll_upload_resumes_after_link_loss_and_server_reboot( void )
{
    setUp();
    use_poll();
    local_file( "w.bin", 30000, 12 );
    s_lb.die_after_tx = 100;
    TEST_ASSERT_EQUAL_INT( VFTP_LINK, vftp_poll_upload( &s_cli, "w.bin", "w.bin" ) );
    servers_init( MTU_DOWN, MTU_UP );
    link_init( false, MTU_UP, MTU_DOWN );
    memset( &s_ev, 0, sizeof( s_ev ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_poll_upload( &s_cli, "w.bin", "w.bin" ) );
    TEST_ASSERT_EQUAL_INT( 1, s_ev.resumes );
    TEST_ASSERT_GREATER_THAN_UINT32( 90u, s_ev.resume_done );
    assert_same( &s_cli_ram, "w.bin", &s_srv_ram, "w.bin" );
    char side[VFTP_SIDECAR_PATH_MAX];
    vftp_sidecar_path( "w.bin", "VFI", true, side, sizeof( side ) );
    TEST_ASSERT_FALSE( vftp_ramfs_exists( &s_srv_ram, side ) );
}

/* ---- naming, framing, POSIX port -------------------------------------------- */

static void test_sidecar_names_are_8_3( void )
{
    char out[VFTP_SIDECAR_PATH_MAX];
    TEST_ASSERT_TRUE( vftp_sidecar_path( "/IOBC/STATIONS.TLE", "VFD", false, out, sizeof( out ) ) );
    TEST_ASSERT_EQUAL_UINT32( 6u + 12u, (uint32_t) strlen( out ) ); /* client side: next to the file */
    TEST_ASSERT_EQUAL_MEMORY( "/IOBC/", out, 6 );
    TEST_ASSERT_EQUAL_STRING( ".VFD", out + 14 );
    char root[VFTP_SIDECAR_PATH_MAX];
    TEST_ASSERT_TRUE( vftp_sidecar_path( "/IOBC/STATIONS.TLE", "VFD", true, root, sizeof( root ) ) );
    TEST_ASSERT_EQUAL_STRING( out + 6, root ); /* server side: same name, in the root */
    TEST_ASSERT_TRUE( vftp_path_is_83( root ) );
    TEST_ASSERT_TRUE( vftp_sidecar_path( "a.tar.gz", "VFI", true, out, sizeof( out ) ) );
    TEST_ASSERT_EQUAL_UINT32( 12u, (uint32_t) strlen( out ) );
    char again[VFTP_SIDECAR_PATH_MAX];
    vftp_sidecar_path( "a.tar.gz", "VFI", true, again, sizeof( again ) );
    TEST_ASSERT_EQUAL_STRING( out, again );
}

static void test_8_3_name_check( void )
{
    TEST_ASSERT_TRUE( vftp_path_is_83( "/VOBC/00000001.BIN" ) );
    TEST_ASSERT_TRUE( vftp_path_is_83( "./a/b/c" ) );
    TEST_ASSERT_TRUE( vftp_path_is_83( "../X~1.TXT" ) );
    TEST_ASSERT_TRUE( vftp_path_is_83( "LIST.TXT" ) );
    TEST_ASSERT_FALSE( vftp_path_is_83( "ABCDEFGHI.BIN" ) ); /* 9-character name */
    TEST_ASSERT_FALSE( vftp_path_is_83( "A.TEXT" ) );        /* 4-character extension */
    TEST_ASSERT_FALSE( vftp_path_is_83( "a.tar.gz" ) );      /* two dots */
    TEST_ASSERT_FALSE( vftp_path_is_83( "VERYLONGDIR/A.B" ) );
    TEST_ASSERT_FALSE( vftp_path_is_83( "A B.TXT" ) );
    TEST_ASSERT_FALSE( vftp_path_is_83( ".HIDDEN" ) );
    TEST_ASSERT_FALSE( vftp_path_is_83( "A." ) );
}

static void test_ccsds_space_packet_header( void )
{
    uint8_t f[32];
    const uint8_t data[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    TEST_ASSERT_EQUAL_UINT16( 16u, vftp_spp_wrap( f, sizeof( f ), VFTP_SPP_APID_BURST, true, 5u, data, 10u ) );
    static const uint8_t hdr[6] = { 0x11, 0x09, 0xC0, 0x05, 0x00, 0x09 }; /* 000 1 0 00100001001 */
    TEST_ASSERT_EQUAL_HEX8_ARRAY( hdr, f, 6 );
    vftp_spp_t sp;
    TEST_ASSERT_TRUE( vftp_spp_unwrap( f, 16u, &sp ) );
    TEST_ASSERT_EQUAL_UINT16( VFTP_SPP_APID_BURST, sp.apid );
    TEST_ASSERT_TRUE( sp.telecommand );
    TEST_ASSERT_EQUAL_UINT16( 10u, sp.len );
    TEST_ASSERT_FALSE( vftp_spp_unwrap( f, 15u, &sp ) ); /* length field disagrees */
    f[0] = 0x31; /* version 1 */
    TEST_ASSERT_FALSE( vftp_spp_unwrap( f, 16u, &sp ) );
    f[0] = 0x11 | 0x08; /* secondary header flag */
    TEST_ASSERT_FALSE( vftp_spp_unwrap( f, 16u, &sp ) );
}

static vftp_ret_t put_via( const vftp_storage_t *fs, const char *path, const uint8_t *data, uint32_t len )
{
    vftp_ret_t st = fs->truncate( fs->ctx, path );
    for ( uint32_t off = 0; st == VFTP_OK && off < len; off += 4096u ) {
        uint32_t n = len - off < 4096u ? len - off : 4096u;
        st = fs->write( fs->ctx, path, off, data + off, (uint16_t) n );
    }
    return st;
}

static void test_posix_storage_end_to_end( void )
{
    char sdir[] = "/tmp/vftp_srv_XXXXXX";
    char cdir[] = "/tmp/vftp_cli_XXXXXX";
    TEST_ASSERT_NOT_NULL( mkdtemp( sdir ) );
    TEST_ASSERT_NOT_NULL( mkdtemp( cdir ) );
    static vftp_posix_fs_t sctx, cctx;
    vftp_storage_posix_init( &s_srv_fs, &sctx, sdir );
    vftp_storage_posix_init( &s_cli_fs, &cctx, cdir );
    servers_init( MTU_DOWN, MTU_UP );
    link_init( true, MTU_UP, MTU_DOWN );
    s_lb.loss_down = 100;
    s_lb.loss_up = 100;

    fill( s_buf, 120000, 77 );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, put_via( &s_cli_fs, "in.bin", s_buf, 120000 ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_upload( &s_cli, "in.bin", "/stored.bin" ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_download( &s_cli, "stored.bin", "out.bin" ) );
    char p[600];
    snprintf( p, sizeof( p ), "%s/out.bin", cdir );
    FILE *f = fopen( p, "rb" );
    TEST_ASSERT_NOT_NULL( f );
    TEST_ASSERT_EQUAL_UINT32( 120000u, (uint32_t) fread( s_buf2, 1, sizeof( s_buf2 ), f ) );
    fclose( f );
    TEST_ASSERT_EQUAL_MEMORY( s_buf, s_buf2, 120000 );

    static list_seen_t seen;
    memset( &seen, 0, sizeof( seen ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_list( &s_cli, "", on_entry, &seen, NULL ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, seen.count ); /* stored.bin only: no ".", "..", no leftover sidecar */
    TEST_ASSERT_EQUAL_STRING( "stored.bin", seen.names[0] );

    snprintf( p, sizeof( p ), "rm -rf %s %s", sdir, cdir );
    TEST_ASSERT_EQUAL_INT( 0, system( p ) );
}


static void local_file_posix( const char *dir, const char *name )
{
    char p[600];
    snprintf( p, sizeof( p ), "%s/%s", dir, name );
    FILE *f = fopen( p, "wb" );
    TEST_ASSERT_NOT_NULL( f );
    fputs( "hello", f );
    fclose( f );
}

/* POSIX port: a directory entry can be indexed but never downloaded, by index or by name; and a
 * list.txt larger than the server's 32 KiB buffer is written in several passes. */
static void test_posix_directories_and_big_list_txt( void )
{
    char sdir[] = "/tmp/vftp_srv_XXXXXX";
    char cdir[] = "/tmp/vftp_cli_XXXXXX";
    TEST_ASSERT_NOT_NULL( mkdtemp( sdir ) );
    TEST_ASSERT_NOT_NULL( mkdtemp( cdir ) );
    static vftp_posix_fs_t sctx, cctx;
    vftp_storage_posix_init( &s_srv_fs, &sctx, sdir );
    vftp_storage_posix_init( &s_cli_fs, &cctx, cdir );
    servers_init( MTU_DOWN, MTU_UP );
    link_init( true, MTU_UP, MTU_DOWN );
    char p[700];
    snprintf( p, sizeof( p ), "mkdir -p %s/VOBC/SUB && cd %s/VOBC && for i in $(seq 1 900); do echo $i > F$i.TXT; done",
              sdir, sdir );
    TEST_ASSERT_EQUAL_INT( 0, system( p ) );

    vftp_index_rep_t info;
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_index( &s_cli, "VOBC", VFTP_INDEX_COUNT_ONLY, &info ) );
    TEST_ASSERT_EQUAL_UINT32( 901u, info.total );
    uint32_t sub = UINT32_MAX;
    for ( uint32_t i = 0; i < 901u && sub == UINT32_MAX; i++ ) {
        TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_index( &s_cli, "VOBC", i, &info ) );
        if ( info.flags & VFTP_ENTRY_DIR ) {
            sub = i;
        }
    }
    TEST_ASSERT_TRUE( sub != UINT32_MAX );
    char remote[VFTP_MAX_PATH];
    TEST_ASSERT_EQUAL_INT( VFTP_ISDIR, vftp_burst_download_index( &s_cli, "VOBC", sub, NULL, remote ) );
    TEST_ASSERT_EQUAL_STRING( "VOBC/SUB", remote );
    use_poll();
    TEST_ASSERT_EQUAL_INT( VFTP_ISDIR, vftp_poll_download_index( &s_cli, "VOBC", sub, NULL, NULL ) );
    TEST_ASSERT_EQUAL_INT( VFTP_REMOTE, vftp_poll_download( &s_cli, "VOBC/SUB", "x" ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_ISDIR, s_cli.remote_ret );
    s_lb.poll = NULL;
    s_lb.burst = &s_burst;
    TEST_ASSERT_EQUAL_INT( VFTP_REMOTE, vftp_burst_download( &s_cli, "VOBC", "x" ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_ISDIR, s_cli.remote_ret );

    char out[VFTP_MAX_PATH];
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_list_regen( &s_cli, "VOBC", out ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_download( &s_cli, out, "list.txt" ) );
    snprintf( p, sizeof( p ),
              "test $(grep -vc '^#' %s/list.txt) -eq 901 && grep -q ' SUB/$' %s/list.txt && "
              "tail -1 %s/list.txt | grep -qx '# 901 entries' && test -f %s/LIST.TXT && ! test -e %s/LISTTMP.TXT",
              cdir, cdir, cdir, sdir, sdir );
    TEST_ASSERT_EQUAL_INT( 0, system( p ) ); /* > 32 KiB of text: several walk+write passes, nothing lost */

    /* an upload onto a directory is refused up front, before anything is truncated */
    local_file_posix( cdir, "d.bin" );
    TEST_ASSERT_EQUAL_INT( VFTP_REMOTE, vftp_burst_upload( &s_cli, "d.bin", "VOBC/SUB" ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_ISDIR, s_cli.remote_ret );
    TEST_ASSERT_EQUAL_INT( VFTP_REMOTE, vftp_burst_upload( &s_cli, "d.bin", "VOBC/" ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_INVAL, s_cli.remote_ret );
    snprintf( p, sizeof( p ), "rm -rf %s %s", sdir, cdir );
    TEST_ASSERT_EQUAL_INT( 0, system( p ) );
}

/* ---- sessions (late packets of an earlier transfer) ---------------------- */

typedef struct {
    int n;
    uint8_t last[VFTP_CFG_PACKET_MAX];
    uint16_t last_len;
} capture_t;

static int capture( void *ctx, const uint8_t *pkt, uint16_t len )
{
    capture_t *c = (capture_t *) ctx;
    c->n++;
    memcpy( c->last, pkt, len );
    c->last_len = len;
    return 0;
}

static vftp_msg_t s_reply;

/* Encodes `m` and hands it to the burst (or poll) server; returns the number of replies. */
static int to_server( bool burst, const vftp_msg_t *m, capture_t *cap )
{
    uint8_t b[VFTP_CFG_PACKET_MAX];
    uint16_t n = vftp_encode( m, b, sizeof( b ) );
    TEST_ASSERT_TRUE( n != 0u );
    int before = cap->n;
    if ( burst ) {
        (void) vftp_burst_server_handle( &s_burst, b, n, capture, cap );
    } else {
        (void) vftp_poll_server_handle( &s_poll, b, n, capture, cap );
    }
    if ( cap->n != before ) {
        TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_decode( cap->last, cap->last_len, &s_reply ) );
    }
    return cap->n - before;
}

static void up_req( vftp_msg_t *m, const char *path, uint32_t size )
{
    memset( m, 0, sizeof( *m ) );
    m->type = VFTP_MSG_UPLOAD_REQUEST;
    m->u.up_req.size = size;
    m->u.up_req.crc32 = 1;
    m->u.up_req.chunk_size = 100;
    vftp_path_set( m->u.up_req.path, path );
}

/* A packet of another session is dropped by the server: nothing written, no reply, and the open
 * transfer is untouched (a late DONE / ABORT cannot end it). Refusals carry session 0. */
static void test_burst_server_drops_other_sessions( void )
{
    setUp();
    capture_t cap;
    memset( &cap, 0, sizeof( cap ) );
    vftp_msg_t m;
    up_req( &m, "s.bin", 300 );
    TEST_ASSERT_EQUAL_INT( 1, to_server( true, &m, &cap ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_MSG_UPLOAD_REPLY, s_reply.type );
    TEST_ASSERT_EQUAL_UINT8( VFTP_OK, s_reply.u.up_rep.ret );
    uint8_t sess = s_reply.session;
    TEST_ASSERT_TRUE( sess != 0u );
    TEST_ASSERT_EQUAL_INT( 1, to_server( true, &m, &cap ) ); /* the same request again: same session */
    TEST_ASSERT_EQUAL_UINT8( sess, s_reply.session );
    up_req( &m, "t.bin", 300 );
    TEST_ASSERT_EQUAL_INT( 1, to_server( true, &m, &cap ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_BUSY, s_reply.u.up_rep.ret );
    TEST_ASSERT_EQUAL_UINT8( 0u, s_reply.session );

    memset( &m, 0, sizeof( m ) );
    m.type = VFTP_MSG_DATA;
    m.session = (uint8_t) ( sess + 1u );
    m.u.data.chunk = 0;
    m.u.data.bytes = s_buf;
    m.u.data.len = 100;
    TEST_ASSERT_EQUAL_INT( 0, to_server( true, &m, &cap ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, s_burst.complete );
    m.type = VFTP_MSG_STATUS_REQUEST;
    TEST_ASSERT_EQUAL_INT( 0, to_server( true, &m, &cap ) );
    m.type = VFTP_MSG_DONE;
    TEST_ASSERT_EQUAL_INT( 0, to_server( true, &m, &cap ) );
    m.type = VFTP_MSG_ABORT;
    TEST_ASSERT_EQUAL_INT( 0, to_server( true, &m, &cap ) );
    TEST_ASSERT_TRUE( vftp_burst_server_busy( &s_burst ) );

    m.type = VFTP_MSG_DATA;
    m.session = sess;
    TEST_ASSERT_EQUAL_INT( 0, to_server( true, &m, &cap ) ); /* DATA is never answered */
    TEST_ASSERT_EQUAL_UINT32( 1u, s_burst.complete );
    m.type = VFTP_MSG_STATUS_REQUEST;
    TEST_ASSERT_EQUAL_INT( 1, to_server( true, &m, &cap ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_MSG_STATUS_REPLY, s_reply.type );
    TEST_ASSERT_EQUAL_UINT8( sess, s_reply.session );
    TEST_ASSERT_EQUAL_UINT32( 1u, s_reply.u.status.complete );
    vftp_burst_server_abort( &s_burst );

    /* the next transfer gets a new session, so the old one's late packets can never match it */
    up_req( &m, "s.bin", 300 );
    TEST_ASSERT_EQUAL_INT( 1, to_server( true, &m, &cap ) );
    TEST_ASSERT_TRUE( s_reply.session != 0u && s_reply.session != sess );
    vftp_burst_server_abort( &s_burst );
}

static void test_poll_server_drops_other_sessions( void )
{
    setUp();
    remote_file( "p.bin", 500, 3 );
    capture_t cap;
    memset( &cap, 0, sizeof( cap ) );
    vftp_msg_t m;
    memset( &m, 0, sizeof( m ) );
    m.type = VFTP_MSG_DOWNLOAD_REQUEST;
    m.u.down_req.chunk_size = 100;
    vftp_path_set( m.u.down_req.path, "p.bin" );
    TEST_ASSERT_EQUAL_INT( 1, to_server( false, &m, &cap ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_OK, s_reply.u.down_rep.ret );
    uint8_t sess = s_reply.session;
    TEST_ASSERT_TRUE( sess != 0u );
    memset( &m, 0, sizeof( m ) );
    m.type = VFTP_MSG_GET_CHUNK;
    m.session = (uint8_t) ( sess ^ 0x80u );
    m.u.get_chunk.chunk = 2;
    TEST_ASSERT_EQUAL_INT( 0, to_server( false, &m, &cap ) );
    m.type = VFTP_MSG_DONE;
    TEST_ASSERT_EQUAL_INT( 0, to_server( false, &m, &cap ) );
    TEST_ASSERT_TRUE( vftp_poll_server_busy( &s_poll ) );
    m.type = VFTP_MSG_GET_CHUNK;
    m.session = sess;
    TEST_ASSERT_EQUAL_INT( 1, to_server( false, &m, &cap ) );
    TEST_ASSERT_EQUAL_UINT8( VFTP_MSG_DATA, s_reply.type );
    TEST_ASSERT_EQUAL_UINT8( sess, s_reply.session );
    TEST_ASSERT_EQUAL_UINT32( 2u, s_reply.u.data.chunk );
    TEST_ASSERT_EQUAL_UINT16( 100u, s_reply.u.data.len );
    vftp_poll_server_abort( &s_poll );
}

/* A link that slips a late DATA of another session (wrong bytes) in front of every real reply to
 * GET_CHUNK: the client must skip it, or the file ends with a CRC mismatch. */
static vftp_link_t s_inner;
static int s_injected;
static bool s_put_back; /* the next packet is the real reply put back: pass it through */

static int inj_open( void *ctx )
{
    (void) ctx;
    return s_inner.open( s_inner.ctx );
}
static void inj_close( void *ctx )
{
    (void) ctx;
    s_inner.close( s_inner.ctx );
}
static int inj_send( void *ctx, const uint8_t *pkt, uint16_t len )
{
    (void) ctx;
    return s_inner.send( s_inner.ctx, pkt, len );
}
static int inj_recv( void *ctx, uint8_t *buf, uint16_t cap, uint32_t timeout_ms )
{
    (void) ctx;
    int n = s_inner.recv( s_inner.ctx, buf, cap, timeout_ms );
    vftp_msg_t m;
    if ( s_put_back ) {
        s_put_back = false;
        return n;
    }
    if ( n > 0 && vftp_decode( buf, (uint16_t) n, &m ) == VFTP_OK && m.type == VFTP_MSG_DATA ) {
        /* put the real reply back at the queue front, hand out a forged one first */
        if ( s_lb.q_count < VFTP_LOOPBACK_QUEUE ) {
            s_lb.q_head = ( s_lb.q_head + VFTP_LOOPBACK_QUEUE - 1u ) % VFTP_LOOPBACK_QUEUE;
            memcpy( s_lb.q[s_lb.q_head], buf, (size_t) n );
            s_lb.q_len[s_lb.q_head] = (uint16_t) n;
            s_lb.q_count++;
            m.session = (uint8_t) ( m.session + 1u );
            memset( s_buf2, 0xA5, m.u.data.len );
            m.u.data.bytes = s_buf2;
            s_injected++;
            s_put_back = true;
            return (int) vftp_encode( &m, buf, cap );
        }
    }
    return n;
}
static void inj_sleep( void *ctx, uint32_t ms )
{
    (void) ctx;
    if ( s_inner.sleep_ms ) {
        s_inner.sleep_ms( s_inner.ctx, ms );
    }
}

static void test_client_skips_replies_of_other_sessions( void )
{
    setUp();
    use_poll();
    remote_file( "q.bin", 3000, 8 );
    s_inner = s_link;
    vftp_link_t inj = s_link;
    inj.ctx = NULL;
    inj.open = inj_open;
    inj.close = inj_close;
    inj.send = inj_send;
    inj.recv = inj_recv;
    inj.sleep_ms = inj_sleep;
    s_cli.cfg.link = &inj;
    s_injected = 0;
    s_put_back = false;
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_poll_download( &s_cli, "q.bin", "q.local" ) );
    TEST_ASSERT_TRUE( s_injected >= 14 ); /* every chunk of 216 B had a forged twin */
    assert_same( &s_srv_ram, "q.bin", &s_cli_ram, "q.local" );
    s_cli.cfg.link = &s_link;
}

/* ---- listing: entry flags and byte-packed pages -------------------------- */

/* A page is filled by bytes, not by a fixed count: with a 100-byte reply, (100 - 12) / (10 + 3)
 * = 6 entries of 3-character names. */
static void test_list_reply_is_packed_by_bytes( void )
{
    setUp();
    servers_init( 100, MTU_UP );
    for ( int i = 0; i < 15; i++ ) {
        char p[16];
        snprintf( p, sizeof( p ), "pk/a%02d", i );
        remote_file( p, 1, (uint32_t) i );
    }
    capture_t cap;
    memset( &cap, 0, sizeof( cap ) );
    vftp_msg_t m;
    memset( &m, 0, sizeof( m ) );
    m.type = VFTP_MSG_LIST_REQUEST;
    vftp_path_set( m.u.list_req.path, "pk" );
    TEST_ASSERT_EQUAL_INT( 1, to_server( true, &m, &cap ) );
    TEST_ASSERT_EQUAL_UINT16( 6u, s_reply.u.list_rep.packet_files );
    TEST_ASSERT_EQUAL_UINT32( 15u, s_reply.u.list_rep.total_files );
    TEST_ASSERT_EQUAL_UINT16( VFTP_LIST_HEADER + 6u * ( VFTP_LIST_ENTRY_FIXED + 3u ), cap.last_len );
    m.u.list_req.limit = 4;
    TEST_ASSERT_EQUAL_INT( 1, to_server( true, &m, &cap ) );
    TEST_ASSERT_EQUAL_UINT16( 4u, s_reply.u.list_rep.packet_files );
    m.u.list_req.limit = 0;
    m.u.list_req.offset = 12;
    TEST_ASSERT_EQUAL_INT( 1, to_server( true, &m, &cap ) );
    TEST_ASSERT_EQUAL_UINT16( 3u, s_reply.u.list_rep.packet_files );
    TEST_ASSERT_EQUAL_UINT32( 12u, s_reply.u.list_rep.first_file );
    TEST_ASSERT_EQUAL_UINT32( 15u, s_reply.u.list_rep.total_files );
    vftp_burst_server_abort( &s_burst );
}

static void posix_put( const char *dir, const char *name, const char *text )
{
    char p[512];
    snprintf( p, sizeof( p ), "%s/%s", dir, name );
    FILE *f = fopen( p, "w" );
    TEST_ASSERT_NOT_NULL( f );
    fputs( text, f );
    fclose( f );
}

typedef struct {
    int n;
    char name[8][VFTP_MAX_FILE_NAME + 1u];
    uint8_t flags[8];
    uint32_t mtime[8];
} flag_seen_t;

static void on_flags( void *ctx, uint32_t total, uint32_t index, const vftp_list_entry_t *e )
{
    flag_seen_t *s = (flag_seen_t *) ctx;
    (void) total;
    if ( index < 8u ) {
        memcpy( s->name[index], e->name, sizeof( e->name ) );
        s->flags[index] = e->flags;
        s->mtime[index] = e->mtime;
        s->n++;
    }
}

/* A directory is flagged (shown "NAME/"), a name longer than 13 is sent cut and flagged, and
 * download_index refuses a cut name instead of fetching a wrong path. */
static void test_list_flags_dirs_and_cut_names( void )
{
    char sdir[] = "/tmp/vftp_flg_XXXXXX";
    TEST_ASSERT_NOT_NULL( mkdtemp( sdir ) );
    static vftp_posix_fs_t sctx;
    vftp_storage_posix_init( &s_srv_fs, &sctx, sdir );
    servers_init( MTU_DOWN, MTU_UP );
    link_init( true, MTU_UP, MTU_DOWN );
    char p[600];
    snprintf( p, sizeof( p ), "mkdir -p %s/D/SUB && touch -d '2026-10-06 14:47:00 UTC' %s/D/SUB", sdir, sdir );
    TEST_ASSERT_EQUAL_INT( 0, system( p ) );
    posix_put( sdir, "D/A.TXT", "hello" );
    posix_put( sdir, "D/a_very_long_file_name.txt", "x" );

    static flag_seen_t seen;
    memset( &seen, 0, sizeof( seen ) );
    uint32_t total = 0;
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_list( &s_cli, "D", on_flags, &seen, &total ) );
    TEST_ASSERT_EQUAL_UINT32( 3u, total );
    int dirs = 0, cuts = 0, files = 0;
    for ( int i = 0; i < seen.n; i++ ) {
        if ( strcmp( seen.name[i], "SUB" ) == 0 ) {
            TEST_ASSERT_EQUAL_UINT8( VFTP_ENTRY_DIR, seen.flags[i] );
            TEST_ASSERT_EQUAL_UINT32( 1791298020u, seen.mtime[i] );
            dirs++;
        } else if ( strcmp( seen.name[i], "A.TXT" ) == 0 ) {
            TEST_ASSERT_EQUAL_UINT8( 0u, seen.flags[i] );
            files++;
        } else {
            TEST_ASSERT_EQUAL_STRING( "a_very_long_f", seen.name[i] );
            TEST_ASSERT_EQUAL_UINT8( VFTP_ENTRY_NAME_CUT, seen.flags[i] );
            cuts++;
        }
    }
    TEST_ASSERT_EQUAL_INT( 1, dirs );
    TEST_ASSERT_EQUAL_INT( 1, files );
    TEST_ASSERT_EQUAL_INT( 1, cuts );

    char out[VFTP_MAX_PATH];
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_list_regen( &s_cli, "D", out ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_download( &s_cli, out, "list.txt" ) );
    uint32_t len = 0;
    const uint8_t *txt = vftp_ramfs_get( &s_cli_ram, "list.txt", &len );
    TEST_ASSERT_NOT_NULL( txt );
    char text[512];
    TEST_ASSERT_TRUE( len < sizeof( text ) );
    memcpy( text, txt, len );
    text[len] = '\0';
    TEST_ASSERT_NOT_NULL( strstr( text, " 2026-10-06_14:47 SUB/\n" ) ); /* UTC */
    TEST_ASSERT_NOT_NULL( strstr( text, " a_very_long_f~\n" ) );
    TEST_ASSERT_NOT_NULL( strstr( text, " A.TXT\n" ) );

    for ( uint32_t i = 0; i < 3u; i++ ) {
        vftp_index_rep_t info;
        TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_index( &s_cli, "D", i, &info ) );
        vftp_ret_t want = VFTP_OK;
        if ( info.flags & VFTP_ENTRY_DIR ) {
            want = VFTP_ISDIR;
        } else if ( info.flags & VFTP_ENTRY_NAME_CUT ) {
            want = VFTP_BADNAME;
        }
        if ( want != VFTP_OK ) {
            TEST_ASSERT_EQUAL_INT( want, vftp_burst_download_index( &s_cli, "D", i, "idx.local", NULL ) );
            use_poll();
            TEST_ASSERT_EQUAL_INT( want, vftp_poll_download_index( &s_cli, "D", i, "idx.local", NULL ) );
            link_init( true, MTU_UP, MTU_DOWN );
        }
    }
    snprintf( p, sizeof( p ), "rm -rf %s", sdir );
    TEST_ASSERT_EQUAL_INT( 0, system( p ) );
}

/* ---- Unix time ----------------------------------------------------------- */

static void test_unix_time_conversions( void )
{
    TEST_ASSERT_EQUAL_UINT32( 0u, vftp_unix_time( 1970, 1, 1, 0, 0, 0 ) );
    TEST_ASSERT_EQUAL_UINT32( 946684800u, vftp_unix_time( 2000, 1, 1, 0, 0, 0 ) );
    TEST_ASSERT_EQUAL_UINT32( 1709164800u, vftp_unix_time( 2024, 2, 29, 0, 0, 0 ) );
    TEST_ASSERT_EQUAL_UINT32( 2147483647u, vftp_unix_time( 2038, 1, 19, 3, 14, 7 ) );
    TEST_ASSERT_EQUAL_UINT32( 4294967295u, vftp_unix_time( 2106, 2, 7, 6, 28, 15 ) ); /* last u32 second */
    TEST_ASSERT_EQUAL_UINT32( 0u, vftp_unix_time( 2106, 2, 7, 6, 28, 16 ) );          /* past it: unknown */
    TEST_ASSERT_EQUAL_UINT32( 0u, vftp_unix_time( 1969, 12, 31, 23, 59, 59 ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, vftp_unix_time( 2026, 13, 1, 0, 0, 0 ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, vftp_unix_time( 2026, 1, 0, 0, 0, 0 ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, vftp_unix_time( 2026, 1, 1, 24, 0, 0 ) );
    /* FAT: date = (year-1980)<<9 | month<<5 | day, time = hour<<11 | min<<5 | sec/2 */
    uint16_t fd = (uint16_t) ( ( ( 2026 - 1980 ) << 9 ) | ( 10 << 5 ) | 6 );
    uint16_t ft = (uint16_t) ( ( 14 << 11 ) | ( 47 << 5 ) | ( 30 / 2 ) );
    TEST_ASSERT_EQUAL_UINT32( 1791298050u, vftp_fat_to_unix( fd, ft ) );
    TEST_ASSERT_EQUAL_UINT32( 0u, vftp_fat_to_unix( 0, 0 ) ); /* month 0: no date */

    vftp_civil_t c;
    static const uint32_t ts[] = { 0u, 59u, 86399u, 951782400u, 1709164800u, 1791298020u, 4294967295u };
    for ( size_t i = 0; i < sizeof( ts ) / sizeof( ts[0] ); i++ ) {
        vftp_unix_to_civil( ts[i], &c );
        TEST_ASSERT_EQUAL_UINT32( ts[i], vftp_unix_time( c.year, c.month, c.day, c.hour, c.minute, c.second ) );
    }
    vftp_unix_to_civil( 1791298020u, &c );
    TEST_ASSERT_EQUAL_UINT16( 2026u, c.year );
    TEST_ASSERT_EQUAL_UINT8( 10u, c.month );
    TEST_ASSERT_EQUAL_UINT8( 6u, c.day );
    TEST_ASSERT_EQUAL_UINT8( 14u, c.hour );
    TEST_ASSERT_EQUAL_UINT8( 47u, c.minute );
    TEST_ASSERT_EQUAL_UINT8( 0u, c.second );
    for ( uint32_t t = 0; t < 0xFFFF0000u; t += 7919u * 3607u ) { /* ~150k days spread over the range */
        vftp_unix_to_civil( t, &c );
        TEST_ASSERT_EQUAL_UINT32( t, vftp_unix_time( c.year, c.month, c.day, c.hour, c.minute, c.second ) );
    }
}

/* ---- poll download resume ------------------------------------------------ */

static bool cli_exists( const char *path )
{
    return vftp_ramfs_exists( &s_cli_ram, path );
}

static void vfp_of( const char *local, char *out )
{
    TEST_ASSERT_TRUE( vftp_sidecar_path( local, "VFP", false, out, VFTP_SIDECAR_PATH_MAX ) );
}

static void test_poll_download_resumes_after_interruption( void )
{
    setUp();
    use_poll();
    remote_file( "pd.bin", 20000, 13 ); /* 93 chunks of 216 B */
    char vfp[VFTP_SIDECAR_PATH_MAX];
    vfp_of( "pd.local", vfp );
    s_lb.die_after_tx = 40;
    TEST_ASSERT_EQUAL_INT( VFTP_LINK, vftp_poll_download( &s_cli, "pd.bin", "pd.local" ) );
    TEST_ASSERT_TRUE( cli_exists( vfp ) );
    link_init( false, MTU_UP, MTU_DOWN );
    memset( &s_ev, 0, sizeof( s_ev ) );
    s_cli.stats.data_rx = 0;
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_poll_download( &s_cli, "pd.bin", "pd.local" ) );
    TEST_ASSERT_EQUAL_INT( 1, s_ev.resumes );
    TEST_ASSERT_TRUE( s_ev.resume_done >= 30u ); /* ~38 chunks came before the link died */
    TEST_ASSERT_EQUAL_UINT32( 93u - s_ev.resume_done, s_cli.stats.data_rx );
    assert_same( &s_srv_ram, "pd.bin", &s_cli_ram, "pd.local" );
    TEST_ASSERT_FALSE( cli_exists( vfp ) );
}

static void test_poll_download_resume_with_new_chunk_size( void )
{
    setUp();
    use_poll();
    remote_file( "pc.bin", 20000, 14 );
    s_lb.die_after_tx = 40;
    TEST_ASSERT_EQUAL_INT( VFTP_LINK, vftp_poll_download( &s_cli, "pc.bin", "pc.local" ) );
    link_init( false, MTU_UP, MTU_DOWN );
    s_cli.cfg.chunk_size = 100;
    memset( &s_ev, 0, sizeof( s_ev ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_poll_download( &s_cli, "pc.bin", "pc.local" ) );
    TEST_ASSERT_EQUAL_INT( 1, s_ev.resumes );
    TEST_ASSERT_TRUE( s_ev.resume_done >= 30u * 216u / 100u );
    assert_same( &s_srv_ram, "pc.bin", &s_cli_ram, "pc.local" );
}

/* The watermark is only trusted while the local file still holds what it claims. */
static void test_poll_download_restarts_if_local_file_shrank( void )
{
    setUp();
    use_poll();
    remote_file( "ps.bin", 20000, 15 );
    s_lb.die_after_tx = 40;
    TEST_ASSERT_EQUAL_INT( VFTP_LINK, vftp_poll_download( &s_cli, "ps.bin", "ps.local" ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_ramfs_put( &s_cli_ram, "ps.local", s_buf, 100 ) );
    link_init( false, MTU_UP, MTU_DOWN );
    memset( &s_ev, 0, sizeof( s_ev ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_poll_download( &s_cli, "ps.bin", "ps.local" ) );
    TEST_ASSERT_EQUAL_INT( 0, s_ev.resumes );
    assert_same( &s_srv_ram, "ps.bin", &s_cli_ram, "ps.local" );
}

/* The remote file changed (same name, same size) between attempts: a large file's CRC is not in
 * the DOWNLOAD_REPLY, so the resume goes ahead -- the end-to-end CRC catches it, the watermark is
 * dropped, and the next run gives a clean copy. */
static void test_poll_download_resume_of_changed_file_fails_then_recovers( void )
{
    setUp();
    use_poll();
    remote_file( "pm.bin", 60000, 16 ); /* > crc_step: CRC computed while serving */
    char vfp[VFTP_SIDECAR_PATH_MAX];
    vfp_of( "pm.local", vfp );
    s_lb.die_after_tx = 60;
    TEST_ASSERT_EQUAL_INT( VFTP_LINK, vftp_poll_download( &s_cli, "pm.bin", "pm.local" ) );
    remote_file( "pm.bin", 60000, 17 );
    link_init( false, MTU_UP, MTU_DOWN );
    memset( &s_ev, 0, sizeof( s_ev ) );
    TEST_ASSERT_EQUAL_INT( VFTP_CRC_MISMATCH, vftp_poll_download( &s_cli, "pm.bin", "pm.local" ) );
    TEST_ASSERT_EQUAL_INT( 1, s_ev.resumes );
    TEST_ASSERT_FALSE( cli_exists( vfp ) );
    link_init( false, MTU_UP, MTU_DOWN );
    memset( &s_ev, 0, sizeof( s_ev ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_poll_download( &s_cli, "pm.bin", "pm.local" ) );
    TEST_ASSERT_EQUAL_INT( 0, s_ev.resumes );
    assert_same( &s_srv_ram, "pm.bin", &s_cli_ram, "pm.local" );
}

/* No room for the .VFP sidecar (the firmware self-test's 4-slot RAM disk hit this): a clear local
 * storage error, the server freed at once, and the same call works once there is room. */
static void test_poll_download_without_room_for_sidecar( void )
{
    setUp();
    use_poll();
    remote_file( "pf.bin", 3000, 18 );
    s_lb.silent_close = true;
    char p[16];
    for ( unsigned i = 0; i < VFTP_RAMFS_MAX_FILES - 1u; i++ ) {
        snprintf( p, sizeof( p ), "fill%u", i );
        TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_ramfs_put( &s_cli_ram, p, s_buf, 1 ) );
    }
    TEST_ASSERT_EQUAL_INT( VFTP_NOSPC, vftp_poll_download( &s_cli, "pf.bin", "pf.local" ) );
    TEST_ASSERT_FALSE( vftp_poll_server_busy( &s_poll ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, s_cli_fs.remove( s_cli_fs.ctx, "fill0" ) );
    link_init( false, MTU_UP, MTU_DOWN );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_poll_download( &s_cli, "pf.bin", "pf.local" ) );
    assert_same( &s_srv_ram, "pf.bin", &s_cli_ram, "pf.local" );
}

/* A download whose local side fails after the server opened the session must not leave the server
 * serving a client that is gone: it waited out its idle timeout (60 s on the OBC) and every request
 * in between timed out (bench, 2026-10-07: "fburst download R0001000 ./"). */
static void test_burst_download_local_failure_frees_the_server( void )
{
    setUp();
    remote_file( "bf.bin", 3000, 19 );
    s_lb.silent_close = true; /* the bench case: no RDP, the server never sees the close */
    char p[16];
    for ( unsigned i = 0; i < VFTP_RAMFS_MAX_FILES; i++ ) {
        snprintf( p, sizeof( p ), "fill%u", i );
        TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_ramfs_put( &s_cli_ram, p, s_buf, 1 ) );
    }
    TEST_ASSERT_EQUAL_INT( VFTP_NOSPC, vftp_burst_download( &s_cli, "bf.bin", "bf.local" ) );
    TEST_ASSERT_EQUAL_UINT32( 1u, (uint32_t) s_ev.starts ); /* the server did open the session */
    TEST_ASSERT_FALSE( vftp_burst_server_busy( &s_burst ) );
    /* room for the file but not for its .VFD: fails one step later, same outcome */
    TEST_ASSERT_EQUAL_INT( VFTP_OK, s_cli_fs.remove( s_cli_fs.ctx, "fill0" ) );
    TEST_ASSERT_EQUAL_INT( VFTP_NOSPC, vftp_burst_download( &s_cli, "bf.bin", "bf.local" ) );
    TEST_ASSERT_FALSE( vftp_burst_server_busy( &s_burst ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, s_cli_fs.remove( s_cli_fs.ctx, "fill1" ) );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_download( &s_cli, "bf.bin", "bf.local" ) );
    assert_same( &s_srv_ram, "bf.bin", &s_cli_ram, "bf.local" );
}

/* A local directory as destination ("./", ".") is refused before the server is asked. */
static void test_download_into_a_local_directory_is_refused_up_front( void )
{
    char sdir[] = "/tmp/vftp_srv_XXXXXX";
    char cdir[] = "/tmp/vftp_cli_XXXXXX";
    TEST_ASSERT_NOT_NULL( mkdtemp( sdir ) );
    TEST_ASSERT_NOT_NULL( mkdtemp( cdir ) );
    static vftp_posix_fs_t sctx, cctx;
    vftp_storage_posix_init( &s_srv_fs, &sctx, sdir );
    vftp_storage_posix_init( &s_cli_fs, &cctx, cdir );
    servers_init( MTU_DOWN, MTU_UP );
    link_init( true, MTU_UP, MTU_DOWN );
    local_file_posix( sdir, "r.bin" );
    char p[700];
    snprintf( p, sizeof( p ), "mkdir %s/VT", cdir );
    TEST_ASSERT_EQUAL_INT( 0, system( p ) );

    const char *dirs[] = { "./", ".", "VT", "VT/" };
    for ( unsigned i = 0; i < sizeof( dirs ) / sizeof( dirs[0] ); i++ ) {
        link_init( true, MTU_UP, MTU_DOWN );
        TEST_ASSERT_EQUAL_INT_MESSAGE( VFTP_ISDIR, vftp_burst_download( &s_cli, "r.bin", dirs[i] ), dirs[i] );
        TEST_ASSERT_EQUAL_UINT32_MESSAGE( 0u, s_lb.up_sent, dirs[i] );
        use_poll();
        TEST_ASSERT_EQUAL_INT_MESSAGE( VFTP_ISDIR, vftp_poll_download( &s_cli, "r.bin", dirs[i] ), dirs[i] );
        TEST_ASSERT_EQUAL_UINT32_MESSAGE( 0u, s_lb.up_sent, dirs[i] );
    }
    link_init( true, MTU_UP, MTU_DOWN );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_download( &s_cli, "r.bin", "VT/r.bin" ) );
    snprintf( p, sizeof( p ), "cmp %s/r.bin %s/VT/r.bin && rm -rf %s %s", sdir, cdir, sdir, cdir );
    TEST_ASSERT_EQUAL_INT( 0, system( p ) );
}

static vftp_ret_t name_ok_83( void *ctx, const char *path )
{
    (void) ctx;
    return vftp_path_is_83( path ) ? VFTP_OK : VFTP_BADNAME;
}

static void expect_badname( vftp_ret_t st, const char *what )
{
    TEST_ASSERT_EQUAL_INT_MESSAGE( VFTP_REMOTE, st, what );
    TEST_ASSERT_EQUAL_UINT8_MESSAGE( VFTP_BADNAME, s_cli.remote_ret, what );
}

static void test_every_client_path_is_checked_for_8_3( void )
{
    setUp();
    s_srv_fs.name_ok = name_ok_83;
    remote_file( "X.ABC", 2222, 41 );
    remote_file( "ABCDEFGH.TXT", 1111, 42 );
    vftp_index_rep_t info;
    char out[VFTP_MAX_PATH];

    expect_badname( vftp_burst_download( &s_cli, "X.ABCD", "x.local" ), "burst download, 4-char extension" );
    expect_badname( vftp_burst_download( &s_cli, "ABCDEFGHI.TXT", "x.local" ), "burst download, 9-char base" );
    expect_badname( vftp_burst_remove( &s_cli, "X.ABCD" ), "rm" );
    uint32_t kept = 0;
    TEST_ASSERT_NOT_NULL_MESSAGE( vftp_ramfs_get( &s_srv_ram, "X.ABC", &kept ), "rm of X.ABCD touched X.ABC" );
    TEST_ASSERT_EQUAL_UINT32( 2222u, kept );
    expect_badname( vftp_burst_list( &s_cli, "TOOLONGDIR", on_entry, NULL, NULL ), "ls" );
    expect_badname( vftp_burst_index( &s_cli, "TOOLONGDIR", 0u, &info ), "index" );
    expect_badname( vftp_burst_list_regen( &s_cli, "TOOLONGDIR", out ), "list_regen" );
    local_file( "u.bin", 300, 43 );
    expect_badname( vftp_burst_upload( &s_cli, "u.bin", "X.ABCD" ), "upload (checked before)" );
    use_poll();
    expect_badname( vftp_poll_download( &s_cli, "ABCDEFGHI.TXT", "x.local" ), "poll download" );
    expect_badname( vftp_poll_index( &s_cli, "TOOLONGDIR", 0u, &info ), "poll index" );

    /* the exact names still work, on both services */
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_poll_download( &s_cli, "ABCDEFGH.TXT", "a.local" ) );
    assert_same( &s_srv_ram, "ABCDEFGH.TXT", &s_cli_ram, "a.local" );
    link_init( true, MTU_UP, MTU_DOWN );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_download( &s_cli, "X.ABC", "x.local" ) );
    assert_same( &s_srv_ram, "X.ABC", &s_cli_ram, "x.local" );
    TEST_ASSERT_EQUAL_INT( VFTP_OK, vftp_burst_remove( &s_cli, "X.ABC" ) );
    s_srv_fs.name_ok = NULL;
}

int main( void )
{
    UNITY_BEGIN();
    RUN_TEST( test_crc32_reference_vector );
    RUN_TEST( test_wire_golden_bytes );
    RUN_TEST( test_wire_min_length_of_every_type );
    RUN_TEST( test_wire_roundtrip_every_type );
    RUN_TEST( test_wire_rejects_malformed );
    RUN_TEST( test_fuzz_servers_survive_garbage );
    RUN_TEST( test_burst_sizes_without_loss );
    RUN_TEST( test_burst_sizes_with_10_percent_loss );
    RUN_TEST( test_burst_30_percent_loss_reorder_and_duplicates );
    RUN_TEST( test_burst_window_overflowing_receive_queue );
    RUN_TEST( test_burst_big_mtu_link );
    RUN_TEST( test_burst_upload_resumes_after_link_loss_and_server_reboot );
    RUN_TEST( test_burst_upload_staging_bounds );
    RUN_TEST( test_burst_download_resumes_with_local_sidecar );
    RUN_TEST( test_burst_download_resume_ignores_stale_sidecar );
    RUN_TEST( test_burst_download_resume_needs_the_local_file );
    RUN_TEST( test_every_client_path_is_checked_for_8_3 );
    RUN_TEST( test_burst_upload_replaces_longer_file );
    RUN_TEST( test_burst_upload_of_empty_file_creates_it );
    RUN_TEST( test_burst_write_behind_batches_storage_writes );
    RUN_TEST( test_burst_upload_window_up );
    RUN_TEST( test_burst_survives_transient_storage_failure );
    RUN_TEST( test_persistent_storage_failure_is_reported );
    RUN_TEST( test_upload_with_bad_name_is_refused_up_front );
    RUN_TEST( test_burst_busy_idempotent_and_abort );
    RUN_TEST( test_burst_malformed_data_is_ignored );
    RUN_TEST( test_end_to_end_crc_catches_corruption );
    RUN_TEST( test_large_file_crc_in_steps );
    RUN_TEST( test_rechunk_matches_byte_reference );
    RUN_TEST( test_burst_upload_resume_with_new_chunk_size );
    RUN_TEST( test_burst_download_resume_with_new_chunk_size );
    RUN_TEST( test_poll_upload_resume_with_new_chunk_size );
    RUN_TEST( test_list_pages_through_a_large_directory );
    RUN_TEST( test_list_of_missing_directory_is_an_error );
    RUN_TEST( test_list_regen_then_download_list_txt );
    RUN_TEST( test_list_regen_of_root_is_atomic );
    RUN_TEST( test_list_ends_conversation_after_last_page );
    RUN_TEST( test_remove_and_remote_error_codes );
    RUN_TEST( test_index_count_and_download_burst );
    RUN_TEST( test_index_count_and_download_poll );
    RUN_TEST( test_list_walks_once_per_page );
    RUN_TEST( test_listing_without_walk_port );
    RUN_TEST( test_poll_sizes_and_loss );
    RUN_TEST( test_poll_upload_resumes_after_link_loss_and_server_reboot );
    RUN_TEST( test_sidecar_names_are_8_3 );
    RUN_TEST( test_8_3_name_check );
    RUN_TEST( test_ccsds_space_packet_header );
    RUN_TEST( test_posix_storage_end_to_end );
    RUN_TEST( test_posix_directories_and_big_list_txt );
    RUN_TEST( test_burst_server_drops_other_sessions );
    RUN_TEST( test_poll_server_drops_other_sessions );
    RUN_TEST( test_client_skips_replies_of_other_sessions );
    RUN_TEST( test_list_reply_is_packed_by_bytes );
    RUN_TEST( test_list_flags_dirs_and_cut_names );
    RUN_TEST( test_unix_time_conversions );
    RUN_TEST( test_poll_download_resumes_after_interruption );
    RUN_TEST( test_poll_download_resume_with_new_chunk_size );
    RUN_TEST( test_poll_download_restarts_if_local_file_shrank );
    RUN_TEST( test_poll_download_resume_of_changed_file_fails_then_recovers );
    RUN_TEST( test_poll_download_without_room_for_sidecar );
    RUN_TEST( test_burst_download_local_failure_frees_the_server );
    RUN_TEST( test_download_into_a_local_directory_is_refused_up_front );
    return UNITY_END();
}
