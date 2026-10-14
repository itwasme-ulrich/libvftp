/**
 * @file vftp_storage.h
 * @brief What the library needs from a filesystem: six path-based calls.
 *
 * Contract every port must honour:
 * - read/write never touch bytes outside [off, off+len); write creates a
 *   missing file and leaves the rest of an existing one untouched.
 * - dirent/walk never report "." or "..", and index 0..n-1 is stable while
 *   the directory does not change. dirent: VFTP_NOENT past the last entry and
 *   for a directory that does not exist.
 * - stat of a directory returns VFTP_ISDIR (never "a 0-byte file").
 * - every call is safe to make from the one task that owns a server/client
 *   instance; a port shared between tasks does its own locking.
 *
 * How to write one: docs/porting.md.
 */
#ifndef VFTP_STORAGE_H
#define VFTP_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include "vftp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One directory entry as a storage port reports it. */
typedef struct {
    char name[VFTP_MAX_FILE_NAME + 1u]; /**< NUL-terminated; a longer name is cut to 13 bytes */
    uint32_t size;  /**< bytes, 0 for a directory */
    uint32_t mtime; /**< seconds since 1970 (UTC; vftp_unix_time(), vftp_fat_to_unix()), 0 = unknown */
    bool is_dir;    /**< entry is a directory */
    bool name_cut;  /**< the real name is longer than #VFTP_MAX_FILE_NAME and `name` holds its start */
} vftp_dirent_t;

/**
 * @brief walk() callback: one entry.
 *
 * Runs inside the port's filesystem call (a mutex may be held): it must not call back into
 * the storage.
 * @param arg the `arg` given to walk()
 * @param index entry position, counted the same way as dirent()
 * @param e the entry (valid only during the call)
 * @return true to continue, false to stop
 */
typedef bool ( *vftp_walk_fn )( void *arg, uint32_t index, const vftp_dirent_t *e );

/**
 * @brief The filesystem as the library sees it: path-based, stateless calls.
 *
 * Every function returns VFTP_OK or an error code (NOENT, IO, NOSPC, INVAL, ISDIR, ...) and
 * receives `ctx` as its first argument. Paths are NUL-terminated, at most 55 characters (sidecar
 * paths up to #VFTP_SIDECAR_PATH_MAX). Set unused optional members to NULL.
 */
typedef struct vftp_storage {
    /** Size of a regular file. VFTP_NOENT if it does not exist, VFTP_ISDIR for a directory. */
    vftp_ret_t ( *stat )( void *ctx, const char *path, uint32_t *size );
    /** Up to `max` bytes at `off` into `buf`; `*got` = bytes read, 0 at or past end of file (not an error). */
    vftp_ret_t ( *read )( void *ctx, const char *path, uint32_t off, uint8_t *buf, uint16_t max, uint16_t *got );
    /** `len` bytes at `off`; creates a missing file, leaves the rest of an existing one untouched. */
    vftp_ret_t ( *write )( void *ctx, const char *path, uint32_t off, const uint8_t *data, uint16_t len );
    /** Create-or-truncate to zero length. */
    vftp_ret_t ( *truncate )( void *ctx, const char *path );
    /** Delete a file. VFTP_NOENT if it does not exist. */
    vftp_ret_t ( *remove )( void *ctx, const char *path );
    /** The index-th entry of `dir` ("" = current/root directory); VFTP_NOENT past the end. */
    vftp_ret_t ( *dirent )( void *ctx, const char *dir, uint32_t index, vftp_dirent_t *out );
    /** Optional (NULL: the library falls back to dirent()). One pass over `dir`, calling fn for
     * entries index >= first until fn returns false or the directory ends. VFTP_OK for an
     * existing directory (an empty one included), VFTP_NOENT if it does not exist. A port whose
     * filesystem rescans from the start on every dirent() call (FAT findfirst/findnext) needs this:
     * listing n entries through dirent() costs O(n^2) directory reads, through walk() O(n). */
    vftp_ret_t ( *walk )( void *ctx, const char *dir, uint32_t first, vftp_walk_fn fn, void *arg );
    /** Optional. Replaces `to` with `from` (used to publish LIST.TXT whole, never half-written). */
    vftp_ret_t ( *rename )( void *ctx, const char *from, const char *to );
    /** Optional. VFTP_BADNAME if this filesystem cannot name `path` exactly. Checked for every path a
     * client names (download, delete, list, list_regen, index, upload) before the storage is touched: a
     * FAT driver without long names may silently cut a longer name to 8.3 and act on another file. */
    vftp_ret_t ( *name_ok )( void *ctx, const char *path );
    void *ctx; /**< passed to every function above */
} vftp_storage_t;

/** Room needed for a sidecar path (see vftp_sidecar_path()). */
#define VFTP_SIDECAR_PATH_MAX ( VFTP_MAX_PATH + 16u )

/**
 * @brief Name of the resume-state file ("sidecar") of a transfer.
 *
 * "XXXXXXXX.<ext>", XXXXXXXX = CRC-32 of the full path in hex: 8.3-safe on every filesystem
 * (FAT without long names included).
 * @param path the transferred file's path
 * @param ext3 extension: "VFM" (burst upload, server), "VFI" (poll upload, server), "VFD" (burst
 *        download, client)
 * @param at_root true: in the filesystem root (server side: data directories, and so
 *        `ls`/count/index of them, never see it); false: next to `path` (ground side)
 * @param[out] out the sidecar path
 * @param cap size of `out` (#VFTP_SIDECAR_PATH_MAX is always enough)
 * @return false if it does not fit `cap`
 */
bool vftp_sidecar_path( const char *path, const char *ext3, bool at_root, char *out, size_t cap );

/**
 * @brief 8.3 check for a FAT port without long file names (use it in vftp_storage_t::name_ok).
 * @param path "/"-separated path; "." and ".." components are allowed
 * @return true if every component is 1..8 name characters, optionally '.' and 1..3 extension
 *         characters, letters/digits and ! # $ % & ' ( ) - @ ^ _ \` { } ~ only
 */
bool vftp_path_is_83( const char *path );

#ifdef __cplusplus
}
#endif
#endif /* VFTP_STORAGE_H */
