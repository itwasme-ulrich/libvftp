/* libvftp: helpers shared by the client and the servers (src/common: linked by both sides).
 * Server-only helpers live in src/server/vftp_server_util.c, client-only ones in the client. */
#include <stdio.h>
#include <string.h>

#include "vftp/vftp_crc32.h"
#include "vftp/vftp_storage.h"
#include "vftp/vftp_types.h"
#include "vftp_internal.h"

const char *vftp_strerror( int ret )
{
    switch ( ret ) {
        case VFTP_OK:
            return "ok";
        case VFTP_NOENT:
            return "no such file or directory";
        case VFTP_INVAL:
            return "invalid request or argument";
        case VFTP_NOSPC:
            return "no space / file too large";
        case VFTP_IO:
            return "storage I/O error";
        case VFTP_EXISTS:
            return "resume state mismatch";
        case VFTP_NOTSUP:
            return "not supported";
        case VFTP_NOMEM:
            return "out of memory";
        case VFTP_BUSY:
            return "service busy with another transfer";
        case VFTP_CRC_MISMATCH:
            return "CRC mismatch";
        case VFTP_RANGE:
            return "out of range / incomplete";
        case VFTP_ISDIR:
            return "is a directory, not a file";
        case VFTP_BADNAME:
            return "name not valid on the server's filesystem (8.3: max 8 chars + '.' + 3)";
        case VFTP_AGAIN:
            return "still computing, ask again";
        case VFTP_TIMEOUT:
            return "timeout (no reply / no progress)";
        case VFTP_LINK:
            return "link error";
        case VFTP_PROTO:
            return "protocol error";
        case VFTP_REMOTE:
            return "refused by server";
        default:
            return "unknown error";
    }
}

/* Days since 1970-01-01 of a proleptic Gregorian date, year >= 1970 (H. Hinnant's algorithm,
 * unsigned because no date before the epoch is ever needed). */
static uint32_t days_from_civil( uint32_t y, uint32_t m, uint32_t d )
{
    y -= ( m <= 2u ) ? 1u : 0u;
    uint32_t era = y / 400u;
    uint32_t yoe = y - era * 400u;
    uint32_t mp = ( m > 2u ) ? m - 3u : m + 9u;
    uint32_t doy = ( 153u * mp + 2u ) / 5u + d - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097u + doe - 719468u;
}

uint32_t vftp_unix_time( int year, int month, int day, int hour, int minute, int second )
{
    if ( year < 1970 || year > 2106 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 ||
         minute < 0 || minute > 59 || second < 0 || second > 59 ) {
        return 0u;
    }
    uint64_t t = (uint64_t) days_from_civil( (uint32_t) year, (uint32_t) month, (uint32_t) day ) * 86400u +
                 (uint64_t) hour * 3600u + (uint64_t) minute * 60u + (uint64_t) second;
    return t > 0xFFFFFFFFu ? 0u : (uint32_t) t;
}

uint32_t vftp_fat_to_unix( uint16_t fat_date, uint16_t fat_time )
{
    if ( fat_date == 0u ) {
        return 0u;
    }
    return vftp_unix_time( 1980 + ( fat_date >> 9 ), ( fat_date >> 5 ) & 0x0F, fat_date & 0x1F, fat_time >> 11,
                           ( fat_time >> 5 ) & 0x3F, ( fat_time & 0x1F ) * 2 );
}

void vftp_unix_to_civil( uint32_t t, vftp_civil_t *out )
{
    uint32_t z = t / 86400u + 719468u;
    uint32_t sod = t % 86400u;
    uint32_t era = z / 146097u;
    uint32_t doe = z - era * 146097u;
    uint32_t yoe = ( doe - doe / 1460u + doe / 36524u - doe / 146096u ) / 365u;
    uint32_t doy = doe - ( 365u * yoe + yoe / 4u - yoe / 100u );
    uint32_t mp = ( 5u * doy + 2u ) / 153u;
    uint32_t d = doy - ( 153u * mp + 2u ) / 5u + 1u;
    uint32_t m = mp < 10u ? mp + 3u : mp - 9u;
    uint32_t y = yoe + era * 400u + ( m <= 2u ? 1u : 0u );
    out->year = (uint16_t) y;
    out->month = (uint8_t) m;
    out->day = (uint8_t) d;
    out->hour = (uint8_t) ( sod / 3600u );
    out->minute = (uint8_t) ( ( sod / 60u ) % 60u );
    out->second = (uint8_t) ( sod % 60u );
}

uint32_t vftp_total_chunks( uint32_t size, uint16_t chunk_size )
{
    if ( chunk_size == 0u ) {
        return 0u;
    }
    return size / chunk_size + ( ( size % chunk_size ) != 0u ? 1u : 0u );
}

uint16_t vftp_chunk_len( uint32_t size, uint16_t chunk_size, uint32_t i )
{
    uint32_t off = i * (uint32_t) chunk_size;
    if ( off >= size ) {
        return 0u;
    }
    uint32_t left = size - off;
    return (uint16_t) ( left < chunk_size ? left : chunk_size );
}

uint32_t vftp_bitmap_fix_count( uint8_t *bm, uint32_t nbits )
{
    uint32_t nbytes = ( nbits + 7u ) / 8u;
    if ( ( nbits & 7u ) != 0u ) {
        bm[nbytes - 1u] = (uint8_t) ( bm[nbytes - 1u] & (uint8_t) ( ( 1u << ( nbits & 7u ) ) - 1u ) );
    }
    uint32_t count = 0;
    for ( uint32_t i = 0; i < nbytes; i++ ) {
        uint8_t b = bm[i];
        while ( b != 0u ) {
            b = (uint8_t) ( b & (uint8_t) ( b - 1u ) );
            count++;
        }
    }
    return count;
}

uint16_t vftp_bitmap_missing( const uint8_t *bm, uint32_t total, vftp_range_t *out, uint16_t max, uint32_t budget )
{
    uint16_t n = 0;
    uint32_t i = 0;
    while ( i < total && n < max ) {
        if ( ( i & 7u ) == 0u && bm[i >> 3] == 0xFFu ) {
            i += 8u;
            continue;
        }
        if ( vftp_bit_get( bm, i ) ) {
            i++;
            continue;
        }
        uint32_t start = i;
        while ( i < total && !vftp_bit_get( bm, i ) ) {
            i++;
        }
        uint32_t count = i - start;
        if ( budget != 0u && count > budget ) {
            count = budget;
        }
        out[n].next = start;
        out[n].count = count;
        n++;
        if ( budget != 0u ) {
            budget -= count;
            if ( budget == 0u ) {
                break;
            }
        }
    }
    return n;
}

vftp_ret_t vftp_fs_read_all( const vftp_storage_t *fs, const char *path, uint32_t off, uint8_t *buf, uint32_t len,
                             uint32_t *got )
{
    uint32_t done = 0;
    while ( done < len ) {
        uint32_t piece = len - done;
        if ( piece > VFTP_IO_PIECE ) {
            piece = VFTP_IO_PIECE;
        }
        uint16_t n = 0;
        vftp_ret_t st = fs->read( fs->ctx, path, off + done, buf + done, (uint16_t) piece, &n );
        if ( st != VFTP_OK ) {
            return st;
        }
        done += n;
        if ( n < piece ) {
            break; /* end of file */
        }
    }
    *got = done;
    return VFTP_OK;
}

vftp_ret_t vftp_fs_write_all( const vftp_storage_t *fs, const char *path, uint32_t off, const uint8_t *data,
                              uint32_t len )
{
    uint32_t done = 0;
    while ( done < len ) {
        uint32_t piece = len - done;
        if ( piece > VFTP_IO_PIECE ) {
            piece = VFTP_IO_PIECE;
        }
        vftp_ret_t st = fs->write( fs->ctx, path, off + done, data + done, (uint16_t) piece );
        if ( st != VFTP_OK ) {
            return st;
        }
        done += piece;
    }
    return VFTP_OK;
}

bool vftp_sidecar_path( const char *path, const char *ext3, bool at_root, char *out, size_t cap )
{
    const char *slash = strrchr( path, '/' );
    size_t dir_len = ( slash != NULL && !at_root ) ? (size_t) ( slash - path ) + 1u : 0u;
    uint32_t h = vftp_crc32( path, strlen( path ) );
    if ( dir_len + 8u + 1u + strlen( ext3 ) + 1u > cap ) {
        return false;
    }
    memcpy( out, path, dir_len );
    int n = snprintf( out + dir_len, cap - dir_len, "%08lX.%s", (unsigned long) h, ext3 );
    return n > 0 && (size_t) n < cap - dir_len;
}

vftp_ret_t vftp_rechunk( const vftp_storage_t *fs, const char *path, uint32_t off, uint16_t old_cs, uint32_t old_total,
                         uint16_t new_cs, uint32_t new_total, uint32_t size, uint8_t *new_bm, uint8_t *scratch,
                         uint32_t scratch_len, uint32_t *held )
{
    uint32_t win_first = 0; /* scratch holds old bitmap bytes [win_first, win_first + win_len) */
    uint32_t win_len = 0;
    uint32_t old_bytes = ( old_total + 7u ) / 8u;
    *held = 0;
    memset( new_bm, 0, ( new_total + 7u ) / 8u );
    if ( old_cs == 0u || new_cs == 0u ) {
        return VFTP_INVAL;
    }
    for ( uint32_t j = 0; j < new_total; j++ ) {
        uint32_t a = j * (uint32_t) new_cs;
        uint32_t b = a + new_cs > size ? size : a + new_cs;
        bool all = b > a;
        for ( uint32_t k = a / old_cs; all && k <= ( b - 1u ) / old_cs; k++ ) {
            if ( k >= old_total ) {
                all = false;
                break;
            }
            uint32_t byte = k >> 3;
            if ( byte < win_first || byte >= win_first + win_len ) {
                win_first = byte;
                win_len = old_bytes - byte < scratch_len ? old_bytes - byte : scratch_len;
                uint32_t got = 0;
                vftp_ret_t st = vftp_fs_read_all( fs, path, off + win_first, scratch, win_len, &got );
                if ( st != VFTP_OK || got != win_len ) {
                    return st != VFTP_OK ? st : VFTP_IO;
                }
            }
            all = ( scratch[byte - win_first] & (uint8_t) ( 1u << ( k & 7u ) ) ) != 0u;
        }
        if ( all ) {
            vftp_bit_set( new_bm, j );
            ( *held )++;
        }
    }
    return VFTP_OK;
}

static bool char_83( char ch )
{
    return ( ch >= 'A' && ch <= 'Z' ) || ( ch >= 'a' && ch <= 'z' ) || ( ch >= '0' && ch <= '9' ) ||
           ( ch != '\0' && strchr( "!#$%&'()-@^_`{}~", ch ) != NULL );
}

bool vftp_path_is_83( const char *path )
{
    const char *p = path;
    while ( *p != '\0' ) {
        if ( *p == '/' ) {
            p++;
            continue;
        }
        const char *end = strchr( p, '/' );
        size_t n = end != NULL ? (size_t) ( end - p ) : strlen( p );
        if ( !( ( n == 1u && p[0] == '.' ) || ( n == 2u && p[0] == '.' && p[1] == '.' ) ) ) {
            size_t base = 0;
            while ( base < n && p[base] != '.' ) {
                if ( !char_83( p[base] ) ) {
                    return false;
                }
                base++;
            }
            if ( base == 0u || base > 8u ) {
                return false;
            }
            if ( base < n ) { /* p[base] == '.' */
                size_t ext = n - base - 1u;
                if ( ext == 0u || ext > 3u ) {
                    return false;
                }
                for ( size_t i = base + 1u; i < n; i++ ) {
                    if ( !char_83( p[i] ) ) {
                        return false; /* a second dot included */
                    }
                }
            }
        }
        p += n;
    }
    return true;
}

size_t vftp_str_len( const char *s, size_t max )
{
    size_t n = 0;
    while ( n < max && s[n] != '\0' ) {
        n++;
    }
    return n;
}
