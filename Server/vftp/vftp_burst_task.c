/**
  ******************************************************************************
  * @file    middleware/vftp/vftp_burst_task.c
  * @brief   vftp_burst service task -- see the header. All protocol decisions
  *          live in libvftp (libvftp/src/vftp_burst_server.c); this file only
  *          picks the port, the storage and the timeouts.
  ******************************************************************************
  */
#include "vftp_burst_task.h"

#include "FreeRTOS.h"
#include "task.h"

#include "os_task.h"
#include "vftp/vftp_server.h"
#include "vftp_csp_serve.h"
#include "vftp_selftest.h"
#include "vftp_storage_fsio.h"
#include "vftp_user_config.h"

#define VFTP_BURST_TASK_PRIORITY ( tskIDLE_PRIORITY + 1 )
#define VFTP_BURST_TASK_STACK ( configMINIMAL_STACK_SIZE * 4 )
// An open conversation silent this long (at least one RDP round trip over the
// slow, lossy link) is abandoned and the service freed for the next client.
#define VFTP_BURST_IDLE_MS 60000U
#define VFTP_IDLE_FREE_MS 5000U // no transfer open: a listing's next page or a closing packet comes fast
#define VFTP_BURST_MTU_TX VFTP_CSP_MAX_PACKET // Sat -> Ground: 211, see vftp_csp_serve.h
#define VFTP_BURST_MTU_RX 150U // Ground -> Sat (security header eats the rest)
#if VFTP_BURST_MTU_RX > VFTP_CSP_MAX_PACKET
#error "VFTP_BURST_MTU_RX exceeds what one CSP packet can carry (vftp_csp_serve.h)"
#endif

static vftp_burst_server_t s_server; // ~1.1 MiB (1 MiB resume bitmap + 32 KiB buffer): static, never on a stack
static csp_socket_t s_sock;

static vftp_srv_action_t Handle( void *engine, const uint8_t *pkt, uint16_t len, vftp_send_fn send, void *ctx ) {
    return vftp_burst_server_handle( ( vftp_burst_server_t * ) engine, pkt, len, send, ctx );
}

static void Abort( void *engine ) {
    vftp_burst_server_abort( ( vftp_burst_server_t * ) engine );
}

static bool Busy( const void *engine ) {
    return vftp_burst_server_busy( ( const vftp_burst_server_t * ) engine );
}

static void VftpBurstTask_Main( void *pv ) {
    ( void ) pv;
#if SAT_BOOT_SELFTESTS
    // Runs the whole library (client + both servers) once over an in-RAM
    // loopback with simulated loss, on this CPU and this RTOS, before serving.
    VftpSelfTest_Run( &s_server );
#endif
    const vftp_server_cfg_t cfg = { VftpStorageFsio_Get(), VFTP_BURST_MTU_TX, VFTP_BURST_MTU_RX };
    if( vftp_burst_server_init( &s_server, &cfg ) != VFTP_OK ) {
        VFTP_LOG_WARN( "burst: bad configuration, service not started" );
        vTaskSuspend( NULL );
    }
    static const VftpCspService_t svc = { "burst", &s_sock, 9U, VFTP_BURST_IDLE_MS, VFTP_IDLE_FREE_MS, &s_server,
                                          Handle, Abort, Busy };
    VftpCsp_Serve( &svc );
}

static const TaskContract_t s_taskContract = {
    "VftpBurst", ( uint8_t ) VFTP_BURST_TASK_PRIORITY, ( uint16_t ) VFTP_BURST_TASK_STACK,
    TASK_NO_HEALTH_ID, // a vftp wedge must never look like a reason to reset the OBC
    0U, 0U, 0U, 1U /* degradable */ };

void VftpBurstTask_Create( void ) {
    ( void ) Os_TaskCreate( &s_taskContract, VftpBurstTask_Main, NULL );
}
