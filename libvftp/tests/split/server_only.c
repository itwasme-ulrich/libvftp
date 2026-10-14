/* Links against libvftp_server.a + libvftp_core.a only (make split-check): fails to link if a
 * server ever depends on client code. */
#include "vftp/vftp_server.h"

static vftp_burst_server_t s_burst;
static vftp_poll_server_t s_poll;

static int discard( void *ctx, const uint8_t *pkt, uint16_t len )
{
    (void) ctx;
    (void) pkt;
    (void) len;
    return 0;
}

int main( void )
{
    static const uint8_t done[1] = { 9u }; /* DONE in both services */
    vftp_server_cfg_t cfg = { NULL, 211u, 150u };
    (void) vftp_burst_server_init( &s_burst, &cfg ); /* refused (no storage): fine, only linking matters */
    (void) vftp_poll_server_init( &s_poll, &cfg );
    (void) vftp_burst_server_handle( &s_burst, done, 1u, discard, NULL );
    (void) vftp_poll_server_handle( &s_poll, done, 1u, discard, NULL );
    vftp_burst_server_abort( &s_burst );
    vftp_poll_server_abort( &s_poll );
    return ( vftp_burst_server_busy( &s_burst ) || vftp_poll_server_busy( &s_poll ) ) ? 1 : 0;
}
