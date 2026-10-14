/**
  ******************************************************************************
  * @file    middleware/vftp/vftp_burst_task.h
  * @brief   vftp_burst on this OBC: libvftp's burst server engine, bound to CSP
  *          port 9 (vftp_csp_serve.c) and to the SD card (vftp_storage_fsio.c),
  *          in one dedicated task. One connection at a time.
  ******************************************************************************
  */
#ifndef VFTP_BURST_TASK_H
#define VFTP_BURST_TASK_H

// Boot only, after CspApp_Init() (csp_init() done) and FsIo_Init().
void VftpBurstTask_Create( void );

#endif // VFTP_BURST_TASK_H
