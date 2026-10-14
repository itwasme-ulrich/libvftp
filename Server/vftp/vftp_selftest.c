/**
  ******************************************************************************
  * @file    middleware/vftp/vftp_selftest.c
  * @brief   See the header. Compiled to nothing unless SAT_BOOT_SELFTESTS.
  ******************************************************************************
  */
#include "vftp_selftest.h"

#if SAT_BOOT_SELFTESTS

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "vftp/vftp.h"
#include "vftp_csp_serve.h" // VFTP_CSP_MAX_PACKET
#include "vftp_link_loopback.h"
#include "vftp_storage_ram.h"
#include "vftp_user_config.h"

#define POOL ( 256U * 1024U ) // VFTP_RAMFS_MAX_FILES (4) slots of 64 KiB per side
#define BURST_SIZE 48000U     // > the 32 KiB staging buffer: write-behind + sidecar path
#define POLL_SIZE 4000U

static uint8_t s_srvPool[POOL];
static uint8_t s_cliPool[POOL];
static vftp_ramfs_t s_srvRam;
static vftp_ramfs_t s_cliRam;
static vftp_storage_t s_srvFs;
static vftp_storage_t s_cliFs;
static vftp_poll_server_t s_poll;
static vftp_loopback_t s_lb;
static vftp_link_t s_link;
static vftp_client_t s_cli;
static uint8_t s_data[BURST_SIZE];

static bool Same( const char *a, const char *b ) {
    uint32_t la = 0;
    uint32_t lb = 0;
    const uint8_t *da = vftp_ramfs_get( &s_cliRam, a, &la );
    const uint8_t *db = vftp_ramfs_get( &s_cliRam, b, &lb );
    return da != NULL && db != NULL && la == lb && memcmp( da, db, la ) == 0;
}

bool VftpSelfTest_Run( vftp_burst_server_t *burst ) {
    const portTickType t0 = xTaskGetTickCount();
    vftp_storage_ram_init( &s_srvFs, &s_srvRam, s_srvPool, POOL );
    vftp_storage_ram_init( &s_cliFs, &s_cliRam, s_cliPool, POOL );
    const vftp_server_cfg_t cfg = { &s_srvFs, ( uint16_t ) VFTP_CSP_MAX_PACKET, 150U }; // the real services' MTUs
    bool ok = vftp_burst_server_init( burst, &cfg ) == VFTP_OK && vftp_poll_server_init( &s_poll, &cfg ) == VFTP_OK;

    memset( &s_lb, 0, sizeof( s_lb ) );
    s_lb.burst = burst;
    s_lb.loss_up = 100U; // per mille
    s_lb.loss_down = 100U;
    s_lb.seed = 0x5A7E1172U;
    vftp_loopback_init( &s_link, &s_lb, 150U, ( uint16_t ) VFTP_CSP_MAX_PACKET );

    vftp_client_cfg_t ccfg;
    vftp_client_default_cfg( &ccfg );
    ccfg.link = &s_link;
    ccfg.fs = &s_cliFs;
    ccfg.retries = 10U;
    ccfg.max_stall = 12U;
    ok = ok && vftp_client_init( &s_cli, &ccfg ) == VFTP_OK;

    uint32_t x = 0x1234567U;
    for( uint32_t i = 0; i < BURST_SIZE; i++ ) {
        x = x * 1103515245U + 12345U;
        s_data[i] = ( uint8_t ) ( x >> 16 );
    }
    ok = ok && vftp_ramfs_put( &s_cliRam, "b.bin", s_data, BURST_SIZE ) == VFTP_OK &&
         vftp_ramfs_put( &s_cliRam, "p.bin", s_data, POLL_SIZE ) == VFTP_OK;

    // burst, cut off mid-upload, then resumed from the sidecar
    s_lb.die_after_tx = 120U;
    const vftp_ret_t cut = vftp_burst_upload( &s_cli, "b.bin", "d/b.bin" );
    vftp_loopback_init( &s_link, &s_lb, 150U, ( uint16_t ) VFTP_CSP_MAX_PACKET );
    s_lb.die_after_tx = 0U;
    const vftp_ret_t up = vftp_burst_upload( &s_cli, "b.bin", "d/b.bin" );
    const uint32_t resent = s_cli.stats.data_tx;
    const vftp_ret_t down = vftp_burst_download( &s_cli, "d/b.bin", "b.back" );
    ok = ok && cut == VFTP_LINK && up == VFTP_OK && down == VFTP_OK && Same( "b.bin", "b.back" );
    // free its slot: the poll download needs two (p.back + its .VFP resume sidecar) of the 4
    ok = ok && s_cliFs.remove( s_cliFs.ctx, "b.back" ) == VFTP_OK;

    // poll
    s_lb.burst = NULL;
    s_lb.poll = &s_poll;
    const vftp_ret_t pup = vftp_poll_upload( &s_cli, "p.bin", "d/p.bin" );
    const vftp_ret_t pdown = vftp_poll_download( &s_cli, "d/p.bin", "p.back" );
    ok = ok && pup == VFTP_OK && pdown == VFTP_OK && Same( "p.bin", "p.back" );

    const unsigned long ms = ( unsigned long ) ( ( xTaskGetTickCount() - t0 ) * portTICK_RATE_MS );
    VFTP_LOG_INFO( "self-test %s: burst %u B cut+resume/%s (resent %lu of %lu chunks)/down %s, poll %u B up %s/down %s, "
                   "10%% loss each way, lost up %lu down %lu, %lu ms",
                   ok ? "PASS" : "FAIL", ( unsigned int ) BURST_SIZE, vftp_strerror( up ), ( unsigned long ) resent,
                   ( unsigned long ) ( ( BURST_SIZE + 143U ) / 144U ), vftp_strerror( down ), ( unsigned int ) POLL_SIZE,
                   vftp_strerror( pup ), vftp_strerror( pdown ), ( unsigned long ) s_lb.up_lost,
                   ( unsigned long ) s_lb.down_lost, ms );
    return ok;
}

#endif // SAT_BOOT_SELFTESTS
