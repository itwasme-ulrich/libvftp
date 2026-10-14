/**
  ******************************************************************************
  * @file    middleware/vftp/vftp_csp_serve.h
  * @brief   The CSP 2.1 transport port of libvftp's server side
  ******************************************************************************
  */
#ifndef VFTP_CSP_SERVE_H
#define VFTP_CSP_SERVE_H

#include <stdint.h>

#include <csp/csp.h>

#include "vftp/vftp_server.h"

#define VFTP_CSP_RDP_HEADER 5U
#define VFTP_CSP_CRC32 4U
#define VFTP_CSP_KISS_CRC32 4U
#define VFTP_CSP_MAX_PACKET ( CSP_BUFFER_SIZE - VFTP_CSP_RDP_HEADER - VFTP_CSP_CRC32 - VFTP_CSP_KISS_CRC32 )

typedef struct {
    const char *name; // log prefix
    csp_socket_t *sock; // owned by the calling task file (static storage)
    uint8_t port;
    uint32_t idle_ms;      // csp_read timeout while a transfer is open: silent this long = abandoned
    uint32_t idle_free_ms; // csp_read timeout with no transfer open (listing, one-shot requests). Short on
                           // purpose: without RDP the OBC never learns that a client gave up, and while
                           // this connection is held the next client cannot even be accepted.
    void *engine;          // vftp_burst_server_t* or vftp_poll_server_t*
    vftp_srv_action_t ( *handle )( void *engine, const uint8_t *pkt, uint16_t len, vftp_send_fn send, void *ctx );
    void ( *abort )( void *engine );
    bool ( *busy )( const void *engine );
} VftpCspService_t;

// Never returns. Call from the service's own task, after csp_init().
void VftpCsp_Serve( const VftpCspService_t *svc );

#endif // VFTP_CSP_SERVE_H
