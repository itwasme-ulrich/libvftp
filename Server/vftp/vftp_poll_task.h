/**
  ******************************************************************************
  * @file    middleware/vftp/vftp_poll_task.h
  * @brief   vftp_poll on this OBC: libvftp's poll server engine on CSP port 8,
  *          storage on the SD card, in one dedicated task.
  ******************************************************************************
  */
#ifndef VFTP_POLL_TASK_H
#define VFTP_POLL_TASK_H

// Boot only, after CspApp_Init() and FsIo_Init().
void VftpPollTask_Create( void );

#endif // VFTP_POLL_TASK_H
