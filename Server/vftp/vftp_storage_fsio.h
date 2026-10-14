/**
  ******************************************************************************
  * @file    middleware/vftp/vftp_storage_fsio.h
  * @brief   libvftp's vftp_storage_t over this board's FsIo_
  ******************************************************************************
  */
#ifndef VFTP_STORAGE_FSIO_H
#define VFTP_STORAGE_FSIO_H

#include "vftp/vftp_storage.h"

const vftp_storage_t *VftpStorageFsio_Get( void );

#endif /* VFTP_STORAGE_FSIO_H */
