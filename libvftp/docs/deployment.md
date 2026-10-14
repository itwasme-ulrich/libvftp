# Ground station, OBC and operations {#deployment}

[TOC]

## 1. Ground station (v-client / csp-client)

### 1.1 Transmit/receive chain
On the bench: `./build/csp-client -a1 -k/dev/ttyUSB0,speed=115200` (ground station = node 1, OBC = node 2).

### 1.2 Commands

| Command | Does |
|---|---|
| `fburst server <node> [chunk] [rdp 0/1] [crc32 0/1]` | Selects the OBC, chunk size (0 = largest), CSP options |
| `fburst opts [timeout_ms] [retries] [window] [gap_ms] [window_up]` | Defaults: 10000, 3, 64, 2000, 12 |
| `fburst upload <local> <remote>` / `download <remote> [local]` | Resumable: run the same command again to continue. A download keeps its state in `XXXXXXXX.VFD` (poll: `.VFP`) next to the local file, deleted when done |
| `fburst ls <dir>` / `count <dir>` / `download_index <dir> <n> [local]` | Listings. Directories print as `NAME/`. Pages are by index: for a consistent snapshot of a directory that is changing, use `list_regen` |
| `fburst list_regen <dir> [local] [down\|nodown]` | The server writes `LIST.TXT` in its root; `down` also fetches and prints it |
| `fburst rm <remote>` | Deletes a file and its resume state |
| `fpoll server/opts/upload/download/count/download_index` | Same, stop-and-wait; upload and download both resumable |

The progress bar shows the measured speed and the ETA. The summary line shows rounds, DATA packets against chunks needed (percent resent) and retries. **If retries are close to the number of rounds**, every round is losing a full timeout: check `window_up` (section 4).

### 1.3 Ground CSP settings

- RDP: `csp_rdp_set_opt(6, 30000, 16000, 1, 8000, 3)`: window 6, conn_timeout 30 s, packet_timeout 16 s, delayed ACKs. The OBC adopts these values (it copies them from the SYN), so **the ground's conn_timeout decides how long the OBC keeps a closed connection**.
- The source port is remembered across restarts (`~/.vclient_csp_sport`, or `$VCLIENT_SPORT_FILE`): a restarted csp-client never reuses a port the OBC still holds. Full notes at  "port" in `csp_conn.c`.
- vftp MTU: 150 up, 211 down (`vftp_link_csp1.h`).

### 1.4 Estimated RF throughput

A downlink DATA packet carries 205 B of data + 6 B vftp header + 4 B CSP header (+5 RDP, +4 CRC32) + ~20 B (header, FCS, flags, bit stuffing) ≈ 240 bytes on air.

| Rate | One packet | Downlink data | One 10-minute pass |
|---|---|---|---|
| 1200 bps | ~1.6 s | ~130 B/s (~0.47 MB/hour) | ~75 KB |
| 9600 bps | ~0.2 s | ~1 KB/s (~3.7 MB/hour) | ~600 KB |

Estimates only, before link errors and STATUS rounds. A large file spans several passes: resume handles that, just run the same command on the next pass. Uploads (144-byte chunks) are proportionally slower.

## 2. OBC as server

### 2.1 Components

| Item | Value |
|---|---|
| Tasks | `VftpBurst` (port 9), `VftpPoll` (port 8): priority idle+1, stack 4×min, **no health ID** (a stuck vftp is no reason to reset the OBC) |
| Instances | `static` in each task: burst ~1.1 MB (1 MiB bitmap + 32 KiB buffer), poll ~6 KB (4 KiB read buffer) |
| Storage | `vftp_storage_fsio.c` >> FsIo >> FAT (no long names): with `walk`, `rename`, `name_ok` (8.3) |
| MTU | Down 211 (`VFTP_CSP_MAX_PACKET`, enforced at compile time with `#error`), up 150 |
| Conversation timeouts | burst: 60 s with a session open / 5 s without; poll: 20 s / 5 s |
| Incremental CRC | 512 KiB per request |
| Sending | Keeps 12 of 48 CSP buffers free; waits ~2 s at most for a buffer; the TRX waits up to 5 s for a free slot |
| RDP | `CspRdpLock` is **released while waiting for the window** (fixes the whole-CSP deadlock) |
| Self-test | The dev build runs client + server over a lossy RAM loopback at boot: log `[vftp] self-test PASS` |

### 2.2 Behaviour in each situation

| Situation | What happens |
|---|---|
| Client vanishes mid-transfer (RDP) | After `conn_timeout` (30 s) the connection closes, `abort()`, the service is free again. The upload sidecar is kept |
| Client vanishes (no RDP) | After 60 s (burst) / 20 s (poll) without packets, `abort()`. Meanwhile a new client waits or times out |
| OBC reboots during an upload | The next upload resumes from `.VFM` / `.VFI` in the root, counting only chunks really on the card |
| Single card error | That chunk is requested again; the user sees nothing |
| Card keeps failing / full | The client gets `storage I/O error` or `no space` and stops. What was written is kept |
| Two clients at once | Each service serves one connection at a time. The second waits to be accepted (the client usually times out). Another request within the same session gets `BUSY` |
| Upload with a non-8.3 name, or onto a directory | Refused at once (`BADNAME`, `ISDIR`), nothing truncated |
| Download of a directory | `ISDIR` (never an empty file) |
| Power loss while writing `LIST.TXT` | The old `LIST.TXT` is intact; the partial `LISTTMP.TXT` is overwritten next time |
| More than ~18 RDP sessions within 30 s | `CSP_CONN_MAX` 20 fills with connections lingering in CLOSE_WAIT (`FAST_CLOSE=0`): new connections are refused until some expire. Only happens with bench scripts |

### 2.3 Files vftp creates on the card

`XXXXXXXX.VFM`, `XXXXXXXX.VFI` (upload resume, deleted when complete), `LIST.TXT` (on each `list_regen`), `LISTTMP.TXT` (only while writing). All in the root. Deleting a `.VFM`/`.VFI` drops that resume state (the next upload starts over).

## 3. OBC as client

1. **A link port for libcsp 2.1.** Like `vftp_link_csp1.c`, with three differences:
   - `csp_send()` in 2.1 returns nothing;
   - send through `CspRdpLock_Send()` when RDP is used;
   - the MTU follows the same 211/224 budget as the server side.

   ```c
   static int l_send( void *ctx, const uint8_t *pkt, uint16_t len )
   {
       Link_t *c = ctx;
       csp_packet_t *p = csp_buffer_get( len );     /* may wait briefly and keep a reserve, like SendOnConn */
       if ( p == NULL ) return -1;
       memcpy( p->data, pkt, len );
       p->length = len;
       CspRdpLock_Send( c->conn, p );               /* always consumes the packet */
       return 0;
   }
   ```
2. **A dedicated task.** Every call blocks until done, possibly for minutes. A single wait can last `timeout_ms × retries` without any event, so do not tie this task to a 5 s health window: use `TASK_NO_HEALTH_ID`/degradable, or check in from `on_event` with a timeout shorter than the health window.
3. **Configuration.** `VFTP_CFG_CLIENT_MAX_CHUNKS` is 4096 today, i.e. downloads up to ~840 KB at 205-byte chunks. Raise it for larger files (8 chunks per byte of RAM). Storage: reuse `VftpStorageFsio_Get()`.
4. **Behaviour.** Exactly as on the ground: download resume through a `.VFD` (burst) or `.VFP` (poll) **next to the destination file** on the card, end-to-end CRC, loss tolerant. Client and server can run on the same OBC (separate instances and tasks).

## 4. Constraints to remember

| Constraint | Value | Why |
|---|---|---|
| Largest downlink packet | **211 B** | 224 − RDP 5 − CRC32 4 − KISS CRC 4. Larger ones are dropped silently |
| Largest uplink packet | 150 B | Uplink budget agreed with the OBC |
| `window_up` | **≤ 12** (≤ 8 against an OBC image older than 2026-10-08) | `window_up` DATA + 1 STATUS_REQUEST arrive back to back: they must fit the OBC router's input queue (`CSP_QFIFO_LEN`, 32; it was 10) and the connection's receive queue (`CSP_CONN_RXQUEUE_LEN`, 16) |
| RX queue vs RDP window | RXQUEUE 16 > 2 × window 6 | Needed for the receiver to send standalone ACKs (the VCSP fork needs only 1 × window) |
| Client timeout | ≥ 10 s | Trip at 1200 bps plus card work |
| Closed RDP connections | Kept 30 s on both ends | `FAST_CLOSE=0` (fleet-wide setting) |
| Concurrent connections on the OBC | 20 (`CSP_CONN_MAX`) | Includes connections lingering in CLOSE_WAIT |
| Path / name | 55 / 13 characters (8.3 on the OBC) | ICD limits (length-prefixed on the wire) |
| File size | < 4 GiB; burst upload ≤ 8 388 608 chunks | 32-bit sizes; bitmap size |

## 5. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Uploads work but **every download times out**; `ls` works | Downlink packet > 211 B: the KISS CRC cannot be appended and the ground drops the frame | New firmware and csp-client (MTU 211) |
| Uploads very slow, retries ≈ rounds | `window_up` larger than the OBC's receive queue; the STATUS_REQUEST is dropped | `fburst opts 10000 3 64 2000 12` |
| Some uploads stall 10 s once in a while (an OBC image older than 2026-10-08) | its router input queue held 10 frames: frames 11..13 of a 12-chunk round were dropped when the router lagged | `fburst opts 10000 3 64 2000 8`, or update the OBC |
| `server refused: name not valid` | the path is not 8.3 (the OBC's FAT has no long names); before 2026-10-08 the OBC silently cut it and served or deleted another file | use NAME.EXT with ≤ 8 + 3 characters in every path component |
| First command after restarting csp-client stalls 16 s or fails after 30 s (RDP) | A source port the OBC still holds was reused | Fixed (`~/.vclient_csp_sport`); or wait 30 s |
| `No more connections available` on the OBC | Too many RDP sessions within 30 s | Space the commands, or turn RDP off for scripts |
| A scripted csp-client over ZMQ loses its first packet | ZMQ "slow joiner" (test harness only) | `sleep 1` before the first command |
| `service busy with another transfer` | A session is still open | Wait for its timeout (≤ 60 s), or run the same command to resume that session |
| `CRC MISMATCH` after a resume | The server file changed (same name, same size) | Run again: the resume state was dropped, this run gives a clean copy |
| `ls` shows `NAME~`; `download_index` says `name cut` | The server holds a name longer than 13 characters (not possible on the OBC's 8.3 FAT) | Download it by its full path |
| `name not valid on the server's filesystem` | Not an 8.3 name | Rename: at most 8 characters + `.` + 3 |
| `is a directory, not a file` | The path is a directory | |
| `storage I/O error` on upload | Card failing or full (2 write errors in a row) | `fs_df`, check the card; what was written is kept |
| OBC unreachable after aborting an RDP transfer | (Old firmware) the RDP lock deadlocked CSP | Firmware with `CspRdpLock_WaitUnlocked` |

When reporting a problem, include: the csp-client output, the OBC UART log (`[vftp]` and `RDP` lines), `csp_status` and `perf_tasks`, and the `server`/`opts` settings in use.
