# Porting and integration {#porting}

[TOC]

## 1. What to copy

The sources are split into three libraries, one folder each:

| Library | Folder | Files | Needed by |
|---|---|---|---|
| `libvftp_core` | `src/common/` | `vftp_crc32.c`, `vftp_wire.c`, `vftp_common.c` | both sides |
| `libvftp_client` | `src/client/` | `vftp_client.c` | a ground station, or an OBC that pulls/pushes files |
| `libvftp_server` | `src/server/` | `vftp_burst_server.c`, `vftp_poll_server.c`, `vftp_server_util.c` | the side that serves files (the OBC) |

| Use | Copy |
|---|---|
| Server only | `include/`, `src/common/`, `src/server/` |
| Client only | `include/`, `src/common/`, `src/client/` |
| Both | `include/`, `src/` |

The public headers (`include/vftp/`, the wire format included) exist once and are shared by both sides: never keep a separate copy per side, or the two ends drift apart. `make split-check` links the client and the servers each with the core alone, so a dependency across the line fails the build.

Include path: `include/` only (user code writes `#include "vftp/vftp.h"`); the library's private headers are included by relative path. The core needs libc only: `memcpy`/`memset`/`memmove`/`memcmp`, `strlen`/`strcmp`/`strchr`/`strrchr`/`strncpy`, `snprintf`. No `malloc`, no time, no OS. C99; builds cleanly with gcc 4.7 (ARM, the firmware's strict flags), gcc 11 `-std=c99 -pedantic`, and from C++ (headers have `extern "C"`).

Then write the ports:

| Side | Write | Samples |
|---|---|---|
| Both | `vftp_storage_t`: real files | `port/storage_posix`, `port/storage_ram`, firmware `vftp_storage_fsio.c` |
| Client | `vftp_link_t`: send/receive datagrams | `vftp_link_csp1.c` (v-client), `port/ccsds_spp/vftp_spp_udp.c`, `port/link_loopback` |
| Server | A transport loop following the 3 rules | firmware `vftp_csp_serve.c` (CSP 2.1), `port/ccsds_spp/vftp_spp_udp.c` (UDP) |

## 2. Configuration

The defaults in `include/vftp/vftp_config.h` suit a PC (files up to 1 GB). On an MCU, put a `vftp_user_config.h` on the include path and build with `-DVFTP_HAVE_USER_CONFIG=1`:

```c
/* vftp_user_config.h -- the SAT firmware's, as an example */
#define VFTP_CFG_PACKET_MAX 256u               /* >= the largest MTU of any link */
#define VFTP_CFG_BURST_MAX_CHUNKS 8388608u     /* 1 MiB bitmap: a ~1.2 GB file at chunk 144 */
#define VFTP_CFG_BURST_BUF_SIZE ( 32u * 1024u ) /* write-behind / read-ahead / CRC scratch */
#define VFTP_CFG_STAGE_MAX_BYTES 4096u         /* staged (no resume) only if this small...       */
#define VFTP_CFG_STAGE_MAX_CHUNKS 32u          /* ...and this few packets: a restart stays cheap */
#define VFTP_CFG_POLL_BUF_SIZE 4096u
#define VFTP_CFG_CRC_STEP_BYTES ( 512u * 1024u )
#define VFTP_CFG_CLIENT_MAX_CHUNKS 4096u       /* the OBC is rarely a client */
#define VFTP_CFG_CLIENT_BUF_SIZE 1024u
#define VFTP_LOG_INFO( fmt, ... ) LOG_INFO( "[vftp] " fmt "\r\n", ##__VA_ARGS__ )
#define VFTP_LOG_WARN( fmt, ... ) LOG_WARNING( "[vftp] " fmt "\r\n", ##__VA_ARGS__ )
```

### RAM sizes (measured with `sizeof`) {#ram}

| Configuration | `vftp_burst_server_t` | `vftp_poll_server_t` | `vftp_client_t` |
|---|---|---|---|
| Defaults (1 GB files) | 1 083 152 B | 2 748 B | 1 053 892 B |
| `BURST_MAX_CHUNKS=200000`, `CLIENT_MAX_CHUNKS=4096` | 59 576 B | 2 748 B | 5 828 B |
| Small MCU: `PACKET_MAX=128`, `BURST_MAX_CHUNKS=8192`, `BURST_BUF=2048`, `POLL_BUF=256`, `CLIENT_MAX_CHUNKS=1024`, `CLIENT_BUF=256` | 4 080 B | 1 180 B | 1 012 B |

Measured on the ARM926 (gcc 4.7). Each instance holds a received and a reply `vftp_msg_t` (636 B with the defaults, sized for a full LIST_REPLY); that is most of `vftp_poll_server_t`.

Code: about 26 KB at `-Os` for the whole library (common + client + server), about 17 KB for a server-only build (common + server). Deepest stack frame 408 B (`vftp_burst_upload`).

Largest burst upload = `BURST_MAX_CHUNKS × chunk_size`, largest burst download = `CLIENT_MAX_CHUNKS × chunk_size`. Poll has no bitmap limit.

## 3. Writing `vftp_storage_t`

The functions take a path and keep no state between calls: the library never holds a file open across packets, so a link that drops at any moment leaves no dangling handle. The full contract is in `vftp_storage.h`. Minimal skeleton:

```c
#include "vftp/vftp_storage.h"

static vftp_ret_t my_stat( void *ctx, const char *path, uint32_t *size )
{   /* directory -> VFTP_ISDIR, missing -> VFTP_NOENT, file -> *size */ }
static vftp_ret_t my_read( void *ctx, const char *path, uint32_t off, uint8_t *buf, uint16_t max, uint16_t *got )
{   /* open, seek(off), read up to max, close; *got = 0 at end of file (not an error) */ }
static vftp_ret_t my_write( void *ctx, const char *path, uint32_t off, const uint8_t *d, uint16_t len )
{   /* open for writing WITHOUT truncating (create if missing), seek(off), write, flush, close */ }
static vftp_ret_t my_truncate( void *ctx, const char *path ) { /* create, or cut to 0 bytes */ }
static vftp_ret_t my_remove( void *ctx, const char *path ) { /* delete the file */ }
static vftp_ret_t my_dirent( void *ctx, const char *dir, uint32_t i, vftp_dirent_t *out )
{   /* i-th entry, skipping "." and ".."; VFTP_NOENT past the last one.
     * memset(out, 0) first, then: name (cut to 13 characters, name_cut = true if it was longer),
     * size, is_dir, mtime in seconds since 1970 UTC (0 = unknown; FAT: vftp_fat_to_unix()). */ }

static const vftp_storage_t s_fs = { my_stat, my_read, my_write, my_truncate, my_remove, my_dirent,
                                     NULL /* walk */, NULL /* rename */, NULL /* name_ok */, NULL /* ctx */ };
```

Common mistakes when writing a port:

| Point | Why it matters |
|---|---|
| `write` must not truncate | Write-behind and resume write into the middle of a file. Opening with mode `"w"` destroys data |
| `stat` of a directory must return `VFTP_ISDIR` | Otherwise a directory is downloaded as a 0-byte file  |
| Provide `walk` when the filesystem rescans from the start on every lookup (FAT `findfirst`/`findnext`) | Without it, listing n entries costs O(n²) directory reads. That was the old `ls ./VOBC` hang |
| The `walk` callback must not call back into storage | The port may be holding its filesystem mutex while walking |
| Provide `rename` so `LIST.TXT` is published atomically | Without it LIST.TXT is written in place, and someone may download it half-written |
| On FAT without long names, implement `name_ok` with `vftp_path_is_83()` | A bad name is refused at the request instead of failing after the whole upload |
| Entry order must be stable while the directory does not change | `download_index n` relies on that order |
| Clear the `vftp_dirent_t` (memset) before filling it, and set `name_cut` when a name is cut | Fields added later default to 0; a cut name is flagged so the ground does not ask for a wrong path |
| `mtime` is Unix seconds (UTC), not the filesystem's raw format | Use `vftp_fat_to_unix(date, time)` for FAT words, or `vftp_unix_time(y, m, d, h, mi, s)`; 0 when unknown |

Test a port without a real link through `port/link_loopback`: client >> loopback >> server >> **your storage** in one process, with simulated loss. See `tests/test_vftp.c` and the firmware's `deploy/host/tests/test_vftp_storage_fsio.c`.

## 4. Writing `vftp_link_t` (client side)

The transport only has to deliver whole datagrams or nothing. Loss, duplication and reordering are fine, since vftp has its own ARQ. The real `vftp_link_csp1.c` (v-client), condensed:

```c
static int l_open( void *ctx )  { c->conn = csp_connect( CSP_PRIO_NORM, c->host, c->port, c->timeout_ms, c->opts );
                                  return c->conn ? 0 : -1; }
static void l_close( void *ctx ) { csp_close( c->conn ); }
static int l_send( void *ctx, const uint8_t *pkt, uint16_t len )
{   csp_packet_t *p = csp_buffer_get( len );   memcpy( p->data, pkt, len );  p->length = len;
    if ( !csp_send( c->conn, p, c->timeout_ms ) ) { csp_buffer_free( p ); return -1; }
    return 0; }
static int l_recv( void *ctx, uint8_t *buf, uint16_t cap, uint32_t timeout_ms )
{   csp_packet_t *p = csp_read( c->conn, timeout_ms );
    if ( p == NULL ) return 0;                       /* timeout */
    int n = p->length <= cap ? p->length : 0;        /* too long: dropped, never truncated */
    memcpy( buf, p->data, n );  csp_buffer_free( p );  return n; }
...
link->mtu_tx = 150; link->mtu_rx = 211;              /* the link's real budget, see protocol.md 5.5 */
```

**The MTU is the most important setting.** Use the smallest limit along the whole path (stack buffers, RDP/CRC/KISS trailers, radio frame size), not a default. An oversized packet may be dropped without any error (the download timeouts of 2026-10-06).

## 5. The transport loop (server side)

The three rules are in [architecture.md section 6](architecture.md). Skeleton for a connection-oriented transport (CSP):

```c
static int send_on_conn( void *ctx, const uint8_t *pkt, uint16_t len )
{   /* get a buffer (bounded wait), copy, send on the connection; 0 = ok */ }

for ( ;; ) {
    csp_conn_t *conn = csp_accept( sock, 1000 );
    if ( conn == NULL ) continue;
    for ( ;; ) {
        uint32_t idle = vftp_burst_server_busy( &srv ) ? 60000 : 5000;
        csp_packet_t *p = csp_read( conn, idle );
        if ( p == NULL ) break;                                   /* timeout / peer gone */
        vftp_srv_action_t a = vftp_burst_server_handle( &srv, p->data, p->length, send_on_conn, conn );
        csp_buffer_free( p );
        if ( a == VFTP_SRV_CLOSE ) break;                         /* rule 2 */
    }
    vftp_burst_server_abort( &srv );                              /* rule 3 (harmless after CLOSE) */
    csp_close( conn );
}
```

The real one on the OBC is `vftp_csp_serve.c`. It adds: 12 CSP buffers kept free for ACKs and ping, a buffer wait of at most ~2 s, and the RDP lock. For a connectionless transport (UDP, SPP) see `vftp_spp_udp.c`: it aborts a session whose `activity` has not moved for `idle_ms`.

The timeout while a session is open (`idle_ms`) must exceed the link's worst round trip, RDP retransmissions included. The timeout with no session open should be short (~5 s): without RDP the server cannot know the client left, and while it holds that connection the next client cannot be accepted.

## 6. Adding it to a build

**Make**

```make
VFTP ?= third_party/libvftp
CFLAGS += -I$(VFTP)/include -DVFTP_HAVE_USER_CONFIG=1 -Iconfig
SRCS += $(wildcard $(VFTP)/src/common/*.c) $(wildcard $(VFTP)/src/server/*.c)   # a server (OBC)
# SRCS += $(wildcard $(VFTP)/src/common/*.c) $(wildcard $(VFTP)/src/client/*.c) # a client (ground)
```

Or use libvftp's own Makefile: `make lib` builds `build/libvftp_core.a`, `build/libvftp_client.a` and `build/libvftp_server.a`; link the side you need plus the core.

**CMake** (as in the SAT firmware)

```cmake
file(GLOB VFTP_CORE   libvftp/src/common/*.c)
file(GLOB VFTP_CLIENT libvftp/src/client/*.c)
file(GLOB VFTP_SERVER libvftp/src/server/*.c)
add_library(vftp_core STATIC ${VFTP_CORE})
target_include_directories(vftp_core PUBLIC libvftp/include ${CONFIG_DIR})
target_compile_definitions(vftp_core PUBLIC VFTP_HAVE_USER_CONFIG=1)
add_library(vftp_server STATIC ${VFTP_SERVER})
target_link_libraries(vftp_server PUBLIC vftp_core)
add_library(vftp_client STATIC ${VFTP_CLIENT})
target_link_libraries(vftp_client PUBLIC vftp_core)
```

The SAT firmware lists the files directly: `common/` + `server/` always, `client/` (with the RAM storage and loopback ports) only when `SAT_BOOT_SELFTESTS` is on, for the boot self-test. A flight image carries no client code.

**waf** (v-client, `lib/libvftp/wscript`): compiles `src/common/*.c`, `src/client/*.c` and `port/storage_posix/*.c` (no server code on the ground), exports `include` and `port/storage_posix`.

**F´**: see [fprime.md](fprime.md).

**Keeping copies in sync**: in the SAT repository, `tools/vendor.sh` copies `include/ src/ port/ tests/` into the firmware and v-client; `--check` reports any copy that drifted. Always edit the canonical `libvftp/`, never a copy.

## 7. Other transports

### CCSDS Space Packet

Already done and tested: `port/ccsds_spp` (`make demo`). Each vftp message is the user data of one unsegmented Space Packet; the service is selected by APID (burst `0x109`, poll `0x108`); the client sends TC (type 1), the server answers TM (type 0).

On a real link (TC/TM Transfer Frames) you only write the send/receive that puts SPPs into frames. Things to watch:
- MTU = the largest frame minus its headers.
- Client>>server: at least 12 bytes + the longest path (UPLOAD_REQUEST, 67 for a 55-character path); 128 or more recommended.
- SPP has no connections, so apply rule 3 through `activity`.

vftp is **not CFDP**. If you must interoperate with systems that speak CFDP, use CFDP.

### Modbus

Modbus is master/slave: every request gets exactly one response, and a slave cannot send on its own.

- **Poll fits naturally**: client = master, server = slave. Put each poll message in a user-defined function code (65–72 or 100–110). A PDU is at most 253 bytes, so the MTU is about 250. In the two cases where the engine sends no reply (`DONE`, an invalid `GET_CHUNK`), the port must answer an empty response itself, and the client side treats an empty response as "nothing yet" (`recv` returns 0).
- **Burst does not map directly**, because one download request produces a stream of DATA. It needs a "mailbox" port: the slave queues outgoing packets (about `window × MTU` RAM) and the master keeps polling for them. Burst then runs at roughly poll speed, so prefer poll on Modbus.

### Any other datagram transport (UDP, CAN with fragmentation, framed UART...)

You need: send/receive one whole packet, with its length, with a timeout. Ordering and reliability are not required. If the transport splits packets into frames (CAN), reassembly is the port's job.

## 8. Checklist for a new platform

1. `make test` on a PC: the core works.
2. Write the storage port, then exercise it through `link_loopback` (upload/download/resume/list) on the target itself (a boot self-test like the OBC's `vftp_selftest.c`).
3. Write the link port, then work out the real end-to-end MTU.
4. Server: exactly the 3 rules, a session timeout longer than the worst round trip, buffers kept free for ACKs and commands.
5. Allocate instances statically; check stack (the core needs ~400 B at most) and RAM against the table above.
6. Run end to end over the real link (the SAT hardware test checklist is an example).
