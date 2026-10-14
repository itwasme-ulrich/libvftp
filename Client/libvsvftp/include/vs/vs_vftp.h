/**
  ******************************************************************************
  * @file    lib/libvsvftp/include/vs/vs_vftp.h
  * @brief   libvftp link over CSP: one CSP connection per vftp call.
  *
  *          vftp client --vftp_link_t send / recv--> vs_vftp_csp_link_t --csp_send / csp_read--> server node
  *
  *          Context: the calling thread; every libcsp call under vs_csp_lock().
  ******************************************************************************
  */
#ifndef VS_VFTP_H
#define VS_VFTP_H

#include <stdbool.h>
#include <stdint.h>

#include <csp/csp.h>

#include "vftp/vftp_link.h"

#ifdef __cplusplus
extern "C" {
#endif

// Link budget agreed with the vftp server, worst case (CSP + CRC32 + RDP + security header): ground to space
// 150 B, space to ground 211 B = 224-byte CSP buffer - RDP header 5 - CRC32 4 - KISS CRC 4. The server uses
// the same values.
#define VS_VFTP_MTU_UP 150u
#define VS_VFTP_MTU_DOWN 211u

typedef struct {
    uint8_t host;
    uint8_t port;
    uint32_t opts;       // CSP_O_RDP / CSP_O_CRC32
    uint32_t timeout_ms; // connect
    bool (*cancel)(void * ctx); // optional: returning true makes the link report broken
    void * cancel_ctx;          // passed to cancel
    csp_conn_t * conn;
} vs_vftp_csp_link_t;

// Sets link up to reach port on host with opts (CSP_O_RDP, CSP_O_CRC32), timeout_ms to connect.
// cancel(cancel_ctx) (optional) makes the link report broken: the transfer stops, and resumes when run again.
void vs_vftp_csp_link_init(vftp_link_t * link, vs_vftp_csp_link_t * ctx, uint8_t host, uint8_t port, uint32_t opts,
                           uint32_t timeout_ms, bool (*cancel)(void * ctx), void * cancel_ctx);

#ifdef __cplusplus
}
#endif
#endif
