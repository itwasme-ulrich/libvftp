/**
 * @file vftp_spp.h
 * @brief vftp over CCSDS Space Packets (CCSDS 133.0-B-2), transport-neutral part.
 *
 * One vftp packet = the user data field of one unsegmented Space Packet. The
 * service is selected by APID; the client sends telecommand packets (type 1),
 * the server answers with telemetry packets (type 0) on the same APID. Primary
 * header, big-endian as CCSDS specifies:
 * @verbatim
   version:3=0 | type:1 | sec_hdr:1=0 | APID:11
   seq_flags:2=0b11 (unsegmented) | seq_count:14
   data_length:16 = user data octets - 1
   @endverbatim
 * This file is pure C (no sockets); vftp_spp_udp.h carries the packets over UDP.
 */
#ifndef VFTP_SPP_H
#define VFTP_SPP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Space Packet primary header size. */
#define VFTP_SPP_HEADER 6u
/** APID of the burst service (0x100 + the CSP port number). */
#define VFTP_SPP_APID_BURST 0x109u
/** APID of the poll service. */
#define VFTP_SPP_APID_POLL 0x108u

/** @brief One unwrapped Space Packet. */
typedef struct {
    uint16_t apid;       /**< application process identifier, 11 bits */
    bool telecommand;    /**< packet type bit: true = TC (client -> server) */
    uint16_t seq;        /**< sequence count, 14 bits */
    const uint8_t *data; /**< points into the unwrapped buffer */
    uint16_t len;        /**< user data octets */
} vftp_spp_t;

/**
 * @brief Builds one unsegmented Space Packet (primary header + user data).
 * @param[out] out packet buffer
 * @param cap bytes available in out
 * @param apid 11-bit APID
 * @param telecommand packet type: true = TC, false = TM
 * @param seq sequence count (14 bits used)
 * @param data user data (one vftp packet)
 * @param len user data octets, >= 1
 * @return packet length, 0 if it does not fit or len == 0
 */
uint16_t vftp_spp_wrap( uint8_t *out, uint16_t cap, uint16_t apid, bool telecommand, uint16_t seq,
                        const uint8_t *data, uint16_t len );

/**
 * @brief Validates and splits one Space Packet.
 * @param in packet bytes
 * @param len packet length
 * @param[out] out header fields and a pointer to the user data inside `in`
 * @return false for anything that is not a well-formed, unsegmented, secondary-header-free
 *         packet of exactly `len` bytes
 */
bool vftp_spp_unwrap( const uint8_t *in, uint16_t len, vftp_spp_t *out );

#ifdef __cplusplus
}
#endif
#endif /* VFTP_SPP_H */
