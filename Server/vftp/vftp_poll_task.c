/**
  ******************************************************************************
  * @file    middleware/vftp/vftp_poll_task.c
  * @brief   vftp_poll service task -- see the header.
  ******************************************************************************
  */
#include "vftp_poll_task.h"

#include "FreeRTOS.h"
#include "task.h"

#include "os_task.h"
#include "vftp/vftp_server.h"
#include "vftp_csp_serve.h"
#include "vftp_storage_fsio.h"
#include "vftp_user_config.h"

#define VFTP_POLL_TASK_PRIORITY ( tskIDLE_PRIORITY + 1 )
#define VFTP_POLL_TASK_STACK ( configMINIMAL_STACK_SIZE * 4 )
// Stop-and-wait: one round trip per chunk, so a shorter silence already means the client is gone.
#define VFTP_POLL_IDLE_MS 20000U
#define VFTP_POLL_MTU_TX VFTP_CSP_MAX_PACKET // Sat -> Ground: 211, see vftp_csp_serve.h
#define VFTP_POLL_MTU_RX 150U
#if VFTP_POLL_MTU_RX > VFTP_CSP_MAX_PACKET
#error "VFTP_POLL_MTU_RX exceeds what one CSP packet can carry (vftp_csp_serve.h)"
#endif

static vftp_poll_server_t s_server;
static csp_socket_t s_sock;

static vftp_srv_action_t Handle( void *engine, const uint8_t *pkt, uint16_t len, vftp_send_fn send, void *ctx ) {
    return vftp_poll_server_handle( ( vftp_poll_server_t * ) engine, pkt, len, send, ctx );
}

static void Abort( void *engine ) {
    vftp_poll_server_abort( ( vftp_poll_server_t * ) engine );
}

static bool Busy( const void *engine ) {
    return vftp_poll_server_busy( ( const vftp_poll_server_t * ) engine );
}

static void VftpPollTask_Main( void *pv ) {
    ( void ) pv;
    const vftp_server_cfg_t cfg = { VftpStorageFsio_Get(), VFTP_POLL_MTU_TX, VFTP_POLL_MTU_RX };
    if( vftp_poll_server_init( &s_server, &cfg ) != VFTP_OK ) {
        VFTP_LOG_WARN( "poll: bad configuration, service not started" );
        vTaskSuspend( NULL );
    }
    static const VftpCspService_t svc = { "poll", &s_sock, 8U, VFTP_POLL_IDLE_MS, 5000U, &s_server, Handle, Abort,
                                          Busy };
    VftpCsp_Serve( &svc );
}

static const TaskContract_t s_taskContract = {
    "VftpPoll", ( uint8_t ) VFTP_POLL_TASK_PRIORITY, ( uint16_t ) VFTP_POLL_TASK_STACK,
    TASK_NO_HEALTH_ID, 0U, 0U, 0U, 1U /* degradable */ };

void VftpPollTask_Create( void ) {
    ( void ) Os_TaskCreate( &s_taskContract, VftpPollTask_Main, NULL );
}
