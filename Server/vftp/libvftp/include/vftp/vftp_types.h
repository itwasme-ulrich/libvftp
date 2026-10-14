/**
 * @file vftp_types.h
 * @brief vftp protocol constants: message type codes, return codes, wire sizes.
 *
 * Two independent services share one message set (one type table, see vftp_msg_type_t):
 * - **vftp_burst**: blast every chunk, the receiver reports missing {next,count}
 *   ranges, upload resume from a persistent bitmap.
 * - **vftp_poll**: stop-and-wait, one chunk per round trip, upload resume from a
 *   single watermark (POLL never has gaps by construction).
 *
 * Wire encoding: every multi-byte integer is little-endian, written and read
 * byte by byte in vftp_wire.c (never by overlaying a struct), so the same code
 * is correct on little- and big-endian hosts and needs no packed structs.
 * Strings (paths, names) are length-prefixed: len:1 then len bytes, no NUL on
 * the wire. Every message that belongs to an open transfer carries the
 * transfer's session byte right after the type. Layouts: vftp_wire.h,
 * docs/protocol.md.
 */
#ifndef VFTP_TYPES_H
#define VFTP_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- wire constants (protocol, not configuration) ---------------------- */

/** Path buffer size, NUL included: a path has at most 55 characters (ICD limit). On the wire a
 * path is length-prefixed, so raising this limit later changes no message layout. */
#define VFTP_MAX_PATH 56u
/** File listing method 1 always writes here, at the server's root. */
#define VFTP_LIST_TXT "LIST.TXT"
/** Longest entry name (characters) a LIST/INDEX entry carries; a longer name is cut and flagged
 * #VFTP_ENTRY_NAME_CUT. Names are length-prefixed on the wire. */
#define VFTP_MAX_FILE_NAME 13u
/** DATA packet header: type(1) + session(1) + chunk index(4). Chunk payload = packet size - this. */
#define VFTP_DATA_HEADER 6u

/** LIST/INDEX entry flags (one byte per entry; unused bits are sent as 0 and ignored). */
#define VFTP_ENTRY_DIR 0x01u      /**< the entry is a directory */
#define VFTP_ENTRY_NAME_CUT 0x02u /**< the name was longer than #VFTP_MAX_FILE_NAME and is cut */

/** CSP port of the burst service (every port in this repository uses it). */
#define VFTP_BURST_CSP_PORT 9u
/** CSP port of the poll service. */
#define VFTP_POLL_CSP_PORT 8u

/* ---- return codes ------------------------------------------------------- */

/**
 * @brief Result of every library call, and the `ret` byte of every reply.
 *
 * Values 0..63 travel on the wire inside `ret` fields and must never change.
 * Values >= 64 are local to one side and are never sent.
 */
typedef enum {
    VFTP_OK = 0,
    VFTP_NOENT = 1,  /**< no such file or directory */
    VFTP_INVAL = 2,  /**< malformed request, bad argument, chunk_size over the link MTU */
    VFTP_NOSPC = 3,  /**< storage full, or file needs more chunks than this build can track */
    VFTP_IO = 4,     /**< storage I/O error */
    VFTP_EXISTS = 5, /**< resume state belongs to a different transfer */
    VFTP_NOTSUP = 6, /**< not supported */
    VFTP_NOMEM = 7,  /**< out of memory */
    VFTP_BUSY = 8,          /**< another transfer is open on this service */
    VFTP_CRC_MISMATCH = 9,  /**< end-to-end file CRC differs */
    VFTP_RANGE = 10,        /**< chunk / entry index out of range, or out of sequence */
    VFTP_ISDIR = 11,        /**< the path names a directory, not a file */
    VFTP_BADNAME = 12,      /**< name not valid on the server's filesystem (e.g. not 8.3 on FAT) */
    VFTP_AGAIN = 13,        /**< CRC_REPLY: still computing (large file), ask again */

    VFTP_TIMEOUT = 64,      /**< local: no (valid) reply in time, or no progress for too many rounds */
    VFTP_LINK = 65,         /**< local: transport reported an error (connection lost, send failed) */
    VFTP_PROTO = 66,        /**< local: peer sent something that makes no sense here */
    VFTP_REMOTE = 67        /**< local: server refused, see vftp_client_t::remote_ret for its code */
} vftp_ret_t;

/**
 * @brief Short, stable, human readable name of a return code.
 * @param ret a vftp_ret_t value (any int is accepted)
 * @return a static string such as "no such file or directory"; "unknown error" for unknown codes
 */
const char *vftp_strerror( int ret );

/* ---- message types (one table for both services) ----------------------- */

/**
 * @brief First byte of every packet. Both services share this table: a type means the same
 *        thing on port 9 (burst) and port 8 (poll), and a service drops the types it does not use.
 */
typedef enum {
    VFTP_MSG_DOWNLOAD_REQUEST = 0x00,   /**< burst, poll */
    VFTP_MSG_DOWNLOAD_REPLY = 0x01,     /**< burst, poll: opens a session */
    VFTP_MSG_UPLOAD_REQUEST = 0x02,     /**< burst, poll */
    VFTP_MSG_UPLOAD_REPLY = 0x03,       /**< burst, poll: opens a session */
    VFTP_MSG_DATA = 0x04,               /**< burst, poll: one chunk, either direction */
    VFTP_MSG_STATUS_REQUEST = 0x05,     /**< burst upload: "what do you hold?" */
    VFTP_MSG_STATUS_REPLY = 0x06,       /**< burst upload: held count + missing ranges */
    VFTP_MSG_CRC_REQUEST = 0x07,        /**< burst, poll */
    VFTP_MSG_CRC_REPLY = 0x08,          /**< burst, poll */
    VFTP_MSG_DONE = 0x09,               /**< burst, poll: ends the session (no reply) */
    VFTP_MSG_ABORT = 0x0A,              /**< burst, poll: ends the session (no reply) */
    VFTP_MSG_LIST_REQUEST = 0x0B,       /**< burst: listing method 2, page through a directory */
    VFTP_MSG_LIST_REPLY = 0x0C,
    VFTP_MSG_DELETE_REQUEST = 0x0D,     /**< burst */
    VFTP_MSG_DELETE_REPLY = 0x0E,
    VFTP_MSG_LIST_REGEN_REQUEST = 0x0F, /**< burst: listing method 1, write LIST.TXT */
    VFTP_MSG_LIST_REGEN_REPLY = 0x10,
    VFTP_MSG_INDEX_REQUEST = 0x11,      /**< burst, poll: listing method 3, count or n-th entry */
    VFTP_MSG_INDEX_REPLY = 0x12,
    VFTP_MSG_WANT = 0x13,               /**< burst download: "send me these chunk ranges" */
    VFTP_MSG_GET_CHUNK = 0x14,          /**< poll download: one chunk */
    VFTP_MSG_DATA_ACK = 0x15            /**< poll upload: one chunk acknowledged */
} vftp_msg_type_t;

/** INDEX_REQUEST.index asking for the entry count only. */
#define VFTP_INDEX_COUNT_ONLY 0xFFFFFFFFu

/* ---- variable-length messages: how many items fit a given packet size --- */
/** STATUS_REPLY header: type, session, ret, total(4), complete(4), entries(2). */
#define VFTP_STATUS_HEADER 13u
/** WANT header: type, session, entries(2). */
#define VFTP_WANT_HEADER 4u
/** One chunk range: next(4), count(4). */
#define VFTP_RANGE_SIZE 8u
/** LIST_REPLY header: type, ret, total_files(4), first_file(4), packet_files(2). */
#define VFTP_LIST_HEADER 12u
/** Fixed part of a LIST_REPLY entry: flags(1), size(4), mtime(4), name_len(1); the name follows. */
#define VFTP_LIST_ENTRY_FIXED 10u

/** Ranges that fit one STATUS_REPLY of `mtu` bytes. */
#define VFTP_STATUS_RANGES_FOR( mtu ) ( ( ( mtu ) - VFTP_STATUS_HEADER ) / VFTP_RANGE_SIZE )
/** Ranges that fit one WANT of `mtu` bytes. */
#define VFTP_WANT_RANGES_FOR( mtu ) ( ( ( mtu ) - VFTP_WANT_HEADER ) / VFTP_RANGE_SIZE )
/** Most entries (1-character names) that fit one LIST_REPLY of `mtu` bytes; real pages are
 * packed entry by entry, as many as fit. */
#define VFTP_LIST_ENTRIES_MAX_FOR( mtu ) ( ( ( mtu ) - VFTP_LIST_HEADER ) / ( VFTP_LIST_ENTRY_FIXED + 1u ) )

/* ---- time: `mtime` is seconds since 1970-01-01 00:00 UTC, 0 = unknown ---- */

/** @brief A calendar date and time (UTC). */
typedef struct {
    uint16_t year;  /**< 1970..2106 */
    uint8_t month;  /**< 1..12 */
    uint8_t day;    /**< 1..31 */
    uint8_t hour;   /**< 0..23 */
    uint8_t minute; /**< 0..59 */
    uint8_t second; /**< 0..59 */
} vftp_civil_t;

/**
 * @brief Calendar date and time (UTC) to seconds since 1970 -- for storage ports that keep dates.
 * @param year 1970..2106
 * @param month 1..12
 * @param day 1..31
 * @param hour 0..23
 * @param minute 0..59
 * @param second 0..59
 * @return seconds since 1970-01-01 00:00 UTC, 0 if out of range
 */
uint32_t vftp_unix_time( int year, int month, int day, int hour, int minute, int second );

/**
 * @brief A FAT directory entry's date and time words to seconds since 1970 (FAT keeps local time
 *        without a zone: it is taken as UTC).
 * @param fat_date bits 15..9 year-1980, 8..5 month, 4..0 day
 * @param fat_time bits 15..11 hour, 10..5 minute, 4..0 second/2
 * @return seconds since 1970, 0 if the date is 0 (unknown) or invalid
 */
uint32_t vftp_fat_to_unix( uint16_t fat_date, uint16_t fat_time );

/**
 * @brief Seconds since 1970 to a calendar date and time (UTC).
 * @param t seconds since 1970-01-01 00:00 UTC
 * @param[out] out the date and time
 */
void vftp_unix_to_civil( uint32_t t, vftp_civil_t *out );

#ifdef __cplusplus
}
#endif
#endif /* VFTP_TYPES_H */
