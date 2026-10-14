/**
  ******************************************************************************
  * @file    middleware/vftp/vftp_storage_fsio.c
  * @brief   vftp_storage_t over FsIo_
  ******************************************************************************
  */
#include "vftp_storage_fsio.h"

#include <string.h>

#include "fsio.h"

static vftp_ret_t ToVftp( FsIoStatus_t st ) {
    switch( st ) {
        case FSIO_OK:
            return VFTP_OK;
        case FSIO_END:
        case FSIO_ERR_NOTFOUND:
            return VFTP_NOENT;
        case FSIO_ERR_ARG:
            return VFTP_INVAL;
        case FSIO_ERR_BUSY:
            return VFTP_BUSY;
        case FSIO_ERR_BADNAME:
            return VFTP_BADNAME;
        case FSIO_ERR_NOCARD:
        case FSIO_ERR_NOFS:
        case FSIO_ERR_IO:
        default:
            return VFTP_IO;
    }
}

static vftp_ret_t FsStat( void *ctx, const char *path, uint32_t *size ) {
    uint8_t dummy[1];
    uint16_t got = 0;
    ( void ) ctx;
    vftp_ret_t st = ToVftp( FsIo_Read( path, 0U, dummy, 0U, &got, size ) ); /* max 0: size only */
    if( st == VFTP_OK && *size == 0U ) {
        // Directories come back as 0-byte "files". A path FsIo_Entry can chdir into is one.
        FsIoEntry_t e;
        FsIoStatus_t d = FsIo_Entry( path, 0U, &e );
        if( d == FSIO_OK || d == FSIO_END ) {
            return VFTP_ISDIR;
        }
    }
    return st;
}

static vftp_ret_t FsRead( void *ctx, const char *path, uint32_t off, uint8_t *buf, uint16_t max, uint16_t *got ) {
    uint32_t size = 0;
    ( void ) ctx;
    return ToVftp( FsIo_Read( path, off, buf, max, got, &size ) );
}

static vftp_ret_t FsWrite( void *ctx, const char *path, uint32_t off, const uint8_t *data, uint16_t len ) {
    ( void ) ctx;
    return ToVftp( FsIo_WriteAt( path, off, data, len ) );
}

static vftp_ret_t FsTruncate( void *ctx, const char *path ) {
    ( void ) ctx;
    return ToVftp( FsIo_Write( path, false, NULL, 0U ) ); /* create-or-truncate */
}

static vftp_ret_t FsRemove( void *ctx, const char *path ) {
    ( void ) ctx;
    return ToVftp( FsIo_Delete( path ) );
}

static vftp_ret_t FsRename( void *ctx, const char *from, const char *to ) {
    ( void ) ctx;
    return ToVftp( FsIo_Rename( from, to ) ); /* LISTTMP.TXT -> LIST.TXT, both at root: native f_rename */
}

static vftp_ret_t FsNameOk( void *ctx, const char *path ) {
    ( void ) ctx;
    return vftp_path_is_83( path ) ? VFTP_OK : VFTP_BADNAME;
}

static bool IsDot( const char *n ) {
    return ( n[0] == '.' && n[1] == '\0' ) || ( n[0] == '.' && n[1] == '.' && n[2] == '\0' );
}

static vftp_ret_t FsDirent( void *ctx, const char *dir, uint32_t index, vftp_dirent_t *out ) {
    FsIoEntry_t e;
    uint32_t logical = 0;
    ( void ) ctx;
    for( uint32_t phys = 0; phys <= 0xFFFFU; phys++ ) {
        FsIoStatus_t st = FsIo_Entry( dir, ( uint16_t ) phys, &e );
        if( st != FSIO_OK ) {
            return ToVftp( st );
        }
        if( IsDot( e.name ) ) {
            continue;
        }
        if( logical++ == index ) {
            memset( out, 0, sizeof( *out ) );
            strncpy( out->name, e.name, sizeof( out->name ) - 1U );
            out->name_cut = strlen( e.name ) >= sizeof( out->name );
            out->size = e.isDir ? 0U : e.size;
            out->is_dir = e.isDir;
            out->mtime = 0U;
            return VFTP_OK;
        }
    }
    return VFTP_NOENT;
}

typedef struct {
    uint32_t first;
    uint32_t logical;
    vftp_walk_fn fn;
    void *arg;
} WalkCtx_t;

static bool WalkOne( void *arg, const FsIoEntry_t *e ) {
    WalkCtx_t *w = ( WalkCtx_t * ) arg;
    if( IsDot( e->name ) ) {
        return true;
    }
    uint32_t index = w->logical++;
    if( index < w->first ) {
        return true;
    }
    vftp_dirent_t d;
    memset( &d, 0, sizeof( d ) );
    strncpy( d.name, e->name, sizeof( d.name ) - 1U );
    d.name_cut = strlen( e->name ) >= sizeof( d.name ); /* FsIo keeps 15 characters, vftp 13 (8.3 fits) */
    d.size = e->isDir ? 0U : e->size;
    d.is_dir = e->isDir;
    return w->fn( w->arg, index, &d );
}

static vftp_ret_t FsWalk( void *ctx, const char *dir, uint32_t first, vftp_walk_fn fn, void *arg ) {
    WalkCtx_t w = { first, 0U, fn, arg };
    ( void ) ctx;
    return ToVftp( FsIo_Walk( dir, WalkOne, &w ) );
}

static const vftp_storage_t s_fsio = { FsStat,   FsRead, FsWrite,  FsTruncate, FsRemove,
                                       FsDirent, FsWalk, FsRename, FsNameOk,   NULL };

const vftp_storage_t *VftpStorageFsio_Get( void ) {
    return &s_fsio;
}
