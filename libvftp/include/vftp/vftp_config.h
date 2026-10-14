/**
 * @file vftp_config.h
 * @brief Compile-time limits and porting hooks for libvftp (RAM sizing table: docs/porting.md).
 *
 * Every value here is a default. A port overrides any of them by building with
 * VFTP_HAVE_USER_CONFIG=1 and putting a "vftp_user_config.h" on the include
 * path (pattern borrowed from libcsp's autoconfig): the library never includes
 * an OS, RTOS or filesystem header itself.
 *
 * Nothing in this file changes the wire format. Wire constants (path length,
 * name length, header sizes) live in vftp_types.h and are fixed by the
 * protocol, not by a port.
 */
#ifndef VFTP_CONFIG_H
#define VFTP_CONFIG_H

#if defined( VFTP_HAVE_USER_CONFIG ) && VFTP_HAVE_USER_CONFIG
#include "vftp_user_config.h"
#endif

#ifndef VFTP_CFG_PACKET_MAX
/** Largest vftp packet (type byte included) any link of this build carries.
 * Sizes the packet buffers inside the server/client contexts; the real limit
 * of a given link is the runtime mtu_tx/mtu_rx, which must not exceed this. */
#define VFTP_CFG_PACKET_MAX 256u
#endif

#ifndef VFTP_CFG_BURST_MAX_CHUNKS
/** Server: most chunks one burst upload can track (resume bitmap, 1 bit each).
 * Default: 2^23 chunks = 1 MiB of bitmap, a 1 GiB file at chunk_size 128. */
#define VFTP_CFG_BURST_MAX_CHUNKS 8388608u
#endif

#ifndef VFTP_CFG_CRC_STEP_BYTES
/** Server: most file bytes one request may make the server read just to compute a CRC.
 * Bounds the time a single packet keeps the server busy (watchdog, link timeouts): a file
 * larger than this is checksummed while it is served and, for what is left, over several
 * CRC_REQUESTs answered VFTP_AGAIN. */
#define VFTP_CFG_CRC_STEP_BYTES ( 1024u * 1024u )
#endif

#ifndef VFTP_CFG_IO_FAIL_LIMIT
/** Server: storage writes that must fail in a row before an upload's STATUS/ACK reports the
 * error and the client stops (one failure is retried silently: a transient SD hiccup). */
#define VFTP_CFG_IO_FAIL_LIMIT 2u
#endif

#ifndef VFTP_CFG_BURST_BUF_SIZE
/** Server: one RAM buffer shared by three jobs that never overlap in time --
 * whole-file staging for small uploads, write-behind batching for large
 * uploads, read-ahead cache (and CRC scratch) for downloads. */
#define VFTP_CFG_BURST_BUF_SIZE ( 32u * 1024u )
#endif

#ifndef VFTP_CFG_STAGE_MAX_BYTES
/** Server, burst upload: a file of at most this many bytes (and at most VFTP_CFG_STAGE_MAX_CHUNKS
 * chunks) is staged -- held whole in the buffer above and written once when complete, with no
 * resume sidecar: an interrupted staged upload starts again from 0. Anything bigger goes
 * write-behind with a sidecar and resumes. Size it by what a restart costs on the slowest link,
 * not by RAM: 4 KiB is ~30-40 s at 200 bit/s (32 KiB would be ~5 minutes, half a pass). */
#define VFTP_CFG_STAGE_MAX_BYTES 4096u
#endif

#ifndef VFTP_CFG_STAGE_MAX_CHUNKS
/** Server, burst upload: the same bound counted in chunks (= DATA packets to resend). A small
 * file sent with a tiny chunk size is many packets: 22 KB at 1-byte chunks took minutes and could
 * never resume while staging was bounded by bytes only. 0 stages only empty files. */
#define VFTP_CFG_STAGE_MAX_CHUNKS 32u
#endif

#ifndef VFTP_CFG_POLL_BUF_SIZE
/** Server: scratch buffer the poll service uses to CRC a file (bigger = fewer storage reads). */
#define VFTP_CFG_POLL_BUF_SIZE 1024u
#endif

#ifndef VFTP_CFG_CLIENT_MAX_CHUNKS
/** Client: most chunks one burst download can track (local resume bitmap). */
#define VFTP_CFG_CLIENT_MAX_CHUNKS 8388608u
#endif

#ifndef VFTP_CFG_CLIENT_BUF_SIZE
/** Client: scratch buffer used to CRC a local file. */
#define VFTP_CFG_CLIENT_BUF_SIZE 4096u
#endif

#ifndef VFTP_LOG_INFO
/** Logging hook, informational. Default: compiled out. A port maps it onto its own logger;
 * format strings are printf-style without a trailing newline. */
#define VFTP_LOG_INFO( ... ) ( (void) 0 )
#endif
#ifndef VFTP_LOG_WARN
/** Logging hook, warnings. Default: compiled out. */
#define VFTP_LOG_WARN( ... ) ( (void) 0 )
#endif

#if VFTP_CFG_PACKET_MAX < 96u
#error "VFTP_CFG_PACKET_MAX too small: an UPLOAD_REQUEST with a 55-character path is 67 bytes"
#endif
#if VFTP_CFG_PACKET_MAX > 65535u
#error "VFTP_CFG_PACKET_MAX must fit uint16_t"
#endif
#if VFTP_CFG_STAGE_MAX_BYTES > VFTP_CFG_BURST_BUF_SIZE
#error "VFTP_CFG_STAGE_MAX_BYTES must not exceed VFTP_CFG_BURST_BUF_SIZE: a staged upload is held whole in that buffer"
#endif

#endif /* VFTP_CONFIG_H */
