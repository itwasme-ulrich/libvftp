/* Links against libvftp_client.a + libvftp_core.a only (make split-check): fails to link if the
 * client ever depends on server code. */
#include "vftp/vftp_client.h"

static vftp_client_t s_client;

int main( void )
{
    vftp_client_cfg_t cfg;
    vftp_client_default_cfg( &cfg );
    if ( vftp_client_init( &s_client, &cfg ) != VFTP_OK ) {
        return 1;
    }
    /* Never run (no link set); referenced so every client entry point must resolve. */
    if ( cfg.link != NULL ) {
        vftp_index_rep_t info;
        char remote[VFTP_MAX_PATH];
        (void) vftp_burst_download( &s_client, "a", "b" );
        (void) vftp_burst_upload( &s_client, "a", "b" );
        (void) vftp_burst_remove( &s_client, "a" );
        (void) vftp_burst_list( &s_client, "", NULL, NULL, NULL );
        (void) vftp_burst_list_regen( &s_client, "", remote );
        (void) vftp_burst_index( &s_client, "", 0u, &info );
        (void) vftp_poll_index( &s_client, "", 0u, &info );
        (void) vftp_burst_download_index( &s_client, "", 0u, NULL, remote );
        (void) vftp_poll_download_index( &s_client, "", 0u, NULL, remote );
        (void) vftp_poll_download( &s_client, "a", "b" );
        (void) vftp_poll_upload( &s_client, "a", "b" );
    }
    return 0;
}
