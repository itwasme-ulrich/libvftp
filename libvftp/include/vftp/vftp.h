/**
 * @file vftp.h
 * @brief libvftp umbrella header.
 *
 * Layering (each layer only knows the ones below it):
 * - vftp_client.h / vftp_server.h -- protocol engines (burst, poll)
 * - vftp_wire.h -- byte-exact encode/decode, all validation
 * - vftp_link.h / vftp_storage.h -- what a port provides: datagrams, files
 * - vftp_crc32.h, vftp_types.h, vftp_config.h
 *
 * No OS, RTOS, filesystem or transport header is included anywhere in
 * include/ or src/; those live in port/ and in the integrating project.
 */
#ifndef VFTP_H
#define VFTP_H

/** Library version, major (changes when the wire format changes incompatibly). */
#define VFTP_VERSION_MAJOR 2
/** Library version, minor. */
#define VFTP_VERSION_MINOR 0
/** Library version as a string. */
#define VFTP_VERSION_STRING "2.0"

#include "vftp_client.h"
#include "vftp_config.h"
#include "vftp_crc32.h"
#include "vftp_link.h"
#include "vftp_server.h"
#include "vftp_storage.h"
#include "vftp_types.h"
#include "vftp_wire.h"

#endif /* VFTP_H */
