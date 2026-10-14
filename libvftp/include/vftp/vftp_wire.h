/**
 * @file vftp_wire.h
 * @brief Host-form vftp messages and their byte-exact encode/decode.
 *
 * One message set serves both services (vftp_msg_type_t); a service simply drops the types it
 * does not use. Decoding is the single validation point of the library: a decoded message has a
 * known type, a length that was checked against that type, strings that fit their buffers and
 * hold no NUL, and an item count that fits both the packet and the array it is stored in.
 * Server and client code only ever look at decoded messages.
 *
 * Byte-by-byte layouts with real examples: docs/protocol.md.
 *
 * @verbatim
 * Conventions: integers little-endian. str = len:1 + len bytes (no NUL), always the LAST field
 * (LIST_REPLY entries end with their name). sess = the session byte the server assigned in
 * DOWNLOAD_REPLY / UPLOAD_REPLY; every message of an open transfer carries it right after the
 * type, so a late packet of an older transfer is dropped instead of being taken for the current one.
 *
 *   00 DOWNLOAD_REQUEST   type chunk_size:2 path:str
 *   01 DOWNLOAD_REPLY     type sess ret:1 size:4 crc32:4 flags:1 (bit0: crc32 valid; a large
 *                         file's CRC is computed while it is served, fetched with CRC_REQUEST)
 *   02 UPLOAD_REQUEST     type size:4 crc32:4 chunk_size:2 path:str
 *   03 UPLOAD_REPLY       type sess ret:1 resume_from:4 (poll: watermark; burst: 0, see STATUS)
 *   04 DATA               type sess chunk:4 bytes...
 *   05 STATUS_REQUEST     type sess
 *   06 STATUS_REPLY       type sess ret:1 total:4 complete:4 entries:2 {next:4 count:4}*entries
 *   07 CRC_REQUEST        type sess
 *   08 CRC_REPLY          type sess ret:1 crc32:4 done:4 (ret AGAIN: done = bytes checked so far)
 *   09 DONE / 0A ABORT    type sess
 *   0B LIST_REQUEST       type offset:4 limit:2 path:str
 *   0C LIST_REPLY         type ret:1 total_files:4 first_file:4 packet_files:2
 *                         {flags:1 size:4 mtime:4 name:str}*packet_files
 *   0D DELETE_REQUEST     type path:str             0E DELETE_REPLY      type ret:1
 *   0F LIST_REGEN_REQUEST type path:str             10 LIST_REGEN_REPLY  type ret:1
 *   11 INDEX_REQUEST      type index:4 path:str     (index 0xFFFFFFFF = count only)
 *   12 INDEX_REPLY        type ret:1 total:4 index:4 flags:1 size:4 mtime:4 name:str
 *   13 WANT               type sess entries:2 {next:4 count:4}*entries
 *   14 GET_CHUNK          type sess chunk:4
 *   15 DATA_ACK           type sess chunk:4 ret:1
 * @endverbatim
 */
#ifndef VFTP_WIRE_H
#define VFTP_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "vftp_config.h"
#include "vftp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Most ranges a host-form STATUS_REPLY / WANT holds (from the largest packet this build handles). */
#define VFTP_WIRE_MAX_RANGES VFTP_WANT_RANGES_FOR( VFTP_CFG_PACKET_MAX )
/** Most entries a host-form LIST_REPLY holds. */
#define VFTP_WIRE_MAX_ENTRIES VFTP_LIST_ENTRIES_MAX_FOR( VFTP_CFG_PACKET_MAX )

/** DOWNLOAD_REPLY.flags bit 0: `crc32` is valid (a large file's CRC comes later, from CRC_REQUEST). */
#define VFTP_DL_CRC_VALID 0x01u

/** @brief Chunks [next, next+count) -- a missing (or wanted) run. */
typedef struct {
    uint32_t next;  /**< first chunk index of the run */
    uint32_t count; /**< chunks in the run */
} vftp_range_t;

/** @brief One directory entry of a LIST_REPLY. */
typedef struct {
    uint8_t flags;  /**< VFTP_ENTRY_DIR, VFTP_ENTRY_NAME_CUT */
    uint32_t size;  /**< bytes; 0 for a directory */
    uint32_t mtime; /**< seconds since 1970 (UTC); 0 = unknown */
    char name[VFTP_MAX_FILE_NAME + 1u]; /**< NUL-terminated */
} vftp_list_entry_t;

/** @brief Payload of a DATA packet. */
typedef struct {
    uint32_t chunk;       /**< chunk index; byte offset = chunk * chunk_size */
    const uint8_t *bytes; /**< decode: points into the decoded buffer */
    uint16_t len;         /**< payload bytes (chunk_size, less for the last chunk) */
} vftp_data_t;

/** @brief INDEX_REQUEST (both services). */
typedef struct {
    uint32_t index;           /**< entry wanted, or #VFTP_INDEX_COUNT_ONLY */
    char path[VFTP_MAX_PATH]; /**< directory */
} vftp_index_req_t;

/** @brief INDEX_REPLY (both services). */
typedef struct {
    uint8_t ret;    /**< OK; NOENT: no such directory; RANGE: index >= total (total still valid) */
    uint32_t total; /**< entries in the directory */
    uint32_t index; /**< echoes the request (lets the client match a late reply) */
    uint8_t flags;  /**< VFTP_ENTRY_DIR, VFTP_ENTRY_NAME_CUT */
    uint32_t size;  /**< bytes */
    uint32_t mtime; /**< seconds since 1970 (UTC), 0 = unknown */
    char name[VFTP_MAX_FILE_NAME + 1u]; /**< entry name, NUL-terminated */
} vftp_index_rep_t;

/** @brief Chunk ranges (STATUS_REPLY and WANT). */
typedef struct {
    uint16_t entries;                         /**< valid items in range[] */
    vftp_range_t range[VFTP_WIRE_MAX_RANGES]; /**< the ranges, lowest first */
} vftp_ranges_t;

/**
 * @brief One message in host form; `type` (vftp_msg_type_t) selects the union member.
 *
 * Members by type: down_req, down_rep, up_req, up_rep, data (DATA), status (STATUS_REPLY),
 * want (WANT), crc_rep, list_req, list_rep, path_req (DELETE_REQUEST, LIST_REGEN_REQUEST),
 * ret_rep (DELETE_REPLY, LIST_REGEN_REPLY), idx_req, idx_rep, get_chunk, data_ack. STATUS_REQUEST,
 * CRC_REQUEST, DONE and ABORT carry only the session.
 */
typedef struct {
    uint8_t type;    /**< vftp_msg_type_t */
    uint8_t session; /**< session of an in-transfer message; 0 for the others */
    union {
        struct {
            uint16_t chunk_size;      /**< chunk size in bytes */
            char path[VFTP_MAX_PATH]; /**< NUL-terminated path */
        } down_req;                   /**< DOWNLOAD_REQUEST */
        struct {
            uint8_t ret;    /**< vftp_ret_t result */
            uint32_t size;  /**< file size in bytes */
            uint32_t crc32; /**< CRC-32 of the whole file (valid if flags & VFTP_DL_CRC_VALID) */
            uint8_t flags;  /**< VFTP_DL_CRC_VALID */
        } down_rep;         /**< DOWNLOAD_REPLY */
        struct {
            uint32_t size;            /**< file size in bytes */
            uint32_t crc32;           /**< CRC-32 of the whole file */
            uint16_t chunk_size;      /**< chunk size in bytes */
            char path[VFTP_MAX_PATH]; /**< NUL-terminated path */
        } up_req;                     /**< UPLOAD_REQUEST */
        struct {
            uint8_t ret;          /**< vftp_ret_t result */
            uint32_t resume_from; /**< poll: first chunk the client must send (the watermark) */
        } up_rep;                 /**< UPLOAD_REPLY */
        vftp_data_t data;         /**< DATA */
        struct {
            uint8_t ret;           /**< vftp_ret_t result */
            uint32_t total;        /**< chunks in the file */
            uint32_t complete;     /**< chunks the server holds */
            vftp_ranges_t missing; /**< ranges still missing */
        } status;                  /**< STATUS_REPLY */
        vftp_ranges_t want;        /**< WANT */
        struct {
            uint8_t ret;    /**< OK, AGAIN, or an error */
            uint32_t crc32; /**< CRC-32 of the whole file (ret OK) */
            uint32_t done;  /**< ret AGAIN: bytes already checked */
        } crc_rep;          /**< CRC_REPLY */
        struct {
            uint32_t offset;          /**< index of the first entry wanted */
            uint16_t limit;           /**< entries wanted (0 = as many as fit the packet) */
            char path[VFTP_MAX_PATH]; /**< directory */
        } list_req;                   /**< LIST_REQUEST */
        struct {
            uint8_t ret;           /**< vftp_ret_t result */
            uint32_t total_files;  /**< entries in the directory */
            uint32_t first_file;   /**< index of entry[0] */
            uint16_t packet_files; /**< entries in this packet */
            vftp_list_entry_t entry[VFTP_WIRE_MAX_ENTRIES]; /**< the entries */
        } list_rep;                                         /**< LIST_REPLY */
        struct {
            char path[VFTP_MAX_PATH]; /**< NUL-terminated path */
        } path_req;                   /**< DELETE_REQUEST, LIST_REGEN_REQUEST */
        struct {
            uint8_t ret; /**< vftp_ret_t result */
        } ret_rep;       /**< DELETE_REPLY, LIST_REGEN_REPLY */
        vftp_index_req_t idx_req; /**< INDEX_REQUEST */
        vftp_index_rep_t idx_rep; /**< INDEX_REPLY */
        struct {
            uint32_t chunk; /**< chunk index */
        } get_chunk;        /**< GET_CHUNK */
        struct {
            uint32_t chunk; /**< chunk index (on RANGE: the server's watermark) */
            uint8_t ret;    /**< vftp_ret_t result */
        } data_ack;         /**< DATA_ACK */
    } u;                    /**< payload, selected by `type` */
} vftp_msg_t;

/**
 * @brief True for the types that belong to an open transfer and carry the session byte.
 * @param type a vftp_msg_type_t value
 * @return the type carries `session`
 */
bool vftp_msg_has_session( uint8_t type );

/**
 * @brief Decodes and validates one packet.
 * @param buf packet bytes (vftp packet only: no transport header)
 * @param len packet length
 * @param[out] m host-form message; DATA bytes point into `buf`
 * @return VFTP_OK, or VFTP_INVAL for anything malformed (unknown type, too short, path too long
 *         or holding a NUL, item count that does not fit). Trailing extra bytes are ignored.
 */
vftp_ret_t vftp_decode( const uint8_t *buf, uint16_t len, vftp_msg_t *m );

/**
 * @brief Encodes one message.
 * @param m message; for DATA, `bytes` may point at buf + #VFTP_DATA_HEADER (the payload is
 *        moved, not copied over itself)
 * @param[out] buf output buffer
 * @param cap bytes available in `buf` (normally the link MTU)
 * @return encoded length, or 0 if it does not fit `cap` or is not a valid message (unknown type,
 *         path too long, too many items)
 */
uint16_t vftp_encode( const vftp_msg_t *m, uint8_t *buf, uint16_t cap );

/**
 * @brief Copies a string into a path buffer -- never silently truncated.
 * @param[out] dst path buffer (#VFTP_MAX_PATH bytes)
 * @param src NUL-terminated path, at most 55 characters
 * @return true, or false (and dst set to "") if it does not fit with its terminator
 */
bool vftp_path_set( char dst[VFTP_MAX_PATH], const char *src );

#ifdef __cplusplus
}
#endif
#endif /* VFTP_WIRE_H */
