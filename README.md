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
  network notifications avoid unnecessary cache resets. macOS subscribes to
  SystemConfiguration interface, IPv4 and Wi-Fi changes on a dispatch queue;
  the three-second interface polling fallback remains enabled.
- **Protected file commits.** Validates paths, sizes, offsets, and chunk indexes;
  stages incoming files in a private temporary batch; and commits without
  overwriting existing destinations.
- **Native integration.** Uses Chromium Views and platform networking APIs.
  Linux Wi-Fi backends talk directly to NetworkManager over D-Bus, without
  launching nmcli, pkexec, a helper process, or a second supplicant.

## Protocol codec boundaries

- `endpoint_info_codec` calls the vendored Nearby Sharing `Advertisement`
  implementation for device information encoding and core decoding. The local
  wrapper bounds input, rejects invalid UTF-8/control characters and malformed
  TLV values, ignores a lone trailing extension byte like upstream, and retains the original bytes for
  discovery association. Unknown TLVs remain accepted when their lengths are
  valid; reserved device types map to unknown as in upstream.
- `advertisement_codec` directly calls upstream BLE medium and Connections
  `BleAdvertisement` parsers, `InstantOnLostAdvertisement::CreateFromBytes`, and
  DCT `DataElement::FromStreamReader`. Local checks restrict service/strategy,
  total size, unknown extension masks and trailing bytes. Large compact
  advertisements are adapted to regular envelopes for upstream parsing: this
  preserves public names beyond the upstream fast-info limit of 17 bytes and
  avoids signed-char promotion of compact medium lengths >=128.
- DCT semantic checks remain local: the upstream DCT parser requires a fixed
  element order and does not check for an empty device-information value before
  indexing it. Our wrapper accepts reordered/unknown elements, rejects duplicates,
  and checks the Sharing hash, PSM and UTF-8 name. Its TLV decoding is upstream.
- GATT header/bloom filtering and advertisement generation remain local. The
  upstream GATT header implementation depends on Nearby feature flags and chooses
  raw versus base64 representation through those flags; our receiver supports
  both wire forms explicitly. Discovery scheduling and peer merging are unchanged.
- Received Connections and Sharing frames use the existing protobuf/envelope
  and session validation in `quick_share_service.cc`. ACK compatibility, consent,
  manifests, chunk ordering and completion checks remain in that implementation.
  Payload received ACKs are sent only for FILE attachments, not BYTES/text.
  Unknown and repeated ACKs are ignored; an ACK for a current outgoing file
  still requires its final chunk and all bytes to have been written.
  File storage enforces component-based path safety and atomic commits; upgrade
  credentials are validated by the platform backends.

The portable upstream codecs are compiled by crQuickShare-local GN targets.
The upstream offline frame validator is not part of these targets. No ChromeOS
Nearby service is linked.

## Applications and platform scope

| Component | Implemented capabilities |
| --- | --- |
| Standalone crQuickShare | LAN file sending/receiving, BLE discovery and Fast Initiation assistance, native confirmation and progress UI |
| crFiles on Linux | LAN and Bluetooth sharing, hotspot upgrades, optional Wi-Fi Direct client/GO, text and link reception to the clipboard |
| crFiles on Windows | LAN and Bluetooth sharing, BLE/GATT discovery, Fast Initiation, Bluetooth-to-LAN and phone-hotspot upgrades; hardware validation remains pending |
| crFiles on macOS | BLE/GATT discovery, outgoing Bluetooth connections with LAN, phone-hotspot and experimental Direct station-client upgrades, LAN reception, and a new FEF3 local-name Bluetooth receiver pending phone validation; the receiver takes priority over the Fast Init experiment |
| crPhotos on macOS | BLE/GATT discovery, outgoing Bluetooth connections with LAN upgrades, and LAN reception; hotspot client integration remains pending |
| Shared protocol core | Framing, authentication, transfer state, cancellation, acknowledgements, and file validation with native Linux/Windows/macOS backends |

Implementation coverage is not a compatibility certification. Android behavior
varies by device and version. User-provided Linux logs show LAN, hotspot, and
Direct client transfers, plus committed incoming file batches. They do not
establish complete device coverage or verify every transferred file by hash.
ChromeOS interoperability and broader Windows/macOS hardware coverage remain to be validated.

On macOS, opening device discovery requests Bluetooth permission. Outgoing
Bluetooth connections use only the classic address carried by a validated
Nearby advertisement, never CoreBluetooth's synthetic device address. The
existing protocol handles the handshake, confirmation and migration to
WIFI_LAN; both peers need a reachable LAN path. crFiles can also request a
phone-hosted hotspot or join a phone-owned Direct group that accepts ordinary
Wi-Fi clients when the peers have no reachable LAN path. Batches up to 72 KiB can use
the existing direct Bluetooth path. macOS system link-security policy may
still require pairing. Bluetooth cancellation also cancels pending SDP setup.

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
Bluetooth features use BlueZ on Linux, Windows Bluetooth APIs, or CoreBluetooth
and IOBluetooth on macOS. Bluetooth reception requires peripheral advertising
and local GATT support. Linux/Windows use their existing discovery publishers;
macOS now has an Apple local-name receiver backend pending phone validation.

## Transport selection

A discovered LAN peer can connect directly over TCP. For Bluetooth-originated
sessions, batches up to **72 KiB (73,728 bytes)** can use Bluetooth; larger batches
require a usable Wi-Fi path. There is no large-file Bluetooth fallback.

When a peer does not offer a path, the coordinator sends one
`UPGRADE_PATH_REQUEST` containing the currently usable client media, such as
`[5, 3]` (LAN, Hotspot), `[5, 8]` (LAN, Direct), or `[5, 8, 3]` (the default
macOS clients), plus role metadata. The peer
selects one path; the list does not start parallel joins or require a particular
selection order. A failure rejects its named candidate, not the entire list.

The default initial peer-offer wait is five seconds. A list request waits up to
15 seconds, then falls back to individual candidates within the remaining
attempt budget: five seconds for LAN and 15 seconds for other media. A path-wait
timeout adds no extra retry delay. Actual join failures clean up networking
before retrying, with 5/10-second backoff. Negotiation is bounded to three
attempts and 120 seconds; individual operations have shorter deadlines.
Multi-medium request behavior still needs Android device validation.

A preferred Direct client can request `[8]` once before the usual candidate
list, within the same attempt/deadline limits. If that request is rejected or
times out, the remaining client media are requested. A delayed valid Direct
offer can still be accepted after a request timeout. An actual Direct client
upgrade failure excludes further Direct client proposals for that session;
recovery must finish successfully before another medium can be tried. This
prevents an unusable Direct path from consuming every retry. A peer offer
already pending still takes precedence over a preference.

Retries stop once channel migration reaches either old-channel drain barrier.
A usable peer offer takes precedence over creating a local network. LAN upgrade
uses the existing single `wifi_lan_socket.ip_address` and `wifi_port` fields.
The address may contain four IPv4 bytes or sixteen IPv6 bytes in network order;
connections require a directly attached unicast route and bind its interface
and source address. IPv6 link-local, IPv4-mapped IPv6, loopback, multicast and
local-device addresses are rejected. IPv6 sources must have a matching on-link
prefix and usable address attributes. Discovery and the ordinary incoming LAN
listener remain IPv4. No upstream protocol changes or address-candidate list
support are included. `LAN upgrade TCP attempt` records the address family,
interface, outcome and OS error without logging addresses.

On Linux, Windows and macOS, a peer `UPGRADE_PATH_REQUEST` containing WIFI_LAN
can be answered with a local LAN path. The session opens its own ephemeral TCP
listener and publishes exactly one IPv4 address and port in the existing
`wifi_lan_socket` fields. It prefers a usable physical Wi-Fi interface,
otherwise Ethernet; ambiguous addresses at the selected priority are rejected
instead of publishing multiple candidates. VPN, bridge and link-local addresses
are excluded. Linux verifies a backing device through sysfs; Windows checks
hardware-interface flags, and macOS restricts selection to physical interface
names and types. The listener binds the selected address and selects its
interface for reply routing (macOS uses IP_BOUND_IF; Linux and Windows use
IP_UNICAST_IF). It checks incoming peers against that interface's current subnet
and closes when the accept attempt ends. This does not change the station network or reuse the
ordinary discovery listener, which rejects connections while a session is busy.
When a peer requests both LAN and Direct, the local LAN provider takes priority.
If LAN setup, connection or introduction fails before either old-channel drain
barrier, an enabled, compatible Linux Direct GO is tried next without requiring
another peer request. The original request must include Direct and advertise
the Direct client role. Failure is flushed on Bluetooth before the fallback;
both attempts share the existing three-attempt and 120-second limits. A usable
peer offer already pending still takes precedence. A Direct-only request can
still schedule GO immediately; LAN can be offered independently of Direct support.

The phone must be able to reach that LAN address. The provider verifies the
phone's CLIENT_INTRODUCTION endpoint, sends CLIENT_INTRODUCTION_ACK, and uses
the existing old-channel barriers while retaining UKEY2 encryption. Each
session schedules at most one local LAN offer; failure before the barriers
returns to the bounded upgrade coordinator. Requests arriving during an
upgrade are ignored. Unsupported repeated host requests are deduplicated, and
one list request produces at most one medium failure rather than a failure for
every advertised medium. The existing event limit remains in place.
`LAN upgrade listener`, `LAN upgrade path offered` and `Upgrade provider
introduction diagnostics` identify this path. This provider path still needs
Android device validation; no upstream protocol extension is involved.

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

### macOS Bluetooth connection recovery

On macOS 27, an outgoing crFiles send that needs Bluetooth bootstrap checks the
target after source preflight and GATT release. If the target is already
connected and this process has not confirmed its classic callbacks through a
successful wire SDP response, setup reconnects that device once before SDP.
This briefly interrupts other Bluetooth services on that device. It uses the
existing reconnecting status, without first waiting for the missing SDP open
callback to time out. Incoming transfers, LAN transfers and diagnostic-only
probes do not trigger this preparation; other macOS versions retain normal setup.

`macOS Bluetooth connection preflight` reports the OS version, public connected
state and whether callbacks were previously confirmed. `macOS Bluetooth initial
connection preparation` identifies the automatic first attempt. The readiness
record is process-local, stores only addresses and never supplies a cached SDP
route. Each send still performs a fresh wire query. A failed SDP query, an
observed disconnected state or a reconnect request clears that target's readiness
record. A target with confirmed callbacks can proceed
without another preflight reconnect.

A failed outgoing setup that has not already reconnected can offer **Reconnect
and retry** in the transfer result bubble. The accompanying notice explains
that the action briefly disconnects other Bluetooth services on that device.
Linux and Windows do not offer or execute this recovery.

The action is tied to the failed session and retains that session's device and
file selection. It is available only before a Bluetooth socket connected, after
the failed worker has released the session, and only for connection errors.
Cancelled, incoming and later transfer failures do not offer it. Changing
preferences or accepting another transfer invalidates the old action. Each
failed ordinary attempt permits one recovery attempt. A setup that already used
the macOS 27 preparation does not offer a second reconnect; recovery does not loop.

Manual recovery starts a new transfer and repeats source preflight and GATT
release before touching the classic connection. Both paths use
`mac_bluetooth_reconnect.mm` and public
IOBluetooth APIs: it registers a disconnect notification, requests close, waits
for that notification, then requests an asynchronous open and waits for
`connectionComplete:status:`. It requests close even when public `isConnected`
is false, since peripheral and classic state updates can arrive separately.
A successful request return alone does not advance to SDP. No private
callback fields or methods are accessed. Pairing and adapter power are unchanged.

After reconnection, the existing socket bridge performs a new wire SDP query
and opens RFCOMM on the returned live channel. There is no cached-channel
fallback. The normal handshake, receiver confirmation and Wi-Fi upgrade follow.
`macOS Bluetooth recovery authorized`, `disconnect confirmed`, `connection
callback` and `bootstrap recovery ready; next=live_sdp` logs distinguish these
steps; `Live SDP query finished` must report `wire_response_confirmed=1` before
using the route.

Native recovery operation logs include a process-local `operation` ID, also
retained by the native delegate after its owner expires.
`macOS Bluetooth recovery callback` records `event=disconnect` or `event=connect`
at `phase=native_entry`, reports `posted` or `post_failed`, and records
`ui_delivery` if the task runs. Native entry/post logs
read no device properties or owner WeakPtr. They record pointers, optional native
status, thread/main-thread identity and monotonic time. `callback_age_us` measures
time since native entry; `owner_present=-1` means it has not been checked on the
UI sequence, and `owner_present=0` on delivery identifies a retired operation.
`has_status=0` on disconnect notifications distinguishes them from connection
completion callbacks that actually supply a native status.

`macOS Bluetooth recovery state` records the current stage, target connection,
pairing state and connection handle before/after native requests, at callback
processing, cancellation, deadlines and completion. Callback processing logs
`disposition=accept`, `ignored_stage_mismatch` or `ignored_device_mismatch`
before applying the existing guards. Device pointer and canonical address
matches are logged separately; identity checks are unchanged. The
`disconnect_callbacks` and `connect_callbacks` counters count UI-side handler
entries, including ignored callbacks, not native-thread events. Together these
logs distinguish no observed native callback, a failed post, a task that has not
run, an expired owner and a callback rejected by an existing guard. They do not
advance connection state or treat a successful request return as completion.

The disconnect and open deadlines are eight and fifteen seconds. If disconnect
confirmation times out, a bounded open is still attempted to restore the link.
For an initially connected device, missing disconnect confirmation fails the
recovery. A device that was and remains publicly disconnected can proceed with
the new open callback and wire SDP, since it need not emit a disconnect event.
Cancelling a transfer stops its result delivery and file sending; a close
already requested still proceeds to the bounded reconnect attempt while the
application is running. Another setup to that address is rejected until this
operation finishes. An outstanding native open keeps only its delegate alive
for a late callback after timeout. Application shutdown or native API failures
can prevent link restoration. Only setup that actually selects reconnection
receives the extended 60-second Bluetooth setup budget. That decision and the
worker's stage/deadline are published together under the pipe mutex after GATT
release; setup that skips reconnection retains its existing deadlines.

The isolated reconnect probe verified connection and SDP on macOS 27. Later
foreground bundle captures also verified manual recovery, RFCOMM bootstrap,
Wi-Fi hotspot upgrade, all file records written and previous-network restoration.
Those version 4 peers closed without file delivery acknowledgements, so the
sender reported sent rather than confirmed delivery. The automatic initial
preparation still requires a rebuilt crFiles and hardware validation.

### macOS hotspot client

crFiles advertises a Hotspot client and joins the Android-provided network using
the reusable `:wifi_client_mac` target (`CoreWlanClient`). The protocol still
handles credentials, encrypted channel migration and transfer acknowledgements.
This backend creates neither a Mac hotspot nor a Wi-Fi Direct group, and it does
not itself enable Bluetooth reception or standard Service Data Fast Initiation
advertising on macOS. The separate receiver backend and Fast Init experiment
are described below.

Opening nearby discovery requests location access in addition to Bluetooth.
macOS requires this access to identify Wi-Fi networks and their BSSIDs; no
location coordinates are requested. Hosts reusing the client must call
`RequestCoreWlanAccess()` on the UI sequence, supply native location usage
descriptions and, when sandboxed, the location entitlement. crFiles includes
English and supported-language permission strings and embeds usage metadata
in its development executable as well as packaging it in the app bundle.

`Wi-Fi location state` diagnostics include the global `services_enabled` switch,
the raw `authorization_status` and its name, and the effective `allowed` flag.
On macOS, access requires both enabled services and `authorized_always`.
`request_preflight`, `request_issued` and `request_returned` distinguish checking
permission from calling the native authorization API; an API return does not
prove a prompt was displayed. Request logs also identify the bundle and presence
of usage descriptions. Only real delegate notifications increment
`authorization_callbacks` and produce `authorization_callback` state updates.
`join_entry` records the session worker's cached decision, including its source,
revision and age. A blocked join schedules `join_blocked_refresh` on the UI
sequence without requesting authorization; a later upgrade retry sees the
refreshed state. `manager_missing` means the discovery permission flow did not
create the manager. These diagnostics do not request location coordinates.

For crFiles, use the [development app launcher](../crFiles/mac/README.md#run-a-development-app-in-the-foreground)
to package the existing executable and open its `.app` through Launch Services.
Authorization request logs record `bundle_is_app`, `application_active` and
`activation_policy`. Requests made while the app is inactive are deferred until
the activation notification; the host calls `CancelDeferredCoreWlanAccessRequest()`
if discovery closes first. This cancels only waiting for activation, not an
authorization request already submitted to macOS.

With one Wi-Fi interface selection is automatic. With multiple radios, choose
the BSD interface name, for example `--quick-share-upgrade-interface=en0`.
`--quick-share-prefer-hotspot` requests Hotspot before LAN; the phone still
chooses whether to offer a path. The client does not power on a disabled radio.

Scanning, association and network recovery run on a background task.
The session worker continues servicing Bluetooth and cancellation every 50 ms
while waiting for native work or an IP address, with up to 100 ms between polls
during a TCP attempt. Association, address readiness and TCP share the existing 40-second
budget; the native CoreWLAN calls themselves cannot be forcibly interrupted.
TCP uses a source address on the selected Wi-Fi interface and is bound to that
interface.
The protocol frequency/band hint is logged but does not override macOS channel
selection. No SSID, password, BSSID or Keychain data is written to diagnostics.

After association, the station backend requires an eligible local source address
and prefix to remain unchanged for at least 250 ms before connecting. Hotspot
uses IPv4; Direct can additionally use the existing IPv6 field described below. This settling
guard does not prove DHCP completion or gateway reachability. It resamples the
source address before each TCP attempt and always binds the original interface;
it never falls back to system routing through another interface.

Transient source-address, network-unreachable, connection-reset/refused and
timeout failures can retry before the Nearby client introduction. There are at
most four TCP attempts, each limited to two seconds, within an eight-second
window starting after the first stable address. These limits also respect the
remaining overall budget. Retry delays increase from 250 to 750 ms while the
existing station association and client lease are retained. Permission failures,
invalid inputs, interface-binding failures and unclassified errors do not retry.
Cancellation, peer rejection and an observed Wi-Fi network change stop the loop.

`Wi-Fi station route state` records address candidates and losses without logging
addresses. `Wi-Fi station TCP attempt finished` records the attempt, route
revision, outcome, numeric `errno`, `errno_name`, retry decision, stop reason and
remaining retry budget. A deadline imposed by the caller has `outcome=timed_out`
and `errno=0`; an OS timeout has `errno_name=ETIMEDOUT`. The final join result
includes `tcp_attempts` and the last TCP outcome/error, including when a separate
cancellation or network change ends the join. `reason=tcp_retry_timed_out` means
the retry window expired while waiting for an address or between attempts. Each
retry uses a fresh socket; the failed sockets have sent no Nearby introduction
or file records.

Before switching an existing connection, the client records only its network
identity. It does not scan the previous network or read its Wi-Fi password,
enterprise username/password or certificate identity from Keychain. Sending no
longer depends on permission to retrieve those credentials. Joining temporarily
replaces that interface's Wi-Fi connection and OS-managed routing. No profile is
explicitly saved or deleted; macOS may retain credentials under its own policy.
Location permission and the system's Wi-Fi association policies still apply.

Completion, failure and cancellation disconnect the session's hotspot once and
wait for macOS AutoJoin, preserving a newer observed network selection or a
disabled radio. An already-connected target is borrowed and left connected.
If the interface is already disconnected, recovery leaves an AutoJoin in
progress alone. The client does not associate to the previous network with a
nil password, change network preferences or toggle Wi-Fi power. AutoJoin uses
system-managed saved credentials; it can select a different network, and it
cannot guarantee reconnection when AutoJoin is disabled or the network is
unavailable.

`CoreWLAN recovery policy` records `mode=system_auto_join` and
`keychain_lookup=0`. `CoreWLAN recovery finished` distinguishes a previous network
connection, another preserved network, a disabled radio, a disconnected target
when there was no previous network, and an AutoJoin timeout. These diagnostics
contain no network identifiers or credentials. A connected SSID does not prove
DHCP or Internet readiness. Native errors log only the domain/code and one
underlying error's domain/code, without descriptions or userInfo dumps.

Cleanup waits within the common budget. If a previous connection existed but
macOS has not joined another network before the deadline, cleanup reports
failure through the existing network-cleanup warning. A native association
returning after that wait still runs background recovery, holding a client
lease so another upgrade cannot race it. Process termination can prevent that
best-effort cleanup; automatic reconnection remains under macOS control.

Hardware validation is still required for transfers across different networks,
repeat transfers, cancellation during scanning/association, denied permissions,
disabled AutoJoin, user network changes, and system reconnection to personal,
open and enterprise Wi-Fi networks after leaving the phone's hotspot.

### macOS Wi-Fi Direct station client

crFiles enables an experimental Direct client by default. It advertises
`support_wifi_direct_group_client=1` and
`support_wifi_direct_group_owner=0`, alongside the existing LAN and Hotspot
clients. The Mac only joins a phone-owned group when the peer supplies a
`WIFI_DIRECT` path with `wifi_direct_credentials`. It never creates a group or
performs Wi-Fi P2P discovery, WPS or GO negotiation.

`MakeMacWifiDirectClientConnector()` reuses the hotspot backend's station
connection: validate the supplied SSID, password, port and at least one usable
address, then use CoreWLAN to associate. The existing `ip_v6_address` field may
supply a sixteen-byte IPv6 link-local GO address; otherwise the IPv4 gateway
is used. A ready IPv6 route takes precedence when neither address has been
attempted, without waiting for DHCP. IPv4 can proceed first if its route becomes
ready earlier. Both the source and destination IPv6 scope IDs, and the TCP
socket's bound interface, use the joined Wi-Fi interface index. Direct
accepts an 8–63 character printable ASCII passphrase or a 64-digit hexadecimal
PSK; CoreWLAN remains responsible for accepting the key. The protocol frequency
is a hint, not a forced radio channel. IPv6-only groups are supported when the
phone accepts legacy station clients and offers a usable link-local path.
Malformed or non-link-local IPv6 credentials are ignored when a valid IPv4
gateway is available; otherwise the credentials are rejected before association.
Both address families share the four-attempt/eight-second TCP retry budget and
250 ms source-address settling guard. When both addresses are usable, each gets
at most two attempts, leaving IPv4 a chance to become ready after fast IPv6
failures. Retryable failures can select the other family without rejoining Wi-Fi.
The station join and TCP logs include `family=IPv6` or `family=IPv4`.
The channel reports `WIFI_DIRECT`, so the existing authenticated introduction,
encryption selection, migration barriers and file-delivery handling remain in
the shared coordinator.

This is a legacy station connection, not an independent P2P interface. It may
replace the Mac's current Wi-Fi link. The phone must accept ordinary Wi-Fi
clients in its group; protocol role advertisement alone does not prove that it
does. Joining and recovery share `CoreWlanClient` with Hotspot, including location
authorization, cancellation, preservation of user network changes and the
system AutoJoin cleanup policy. No previous Wi-Fi password is read and no new
entitlement is required by this station backend.

For a Direct-first capture, prepare the development app, quit existing instances,
then run:

```sh
python3 crQuickShare/tools/capture_mac_bluetooth.py --network-diagnostics out/arm64/crFiles.app --quick-share-prefer-direct-client
```

`--quick-share-prefer-direct-client` skips the initial five-second peer-offer
wait and requests Direct alone once if no valid path is already pending. It
does not force the phone to offer Direct. Default requests keep LAN ahead of
Direct and Hotspot; they are supported-medium lists, not a forced selection
sequence. `--quick-share-prefer-hotspot` takes precedence when both preferences
are supplied. `--disable-quick-share-mac-direct-client` disables the client and
ignores its preference, retaining LAN and Hotspot for comparison. The existing
`--quick-share-upgrade-interface=en0` selects the station interface for both
clients. The Linux `--quick-share-direct-go` switch does not enable a Mac GO.

Expected diagnostics include `direct_client=1; direct_go=0`, an accepted path
with `medium=8; direct_credentials=1`, `stage=direct_client_join`,
`Direct client join requested` with `role=legacy_station` and
`independent_p2p_interface=0`, then a TCP-ready/channel-upgraded medium of
`WIFI_DIRECT`. A capability log, path offer or association alone does not prove
successful file delivery. Transient TCP failures can retry within the same
station join under the limits above. If that join fails, `Direct client fallback`
excludes further Direct client paths for that session. Hardware validation is
still required for phone compatibility, repeat sends, cancellation, malformed or
IPv6-only paths, fallback to LAN/Hotspot, and cleanup after late association.

### macOS Bluetooth receiver

crFiles now wires `BluetoothReceiver` into macOS reception. The existing
Chromium `CreateRfcommService` implementation publishes the NearbySharing SDP
service (`a82efa21-ae5c-3dde-9bbc-f16da7b16c5a`) and accepts RFCOMM sockets.
Incoming sockets enter `BootstrapProbe::Receive` and the existing encrypted
session, user confirmation, file storage and Wi-Fi upgrade flow. Reception
does not enable automatic acceptance or change the LAN-first upgrade policy.

`BluetoothPeripheralMac` supplies the platform-specific discovery publisher:

- One primary FEF3 GATT service with readable slot
  `00000000-0000-3000-8000-000000000000`, containing the same full advertisement
  as Linux/Windows, including the real local classic Bluetooth address.
- FEF3 service UUID plus a Base64URL local name. The 15-byte V2 header contains
  one slot, the NearbySharing bloom filter and a four-byte advertisement hash.
  Its encoded name is exactly 20 bytes, with no extra Base64 layer or truncation.
  No L2CAP PSM or extended-advertisement capability is claimed.
- Dynamic read/blob responses validate the requested characteristic, current
  visibility and offset. Writes are rejected. Read diagnostics do not claim
  that the phone displayed the endpoint.

This follows the local-name transport used by
[Nearby's Apple GATT server](https://github.com/google/nearby/blob/main/internal/platform/implementation/apple/Mediums/BLE/GNCBLEGATTServer.m)
for raw V2 headers. It is not a Service Data advertisement. Compatibility with
the installed Android Quick Share version remains a hardware validation item;
an OS advertising callback alone does not prove discovery.

The receiver is enabled with Quick Share reception, including when the outgoing
device dialog is closed. It requests Bluetooth permission asynchronously before
calling classic APIs; a pending or denied permission cannot start the listener.
GATT setup and advertising each have a 10-second limit, followed by the shared
15-second retry backoff on failure. Busy sessions withdraw advertising and deny
GATT reads. Disabling reception, changing identity or shutting down retires the
native manager and listener; callbacks from retired managers are ignored.

On macOS, foreground send discovery temporarily withdraws FEF3 receiver
advertising and runs the FE2C Fast Init experiment. The existing RFCOMM listener
and GATT service are retained; this handoff does not change their channel or
endpoint identity. If a receiver advertising start is pending, its callback
must settle before Fast Init starts. Normal setup timeouts still apply.
Closing the send picker or starting a transfer stops Fast Init before reception
advertising can resume; a busy session keeps receiver advertising withdrawn.
Disabling the Fast Init experiment leaves receiver advertising available during
discovery. This arbitration applies only to macOS; Linux and Windows are unchanged.
For a receiver-disabled baseline comparison, restart with
`--disable-quick-share-mac-bluetooth-receiver`; LAN reception and the previous
outgoing discovery path remain available.

No build or hardware transfer has been run for this receiver change. Validate
with a rebuilt app and the phone's **Send** device picker, in this order:

1. `Bluetooth receiver listener ready`, `macOS receiver GATT ready` and
   `macOS receiver advertising started` prove local setup only.
2. `macOS receiver GATT read` proves that a remote central read the slot.
   Check that the phone actually lists the configured crFiles device name.
3. Selecting the Mac should produce `Bluetooth incoming socket ready` and
   `Bluetooth receiver incoming connection accepted`, then the existing
   verification/acceptance prompt. Rejecting must not write a received file.
4. Accept a file below 72 KiB, then a larger file over LAN. Check the received
   file and session outcome; sender-side socket writes alone are insufficient.
5. Repeat after cancel, Bluetooth off/on, reception disable/enable and rename.
   Advertising must resume only while reception is enabled and idle. Quit
   during setup and during a pending transfer to check cleanup.

If advertising starts but no GATT read is logged, capture the actual BLE local
name and service UUID before investigating RFCOMM or transfer code. If GATT is
read but no incoming socket arrives, inspect the phone's SDP/RFCOMM attempt.

The first macOS receiver capture confirmed that the phone displayed crFiles
and read the 55-byte GATT value twice (offsets 0, 22 and 44), but no incoming
session was observed. Discovery is therefore verified for that phone; RFCOMM
acceptance and file reception are still unverified. Additional diagnostics trace:

- `macOS receiver advertised route`: the exact classic address embedded in GATT
  and the endpoint ID. Compare it with `local_address` in the SDP registration.
- `Bluetooth RFCOMM listener SDP registration`: local record handle, assigned
  RFCOMM channel, service classes and protocol descriptors. This is a local
  registration snapshot, not evidence that the phone queried SDP.
- `Bluetooth RFCOMM incoming notification registered`: notification presence,
  channel, listener and socket identity. A missing notification now fails setup
  and withdraws the SDP record instead of reporting the listener ready.
- `Bluetooth RFCOMM incoming native callback`: notification identity, expected
  and received channel, peer address, native open state and filter disposition.
  The existing notification identity filter remains unchanged.
- `Bluetooth incoming channel queued`, `Bluetooth incoming accept started`,
  `Bluetooth incoming accept delegate attached` and
  `Bluetooth incoming accept completed`: distinguish queueing from delegate
  attachment and completion. `open_completion_pending=1` is a snapshot, not a
  timeout or proof of failure.
- `macOS receiver accept callback`: application admission, followed by the
  existing bootstrap/session logs if the connection is accepted.

GATT reads without any incoming native callback leave remote SDP/connect failure
and local notification delivery unresolved; phone-side logs or a Bluetooth trace
are needed to distinguish those cases. These diagnostics do not infer a failed
handshake or a UI problem from missing incoming transport events.

The next capture exposed an invalid local SDP route: both the RFCOMM protocol
descriptor and incoming notification used channel 0, despite successful record
publication and notification registration. The advertised classic address
matched the listener's local address. macOS had preserved the old automatic
allocation placeholder instead of assigning a usable channel.

Automatic RFCOMM publication now starts with channel 1. As documented by
`IOBluetoothSDPServiceRecord`, the system may assign an alternative if that
channel is occupied; the listener uses the actual channel read back from SDP.
All returned channels must pass the SDK's `RFCOMM_CHANNEL_ID_IS_VALID` check
(1 through 30). An explicitly requested channel must also match exactly;
invalid requests fail before publication, and invalid or mismatched published
records are withdrawn. Channel 0 cannot reach the notification registration or
the receiver's GATT advertising stage. A subsequent rebuilt-app capture verified
SDP channel 1 and notification registration for channel 1, but still showed no
application incoming callback. Reading back SDP does not prove which channel
the native stack actually delivers to this listener.

For automatic requests the diagnostics retain `requested_channel=0` to mean
unspecified, alongside `automatic_channel=1` and `publication_channel=1`.
The actual `rfcomm_channel` and SDP RFCOMM descriptor must now be in 1..30 and
match the incoming notification's channel; they need not equal 1.

#### macOS receiver channel selection and cleanup

The controlled phone runs on 2026-10-25 found no native incoming callback on
explicit channel 1, but channel 29 matched the SDP/notification/native channel,
completed the incoming handshake and confirmation, upgraded to encrypted LAN,
and committed a 463805-byte file with `delivery_confirmed=1`. This verifies that
phone's channel-29 receive path; it does not establish why channel 1 failed.

The macOS Quick Share receiver now explicitly requests channel 29 by default.
If RFCOMM service creation reports failure (including SDP channel validation or
notification registration failure), the next attempt uses 28, 27, 26, then 30,
cycling through those five candidates with the existing 15-second retry delay.
There is only one candidate attempt per retry interval. The candidates are not
an availability guarantee. No GATT advertisement is started for a failed
listener. Other Chromium services retain their existing automatic selection.

Once setup succeeds, the receiver keeps its listener and selected candidate
through transfers; busy sessions only withdraw advertising. Subsequent setup
within the same receiver lifetime starts with the retained candidate. GATT or
accept failures do not themselves advance the candidate, and absence of a phone
connection never triggers channel rotation. This avoids invalidating cached
phone SDP merely because the receiver is idle. A new app process starts at 29.

Native listener shutdown disables callback dispatch before unregistering the
notification and clears its token so destruction cannot unregister it again.
The shared RFCOMM/L2CAP cleanup removes SDP after stopping notifications and logs
the record handle, removal status and success flag. Nonzero removal status must
be investigated, not treated as proof of successful daemon cleanup. Repeated
RFCOMM listen calls on an occupied socket fail without replacing its record.

After rebuilding separately, validate the default path without verification flags:

```sh
python3 crFiles/tools/run_macos.py out/arm64
```

Expect `macOS receiver channel selection; requested_channel=29`, matching SDP
and notification channels, then native `result=matched` on a phone connection.
Send two files in successive sessions, toggle reception off/on, then restart
the app and repeat. Confirm successful file commits, advertising resumption,
and the SDP removal status when the listener actually stops. These production
selection and cleanup changes have not yet been built or hardware-validated.
The successful earlier run used the explicit verification switch.

#### Controlled macOS incoming channel verification

Use `--quick-share-mac-receiver-rfcomm-channel=N` to request one explicit channel
in 1..30. This opt-in mode permits at most one listener creation attempt per
receiver lifetime, disables setup retries and requires an app restart after
the listener is stopped. Invalid input disables Bluetooth reception for that
run; it does not silently select another channel. A mismatched SDP channel
fails setup before GATT advertising. Without this switch, the receiver uses
the candidate policy above and normal retries. File acceptance still requires the
existing confirmation flow.

After rebuilding separately, fully quit all crFiles instances. Run the baseline,
make one phone-to-Mac send attempt, then quit to collect the verification summary:

```sh
python3 crFiles/tools/run_macos.py out/arm64 -- --quick-share-mac-receiver-rfcomm-channel=1
```

Repeat in a fresh process with a different explicit channel:

```sh
python3 crFiles/tools/run_macos.py out/arm64 -- --quick-share-mac-receiver-rfcomm-channel=29
```

Channel 29 is a comparison value, not a guarantee that it is free. For each run,
reopen the phone's Send device picker, select the freshly advertised Mac and
check for fresh GATT reads. The service UUID remains unchanged, so the phone
may cache SDP: where a phone/system Bluetooth trace is available, verify that
its attempted RFCOMM channel equals this run's SDP value. An attempt against
the old channel does not test the new listener.

Correlate `Bluetooth RFCOMM listener SDP registration`, notification registration
and `Bluetooth RFCOMM incoming channel verification` by socket/listener identity:

- `result=matched`: a native incoming callback with the registered notification
  carried the same channel as the published SDP record. Check `is_open` and the
  subsequent accept/session logs separately; channel identity is not proof of
  protocol handshake or file delivery.
- `result=mismatch`: that native callback carried a different channel. Preserve
  the complete callback and SDP logs for investigation.
- The shutdown `incoming channel verification summary` reports `unobserved`
  if no qualifying incoming callback arrived. This is inconclusive, not proof
  of a channel mismatch. A mismatch takes precedence over matches in the summary.

If only one explicit channel works, this supports a channel-dependent failure;
it does not by itself prove silent native channel reassignment. This experiment
uses the app's public RFCOMM APIs and does not need a bluetoothd memory dump.
The channel-1/channel-29 hardware results are recorded above.

### macOS Fast Initiation local-name experiment

The macOS experiment remains enabled by default during Quick Share discovery.
Foreground discovery takes advertising priority over the Bluetooth receiver,
which resumes advertising when discovery ends and reception is enabled and idle.
To compare the experiment independently, disable the receiver in both runs:

```sh
out/arm64/crFiles --disable-quick-share-mac-bluetooth-receiver
out/arm64/crFiles --disable-quick-share-mac-bluetooth-receiver --disable-quick-share-mac-fast-init-experiment
```

Exit the application between runs so the singleton starts with the intended
command line. Open the Quick Share device selection dialog to start discovery.
The legacy `--quick-share-mac-fast-init-experiment` flag remains accepted; the
disable flag takes precedence if both are supplied.
The experiment reuses discovery's Bluetooth permission request. An opted-out
experiment does not create a CoreBluetooth peripheral manager or advertise.
Keep Bluetooth enabled, keep the phone's visibility and screen state consistent,
and leave its Receive page closed when measuring wakeup behavior. Repeat both
conditions; note any phone notification, tapping a notification or opening
Receive, since these can change the result independently of the broadcaster.

`MacFastInitExperiment` uses only public `CBPeripheralManager` APIs. It advertises
the FE2C service UUID and local name `_BKOBEI`, which is the unpadded URL-safe
Base64 encoding of Silent bytes `fc128e0442` (V1, type=1 in bits 4..2).
This follows the generic Nearby Apple local-name encoding convention. Silent
does not request a nearby-sharing notification; absence of a notification is not
evidence that the phone ignored the advertisement. Linux and Windows continue to
use Notify Service Data. The macOS experiment sends no Service Data, manufacturer
data, personal device name, file metadata or user identifiers, and registers no GATT
service. It does not power on Bluetooth or initiate a connection.

This encoding is not equivalent to the standard FE2C Service Data advertisement.
Chromium's Fast Initiation scanner matches Service Data directly, and Android
Fast Initiation support for the local-name alternative is not established across
devices. A receiver-disabled run on 2026-10-25 found a connectable phone within
3.5 seconds, with the user reporting that the phone was outside its Receive page
and not in Everyone visibility mode. This is positive interoperability evidence,
not proof of causality or general Android support. A successful CoreBluetooth
callback alone proves OS acceptance only. The log explicitly records
`mode=silent`, `wire_service_data=0` and `peer_response_confirmed=0`; neither notification nor
remote discovery should be inferred from the callback alone.

To validate macOS advertising arbitration after rebuilding separately, launch
without switches, open the send picker with the phone's Receive page closed,
and look for `paused_for_discovery=1`, receiver advertising withdrawal, then
`FastInit experiment advertising started`. Closing the picker should log
Fast Init stopping, `paused_for_discovery=0`, and receiver advertising resuming
without another RFCOMM/SDP registration when the listener was already prepared.
Repeat rapid picker open/close and test an incoming transfer afterwards. With
`--disable-quick-share-mac-fast-init-experiment`, opening the picker must not
pause reception advertising. The arbitration change has not been compiled or
hardware-validated here.

The experiment runs while the device dialog is discovering, stops when discovery
is paused, the dialog closes, a transfer starts or the host shuts down, and waits
for normal Bluetooth authorization before allocating a peripheral manager. Each
cancelled/failed attempt retires its manager and ignores late callbacks from it.
Initialization and advertising callbacks have 10-second wait limits; failures
retry after 15 seconds while discovery remains active. Radio-off and permission
denial remain observable without changing the radio power state.

Logs include configuration, manager state, advertising request/completion/stop,
attempt number and monotonic microsecond timestamps. `macOS discovery comparison`
identifies the scan's opt-in mode and cache counts. New GATT headers and resolved
endpoints report scan elapsed time and `observation_time_ticks_us`; endpoint logs
report name/classic-route presence rather than names. Compare the target endpoint's
fresh metadata time with `advertising_time_ticks_us`, excluding readiness entries
marked `retained_before_scan=1`. A timing difference alone does not establish that
the phone recognized Fast Initiation; record phone behavior and repeat control
runs to separate a response from normal advertisement rotation or cached devices.

When a GATT header is seen but the device remains unnamed, inspect `BLE GATT
inventory` and `BLE GATT service selection`. Each request logs service UUIDs,
instance identifiers, discovery flags, and the FEF3 characteristic UUIDs,
properties and matching slot indices. Details are capped at 32 services and 64
FEF3 characteristics, with matching services listed first and omitted counts
reported. Inventories also run on an interrupted or timed-out request that had
connected but had not reached slot lookup. They contain no characteristic values
or device names. The per-service discovery flag is informational on BlueZ, whose
backend uses the device's ServicesResolved state instead.

Selection considers all FEF3 instances and pins the unique instance containing a
readable expected slot for the duration of that request. Multiple readable
instances produce `service_ambiguous`; multiple readable copies of one slot
produce `characteristic_ambiguous`. A single FEF3 service with no usable slot
reports `characteristic_missing` or `characteristic_not_readable`, including the
expected UUID and counts. Resolution never combines different service instances
or guesses alternative slot UUIDs. Failed lookups retain the existing bounded
retry/backoff behavior and do not remove cached endpoints merely because a
characteristic is absent.

On macOS, `Bluetooth macOS GATT` logs trace service requests, FEF3 characteristic
and descriptor callbacks, processed characteristic counts, completion and service
invalidation. Errors for any service include the NSError domain and code because
they can block device-wide completion. Compare `native_characteristics` with
`wrapped_characteristics`, and check the callback's `disposition` and discovery
flags to distinguish an empty native result from an ignored callback or a native
error. A processed callback with `callback_expected=0` had no matching pending
characteristic request. These diagnostics do not establish that the phone
recognized the Fast Initiation experiment.

macOS GATT resolution keeps its 15-second request deadline. A timeout, pause,
revoked advertisement or shutdown also retires an outstanding connection waiter
and releases the discovery scheduler. CoreBluetooth connection attempts have no
automatic timeout. The backend requests native cancellation only when our
callback is the sole pending waiter and there are no established GATT references;
it completes that callback without waiting for a native cancellation event. New
requests for that peripheral fail while cancellation remains unacknowledged, and
a late native connection is cancelled without starting service discovery. Other
peripherals can continue after the existing one-second scheduling delay.

Shared attempts are left running for their other callers. Discovery retains one
abandoned waiter per address and skips that address until its callback or device
removal, avoiding duplicate native attempts; abandoned waiters are capped at 128.
Late successes release their reference on a posted task so shared callbacks can
first acquire their own references. Cancellation does not add a second failure
or erase resolved endpoints. The normal per-device 30/60/120-second failure
backoff and fresh-advertisement requirement remain in effect.

After a timeout, look for `BLE GATT connect cancellation requested` and `BLE GATT
scheduler released`, then a request with a new token for another fresh candidate.
`native_cancel_requested=1` records the cancellation call; `Bluetooth macOS GATT
connect cancellation finished` records its native acknowledgement. An OS that
never acknowledges cancellation can still leave that individual peripheral
unavailable, but it no longer holds up the whole discovery queue. A retained
shared waiter reports `waiting_for_shared_callback=1`.

For macOS outgoing Bluetooth setup, `Bluetooth RFCOMM native open returned`
records the native return status and channel presence. While opening, a
`Bluetooth RFCOMM state; event=opening` snapshot runs once per second until a
delegate open/close callback or the socket's existing 10-second deadline. Fields
include `is_open`, `device_connected`, `delegate_present`, `delegate_matches`,
MTU, channel ID, native object identity and whether an open callback was received.
`is_open=0` also covers opening and closing; it does not prove a closed channel.
`is_open=1; open_callback_received=0` exposes a callback/state discrepancy, but
does not synthesize a successful connection. Delegate open/close events, write
errors and the first data event are logged without payload contents.

Releasing an RFCOMM wrapper attempts native close even when the open callback
never arrived. `Bluetooth RFCOMM native close result` reports `detach_status`
and `close_status`; a zero close result reports API success, not independent
confirmation of remote teardown. Cleanup skips a channel whose delegate has
changed to another owner. The old delegate loses its C++ owner before native
cleanup; already-posted wrapper callbacks become invalid, and native callbacks
already queued for a detached delegate have a one-turn lifetime grace period.
If native detachment fails and the channel still points to that delegate, it is
retained for a terminal callback (`retained_for_native_callback=1`) rather than
leaving a dangling native target. A late open callback retries retirement.

A channel that closes before open completion now completes the pending socket
request with an error. These changes add lifecycle diagnostics and cleanup;
hardware validation is still required for the underlying RFCOMM opening timeout.

SDP setup logs now share a `query_id` (the outgoing attempt's monotonic start in
microseconds). `Bluetooth outgoing setup started` includes the socket identity,
target address, service UUID and Unix wall-clock milliseconds for correlation
with system logs. Native query request/return/callback logs distinguish the
macOS connection-only request from the delegate request, include microsecond
timings, and mark callbacks delivered synchronously before the API returns.
`source=start_failure` means a failed API call was forwarded locally, rather
than a native delegate callback. Each reissued native query has its own
`native_request` number within the same attempt.

`Bluetooth SDP inventory` runs before the query and after completion (including
failure). It enumerates every record matching the target UUID and reports
record handle, RFCOMM/L2CAP extraction status and port, service class UUIDs and
protocol descriptors. Detailed output is capped at 32 matching records per
inventory; counts and routing comparisons include all matches. Attribute output
is limited to 32 elements and bounded depth, without names or text/URL values.
Distinct transport counts expose multiple mappings instead of hiding them
behind the first result. `Bluetooth channel open requested` records the actual
selected record/handle and descriptors even if it falls outside the log limit.
Selection remains `first_native_uuid_match`, matching the existing native lookup
policy; this diagnostic change does not guess a different channel or retry an
unverified service.

`routing_snapshot_unchanged=1` only compares the service count and sorted matching
record handles, transport statuses and ports before/after the query. It does not
mean the cache is stale or fresh, and changes to names or other attributes are
not part of this comparison. Neither immediate SDP completion nor
`getLastServicesUpdate` proves a fresh over-the-air query on the macOS bridge;
the logs explicitly report `cache_freshness_verified=0`. Native device pairing
and connection handles are also recorded for correlation; they do not establish
that an RFCOMM channel is open.

To capture one reproduction from the source root, first quit any existing
crFiles instance, then run:

```sh
python3 crQuickShare/tools/capture_mac_bluetooth.py out/arm64/crFiles
```

The collector starts a live `/usr/bin/log stream` before launching crFiles and
prints a new temporary capture directory. Reproduce once, then quit crFiles.
It saves `app.log`, `system.log`, `system-stderr.log` and `metadata.json` (OS
version, executable modification time, PID, arguments and UTC capture bounds).
An optional `--output-dir` before the executable creates a new directory and
refuses to overwrite an existing one. Ctrl+C stops the captured run. It never
builds, changes Bluetooth settings, elevates privileges or stops existing app
instances/system daemons. Logger startup failures prevent launching the app;
later logger failures are reported while application logging continues. Check
`capture_failed` and logger errors before interpreting missing system events.
If access is denied, arrange system-log capture separately with the required
local permissions and launch crFiles normally; do not run the app as root.

For hotspot/Keychain diagnosis, preserve the application bundle launch path:

```sh
python3 crFiles/tools/run_macos.py --prepare-only out/arm64
python3 crQuickShare/tools/capture_mac_bluetooth.py --network-diagnostics out/arm64/crFiles.app
```

These commands use existing build artifacts and do not compile. Close crFiles
first. The collector opens the exact bundle in the foreground through Launch
Services and streams its `app.log` to the terminal. In this mode Ctrl+C stops
capture and the launcher's wait process; quit the app separately. Additional
files are `stdout.log` and `launch-services.log`. Metadata identifies
`launcher_pid`/`launcher_returncode`; the app's own PID/exit status is not supplied
by this launch method. `--lldb` remains an executable-only probe mode and cannot
be combined with a bundle launch.

`--network-diagnostics` adds Wi-Fi/CoreWLAN, securityd/secd and Wi-Fi Keychain
service logs, plus XPC/launchd messages referring to this app or Wi-Fi Keychain
services. `codesign.log` records read-only signing diagnostics to help correlate
service authorization and code identity. The exact predicate and commands are
stored in metadata. The collector does not enable private logging or dump any
Keychain items; existing macOS privacy settings are unchanged. This flag
also works with the existing executable/LLDB capture modes.

For a timeout with `is_open=0`, correlate the target address and time window to
system connection, authentication, SDP and RFCOMM events. If unified logs do not
show the wire exchange, an additional PacketLogger/HCI capture is needed to
verify the SDP response's service-to-channel mapping and RFCOMM negotiation.
That capture can distinguish no transmitted request, rejection and no remote
response; the application logs alone cannot. The collector does not enable HCI
logging or collect raw Bluetooth packets automatically.

macOS Quick Share bootstrap connections now resolve their RFCOMM endpoint with
`MacSdpClient` before opening the data socket. This client opens its own classic
L2CAP channel to the phone's SDP server (PSM `0x0001`) using public IOBluetooth
APIs, then sends an SDP ServiceSearchAttributeRequest (`0x06`) for the advertised
service UUID. It requests only ServiceRecordHandle, ServiceClassIDList and
ProtocolDescriptorList. It validates response PDU/transaction/length fields,
assembles continuation fragments, and requires the exact target service class
and an L2CAP/RFCOMM protocol stack with a server channel in `1..30`. Multiple
records mapping to the same port are accepted; different valid ports are
reported as ambiguous instead of selecting the first one.

The query has a 10-second deadline, a 1 KiB PDU buffer, 512 attribute bytes per
response, a 16 KiB assembled attribute limit, at most 64 response transactions
and 32 records. Parsing walks only the required schema without recursive
descent. Each request's storage remains valid through native write completion;
the client waits for that completion before sending a continuation or completing
the query. Native callbacks are posted onto the UI sequence, including callbacks
that arrive before the open API returns. Cancellation invalidates pending tasks
and detaches/closes only the client's SDP channel before releasing its buffers.
It does not disconnect the device's shared baseband link or change pairing.

`Live SDP query started`, `Live SDP write requested/returned`, `Live SDP response received`
and `Live SDP query finished` trace the operation. The final log reports
`live_channel`, `cached_channel`, `mapping_changed` and
`wire_response_confirmed`. A confirmed response is distinct from successful
service resolution: an absent, malformed or ambiguous service still fails.
`mapping_changed=-1` means the live and cached routes could not be compared;
it does not mean that the cached route is current.
The cached mapping is used for comparison only; there is no fallback to it on
query failure. An unsupported native SDP channel or missing native callback
therefore produces a bounded, explicit live-SDP failure.

On success the Quick Share socket forwards the resolved port to the new public
`BluetoothSocketMac::ConnectToRfcommChannel` entry point. This method validates
the port and shares the existing native channel-open/data/cleanup path, without
another system-cache lookup (`selection=caller_resolved_channel`). Other
Chromium callers of `BluetoothSocketMac::Connect` retain their existing SDP
behavior. The Quick Share wrapper remains the pending socket throughout both
phases, so transfer cancellation covers live discovery and RFCOMM opening.
The live query does not repair the macOS bridge's missing RFCOMM failure
callback: if the phone rejects even the live port and the bridge omits the
callback, the existing 10-second channel deadline still applies. A hardware
reproduction is needed to establish whether a stale cached port caused the
observed rejection.

Wire format reference: [Bluetooth Core, SDP Service Search Attribute transaction](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-62/out/en/host/service-discovery-protocol--sdp--specification.html).

To diagnose a native channel that connects in system logs but never becomes
ready in the application, enable automatic native stack capture:

```sh
python3 crQuickShare/tools/capture_mac_bluetooth.py --lldb out/arm64/crFiles
```

`--lldb` is a collector option and must precede the executable. It launches a
new process under Xcode's LLDB with user LLDB initialization disabled. It never
attaches to an existing process. The usual four capture files remain, and
`app-stdout.log`, `lldb.log`, `lldb-config.json`, `lldb-process.json`, `lldb-result.json` and
`native-stacks.jsonl` describe the debugger session. Check both `capture_failed`
in metadata and `success`/`cleanup_confirmed` in the debugger result. Debugger
launch/permission errors are explicit failures; the collector never elevates
privileges or changes signing, debugging permissions or Bluetooth settings.
The debugger opens stdout and stderr paths separately: application diagnostics
from stderr go to `app.log`, and stdout goes to `app-stdout.log`. This avoids
macOS debugserver routing stderr through LLDB's console when given a duplicate
file action. Captures made before this correction may have an empty `app.log`;
their application diagnostics remain available in `lldb.log` and unified logs.

Breakpoints record native L2CAP open, delegate assignment, close, channel
destruction, and our delegate's open/closed entry points. They also watch the
system bridge's `CBL2CAPChannel` destruction and `CBClassicPeer` close method;
these names are debugger observation points, not private APIs called by the
application. Each hit records UTC, PID/TID, argument registers and up to 24
frames, then automatically continues. In ordinary `--lldb` captures, no debugger
expressions, object descriptions or Objective-C messages run in the target. After 256 hits the
breakpoints are disabled and a `hit_limit` record is emitted. Breakpoint pauses
can still affect timing; compare with a normal capture if behavior changes.
Unresolved methods and hit counts are listed in `lldb-result.json`; missing
symbols or zero hits alone do not prove that a method was never called.

`Live SDP native event` logs the callback before posting to the UI sequence.
`Live SDP UI delivery` logs queue delay and whether the owner still exists.
`Live SDP state` logs the returned/callback channel addresses, delegate identity,
local CID and the disposition before any state or identity check can discard an
event. `Live SDP cleanup requested` precedes application detachment/close and
includes the cleanup reason. These all share the query ID, and a retired
delegate keeps its original ID even after the client starts another query.
State inspection stays on the UI sequence; native entries only log identity.
The public L2CAP API has no RFCOMM-style `isOpen` accessor, so local CID and
observed callbacks are reported without inferring openness from a nonzero CID.

On the observed macOS 27 cold-connection path, the framework reported device
connection completion and internally opened L2CAP synchronously before invoking
our open delegate with a different wrapper from the asynchronous return value.
`Live SDP state` now reports both wrappers' device/address, delegate, PSM,
direction and local/remote CIDs. `delegate_matches` refers to the stored wrapper;
`callback_delegate_matches` refers to the callback wrapper. Callback source and
query ID are checked independently of the wrapper address.

A successful open can hand off to a replacement only once, before any SDP
transaction starts. Both wrappers must be outgoing PSM 1 channels for the
canonical target address and have this query's delegate. `channel_handoff` with
`disposition=accepted` records the change; `channel_handoff_rejected` records
the failing condition and ends the query without waiting for its deadline.
Normal opens validate ownership as well. No handoff is attempted from data,
write or close callbacks. All subsequent I/O uses the accepted wrapper, while
events from the superseded wrapper are ignored explicitly. This does not make
a missing native open callback succeed or establish a live SDP route by itself.

The superseded wrapper remains retained until query completion/cancellation;
it is not detached or closed during handoff because the wrappers may share
native state. Cleanup first retires the active wrapper, then the superseded
one, respecting delegate ownership. Retirement retains each channel whose
delegate could not be detached, so cleaning one wrapper does not drop the
retirement protection for another. Application code uses only public APIs.

The debugger also captures the bridge between the manager's connection-status
handler, each classic peer's L2CAP callbacks, and the IOBluetooth wrapper's
`_cbChannel`. Selected object fields are read directly from target memory;
their offsets and encodings are discovered from local framework metadata for
each run, with matching host/target architecture required. `runtime_layout`
records the schema, and `bridge` on breakpoint records contains the snapshots.
Object contents such as device names and transferred data are not read.
Additional passive breakpoints watch the L2CAP wrapper's device-connection
completion, the device's synchronous L2CAP open, and wrapper initialization.
Our delegate entry snapshots the callback wrapper and its native bridge, allowing
the returned and callback objects to be correlated through the internal reopen
path without invoking any private methods.

When a connection-status or peer L2CAP callback is assigned, the debugger reads
the block's invoke pointer using the [Clang block ABI](https://clang.llvm.org/docs/Block-ABI-Apple.html),
normalizes the code address through LLDB, and watches that exact address.
`block_watch_configured` and `block:connection_status`/`block:l2cap_connected`
hit counts distinguish registration from invocation without executing the
callback. No methods are swizzled and no callback slots or target memory are
modified. Private field and method names here describe debugger observations,
not additional application dependencies on those APIs.

In the observed macOS 27 failure, the manager's status handler was installed
but never invoked during the probe. The target peer was already connected;
both peer L2CAP callback slots stayed null and their setters were never hit.
The native channel and wrapper referred to the same peer, but
`peerL2CAPChannelConnected:error:` and `setCbChannel:` were never reached.
The native channel was released after the successful open event while the
wrapper remained alive until timeout. This identifies missing peer callback
initialization before native-to-wrapper association, rather than an application
delegate being released early. It does not establish a public API workaround.

On arm64, at function entry `x0` is the receiver; `x2` is the first explicit
argument. For our open callback `x2` is the callback channel and `x3` is the
status. For the device's open API, `x2` is the output-pointer address, `x3` the
PSM, and `x5` the delegate. Correlate these with the application's channel and
delegate addresses. A framework `dealloc` line alone does not establish that
the returned channel was destroyed; it may refer to a temporary wrapper.

An isolated probe uses the same executable and SDP client but bypasses singleton
activation, the file window, discovery, FastInit and RFCOMM connection:

```sh
python3 crQuickShare/tools/capture_mac_bluetooth.py --lldb out/arm64/crFiles --quick-share-mac-sdp-probe=94:45:60:6D:A7:D5
```

Replace the address with the classic address from `Bootstrap connection entry`,
not the BLE discovery address. Keep the phone's Quick Share receive page open.
The probe performs one normal SDP query and exits automatically (0 for a usable
live route, 1 for query failure, 2 for invalid address); it does not send a file.
Quit other crFiles instances for an isolated baseline. A separate full-app run
with discovery enabled provides the comparison; do not run the two at once.

The opt-in `--public-init-order` collector option compares public API
initialization sequences using the existing executable, without rebuilding it:

```sh
python3 crQuickShare/tools/capture_mac_bluetooth.py --lldb --public-init-order settled-first out/arm64/crFiles --quick-share-mac-sdp-probe=94:45:60:6D:A7:D5
```

This option is accepted only with `--lldb` and the single isolated SDP probe
argument. Each comparison must use a new process and capture directory.

| Order | Additional public API calls |
| --- | --- |
| `controller-first` | `IOBluetoothHostController.defaultController` before obtaining the device |
| `inquiry-first` | `IOBluetoothDeviceInquiry.inquiryWithDelegate:nil` before obtaining the device; does not start inquiry |
| `connection-first` | Check `isConnected`, then `openConnection:nil` before opening the SDP channel |
| `all-first` | Controller, Inquiry, device, then connection-first sequence |
| `settled-first` | Same as all-first, with one second of `CFRunLoopRunInMode` after Controller/Inquiry initialization and before obtaining the device |
| `reconnect` | Obtain the target device, call `closeConnection`, confirm disconnection, then call `openConnection:nil` and confirm reconnection before starting SDP |

Unlike passive captures, this explicit experiment runs fixed LLDB expressions
that call only the listed public APIs. It does not invoke private bridge
methods, modify callback fields, pair a device or send a file. Only the explicit
`reconnect` variant disconnects the target's existing baseband link.
Connection-first checks require an already-connected
device: a nil target makes [openConnection:](https://developer.apple.com/documentation/iobluetooth/iobluetoothdevice/openconnection(_:)?language=objc)
synchronous, so those three variants stop if `isConnected` is false. Ordinary
expressions have a three-second debugger timeout; an expression failure aborts the owned
probe and is reported as a failed capture.

`public_api_requested`/`public_api_returned` record execution, result and elapsed
time; `public_init_device_before`/`public_init_device_after` record the peer's
callback slots around the connection call. Trace breakpoints are suppressed
while an expression executes; hit counts cannot establish what ran inside that
interval. After it returns, passive observations recover installed callback
blocks from memory and resume tracing. Interpret the final slot values, native
channel association and SDP result together. `success` in `lldb-result.json`
means that capture and initialization stages completed; `exit_status=1` still
means the SDP query failed.

On macOS 27, one baseline and one run of each of the five sequences above all
reproduced the same already-connected Android failure. Public initializers
returned objects; the three connection-first variants reported `isConnected=1`
and `openConnection:nil` returned 0. The settled variant spent 1,022 ms in its
run loop window. In every run, the peer was in internal state 2 with both L2CAP
callback slots null when the native open event arrived. The native channel had
a nonzero CID and the same peer as the wrapper, but the wrapper's `_cbChannel`
remained null, neither forwarding nor association callback ran after resuming,
and SDP timed out. These tested initialization orders did not repair the bridge
in this scenario. That matrix did not cover a genuinely new baseband connection;
the separate reconnection result below does not establish file delivery.

The explicit reconnection probe can run against the same existing executable:

```sh
python3 crQuickShare/tools/capture_mac_bluetooth.py --lldb --public-init-order reconnect out/arm64/crFiles --quick-share-mac-sdp-probe=94:45:60:6D:A7:D5
```

This variant really calls the public [closeConnection](https://developer.apple.com/documentation/iobluetooth/iobluetoothdevice/closeconnection())
API on the specified phone. It interrupts other Bluetooth services on that
device without changing pairing or the adapter's power state. It does not run
on the production send path. The experiment stops before the first device lookup,
creates and retains the target through the public constructor, and completes
reconnection before the SDP deadline and native channel exist. It allows eight
seconds for each state transition, five seconds for the close expression and
25 seconds for synchronous open expressions. If initially disconnected, it first
establishes a connection so the subsequent close/open comparison has a connected
baseline. A reconnect request and state check are also attempted if waiting for
disconnection fails. Debugger execution failures can prevent recovery and remain
explicit failures. The diagnostic target is retained until the owned probe exits.

`public_reconnect_state` records public `isConnected` and passive peer snapshots
at each stage. A disconnected public state and peer state 0 must both be seen
before the new open request; reconnection requires public connected state and
peer state 2. `reconnect_transition_observed` records that local sequence, and
`reconnect_connected_confirmed` records the final connected check. Neither flag
alone proves an actual ACL disconnect; correlate `ACL disconnected` and
`ACL connected` for the same address in `system.log`.

In the macOS 27 reconnection validation, the phone started connected with null
L2CAP callbacks. The close expression returned 0 in 11 ms, but the daemon's ACL
disconnect event arrived about three seconds later. The peer then reached state
0. The new open returned 0 in 3,437 ms; the daemon confirmed ACL connection and
the peer reached state 2 with both L2CAP callback slots populated. The native
L2CAP callback block, `peerL2CAPChannelConnected:error:` and `setCbChannel:` were
then observed, and `_cbChannel` acquired the native channel. SDP wrote 38 bytes,
received a 60-byte response, resolved the live Quick Share RFCOMM channel 18 and
exited 0 with `wire_response_confirmed=1`. This supports the missing-initialization
diagnosis for an already-connected peer: a real connection transition repaired
the bridge in that process. RFCOMM bootstrap and file transfer were not tested.

A subsequent macOS 27 foreground capture distinguished this missing-callback
case from the replacement-wrapper case described above. The first PSM 1 open
at 19:29:11.128 succeeded in bluetoothd at 19:29:11.341 (CID `0x5409`).
CoreBluetooth received the open event and requested channel close at
19:29:11.344, without `connectL2CAPCallback`, wrapper association or an application
open callback. The application's deadline expired about ten seconds after its
request. An actual reconnect restored forwarding and the next SDP query
completed in 66 ms. No replacement-wrapper handoff was attempted in that capture.

Read-only inspection of that local framework's method implementations narrows
the initialization condition further. The coordinator installs the peer's
L2CAP and RFCOMM callbacks inside its connection-status handler when the peer
is connected; its disconnected path clears them. Looking up an already-connected
peer does not perform the installation. The already-connected branch of
`openConnection:` returns success, optionally scheduling the caller's completion,
without requesting a new peer connection. This explains why a connection-first
public API call did not repair the failed peer while a real disconnect/open
transition did. These implementation details are observations of this OS build,
not APIs or private fields used by application code. The macOS 27 initial-send
preparation above uses the verified public transition; callback identity checks
and the guarded replacement-wrapper handoff remain in place.

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
