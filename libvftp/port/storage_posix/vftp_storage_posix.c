/* vftp_storage_t over POSIX files. Stateless per call
 * open, one read or write, close -- so host runs exercise the same I/O shape
 * the OBC does. */
#define _POSIX_C_SOURCE 200809L

#include "vftp_storage_posix.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static bool join( const vftp_posix_fs_t *fs, const char *path, char *out )
{
    int n;
    if ( fs->root[0] == '\0' ) {
        n = snprintf( out, VFTP_POSIX_PATH_MAX, "%s", path[0] != '\0' ? path : "." );
    } else {
        while ( path[0] == '/' ) {
            path++;
        }
        n = snprintf( out, VFTP_POSIX_PATH_MAX, "%s/%s", fs->root, path );
    }
    return n > 0 && (size_t) n < VFTP_POSIX_PATH_MAX;
}

/* st_mtime is already seconds since 1970 (UTC); outside 1970..2106 it is reported as unknown. */
static uint32_t unix_mtime( const struct stat *st )
{
    return ( st->st_mtime > 0 && (unsigned long long) st->st_mtime <= 0xFFFFFFFFull ) ? (uint32_t) st->st_mtime : 0u;
}

static vftp_ret_t from_errno( int e )
{
    switch ( e ) {
        case ENOENT:
        case ENOTDIR:
            return VFTP_NOENT;
        case ENOSPC:
        case EFBIG:
            return VFTP_NOSPC;
        case EISDIR:
        case EINVAL:
        case ENAMETOOLONG:
            return VFTP_INVAL;
        default:
            return VFTP_IO;
    }
}

static vftp_ret_t p_stat( void *ctx, const char *path, uint32_t *size )
{
    char full[VFTP_POSIX_PATH_MAX];
    struct stat st;
    if ( !join( (vftp_posix_fs_t *) ctx, path, full ) ) {
        return VFTP_INVAL;
    }
    if ( stat( full, &st ) != 0 ) {
        return from_errno( errno );
    }
    if ( !S_ISREG( st.st_mode ) ) {
        return S_ISDIR( st.st_mode ) ? VFTP_ISDIR : VFTP_NOENT;
    }
    if ( (unsigned long long) st.st_size > 0xFFFFFFFFull ) {
        return VFTP_NOSPC; /* vftp sizes are 32-bit */
    }
    *size = (uint32_t) st.st_size;
    return VFTP_OK;
}

static vftp_ret_t p_read( void *ctx, const char *path, uint32_t off, uint8_t *buf, uint16_t max, uint16_t *got )
{
    char full[VFTP_POSIX_PATH_MAX];
    if ( !join( (vftp_posix_fs_t *) ctx, path, full ) ) {
        return VFTP_INVAL;
    }
    FILE *fp = fopen( full, "rb" );
    if ( fp == NULL ) {
        return from_errno( errno );
    }
    vftp_ret_t ret = VFTP_OK;
    *got = 0;
    if ( fseek( fp, (long) off, SEEK_SET ) != 0 ) {
        ret = VFTP_IO;
    } else {
        size_t n = fread( buf, 1, max, fp );
        if ( n < max && ferror( fp ) ) {
            ret = VFTP_IO;
        }
        *got = (uint16_t) n;
    }
    fclose( fp );
    return ret;
}

static vftp_ret_t p_write( void *ctx, const char *path, uint32_t off, const uint8_t *data, uint16_t len )
{
    char full[VFTP_POSIX_PATH_MAX];
    if ( !join( (vftp_posix_fs_t *) ctx, path, full ) ) {
        return VFTP_INVAL;
    }
    FILE *fp = fopen( full, "r+b" ); /* keep what is outside [off, off+len) */
    if ( fp == NULL && errno == ENOENT ) {
        fp = fopen( full, "w+b" );
    }
    if ( fp == NULL ) {
        return from_errno( errno );
    }
    vftp_ret_t ret = VFTP_OK;
    if ( fseek( fp, (long) off, SEEK_SET ) != 0 ) {
        ret = VFTP_IO;
    } else if ( len != 0u && fwrite( data, 1, len, fp ) != (size_t) len ) {
        ret = from_errno( errno );
    }
    if ( fclose( fp ) != 0 && ret == VFTP_OK ) {
        ret = from_errno( errno );
    }
    return ret;
}

static vftp_ret_t p_truncate( void *ctx, const char *path )
{
    char full[VFTP_POSIX_PATH_MAX];
    if ( !join( (vftp_posix_fs_t *) ctx, path, full ) ) {
        return VFTP_INVAL;
    }
    FILE *fp = fopen( full, "wb" );
    if ( fp == NULL ) {
        return from_errno( errno );
    }
    return fclose( fp ) == 0 ? VFTP_OK : VFTP_IO;
}

static vftp_ret_t p_remove( void *ctx, const char *path )
{
    char full[VFTP_POSIX_PATH_MAX];
    if ( !join( (vftp_posix_fs_t *) ctx, path, full ) ) {
        return VFTP_INVAL;
    }
    return remove( full ) == 0 ? VFTP_OK : from_errno( errno );
}

static vftp_ret_t p_rename( void *ctx, const char *from, const char *to )
{
    char a[VFTP_POSIX_PATH_MAX];
    char b[VFTP_POSIX_PATH_MAX];
    if ( !join( (vftp_posix_fs_t *) ctx, from, a ) || !join( (vftp_posix_fs_t *) ctx, to, b ) ) {
        return VFTP_INVAL;
    }
    return rename( a, b ) == 0 ? VFTP_OK : from_errno( errno ); /* atomic, replaces `to` */
}

static vftp_ret_t p_dirent( void *ctx, const char *dir, uint32_t index, vftp_dirent_t *out )
{
    char full[VFTP_POSIX_PATH_MAX];
    if ( !join( (vftp_posix_fs_t *) ctx, dir, full ) ) {
        return VFTP_INVAL;
    }
    DIR *dp = opendir( full );
    if ( dp == NULL ) {
        return from_errno( errno );
    }
    /* readdir order is stable while the directory is unchanged, which is all
     * the index contract asks for. */
    vftp_ret_t ret = VFTP_NOENT;
    uint32_t i = 0;
    struct dirent *ent;
    while ( ( ent = readdir( dp ) ) != NULL ) {
        if ( strcmp( ent->d_name, "." ) == 0 || strcmp( ent->d_name, ".." ) == 0 ) {
            continue;
        }
        if ( i++ != index ) {
            continue;
        }
        char item[VFTP_POSIX_PATH_MAX];
        struct stat st;
        int n = snprintf( item, sizeof( item ), "%s/%s", full, ent->d_name );
        if ( n <= 0 || (size_t) n >= sizeof( item ) || stat( item, &st ) != 0 ) {
            ret = VFTP_IO;
            break;
        }
        memset( out, 0, sizeof( *out ) );
        size_t nl = strlen( ent->d_name );
        if ( nl > sizeof( out->name ) - 1u ) {
            nl = sizeof( out->name ) - 1u; /* longer than a listing carries: cut, and say so */
            out->name_cut = true;
        }
        memcpy( out->name, ent->d_name, nl );
        out->size = S_ISREG( st.st_mode ) ? (uint32_t) st.st_size : 0u;
        out->is_dir = S_ISDIR( st.st_mode );
        out->mtime = unix_mtime( &st );
        ret = VFTP_OK;
        break;
    }
    closedir( dp );
    return ret;
}

static vftp_ret_t p_walk( void *ctx, const char *dir, uint32_t first, vftp_walk_fn fn, void *arg )
{
    char full[VFTP_POSIX_PATH_MAX];
    if ( !join( (vftp_posix_fs_t *) ctx, dir, full ) ) {
        return VFTP_INVAL;
    }
    DIR *dp = opendir( full );
    if ( dp == NULL ) {
        return from_errno( errno );
    }
    uint32_t i = 0;
    struct dirent *ent;
    while ( ( ent = readdir( dp ) ) != NULL ) {
        if ( strcmp( ent->d_name, "." ) == 0 || strcmp( ent->d_name, ".." ) == 0 ) {
            continue;
        }
        uint32_t index = i++;
        if ( index < first ) {
            continue;
        }
        char item[VFTP_POSIX_PATH_MAX];
        struct stat st;
        vftp_dirent_t e;
        memset( &e, 0, sizeof( e ) );
        size_t nl = strlen( ent->d_name );
        if ( nl > sizeof( e.name ) - 1u ) {
            nl = sizeof( e.name ) - 1u;
            e.name_cut = true;
        }
        memcpy( e.name, ent->d_name, nl );
        int n = snprintf( item, sizeof( item ), "%s/%s", full, ent->d_name );
        if ( n > 0 && (size_t) n < sizeof( item ) && stat( item, &st ) == 0 ) {
            e.size = S_ISREG( st.st_mode ) ? (uint32_t) st.st_size : 0u;
            e.is_dir = S_ISDIR( st.st_mode );
            e.mtime = unix_mtime( &st );
        }
        if ( !fn( arg, index, &e ) ) {
            break;
        }
    }
    closedir( dp );
    return VFTP_OK;
}

void vftp_storage_posix_init( vftp_storage_t *st, vftp_posix_fs_t *ctx, const char *root )
{
    snprintf( ctx->root, sizeof( ctx->root ), "%s", root ? root : "" );
    size_t n = strlen( ctx->root );
    while ( n > 1u && ctx->root[n - 1u] == '/' ) {
        ctx->root[--n] = '\0';
    }
    st->stat = p_stat;
    st->read = p_read;
    st->write = p_write;
    st->truncate = p_truncate;
    st->remove = p_remove;
    st->dirent = p_dirent;
    st->walk = p_walk;
    st->rename = p_rename;
    st->name_ok = NULL; /* any name the host accepts */
    st->ctx = ctx;
}
