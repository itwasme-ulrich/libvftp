/**
  ******************************************************************************
  * @file    middleware/vftp/vftp_selftest.h
  * @brief   Boot self-test of libvftp on the target itself: the real client and
  *          both real server engines, through libvftp's in-RAM impaired loopback
  *          (10% loss each way) on RAM filesystems -- so the protocol is
  *          exercised on this CPU and this RTOS without any ground station.
  ******************************************************************************
  */
#ifndef VFTP_SELFTEST_H
#define VFTP_SELFTEST_H

#include <stdbool.h>

#include "vftp/vftp_server.h"

// Borrows `burst` (the service's own engine, before it is configured for the SD
// card) and logs one PASS/FAIL line. SAT_BOOT_SELFTESTS builds only.
bool VftpSelfTest_Run( vftp_burst_server_t *burst );

#endif // VFTP_SELFTEST_H
