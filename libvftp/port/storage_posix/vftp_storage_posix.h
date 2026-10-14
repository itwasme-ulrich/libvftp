/**
 * @file vftp_storage_posix.h
 * @brief vftp_storage_t over POSIX files (fopen/fread/fwrite, opendir/stat).
 *
 * Every path the library passes is taken relative to `root` ("" = use paths
 * as given). A server serving a directory gives it a root; a ground client
 * reading/writing the user's own files uses "".
 */
#ifndef VFTP_STORAGE_POSIX_H
#define VFTP_STORAGE_POSIX_H

#include "vftp/vftp_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longest host path (root + vftp path) the port handles. */
#define VFTP_POSIX_PATH_MAX 512u

/** @brief Port state. */
typedef struct {
    char root[VFTP_POSIX_PATH_MAX]; /**< directory every vftp path is relative to, "" = as given */
} vftp_posix_fs_t;

/**
 * @brief Fills `st` with the POSIX implementation (walk and rename included, name_ok NULL).
 * @param[out] st storage interface to fill
 * @param[out] ctx port state (must outlive `st`)
 * @param root directory served, "" or NULL = paths used as given
 */
void vftp_storage_posix_init( vftp_storage_t *st, vftp_posix_fs_t *ctx, const char *root );

#ifdef __cplusplus
}
#endif
#endif /* VFTP_STORAGE_POSIX_H */
