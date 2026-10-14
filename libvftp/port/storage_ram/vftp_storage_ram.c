/* vftp_storage_t in RAM -- see the header. */
#include "vftp_storage_ram.h"

#include <string.h>

static const char *norm( const char *p )
{
    for ( ;; ) {
        if ( p[0] == '/' ) {
            p++;
        } else if ( p[0] == '.' && p[1] == '/' ) {
            p += 2;
        } else {
            return p;
        }
    }
}

/* Length of `p`'s directory part ("" -> 0). */
static size_t dir_len( const char *p )
{
    const char *slash = strrchr( p, '/' );
    return slash ? (size_t) ( slash - p ) : 0u;
}

static vftp_ramfs_file_t *find( vftp_ramfs_t *fs, const char *path )
{
    const char *p = norm( path );
    for ( uint32_t i = 0; i < VFTP_RAMFS_MAX_FILES; i++ ) {
        if ( fs->file[i].used && strcmp( fs->file[i].path, p ) == 0 ) {
            return &fs->file[i];
        }
    }
    return NULL;
}

static uint8_t *data_of( vftp_ramfs_t *fs, const vftp_ramfs_file_t *f )
{
    return fs->pool + (uint32_t) ( f - fs->file ) * fs->slot_cap;
}

static vftp_ramfs_file_t *create( vftp_ramfs_t *fs, const char *path )
{
    const char *p = norm( path );
    if ( p[0] == '\0' || strlen( p ) >= sizeof( fs->file[0].path ) ) {
        return NULL;
    }
    for ( uint32_t i = 0; i < VFTP_RAMFS_MAX_FILES; i++ ) {
        if ( !fs->file[i].used ) {
            fs->file[i].used = true;
            fs->file[i].size = 0;
            memcpy( fs->file[i].path, p, strlen( p ) + 1u );
            return &fs->file[i];
        }
    }
    return NULL;
}

static vftp_ret_t r_stat( void *ctx, const char *path, uint32_t *size )
{
    vftp_ramfs_file_t *f = find( (vftp_ramfs_t *) ctx, path );
    if ( f == NULL ) {
        return VFTP_NOENT;
    }
    *size = f->size;
    return VFTP_OK;
}

static vftp_ret_t r_read( void *ctx, const char *path, uint32_t off, uint8_t *buf, uint16_t max, uint16_t *got )
{
    vftp_ramfs_t *fs = (vftp_ramfs_t *) ctx;
    vftp_ramfs_file_t *f = find( fs, path );
    fs->n_reads++;
    if ( f == NULL ) {
        return VFTP_NOENT;
    }
    uint32_t n = off >= f->size ? 0u : f->size - off;
    if ( n > max ) {
        n = max;
    }
    memcpy( buf, data_of( fs, f ) + off, n );
    *got = (uint16_t) n;
    return VFTP_OK;
}

static vftp_ret_t r_write( void *ctx, const char *path, uint32_t off, const uint8_t *data, uint16_t len )
{
    vftp_ramfs_t *fs = (vftp_ramfs_t *) ctx;
    fs->n_writes++;
    if ( ( fs->fail_write_at != 0u && fs->n_writes == fs->fail_write_at ) ||
         ( fs->fail_write_from != 0u && fs->n_writes >= fs->fail_write_from ) ) {
        return VFTP_IO;
    }
    vftp_ramfs_file_t *f = find( fs, path );
    if ( f == NULL ) {
        f = create( fs, path );
    }
    if ( f == NULL ) {
        return VFTP_NOSPC;
    }
    if ( (uint64_t) off + len > fs->slot_cap ) {
        return VFTP_NOSPC;
    }
    uint8_t *d = data_of( fs, f );
    if ( off > f->size ) {
        memset( d + f->size, 0, off - f->size ); /* a hole reads as zeros, like a sparse POSIX file */
    }
    memcpy( d + off, data, len );
    if ( off + len > f->size ) {
        f->size = off + len;
    }
    return VFTP_OK;
}

static vftp_ret_t r_truncate( void *ctx, const char *path )
{
    vftp_ramfs_t *fs = (vftp_ramfs_t *) ctx;
    vftp_ramfs_file_t *f = find( fs, path );
    if ( f == NULL ) {
        f = create( fs, path );
    }
    if ( f == NULL ) {
        return VFTP_NOSPC;
    }
    f->size = 0;
    return VFTP_OK;
}

static vftp_ret_t r_remove( void *ctx, const char *path )
{
    vftp_ramfs_file_t *f = find( (vftp_ramfs_t *) ctx, path );
    if ( f == NULL ) {
        return VFTP_NOENT;
    }
    f->used = false;
    return VFTP_OK;
}

static vftp_ret_t r_rename( void *ctx, const char *from, const char *to )
{
    vftp_ramfs_t *fs = (vftp_ramfs_t *) ctx;
    vftp_ramfs_file_t *f = find( fs, from );
    const char *p = norm( to );
    if ( f == NULL ) {
        return VFTP_NOENT;
    }
    if ( p[0] == '\0' || strlen( p ) >= sizeof( f->path ) ) {
        return VFTP_INVAL;
    }
    vftp_ramfs_file_t *old = find( fs, to );
    if ( old != NULL && old != f ) {
        old->used = false;
    }
    memmove( f->path, p, strlen( p ) + 1u );
    return VFTP_OK;
}

static vftp_ret_t r_name_ok( void *ctx, const char *path )
{
    const vftp_ramfs_t *fs = (const vftp_ramfs_t *) ctx;
    return ( !fs->fat83 || vftp_path_is_83( path ) ) ? VFTP_OK : VFTP_BADNAME;
}

/* Entries of `dir` in slot order: files directly in it, then nothing else (no subdirectories). */
static vftp_ret_t ram_each( vftp_ramfs_t *fs, const char *dir, uint32_t first, vftp_walk_fn fn, void *arg )
{
    const char *d = norm( dir );
    size_t dl = strlen( d );
    while ( dl > 0u && d[dl - 1u] == '/' ) {
        dl--;
    }
    bool exists = dl == 0u;
    uint32_t i = 0;
    for ( uint32_t k = 0; k < VFTP_RAMFS_MAX_FILES; k++ ) {
        const vftp_ramfs_file_t *f = &fs->file[k];
        if ( !f->used || dir_len( f->path ) != dl || strncmp( f->path, d, dl ) != 0 ) {
            continue;
        }
        exists = true;
        uint32_t index = i++;
        if ( index < first ) {
            continue;
        }
        vftp_dirent_t e;
        memset( &e, 0, sizeof( e ) );
        strncpy( e.name, f->path + ( dl == 0u ? 0u : dl + 1u ), sizeof( e.name ) - 1u );
        e.size = f->size;
        if ( !fn( arg, index, &e ) ) {
            break;
        }
    }
    return exists ? VFTP_OK : VFTP_NOENT;
}

typedef struct {
    uint32_t want;
    vftp_dirent_t *out;
    bool found;
} pick_t;

static bool pick( void *arg, uint32_t index, const vftp_dirent_t *e )
{
    pick_t *p = (pick_t *) arg;
    if ( index == p->want ) {
        *p->out = *e;
        p->found = true;
        return false;
    }
    return true;
}

static vftp_ret_t r_dirent( void *ctx, const char *dir, uint32_t index, vftp_dirent_t *out )
{
    vftp_ramfs_t *fs = (vftp_ramfs_t *) ctx;
    pick_t p = { index, out, false };
    fs->n_dirent++;
    (void) ram_each( fs, dir, index, pick, &p );
    return p.found ? VFTP_OK : VFTP_NOENT;
}

static vftp_ret_t r_walk( void *ctx, const char *dir, uint32_t first, vftp_walk_fn fn, void *arg )
{
    vftp_ramfs_t *fs = (vftp_ramfs_t *) ctx;
    fs->n_walk++;
    return ram_each( fs, dir, first, fn, arg );
}

void vftp_storage_ram_init( vftp_storage_t *st, vftp_ramfs_t *fs, uint8_t *pool, uint32_t pool_size )
{
    memset( fs, 0, sizeof( *fs ) );
    fs->pool = pool;
    fs->slot_cap = pool_size / VFTP_RAMFS_MAX_FILES;
    st->stat = r_stat;
    st->read = r_read;
    st->write = r_write;
    st->truncate = r_truncate;
    st->remove = r_remove;
    st->dirent = r_dirent;
    st->walk = r_walk;
    st->rename = r_rename;
    st->name_ok = r_name_ok;
    st->ctx = fs;
}

vftp_ret_t vftp_ramfs_put( vftp_ramfs_t *fs, const char *path, const uint8_t *data, uint32_t len )
{
    vftp_ramfs_file_t *f = find( fs, path );
    if ( f == NULL ) {
        f = create( fs, path );
    }
    if ( f == NULL || len > fs->slot_cap ) {
        return VFTP_NOSPC;
    }
    memcpy( data_of( fs, f ), data, len );
    f->size = len;
    return VFTP_OK;
}

const uint8_t *vftp_ramfs_get( vftp_ramfs_t *fs, const char *path, uint32_t *len )
{
    vftp_ramfs_file_t *f = find( fs, path );
    if ( f == NULL ) {
        return NULL;
    }
    *len = f->size;
    return data_of( fs, f );
}

bool vftp_ramfs_exists( vftp_ramfs_t *fs, const char *path )
{
    return find( fs, path ) != NULL;
}
