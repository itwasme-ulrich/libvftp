/**
 * @file vftp_storage_ram.h
 * @brief vftp_storage_t in a caller-provided RAM pool: no OS, no libc I/O.
 *
 * For tests and for running the whole protocol on a bare target (the OBC
 * boot self-test uses it). Fixed slots of pool_size/VFTP_RAMFS_MAX_FILES
 * bytes each; directories are implicit in paths ("a/b.bin" lives in "a").
 */
#ifndef VFTP_STORAGE_RAM_H
#define VFTP_STORAGE_RAM_H

#include "vftp/vftp_config.h" /* a port may set VFTP_RAMFS_MAX_FILES in its user config */
#include "vftp/vftp_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef VFTP_RAMFS_MAX_FILES
/** Files the RAM filesystem holds (each gets pool_size / VFTP_RAMFS_MAX_FILES bytes). */
#define VFTP_RAMFS_MAX_FILES 16u
#endif

/** @brief One file slot. */
typedef struct {
    char path[VFTP_SIDECAR_PATH_MAX]; /**< normalised path */
    uint32_t size;                    /**< bytes */
    bool used;                        /**< slot holds a file */
} vftp_ramfs_file_t;

/** @brief RAM filesystem state, with counters and fault injection for tests. */
typedef struct {
    vftp_ramfs_file_t file[VFTP_RAMFS_MAX_FILES]; /**< file slots */
    uint8_t *pool;     /**< caller's memory */
    uint32_t slot_cap; /**< bytes per file */
    uint32_t n_reads;  /**< storage calls made, for tests that check I/O batching */
    uint32_t n_writes; /**< write() calls */
    uint32_t n_dirent; /**< dirent() calls */
    uint32_t n_walk;   /**< walk() calls */
    uint32_t fail_write_at;   /**< fault injection: the n-th write (1-based) fails with VFTP_IO, 0 = never */
    uint32_t fail_write_from; /**< fault injection: every write from the n-th on fails (a dead card), 0 = never */
    bool fat83;               /**< name_ok() enforces FAT 8.3 names, like the OBC's filesystem */
} vftp_ramfs_t;

/**
 * @brief Fills `st` with the RAM implementation (all optional ops included).
 * @param[out] st storage interface to fill
 * @param[out] fs filesystem state (cleared)
 * @param pool caller's memory for file contents
 * @param pool_size bytes in pool
 */
void vftp_storage_ram_init( vftp_storage_t *st, vftp_ramfs_t *fs, uint8_t *pool, uint32_t pool_size );

/**
 * @brief Test helper: creates or replaces a whole file without going through vftp_storage_t.
 * @param fs filesystem
 * @param path file path
 * @param data contents
 * @param len bytes
 * @return VFTP_OK or VFTP_NOSPC
 */
vftp_ret_t vftp_ramfs_put( vftp_ramfs_t *fs, const char *path, const uint8_t *data, uint32_t len );

/**
 * @brief Test helper: a file's contents.
 * @param fs filesystem
 * @param path file path
 * @param[out] len its size
 * @return pointer to the contents, NULL if the file does not exist
 */
const uint8_t *vftp_ramfs_get( vftp_ramfs_t *fs, const char *path, uint32_t *len );

/**
 * @brief Test helper: does a file exist.
 * @param fs filesystem
 * @param path file path
 * @return true if it exists
 */
bool vftp_ramfs_exists( vftp_ramfs_t *fs, const char *path );

#ifdef __cplusplus
}
#endif
#endif /* VFTP_STORAGE_RAM_H */
