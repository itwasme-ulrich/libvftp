# Wire protocol (byte by byte) {#protocol}

[TOC]

This page is enough to write a client or a server **without libvftp** (in Python, inside existing ground software, or just to read a hex log). Every hex example below was produced by the library's own encoder (`make frames`, `tools/vftp_frames.c`) or recorded from the real server. The Python example of section 4 was run against the real server, and the CSP/KISS frames of section 5 were checked against csp-client's real KISS decoder.

## 1. Conventions

- **One datagram = one vftp message.** Byte 0 is the `type`; the rest depends on it. Both services share one type table (section 2). A service ignores the types it does not use.
- **Integers are little-endian**, read and written byte by byte. (The CSP header of section 5 is big-endian; do not mix the two layers.)
- **str**: a length byte followed by that many bytes, **no NUL**. A string is always the **last** field of a message (or of a LIST_REPLY entry), so nothing after it moves when its length changes.
  - A **path** holds at most **55** bytes. A longer path, or one containing a `00` byte, makes the whole message invalid.
  - An entry **name** (LIST_REPLY, INDEX_REPLY) holds at most 13 bytes from this server. A decoder that receives a longer name keeps what fits and sets `VFTP_ENTRY_NAME_CUT` (see `flags`).
- **sess**: the session byte. The server picks it (1..255, never 0) when it accepts a DOWNLOAD_REQUEST or UPLOAD_REQUEST, and sends it in the reply. **Every message of that transfer carries it right after the type**, in both directions. A refused request is answered with session 0. See section 2.2.
- **ret**: 1 byte, codes in section 3.
- **flags** (LIST_REPLY and INDEX_REPLY entries): bit 0 `VFTP_ENTRY_DIR` = the entry is a directory; bit 1 `VFTP_ENTRY_NAME_CUT` = the name is only the start of a longer one; other bits 0 (ignore them).
- **mtime**: seconds since 1970-01-01 00:00 UTC (u32, good until 2106); **0 = unknown**. The library has `vftp_unix_time()`, `vftp_fat_to_unix()` and `vftp_unix_to_civil()` for storage ports and for printing.
- **Length**: a packet shorter than its layout is rejected; **trailing extra bytes are ignored**.
- **chunk**: chunk n holds file bytes `[n·cs, min((n+1)·cs, size))`, with `cs` = chunk_size. Chunk count = `ceil(size / cs)`; an empty file has 0 chunks.

## 2. Messages

C = client (ground), S = server (OBC). B = used by the burst service (CSP port 9), P = used by the poll service (CSP port 8).

| type | Name | Svc | Dir | Layout | Length |
|---|---|---|---|---|---|
| `00` | DOWNLOAD_REQUEST | B P | C>>S | type, chunk_size u16, path str | 4 + p |
| `01` | DOWNLOAD_REPLY | B P | S>>C | type, **sess**, ret u8, size u32, crc32 u32, flags u8 (bit 0 = crc32 valid) | 12 |
| `02` | UPLOAD_REQUEST | B P | C>>S | type, size u32, crc32 u32, chunk_size u16, path str | 12 + p |
| `03` | UPLOAD_REPLY | B P | S>>C | type, **sess**, ret u8, resume_from u32 (poll: watermark; burst: 0) | 7 |
| `04` | DATA | B P | both | type, **sess**, chunk u32, bytes… | 6 + n |
| `05` | STATUS_REQUEST | B | C>>S | type, **sess** | 2 |
| `06` | STATUS_REPLY | B | S>>C | type, **sess**, ret u8, total u32, complete u32, entries u16, {next u32, count u32} × entries | 13 + 8·e |
| `07` | CRC_REQUEST | B P | C>>S | type, **sess** | 2 |
| `08` | CRC_REPLY | B P | S>>C | type, **sess**, ret u8, crc32 u32, done u32 | 11 |
| `09` | DONE | B P | C>>S | type, **sess** (no reply) | 2 |
| `0A` | ABORT | B P | C>>S | type, **sess** (no reply) | 2 |
| `0B` | LIST_REQUEST | B | C>>S | type, offset u32, limit u16 (0 = as many as fit), path str | 8 + p |
| `0C` | LIST_REPLY | B | S>>C | type, ret u8, total_files u32, first_file u32, packet_files u16, {flags u8, size u32, mtime u32, name str} × packet_files | 12 + Σ(10 + name) |
| `0D` | DELETE_REQUEST | B | C>>S | type, path str | 2 + p |
| `0E` | DELETE_REPLY | B | S>>C | type, ret u8 | 2 |
| `0F` | LIST_REGEN_REQUEST | B | C>>S | type, path str | 2 + p |
| `10` | LIST_REGEN_REPLY | B | S>>C | type, ret u8 | 2 |
| `11` | INDEX_REQUEST | B P | C>>S | type, index u32 (`FFFFFFFF` = count only), path str | 6 + p |
| `12` | INDEX_REPLY | B P | S>>C | type, ret u8, total u32, index u32, flags u8, size u32, mtime u32, name str | 20 + n |
| `13` | WANT | B | C>>S | type, **sess**, entries u16, {next u32, count u32} × entries | 4 + 8·e |
| `14` | GET_CHUNK | P | C>>S | type, **sess**, chunk u32 | 6 |
| `15` | DATA_ACK | P | S>>C | type, **sess**, chunk u32, ret u8 | 7 |

`p` = path length, `n` = name length (or payload length for DATA), `e` = ranges.

### 2.1 What fits in one packet

| | Formula | OBC downlink (211 B) | OBC uplink (150 B) |
|---|---|---|---|
| DATA payload (largest chunk) | MTU − 6 | **205** | **144** |
| STATUS_REPLY ranges | (MTU − 13) / 8 | 24 | |
| WANT ranges | (MTU − 4) / 8 | | 18 |
| LIST_REPLY entries | as many as fit MTU − 12, each 10 + name bytes | 9 full 8.3 names (12 chars); more with shorter names | |

LIST_REPLY pages are **filled by bytes**: the server adds entries while the next one fits the MTU (and `limit`, if non-zero). Short names give more entries per page. Paging is by index (`offset` = `first_file` + `packet_files` of the previous page); a directory that changes between pages may shift entries. **For a consistent snapshot use LIST_REGEN** (the server writes the whole listing to `LIST.TXT` in one pass) and download that file.

### 2.2 Sessions

The session byte keeps two transfers apart. A client that times out and runs again gets a **new** session, so a packet of the first attempt that arrives late (a delayed DATA, a duplicated DONE) cannot be taken for one of the new transfer.

- Server, transfer open: a message with a session byte that does **not** match the open session is **dropped**: nothing written, no reply, and it does not count as activity (so it cannot keep a dead session alive). A late DONE or ABORT therefore cannot end the current transfer.
- Server, no transfer open: such a message is handled as it always was with nothing open: a request is answered with an error (e.g. INVAL) echoing its session byte, DATA/DONE/ABORT are ignored.
- A repeated identical DOWNLOAD/UPLOAD_REQUEST while that transfer is open is answered with the **same** session (the first reply was lost). A different request gets `BUSY` with session 0.
- Client: a reply whose session does not match the current transfer is ignored, and the client keeps waiting.
- Client: once the server has opened a transfer, the client always ends it with DONE or ABORT, also when it gives up for a local reason (the local file cannot be created, too many chunks). Without that, a transport that does not signal a close (CSP without RDP, UDP, SPP) leaves the server busy until its idle timeout, and every request until then times out. A local directory as destination is refused before the request is sent.
- Sessions go 1, 2, … 255, 1, … and restart at 1 after a server reboot. Nothing gets stuck on a mismatch: the worst case is a timeout, after which the client runs again and resumes from its sidecar.

### 2.3 Hex examples

```
DOWNLOAD_REQUEST     00 CD 00 08 56 54 2F 42 2E 42 49 4E                cs=205 (CD 00), path len 8 "VT/B.BIN"
DOWNLOAD_REPLY       01 2A 00 00 90 01 00 01 70 DF C4 01                sess=2A, ret=OK, size=102400, crc=0xC4DF7001, flags=1
WANT                 13 2A 02 00 00 00 00 00 28 00 00 00 64 00 00 00 18 00 00 00
                                                                         2 ranges: [0,+40) and [100,+24)
DATA                 04 2A 03 00 00 00 48 45 4C 4C 4F C0 DB 21          chunk=3, 8 data bytes
UPLOAD_REQUEST       02 00 90 01 00 01 70 DF C4 90 00 08 56 54 2F 42 2E 42 49 4E
                                                                         size=102400, crc, cs=144 (90 00), "VT/B.BIN"
UPLOAD_REPLY         03 2A 00 00 00 00 00                               sess=2A, ret=OK
UPLOAD_REPLY         03 00 0C 00 00 00 00                               refused: sess=0, ret=12 BADNAME
STATUS_REQUEST       05 2A
STATUS_REPLY         06 2A 00 C8 02 00 00 C1 02 00 00 01 00 BA 02 00 00 07 00 00 00
                                                                         total=712, complete=705, missing [698,+7)
CRC_REQUEST          07 2A
CRC_REPLY (AGAIN)    08 2A 0D 00 00 00 00 00 00 08 00                   ret=13 AGAIN, done=524288
CRC_REPLY (OK)       08 2A 00 01 70 DF C4 00 90 01 00                   ret=OK, crc=0xC4DF7001, done=102400
DONE / ABORT         09 2A / 0A 2A
LIST_REQUEST         0B 00 00 00 00 00 00 04 56 4F 42 43                offset=0, limit=0, "VOBC"
LIST_REPLY           0C 00 0B 00 00 00 00 00 00 00 02 00                ret=OK, total=11, first=0, 2 in this page
                     01 00 00 00 00 E4 09 C5 6A 03 49 4D 47            dir "IMG", mtime 1791298020 = 2026-10-06 14:47:00 UTC
                     00 B8 0B 00 00 E4 09 C5 6A 08 4C 4F 47 31 2E 54 58 54
                                                                         file "LOG1.TXT", 3000 B
DELETE_REQUEST       0D 08 56 54 2F 42 2E 42 49 4E
DELETE_REPLY         0E 01                                              ret=NOENT
LIST_REGEN_REQUEST   0F 04 56 4F 42 43
INDEX_REQUEST(count) 11 FF FF FF FF 04 56 4F 42 43
INDEX_REQUEST (n=2)  11 02 00 00 00 04 56 4F 42 43
INDEX_REPLY          12 00 0B 00 00 00 02 00 00 00 00 B8 0B 00 00 E4 09 C5 6A 08 4C 4F 47 31 2E 54 58 54
                                                                         total=11, index=2, file, 3000 B, "LOG1.TXT"
DOWNLOAD_REPLY       01 07 00 C0 C6 2D 00 00 00 00 00 00                poll: size=3000000, flags=0 (CRC comes from CRC_REQUEST)
GET_CHUNK            14 07 05 00 00 00                                  poll: sess=07, chunk 5
UPLOAD_REPLY         03 07 00 62 00 00 00                               poll: OK, resume_from = 98
DATA_ACK             15 07 62 00 00 00 00                               poll: chunk 98, ret OK
```

### 2.4 Who sends what

- **Burst download**: DOWNLOAD_REQUEST >> DOWNLOAD_REPLY; then rounds of WANT >> a stream of DATA (the client computes the ranges still missing; the first WANT asks for everything); CRC_REQUEST >> CRC_REPLY (repeat while AGAIN); DONE.
- **Burst upload**: UPLOAD_REQUEST >> UPLOAD_REPLY; then rounds of DATA × window, STATUS_REQUEST >> STATUS_REPLY (what is still missing); CRC_REQUEST >> CRC_REPLY; DONE.
- **Poll download**: DOWNLOAD_REQUEST >> DOWNLOAD_REPLY; GET_CHUNK n >> DATA n, one at a time; CRC_REQUEST >> CRC_REPLY; DONE.
- **Poll upload**: UPLOAD_REQUEST >> UPLOAD_REPLY (resume_from); DATA n >> DATA_ACK n, one at a time, starting at resume_from; CRC_REQUEST >> CRC_REPLY; DONE.

STATUS_REPLY only goes server >> client ("I hold `complete` of `total`, these ranges are missing"); WANT only goes client >> server ("send me these ranges"). As with CFDP's NAK PDU, the side receiving the file names what it lacks, in its own message type.

## 3. Return codes (`ret`)

| Code | Name | Meaning |
|---|---|---|
| 0 | OK | |
| 1 | NOENT | No such file or directory |
| 2 | INVAL | Malformed request; chunk_size over the MTU; empty path or path ending in `/`; a session message with no transfer open |
| 3 | NOSPC | Card full, or the file needs more chunks than this build's bitmap |
| 4 | IO | Card read/write error (upload: repeated failures, see architecture) |
| 5 | EXISTS | Resume state belongs to a different transfer |
| 6 | NOTSUP | Not supported |
| 7 | NOMEM | Out of memory |
| 8 | BUSY | The service has another session open |
| 9 | CRC_MISMATCH | CRC differs |
| 10 | RANGE | Index out of range; poll: chunk out of sequence (the ACK's `chunk` is the watermark) |
| 11 | ISDIR | The path is a directory |
| 12 | BADNAME | The server's filesystem cannot name this path exactly (FAT 8.3). Checked for every path a request names (download, delete, list, list_regen, index, upload) before the storage is touched: a FAT driver without long names cuts a longer name to 8.3 and would otherwise act on another file. On the client: a listed name was cut |
| 13 | AGAIN | CRC_REPLY: still computing, ask again; `done` = bytes checked so far |

Codes 64–67 (TIMEOUT, LINK, PROTO, REMOTE) exist only locally on the client and **never appear on the wire**.

## 4. Decoding without the library

A minimal poll download and one LIST page in Python, needing only a send/receive function `xfer` (CSP port 8 for the poll download, port 9 for the list). It shows the format and does not handle losses: a real client must drop a reply whose session does not match and keep waiting, rather than assert. This exact code was run against the library's server.

```python
import struct, zlib

def lstr(s, maxlen=55):             # str: length byte + bytes, no NUL
    b = s.encode()
    assert len(b) <= maxlen
    return bytes([len(b)]) + b

def poll_download(xfer, remote, cs=205):
    # DOWNLOAD_REQUEST -> DOWNLOAD_REPLY, which assigns the session byte
    rep = xfer(struct.pack("<BH", 0x00, cs) + lstr(remote))
    t, sess, ret, size, crc, flags = struct.unpack_from("<BBBIIB", rep)
    assert t == 0x01 and ret == 0, f"server ret={ret}"
    data = bytearray()
    for n in range((size + cs - 1) // cs):           # GET_CHUNK n -> DATA n
        rep = xfer(struct.pack("<BBI", 0x14, sess, n))
        t, s, chunk = struct.unpack_from("<BBI", rep)
        assert t == 0x04 and s == sess and chunk == n  # real code: drop and wait again
        data += rep[6:]
    while True:                                      # CRC_REQUEST, repeat while AGAIN
        rep = xfer(bytes([0x07, sess]))
        t, s, ret, rcrc, done = struct.unpack_from("<BBBII", rep)
        if ret != 13:
            break
    assert ret == 0 and zlib.crc32(data) == rcrc     # CRC-32 IEEE, same as zlib
    xfer(bytes([0x09, sess]), expect_reply=False)    # DONE
    return bytes(data)

def list_page(xfer, path, offset=0):                 # burst port: one LIST_REPLY page
    rep = xfer(struct.pack("<BIH", 0x0B, offset, 0) + lstr(path))
    t, ret, total, first, count = struct.unpack_from("<BBIIH", rep)
    assert t == 0x0C and ret == 0, f"server ret={ret}"
    pos, out = 12, []
    for _ in range(count):
        flags, size, mtime, n = struct.unpack_from("<BIIB", rep, pos)
        name = rep[pos + 10:pos + 10 + n].decode()
        pos += 10 + n
        out.append((name + "/" if flags & 1 else name, size, mtime))
    return total, first, out
```

The file content CRC is **CRC-32/IEEE** (`zlib.crc32`: polynomial 0xEDB88320, init and final xor 0xFFFFFFFF; `"123456789"` >> `0xCBF43926`). `mtime` prints with `datetime.datetime.fromtimestamp(mtime, datetime.timezone.utc)`.

## 5. On the SAT link: CSP v1 + RDP + KISS

A vftp message is the **data** of a CSP packet. The OBC runs libcsp 2.1 with **version 1** headers (`csp_conf.version = 1`); the ground runs libcsp 1.x.

### 5.1 CSP v1 header (4 bytes, big-endian)

| Bits | 31..30 | 29..25 | 24..20 | 19..14 | 13..8 | 7..0 |
|---|---|---|---|---|---|---|
| Field | prio | src | dst | dport | sport | flags |

flags: `0x01` CRC32, `0x02` RDP, `0x08` HMAC (`0x04` XTEA only in 1.x).

Example, ground (node 1, sport 40) >> OBC (node 2) port 9, prio 2: `0x82226800` >> `82 22 68 00`.

### 5.2 Trailers (all after the data, in this order)

| Part | Size | Present when | Content |
|---|---|---|---|
| RDP | 5 B | the connection uses `CSP_O_RDP` | flags u8 (`08` SYN, `04` ACK, `02` EAK, `01` RST), seq u16 **BE**, ack u16 **BE** |
| CSP CRC32 | 4 B | the connection uses `CSP_O_CRC32` | **CRC-32C** (Castagnoli) **big-endian** over the data (RDP trailer included, header excluded) |
| KISS frame CRC32 | 4 B | always, on a KISS link | CRC-32C big-endian over all the data above (header excluded) |

CSP uses **CRC-32C (polynomial 0x82F63B78)**, not the CRC-32 IEEE of vftp. Use that polynomial when decoding frames by hand.

### 5.3 KISS frame

`C0 00` + [CSP header | data | trailers], with every `C0` escaped as `DB DC` and every `DB` as `DB DD`, then a closing `C0`.

### 5.4 Three sample frames (accepted by csp-client's KISS decoder)

```
DOWNLOAD_REQUEST, no RDP/CRC (ground -> OBC):
  csp : 82 22 68 00 | 00 CD 00 08 56 54 2F 42 2E 42 49 4E | D1 EB 2F 24
  kiss: C0 00 82 22 68 00 00 CD 00 08 56 54 2F 42 2E 42 49 4E D1 EB 2F 24 C0              (23 B)

8-byte DATA, session 2A, CRC32 (OBC -> ground, sport 9 -> dport 40):
  csp : 84 1A 09 01 | 04 2A 03 00 00 00 48 45 4C 4C 4F C0 DB 21 | FD 80 C5 8B | 10 39 99 13
                      ^ vftp DATA                                 ^ CSP CRC32   ^ KISS CRC
  kiss: C0 00 84 1A 09 01 04 2A 03 00 00 00 48 45 4C 4C 4F DB DC DB DD 21 FD 80 C5 8B 10 39 99 13 C0
                                                            ^C0->DB DC ^DB->DB DD

8-byte DATA, RDP + CRC32 (seq 0x1234, ack 0x5678):
  csp : 84 1A 09 03 | 04 2A 03 00 00 00 48 45 4C 4C 4F C0 DB 21 | 04 12 34 56 78 | 1D 46 12 4C | D3 EF 59 65
                                                                  ^ RDP: ACK, seq, ack
```

### 5.5 Size budget: why 211 bytes at most

libcsp 2.1 on the OBC puts **every trailer inside the same data area of the buffer** (`CSP_BUFFER_SIZE` = 224 bytes):

```
224  =  vftp (at most 211)  +  RDP 5  +  CRC32 4  +  KISS CRC 4
```

The KISS CRC append (`csp_kiss_tx`) **ignores the out-of-room error**: the frame still goes out, without a CRC, and the ground drops it silently. The vftp downlink MTU is therefore **211** (firmware `VFTP_CSP_MAX_PACKET`, ground `VFTP_CSP1_MTU_DOWN`), i.e. 205-byte download chunks after the 6-byte DATA header. The uplink uses 150 (144-byte chunks).

## 6. A complete session (poll download, 8-byte file, no RDP)

Recorded from the real server:

```
C->S  00 08 00 06 2F 41 2E 42 49 4E                  DOWNLOAD_REQUEST cs=8 "/A.BIN"
S->C  01 01 00 08 00 00 00 11 F1 8D 10 01            sess=01, OK, size 8, crc 0x108DF111, flags=1
C->S  14 01 00 00 00 00                              GET_CHUNK 0
S->C  04 01 00 00 00 00 48 45 4C 4C 4F C0 DB 21      DATA 0 (8 bytes)
C->S  07 01                                          CRC_REQUEST
S->C  08 01 00 11 F1 8D 10 08 00 00 00               OK, crc, done=8
C->S  09 01                                          DONE  (the server closes the connection)
C->S  07 01                                          a late duplicate, after DONE:
S->C  08 01 02 00 00 00 00 00 00 00 00               INVAL (no transfer open), session echoed
```

## 7. Sidecars on storage {#sidecars}

All integers little-endian. `path_crc` = CRC-32 of the path string.

| File | Where | Header (24 bytes) | After the header |
|---|---|---|---|
| `.VFM` (burst upload) | server root | magic `56 54 46 4D` ("VTFM"), version u16=1, chunk_size u16, size u32, crc32 u32, total u32, path_crc u32 | bitmap of `ceil(total/8)` bytes, chunk `i` = byte `i/8`, bit `i%8` |
| `.VFI` (poll upload) | server root | magic "VTFI", version, chunk_size, size, crc32, **committed u32**, path_crc | (nothing) |
| `.VFD` (burst download) | next to the local file | magic "VTFD", version, chunk_size, size, crc32 (0 if unknown), total, path_crc | bitmap |
| `.VFP` (poll download) | next to the local file | magic "VTFP", version, chunk_size, size, crc32 (0 if unknown), **committed u32**, path_crc (of the remote path) | (nothing) |

A sidecar is reused only if magic, version, size, crc32 and path_crc all match (a `.VFP` also needs the local file to still hold its `committed` chunks). chunk_size and total may differ: the data already held is then converted to the new chunk size (architecture.md, section 5).
