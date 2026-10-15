# crQuickShare

A native desktop Quick Share implementation built on Chromium 154. It provides
an account-free LAN sharing app and reusable discovery, protocol, Bluetooth,
and Wi-Fi upgrade modules for crFiles.

## Technical highlights

- **Direct, local transfers.** Discover peers over mDNS or BLE and transfer files
  without a Google account, cloud relay, or Google backend dependency.
- **Shared protocol implementation.** Reuses Chromium's Nearby protobufs, UKEY2
  P256/SHA512, and D2D secure-channel implementation rather than implementing
  cryptographic algorithms from scratch.
- **Multiple transport options.** crFiles supports Bluetooth bootstrap and small
  transfers, LAN upgrades, Linux hotspot clients, and experimental Linux Wi-Fi
  Direct client/Group Owner roles.
- **Controlled channel migration.** Validates peer paths, exchanges introduction
  messages, drains the old channel through protocol barriers, and resumes file
  traffic on the replacement channel with normal keepalive monitoring.
- **Network-aware discovery.** Keeps mDNS caches per interface, supports multiple
  IPv4 addresses, and refreshes discovery after link changes or resume. Unchanged
  network notifications avoid unnecessary cache resets.
- **Protected file commits.** Validates paths, sizes, offsets, and chunk indexes;
  stages incoming files in a private temporary batch; and commits without
  overwriting existing destinations.
- **Native integration.** Uses Chromium Views and platform networking APIs.
  Linux Wi-Fi backends talk directly to NetworkManager over D-Bus, without
  launching nmcli, pkexec, a helper process, or a second supplicant.

## Applications and platform scope

| Component | Implemented capabilities |
| --- | --- |
| Standalone crQuickShare | LAN file sending/receiving, BLE discovery and Fast Initiation assistance, native confirmation and progress UI |
| crFiles on Linux | LAN and Bluetooth sharing, hotspot upgrades, optional Wi-Fi Direct client/GO, text and link reception to the clipboard |
| crFiles on Windows | LAN and Bluetooth sharing, BLE/GATT discovery, Fast Initiation, Bluetooth-to-LAN and phone-hotspot upgrades; hardware validation remains pending |
| Shared protocol core | Framing, authentication, transfer state, cancellation, acknowledgements, and file validation with native Linux/Windows/macOS backends |

Implementation coverage is not a compatibility certification. Android behavior
varies by device and version. User-provided Linux logs show LAN, hotspot, and
Direct client transfers, plus committed incoming file batches. They do not
establish complete device coverage or verify every transferred file by hash.
ChromeOS interoperability and Windows hardware coverage remain to be validated.

## Getting started

Run the standalone app from an existing build:

```sh
out/crquickshare/crQuickShare
out/crquickshare/crQuickShare --device-name=Office --receive-dir=/home/user/Downloads/Shared
out/crquickshare/crQuickShare --lang=zh-CN
```

Select files, choose a discovered peer, compare the verification codes, and
confirm on both devices. To receive, enable the app's ten-minute visibility
window and approve the incoming offer. Reception is off by default in the
standalone app; crFiles manages its own reception settings.

`--receive-dir` requires an absolute path and defaults to
`~/Downloads/crQuickShare`. `--device-name` defaults to `crQuickShare`; the
protocol name is limited to 80 UTF-8 bytes. The UI supports English and `ja`,
`ko`, `zh-CN`, `zh-HK`, `zh-TW`, `ar`, `fr`, `fr-CA`, `ru`, `es`, `es-419`,
`pt-BR`, and `pt-PT`.

LAN sharing requires reachable UDP 5353 and the application's TCP listener on
the same IPv4 subnet. AP isolation, multicast filtering, or a firewall can
prevent discovery or connection. The app does not change firewall rules.
Bluetooth features require BlueZ on Linux or the corresponding Windows APIs;
receiving additionally requires peripheral advertising and local GATT support.

## Transport selection

A discovered LAN peer can connect directly over TCP. For Bluetooth-originated
sessions, batches up to **72 KiB (73,728 bytes)** can use Bluetooth; larger batches
require a usable Wi-Fi path. There is no large-file Bluetooth fallback.

When a peer does not offer a path, the coordinator sends one
`UPGRADE_PATH_REQUEST` containing the currently usable client media, such as
`[5, 3]` (LAN, Hotspot) or `[5, 8]` (LAN, Direct), plus role metadata. The peer
selects one path; the list does not start parallel joins or require a particular
selection order. A failure rejects its named candidate, not the entire list.

The default initial peer-offer wait is five seconds. A list request waits up to
15 seconds, then falls back to individual candidates within the remaining
attempt budget: five seconds for LAN and 15 seconds for other media. A path-wait
timeout adds no extra retry delay. Actual join failures clean up networking
before retrying, with 5/10-second backoff. Negotiation is bounded to three
attempts and 120 seconds; individual operations have shorter deadlines.
Multi-medium request behavior still needs Android device validation.

Retries stop once channel migration reaches either old-channel drain barrier.
A usable peer offer takes precedence over creating a local network. LAN upgrade
addresses must be directly attached IPv4 unicast addresses; connections bind
to the selected interface and source address.

### Linux hotspot client

For crFiles, select the adapter explicitly when needed:

```sh
out/debian/crFiles --quick-share-upgrade-interface=wlp3s0
```

The backend joins a phone-provided hotspot using a volatile, user-restricted NM
profile. Association, address validation, and TCP connection share a 40-second
budget. It uses automatic band/channel selection and services Bluetooth control
messages while waiting. Joining may replace the adapter's current Wi-Fi
connection; cleanup restores or preserves the connection and removes the
session's temporary profile.

`--quick-share-prefer-hotspot` skips the initial peer-offer wait and places
Hotspot first in the candidate list. It does not force the phone's choice or
create a local hotspot. Automatic hotspot selection requires a sole wireless
interface.

### Windows hotspot client

crFiles uses Native Wi-Fi to join the phone's WPA2-Personal/AES hotspot.
The shared coordinator advertises LAN and Hotspot client capabilities; Wi-Fi
Direct client/GO is not enabled on Windows. Existing Bluetooth small transfers
and LAN transfers use the same paths as before.

With one Wi-Fi radio, selection is automatic. With multiple radios, specify the
Windows interface alias (including localized aliases) explicitly:

```powershell
.\crFiles.exe --quick-share-upgrade-interface="Wi-Fi"
```

`--quick-share-prefer-hotspot` also applies on Windows. Selection and WLAN calls
run on the transfer worker. The join uses a uniquely named in-memory temporary
profile passed to `WlanConnect`; it does not call `WlanSetProfile`, save passwords
to the profile store, or run shell commands. SSID bytes are encoded as hex and
password XML is escaped. Credentials never appear in diagnostic logs.

Association, IPv4 address readiness and TCP connection share the coordinator's
40-second budget. Polling services Bluetooth control/cancellation every 50 ms
between native calls. Native WLAN RPC calls themselves have OS-controlled
latency. TCP is bound to the selected interface and a directly attached IPv4
address. Joining can replace that adapter's existing Wi-Fi connection; Windows
manages DHCP routes and DNS normally (unlike Linux's `never-default` profile).

Cleanup cancels a pending join or disconnects the session's temporary network,
then reconnects the previous saved profile within the shared cleanup budget.
A newer network selection observed through WLAN notifications is preserved.
An existing connection without a restorable saved profile, an interface already
connecting, or an ambiguous adapter selection causes the upgrade to fail before
changing networking. Cleanup failures remain visible through the common warning.

Native Wi-Fi service/driver availability and Windows Wi-Fi/location permissions
can deny the operation; numeric API errors and association reason codes are
logged. This backend still requires Windows hardware validation: phone-to-PC
and PC-to-phone transfers, repeat sessions, join cancellation, missing hotspots,
user network switches, multiple radios, and restoration after failures.

### Linux Wi-Fi Direct (experimental)

Enable Wi-Fi Direct in **crFiles Settings → Quick Share → Advanced**, or run:

```sh
out/debian/crFiles --quick-share-upgrade-interface=wlp3s0 --quick-share-direct-go
```

This requires the downstream `network-manager-go` package
`1.58.1-1+crquickshare2` or later, its wpa_supplicant backend, and a radio supporting
concurrent STA + P2P Client/GO. See the [NetworkManager integration notes](../network-manager-go/contrib/crquickshare/README.md)
and [validation record](../network-manager-go/contrib/crquickshare/VALIDATION.md).

NM owns the independent P2P interface, addressing, DHCP, and teardown. A private
D-Bus connection binds the volatile activation to the session. The client avoids
adopting peer default routes or DNS; the GO uses NM's shared IPv4 mode, including
its forwarding/NAT and firewall management. Cleanup targets only the session's
own connection, interface, and profile.

Automatic selection chooses an available NM-managed adapter with a free P2P
device. Explicit adapter selections never silently switch to another adapter.
The current policy excludes AP, mesh, and IBSS modes to protect existing
connections. Driver-specific concurrency still requires hardware validation.

Direct is opt-in and disables the station-replacing hotspot backend. Command-line
settings override saved preferences for the current run. The optional
`--quick-share-prefer-direct-go` adjusts Direct preference; a pending peer path
still wins. The old `--quick-share-direct-go-helper` option is unsupported.

## Authentication and data handling

Anonymous paired-key exchange and a four-digit verification code support sharing
without account login. Device names and discovery records are display/routing
information, not authenticated identity; matching names never authorize a transfer.

LAN retains UKEY2 encryption. Hotspot and Direct clients can honor an
authenticated peer's request to disable Nearby record encryption on the
WPA-protected replacement link; otherwise they retain UKEY2. The local Direct GO
retains UKEY2. Upgrade completion does not confirm file delivery.

File batches support up to 1,024 regular files and 1 TiB total. The core checks
metadata, available space, relative paths, payload lengths, and chunk ordering.
Incoming parent-folder metadata can reconstruct a directory tree; standalone
sending selects regular files, not directories or symlinks. Empty directories
are unsupported.

Incoming files are staged in a hidden temporary directory and committed as a
non-overwriting `Transfer-<random>` batch. Failed or cancelled uncommitted batches
are removed; committed files survive later disconnects. Abnormal process exit
can leave `.incoming-*` directories, which are not automatically resumed.

crFiles also receives text, URLs, addresses, and phone numbers as plain text:
up to 16 attachments and 1 MiB per batch, copied once to the clipboard after
confirmation and complete receipt. Links are not opened automatically. Mixed
file/text batches, account/contact sharing, WebRTC, relays, resume, and automatic
acceptance are unsupported. General LAN discovery and transfer use IPv4.

## Discovery and diagnostics

mDNS uses `_FC9F5ED42C8A._tcp.local` with probing, conflict handling, known-answer
suppression, TTL refresh, and goodbye handling. BLE supports FEF3 advertisements,
indexed GATT discovery slots, and Fast Initiation assistance. GATT reads and
caches are bounded, with expiry and retry backoff. DCT advertisements can be
listed, but DCT/L2CAP transfer is not implemented.

Discovery records merge using protocol identifiers and routing information,
never names alone. Bluetooth availability does not guarantee that the phone
will advertise a LAN endpoint or accept a transport upgrade.

Logs use the `crQuickShare:` prefix. Follow one `session` through:

```text
Connection stage → Upgrade request/path decision → channel upgraded
                 → file_transfer → Connection outcome → network cleanup
```

- `socket_completed=1` means a local write completed, not that the peer read it.
- `channel upgraded` means migration completed; normal traffic resumes without
  a separate heartbeat ACK gate. Regular keepalives monitor the new channel.
- `delivery_confirmed=1` records confirmed delivery; `success=1` with
  `delivery_confirmed=0` is legacy send completion without explicit delivery ACK.
- `Receive batch committed` records a completed local file batch.
- `network_restored_or_preserved` and cleanup results describe network teardown
  separately from the transfer outcome.

Diagnostics include interface/address and BLE association information, but do
not log file contents, passwords, verification codes, or cryptographic keys.
Use packet captures and NM/kernel logs when local socket events cannot explain a
stall. Device validation should compare file hashes and cover cancellation,
rejection, repeated transfers, network changes, and temporary-profile cleanup.

## Build integration

From the Chromium source root, use a dedicated output directory and `autoninja`:

```sh
gn gen out/crquickshare --args='is_debug=false is_component_build=false symbol_level=0 chrome_pgo_phase=0 enable_libcr_shared=false root_extra_deps=["//crQuickShare"]'
autoninja -C out/crquickshare crQuickShare
```

The GN target is `//crQuickShare:crQuickShare`. The optional Makefile wrapper is
[chromium_src_Makefile](chromium_src_Makefile); `CQS_SHARED=true` enables the shared
libcr configuration. Shared builds require a matching `libcr.so`, source version,
and build configuration. Deploy ICU, resource PAKs, and platform runtime
dependencies alongside the executable; copying the executable alone is insufficient.

Resource validation:

```sh
python3 crQuickShare/tools/validate_resources.py
```

## Source map

| Area | Main files |
| --- | --- |
| Standalone UI | `crquickshare_main.cc`, `crquickshare_window.cc` |
| Protocol and session coordination | `quick_share_service.cc`, `transport_channel.*` |
| LAN discovery | `lan_discovery_mdns.cc`, `mdns_codec.*`, native discovery backends |
| Bluetooth | `ble_discovery.cc`, `ble_wakeup.cc`, `bluetooth_receiver.cc`, `bootstrap_probe.cc` |
| Linux Wi-Fi upgrades | `hotspot_upgrade_linux.cc`, `wifi_direct_go_linux.cc` |
| File validation and commit | `file_storage.*`, platform storage backends |
| Localization | `crquickshare_strings.grd`, `translations/` |

Protocol references live in Chromium's `third_party/nearby` and `third_party/ukey2`,
including `offline_wire_formats.proto`, Sharing's `wire_format.proto`, and
`offline_frames.cc`. This directory is maintained as a separate Git worktree.
