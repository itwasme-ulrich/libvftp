/**
  ******************************************************************************
  * @file    lib/libvsvftp/include/vs/vs_vftp_cmd.h
  * @brief   The `fburst` and `fpoll` commands of the vcli shell.
  *
  *          vcli --fburst / fpoll ...--> cmd/vs_vftp_cmd.c --libvftp client + vs_vftp link--> vftp server
  ******************************************************************************
  */
#ifndef VS_VFTP_CMD_H
#define VS_VFTP_CMD_H

#ifdef __cplusplus
extern "C" {
#endif

// Registers `fburst` and `fpoll` under the "Services" heading of help. Call once, after vcli_init().
void vs_vftp_register_commands(void);

#ifdef __cplusplus
}
#endif
#endif
