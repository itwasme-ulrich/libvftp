/**
 * @file vftp_crc32.h
 * @brief CRC-32/IEEE 802.3 (zlib, PNG, Ethernet): reflected polynomial
 *        0xEDB88320, init 0xFFFFFFFF, final xor 0xFFFFFFFF. "123456789" ->
 *        0xCBF43926. Bundled so the library depends on no one else's CRC.
 */
#ifndef VFTP_CRC32_H
#define VFTP_CRC32_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initial value of a running CRC. */
#define VFTP_CRC32_INIT 0xFFFFFFFFu

/**
 * @brief Running form: `crc = VFTP_CRC32_INIT; crc = vftp_crc32_update(crc, ...)...;
 *        result = vftp_crc32_final(crc)`.
 * @param crc running value
 * @param data bytes
 * @param len byte count
 * @return the updated running value
 */
uint32_t vftp_crc32_update( uint32_t crc, const void *data, size_t len );

/**
 * @brief Finishes a running CRC.
 * @param crc running value
 * @return the CRC-32
 */
uint32_t vftp_crc32_final( uint32_t crc );

/**
 * @brief One-shot CRC-32 ("123456789" -> 0xCBF43926).
 * @param data bytes
 * @param len byte count
 * @return the CRC-32
 */
uint32_t vftp_crc32( const void *data, size_t len );

/** @brief State of an incremental CRC over a file (inside the server contexts; driven by the library). */
typedef struct {
    uint32_t size;  /**< file size */
    uint32_t off;   /**< bytes [0, off) are in `state` */
    uint32_t state; /**< running value */
    uint32_t value; /**< final CRC once done */
    bool done;      /**< value is valid */
} vftp_crcjob_t;

#ifdef __cplusplus
}
#endif
#endif /* VFTP_CRC32_H */
