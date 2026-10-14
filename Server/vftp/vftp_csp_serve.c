/**
  ******************************************************************************
  * @file    middleware/vftp/vftp_csp_serve.c
  * @brief   CSP 2.1 server loop for libvftp -- see the header.
  ******************************************************************************
  */
#include "vftp_csp_serve.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include <csp/csp.h>
#include <csp/arch/csp_queue.h>

#include "csp_rdp_lock.h" // libcsp 2.1's shared RDP queues are not thread-safe; see that file's header
#include "vftp_user_config.h"

#define ACCEPT_TIMEOUT_MS 1000U
#define BUFFER_WAIT_TRIES 100U
#define BUFFER_WAIT_STEP_MS 20U
#define BUFFER_RESERVE 12

static int SendOnConn( void *ctx, const uint8_t *pkt, uint16_t len ) {
    csp_conn_t *conn = ( csp_conn_t * ) ctx;
    csp_packet_t *p = NULL;
    for( uint32_t i = 0; i < BUFFER_WAIT_TRIES && p == NULL; i++ ) {
        if( csp_buffer_remaining() > BUFFER_RESERVE ) {
            p = csp_buffer_get( len );
        }
        if( p == NULL ) {
            vTaskDelay( BUFFER_WAIT_STEP_MS / portTICK_RATE_MS );
        }
    }
    if( p == NULL || len > sizeof( p->data ) ) {
        if( p != NULL ) {
            csp_buffer_free( p );
        }
        return -1;
    }
    memcpy( p->data, pkt, len );
    p->length = len;
    CspRdpLock_Send( conn, p ); // always consumes the packet
    return 0;
}

#define PREEMPT_SILENCE_MS 3000U
#define READ_SLICE_MS 250U

static csp_packet_t *ReadOrGiveWay( const VftpCspService_t *svc, csp_conn_t *conn, uint32_t timeout_ms ) {
    portTickType start = xTaskGetTickCount();
    for( ;; ) {
        uint32_t waited = ( uint32_t ) ( xTaskGetTickCount() - start ) * portTICK_RATE_MS;
        if( waited >= timeout_ms ) {
            return NULL; // silent too long: abandoned
        }
        if( waited >= PREEMPT_SILENCE_MS && csp_queue_size( svc->sock->rx_queue ) > 0 ) {
            VFTP_LOG_INFO( "%s: silent %lu ms and a new connection waits: giving way", svc->name,
                           ( unsigned long ) waited );
            return NULL;
        }
        uint32_t slice = timeout_ms - waited < READ_SLICE_MS ? timeout_ms - waited : READ_SLICE_MS;
        csp_packet_t *pkt = csp_read( conn, slice );
        if( pkt != NULL || !csp_conn_is_active( conn ) ) {
            return pkt; // a packet, or the peer is gone (RDP closed)
        }
    }
}

static void ServeConnection( const VftpCspService_t *svc, csp_conn_t *conn ) {
    for( ;; ) {
        csp_packet_t *pkt = ReadOrGiveWay( svc, conn, svc->busy( svc->engine ) ? svc->idle_ms : svc->idle_free_ms );
        if( pkt == NULL ) {
            return; // timeout, peer gone, or a newer client waits: the caller aborts
        }
        vftp_srv_action_t a = svc->handle( svc->engine, pkt->data, pkt->length, SendOnConn, conn );
        csp_buffer_free( pkt );
        if( a == VFTP_SRV_CLOSE ) {
            return;
        }
    }
}

void VftpCsp_Serve( const VftpCspService_t *svc ) {
    csp_socket_t *sock = svc->sock;
    memset( sock, 0, sizeof( *sock ) );
    csp_bind( sock, svc->port );
    csp_listen( sock, 1 );
    VFTP_LOG_INFO( "%s listening, CSP port %u", svc->name, ( unsigned int ) svc->port );
    for( ;; ) {
        csp_conn_t *conn = csp_accept( sock, ACCEPT_TIMEOUT_MS );
        if( conn == NULL ) {
            continue;
        }
        ServeConnection( svc, conn );
        svc->abort( svc->engine )
        CspRdpLock_Close( conn );
    }
}
