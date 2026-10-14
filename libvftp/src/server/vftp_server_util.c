/* libvftp: helpers only the burst and poll servers use (directory scans, INDEX answers, the
 * incremental file CRC, upload target checks). Part of the server library; a client-only build
 * never compiles it. */
#include <string.h>

#include "vftp/vftp_crc32.h"
#include "vftp_server_internal.h"

typedef struct {
    uint32_t first;
    uint32_t end; /* first + max, saturated */
    vftp_walk_fn fn;
    void *arg;
    uint32_t count;
    bool stop_early;
    bool stopped;
} scan_t;

static bool scan_cb( void *arg, uint32_t index, const vftp_dirent_t *e )
{
    scan_t *sc = (scan_t *) arg;
    sc->count = index + 1u;
    if ( index >= sc->first && index < sc->end && sc->fn != NULL && !sc->fn( sc->arg, index, e ) ) {
        sc->stopped = true;
        return false;
    }
    if ( sc->stop_early && index + 1u >= sc->end ) {
        sc->stopped = true;
        return false;
    }
    return true;
}

vftp_ret_t vftp_dir_scan( const vftp_storage_t *fs, const char *dir, uint32_t first, uint32_t max, vftp_walk_fn fn,
                          void *arg, uint32_t *total, bool stop_early )
{
    vftp_ret_t named = vftp_client_path_ok( fs, dir ); /* list, list_regen and index all scan through here */
    if ( named != VFTP_OK ) {
        *total = 0;
        return named;
    }
    scan_t sc = { first, ( max > UINT32_MAX - first ) ? UINT32_MAX : first + max, fn, arg, 0u, stop_early, false };
    if ( fs->walk != NULL ) {
        vftp_ret_t st = fs->walk( fs->ctx, dir, 0u, scan_cb, &sc );
        if ( st == VFTP_OK && !sc.stopped ) {
            *total = sc.count;
        }
        return st;
    }
    /* Fallback: dirent() per index (may rescan the directory each time on some ports). */
    vftp_dirent_t e;
    vftp_ret_t st = fs->dirent( fs->ctx, dir, 0u, &e );
    if ( st != VFTP_OK ) {
        *total = 0;
        return st;
    }
    uint32_t i = 0;
    do {
        if ( !scan_cb( &sc, i, &e ) ) {
            break;
        }
        i++;
    } while ( fs->dirent( fs->ctx, dir, i, &e ) == VFTP_OK );
    if ( !sc.stopped ) {
        *total = sc.count;
    }
    return VFTP_OK;
}

static bool index_cb( void *arg, uint32_t index, const vftp_dirent_t *e )
{
    vftp_index_rep_t *r = (vftp_index_rep_t *) arg;
    r->index = index;
    r->flags = (uint8_t) ( ( e->is_dir ? VFTP_ENTRY_DIR : 0u ) | ( e->name_cut ? VFTP_ENTRY_NAME_CUT : 0u ) );
    r->size = e->size;
    r->mtime = e->mtime;
    memcpy( r->name, e->name, sizeof( r->name ) );
    r->name[VFTP_MAX_FILE_NAME] = '\0';
    r->ret = VFTP_OK;
    return true;
}

void vftp_index_lookup( const vftp_storage_t *fs, const vftp_index_req_t *q, vftp_index_rep_t *r )
{
    memset( r, 0, sizeof( *r ) );
    r->ret = VFTP_RANGE;
    bool count_only = q->index == VFTP_INDEX_COUNT_ONLY;
    uint32_t total = 0;
    vftp_ret_t st = vftp_dir_scan( fs, q->path, count_only ? 0u : q->index, count_only ? 0u : 1u, index_cb, r,
                                   &total, false );
    r->total = total;
    r->index = q->index;
    if ( st != VFTP_OK && total == 0u ) {
        r->ret = (uint8_t) st; /* NOENT: no such directory (or, on a port without walk(), an empty one) */
    } else if ( count_only ) {
        r->ret = VFTP_OK;
    }
}

void vftp_crcjob_init( vftp_crcjob_t *j, uint32_t size )
{
    j->size = size;
    j->off = 0;
    j->state = VFTP_CRC32_INIT;
    j->done = size == 0u;
    j->value = j->done ? vftp_crc32_final( VFTP_CRC32_INIT ) : 0u;
}

void vftp_crcjob_feed( vftp_crcjob_t *j, uint32_t off, const uint8_t *data, uint32_t len )
{
    if ( j->done || off > j->off || off + len <= j->off ) {
        return; /* a gap before it, or nothing new: the step function will read it later */
    }
    uint32_t skip = j->off - off; /* overlap with what is already in */
    uint32_t n = len - skip;
    if ( n > j->size - j->off ) {
        n = j->size - j->off;
    }
    j->state = vftp_crc32_update( j->state, data + skip, n );
    j->off += n;
    if ( j->off == j->size ) {
        j->value = vftp_crc32_final( j->state );
        j->done = true;
    }
}

vftp_ret_t vftp_crcjob_step( vftp_crcjob_t *j, const vftp_storage_t *fs, const char *path, uint8_t *buf,
                             uint32_t buf_size, uint32_t budget )
{
    if ( budget == 0u ) {
        budget = UINT32_MAX; /* no limit: the whole file now */
    }
    while ( !j->done && budget != 0u ) {
        uint32_t want = j->size - j->off;
        if ( want > buf_size ) {
            want = buf_size;
        }
        if ( want > budget ) {
            want = budget;
        }
        uint32_t got = 0;
        vftp_ret_t st = vftp_fs_read_all( fs, path, j->off, buf, want, &got );
        if ( st != VFTP_OK ) {
            return st;
        }
        if ( got != want || got == 0u ) {
            return VFTP_IO; /* the file is shorter than it was */
        }
        uint32_t before = j->off;
        vftp_crcjob_feed( j, j->off, buf, got );
        if ( j->off == before ) {
            return VFTP_IO; /* no progress: never spin */
        }
        budget -= got;
    }
    return VFTP_OK;
}

vftp_ret_t vftp_client_path_ok( const vftp_storage_t *fs, const char *path )
{
    if ( fs->name_ok == NULL ) {
        return VFTP_OK;
    }
    return fs->name_ok( fs->ctx, path ) == VFTP_OK ? VFTP_OK : VFTP_BADNAME;
}

vftp_ret_t vftp_upload_target_ok( const vftp_storage_t *fs, const char *path )
{
    size_t n = strlen( path );
    if ( n == 0u || path[n - 1u] == '/' ) {
        return VFTP_INVAL;
    }
    vftp_ret_t st = vftp_client_path_ok( fs, path );
    if ( st != VFTP_OK ) {
        return st;
    }
    uint32_t size = 0;
    return fs->stat( fs->ctx, path, &size ) == VFTP_ISDIR ? VFTP_ISDIR : VFTP_OK;
}

bool vftp_str_eq( const char *a, const char *b )
{
    return strcmp( a, b ) == 0;
}

bool vftp_str_ieq( const char *a, const char *b )
{
    for ( ;; ) {
        unsigned char x = (unsigned char) *a++;
        unsigned char y = (unsigned char) *b++;
        if ( x >= 'A' && x <= 'Z' ) {
            x = (unsigned char) ( x - 'A' + 'a' );
        }
        if ( y >= 'A' && y <= 'Z' ) {
            y = (unsigned char) ( y - 'A' + 'a' );
        }
        if ( x != y ) {
            return false;
        }
        if ( x == '\0' ) {
            return true;
        }
    }
}
