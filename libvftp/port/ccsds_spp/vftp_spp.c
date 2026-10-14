/* CCSDS Space Packet primary header for vftp -- see the header. */
#include "vftp_spp.h"

#include <string.h>

uint16_t vftp_spp_wrap( uint8_t *out, uint16_t cap, uint16_t apid, bool telecommand, uint16_t seq,
                        const uint8_t *data, uint16_t len )
{
    if ( len == 0u || (uint32_t) len + VFTP_SPP_HEADER > cap || apid > 0x7FFu ) {
        return 0u;
    }
    uint16_t w0 = (uint16_t) ( ( telecommand ? 0x1000u : 0u ) | apid );
    uint16_t w1 = (uint16_t) ( 0xC000u | ( seq & 0x3FFFu ) );
    uint16_t w2 = (uint16_t) ( len - 1u );
    out[0] = (uint8_t) ( w0 >> 8 );
    out[1] = (uint8_t) ( w0 & 0xFFu );
    out[2] = (uint8_t) ( w1 >> 8 );
    out[3] = (uint8_t) ( w1 & 0xFFu );
    out[4] = (uint8_t) ( w2 >> 8 );
    out[5] = (uint8_t) ( w2 & 0xFFu );
    memmove( out + VFTP_SPP_HEADER, data, len );
    return (uint16_t) ( len + VFTP_SPP_HEADER );
}

bool vftp_spp_unwrap( const uint8_t *in, uint16_t len, vftp_spp_t *out )
{
    if ( len < VFTP_SPP_HEADER + 1u ) {
        return false;
    }
    unsigned int w0 = ( (unsigned int) in[0] << 8 ) | in[1];
    unsigned int w1 = ( (unsigned int) in[2] << 8 ) | in[3];
    unsigned int w2 = ( (unsigned int) in[4] << 8 ) | in[5];
    if ( ( w0 >> 13 ) != 0u || ( w0 & 0x0800u ) != 0u || ( w1 >> 14 ) != 3u || w2 + 1u + VFTP_SPP_HEADER != len ) {
        return false;
    }
    out->apid = (uint16_t) ( w0 & 0x7FFu );
    out->telecommand = ( w0 & 0x1000u ) != 0u;
    out->seq = (uint16_t) ( w1 & 0x3FFFu );
    out->data = in + VFTP_SPP_HEADER;
    out->len = (uint16_t) ( len - VFTP_SPP_HEADER );
    return true;
}
