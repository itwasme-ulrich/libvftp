/**
  ******************************************************************************
  * @file    middleware/vftp/vftp_user_config.h
  * @brief   
  ******************************************************************************
  */
#ifndef VFTP_USER_CONFIG_H
#define VFTP_USER_CONFIG_H

#include "print_lock.h"

/* CSP_BUFFER_SIZE is 224: no vftp packet on this board is ever longer. */
#define VFTP_CFG_PACKET_MAX 256u

/* Upload resume bitmap: 2^23 chunks = 1 MiB of SDRAM (32 MiB on board, ~1.9 MiB used before
 * this) = a 1.2 GB file at the full 144 B uplink chunk -- worst case sized, not typical.
 * Staging / write-behind / read-ahead buffer: 32 KiB. */
#define VFTP_CFG_BURST_MAX_CHUNKS 8388608u
#define VFTP_CFG_BURST_BUF_SIZE ( 32u * 1024u )
#define VFTP_CFG_STAGE_MAX_BYTES 4096u
#define VFTP_CFG_STAGE_MAX_CHUNKS 32u

/* Poll's CRC scratch: every FsIo_Read opens/seeks/closes, so 4 KiB pieces, not 1 KiB. */
#define VFTP_CFG_POLL_BUF_SIZE 4096u

/* At most 512 KiB of card reads per request for a file CRC (well under a second on SD, far
 * under the ground's 10 s reply timeout); a bigger file finishes over CRC_REQUEST/AGAIN.
 * Neither vftp task carries a health ID, so this bounds link latency, not a watchdog. */
#define VFTP_CFG_CRC_STEP_BYTES ( 512u * 1024u )

/* The OBC runs the client side only in the boot self-test (vftp_selftest.c). */
#define VFTP_CFG_CLIENT_MAX_CHUNKS 4096u
#define VFTP_CFG_CLIENT_BUF_SIZE 1024u
#define VFTP_RAMFS_MAX_FILES 4u

#define VFTP_LOG_INFO( fmt, ... ) LOG_INFO( "[vftp] " fmt "\r\n", ##__VA_ARGS__ )
#define VFTP_LOG_WARN( fmt, ... ) LOG_WARNING( "[vftp] " fmt "\r\n", ##__VA_ARGS__ )

#endif /* VFTP_USER_CONFIG_H */
