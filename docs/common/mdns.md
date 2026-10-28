# mDNS Common Header (`src/common/mdns.h`)

Vendored, public-domain mDNS / DNS-SD single-header library used to announce `kyper` and `whisper-cast` on the LAN.

## What it is
- Upstream: [mjansson/mdns](https://github.com/mjansson/mdns) — "mDNS/DNS-SD library — Public Domain — 2017 Mattias Jansson".
- Implements RFC 6762 (mDNS) and RFC 6763 (DNS-SD); cross-platform C, `#pragma once`, no build step.
- Vendored verbatim: do not edit in place — re-copy from upstream if an update is ever needed.

## Where it is used
- `kyper`: announces `kyper.local` + `kyper._http._tcp.local` on the configured HTTP port.
- `whisper-cast`: announces `<name>.local` + `<name>._http._tcp.local` on the configured HTTP port.
- Both tools open one IPv4 socket bound to the first non-loopback interface, answer `MDNS_ENTRYTYPE_QUESTION` records only, and send replies with `mdns_query_answer_multicast` (TTL 60).

## Gotchas
- **`mdns_string_extract` output is NOT NUL-terminated and keeps the trailing dot.** Callers must copy `s.str[0..s.length]` into their own buffer and strip the dot before comparing (`kyper` and `whisper-cast` each have a local `ExtractMdnsName`-style helper).
- **Windows multicast loopback is one-way**: `IP_MULTICAST_LOOP` delivers looped copies only to the *sending* socket, so a same-host probe will not see replies. Verify name resolution from a phone or second machine.
- **Port 5353 may already be held** by another mDNS stack (Bonjour, Windows). `mdns_socket_open_ipv4` then fails; both tools log a degradation notice and stay reachable by raw IP. This is never fatal.
- **Win32 sockets are non-blocking**: the listener polls with one `recvfrom` per loop iteration (tools use a `Sleep(20)` cadence).
- **Buffer sizing is the caller's job**: tools allocate fixed 8 KB TX/RX buffers; answers larger than the buffer are truncated by the library.

---

## Verified

- [x] In use by kyper + whisper-cast: A/PTR/SRV + `_http._tcp`, TTL 60, 20 ms poll thread, clean shutdown
- [x] Pattern for new tools: wrap `mdns_string_extract` with `ExtractMdnsName` (strip dot, NUL-terminate)
- [x] Debug-verified (whisper-cast): reply looped back to own socket
