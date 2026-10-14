/* libvftp private helpers shared by the client and the servers -- not installed, not part of the API.
 * Server-only helpers: src/server/vftp_server_internal.h. */
#ifndef VFTP_INTERNAL_H
#define VFTP_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "vftp/vftp_config.h"
#include "vftp/vftp_crc32.h"
#include "vftp/vftp_storage.h"
#include "vftp/vftp_types.h"
#include "vftp/vftp_wire.h"

/* Largest single storage read/write the library issues (fits uint16_t len). */
#define VFTP_IO_PIECE 32768u

static inline void vftp_put_u16( uint8_t *p, uint16_t v )
{
    p[0] = (uint8_t) ( v & 0xFFu );
    p[1] = (uint8_t) ( v >> 8 );
}

static inline void vftp_put_u32( uint8_t *p, uint32_t v )
{
    p[0] = (uint8_t) ( v & 0xFFu );
    p[1] = (uint8_t) ( ( v >> 8 ) & 0xFFu );
    p[2] = (uint8_t) ( ( v >> 16 ) & 0xFFu );
    p[3] = (uint8_t) ( v >> 24 );
}

static inline uint16_t vftp_get_u16( const uint8_t *p )
{
    return (uint16_t) ( (unsigned int) p[0] | ( (unsigned int) p[1] << 8 ) );
}

static inline uint32_t vftp_get_u32( const uint8_t *p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 ) | ( (uint32_t) p[3] << 24 );
}

static inline bool vftp_bit_get( const uint8_t *bm, uint32_t i )
{
    return ( bm[i >> 3] & (uint8_t) ( 1u << ( i & 7u ) ) ) != 0u;
}

static inline void vftp_bit_set( uint8_t *bm, uint32_t i )
{
    bm[i >> 3] = (uint8_t) ( bm[i >> 3] | (uint8_t) ( 1u << ( i & 7u ) ) );
}

static inline void vftp_bit_clear( uint8_t *bm, uint32_t i )
{
    bm[i >> 3] = (uint8_t) ( bm[i >> 3] & (uint8_t) ~(uint8_t) ( 1u << ( i & 7u ) ) );
}

/* Number of chunks a file of `size` bytes splits into (0 for an empty file). */
uint32_t vftp_total_chunks( uint32_t size, uint16_t chunk_size );
/* Length of chunk `i` (chunk_size for all but a short last one). */
uint16_t vftp_chunk_len( uint32_t size, uint16_t chunk_size, uint32_t i );
/* Set bits among the first `nbits` of `bm` (bits past nbits are cleared first). */
uint32_t vftp_bitmap_fix_count( uint8_t *bm, uint32_t nbits );
/* Up to `max` missing-chunk {next,count} ranges of `bm` (bits 0..total-1), lowest first,
 * covering at most `budget` chunks in total (0 = no budget). Returns the range count. */
uint16_t vftp_bitmap_missing( const uint8_t *bm, uint32_t total, vftp_range_t *out, uint16_t max, uint32_t budget );

/* Whole-range storage I/O in VFTP_IO_PIECE pieces. */
vftp_ret_t vftp_fs_read_all( const vftp_storage_t *fs, const char *path, uint32_t off, uint8_t *buf, uint32_t len,
                             uint32_t *got );
vftp_ret_t vftp_fs_write_all( const vftp_storage_t *fs, const char *path, uint32_t off, const uint8_t *data,
                              uint32_t len );

/* Resume across a chunk_size change: chunk j of the new chunking is held if every old chunk that
 * overlaps its bytes was held. The old bitmap (old_total bits) is streamed from `path` at `off`
 * through `scratch`; the new one (new_total bits) is written to new_bm. Returns new chunks held. */
vftp_ret_t vftp_rechunk( const vftp_storage_t *fs, const char *path, uint32_t off, uint16_t old_cs, uint32_t old_total,
                         uint16_t new_cs, uint32_t new_total, uint32_t size, uint8_t *new_bm, uint8_t *scratch,
                         uint32_t scratch_len, uint32_t *held );

size_t vftp_str_len( const char *s, size_t max );

#endif /* VFTP_INTERNAL_H */
