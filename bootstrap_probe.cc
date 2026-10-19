// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#include "crQuickShare/bootstrap_probe.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_WIN)
#include "device/bluetooth/bluetooth_socket_win.h"
#include "device/bluetooth/bluetooth_socket_thread.h"
#endif
#include <algorithm>
#include <condition_variable>
#include <chrono>
#include <optional>
#include <mutex>
#include <thread>
#include <utility>
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/timer/timer.h"
#include "base/task/single_thread_task_runner.h"
#include "crQuickShare/transport_channel.h"
#include "device/bluetooth/bluetooth_adapter.h"
#include "device/bluetooth/bluetooth_device.h"
#include "device/bluetooth/bluetooth_discovery_filter.h"
#include "device/bluetooth/bluetooth_discovery_session.h"
#include "device/bluetooth/bluetooth_socket.h"
#include "device/bluetooth/public/cpp/bluetooth_uuid.h"
#include "net/base/io_buffer.h"
namespace crquickshare {
namespace {
constexpr size_t kBufferLimit = 64 * 1024;
// nearby::Uuid("NearbySharing"): Java nameUUIDFromBytes / MD5 UUID v3.
constexpr char kServiceUuid[] = "a82efa21-ae5c-3dde-9bbc-f16da7b16c5a";
struct Pipe {
  std::mutex mutex;
  std::condition_variable changed;
  std::string incoming, outgoing;
  std::string connection_error;
  bool closed = false, done = false, cancelled = false;
  bool close_requested = false;
  bool setup_required = false, setup_requested = false;
  bool gatt_released = false, connected = false;
  base::TimeTicks connect_started;
  uint64_t sent_bytes = 0;
  // Installed on the UI sequence before publishing this pipe to the worker.
  // Posting never dereferences the UI-owned WeakPtr on the worker sequence.
  std::function<void()> post_pump;
  bool pump_posted = false;
  void Wake() {
    {
      std::lock_guard guard(mutex);
      if (pump_posted || !post_pump) return;
      pump_posted = true;
    }
    post_pump();
  }
  void Finished() {
    { std::lock_guard guard(mutex); done = true; changed.notify_all(); }
    Wake();
  }
};
class ProbeChannel final : public TransportChannel {
 public:
  explicit ProbeChannel(std::shared_ptr<Pipe> pipe);
  ~ProbeChannel() override;
  Result Read(base::span<char> bytes) override;
  Result Write(base::span<const char> bytes) override;
  Ready Wait(bool read, bool write, int timeout_ms) override;
  void Close() override;
  const char* name() const override;
  std::string connection_error() const override;
  SetupResult PrepareConnection(const std::function<bool()>& active,
                               const std::function<void(const char*)>& stage) override;
 private:
  std::shared_ptr<Pipe> pipe_;
};
ProbeChannel::ProbeChannel(std::shared_ptr<Pipe> pipe) : pipe_(std::move(pipe)) {}
ProbeChannel::~ProbeChannel() { Close(); }
TransportChannel::SetupResult ProbeChannel::PrepareConnection(
    const std::function<bool()>& active,
    const std::function<void(const char*)>& stage) {
  {
    std::lock_guard guard(pipe_->mutex);
    if (!pipe_->setup_required) return {};
    pipe_->setup_requested = true;
  }
  pipe_->Wake();
  const auto started = base::TimeTicks::Now();
  const auto overall_deadline = started + base::Seconds(45);
  bool connecting = false;
  stage("gatt_release_wait");
  while (active()) {
    std::unique_lock guard(pipe_->mutex);
    if (pipe_->closed || pipe_->done)
      return {false, false, pipe_->connection_error.empty() ?
          "Bluetooth connection setup stopped" : pipe_->connection_error};
    if (pipe_->gatt_released && !connecting) {
      connecting = true;
      guard.unlock();
      stage("bluetooth_connect");
      continue;
    }
    if (pipe_->connected) return {};
    const auto until = std::min(overall_deadline, connecting ?
        pipe_->connect_started + base::Seconds(20) : started + base::Seconds(25));
    if (base::TimeTicks::Now() >= until) {
      const std::string error = connecting ? "Bluetooth connection timed out" :
          "Bluetooth GATT release timed out";
      pipe_->connection_error = error;
      pipe_->closed = true;
      pipe_->changed.notify_all();
      return {false, true, error};
    }
    pipe_->changed.wait_for(guard, std::chrono::milliseconds(100));
  }
  std::lock_guard guard(pipe_->mutex);
  return {false, false, pipe_->connection_error.empty() ?
      "Bluetooth connection setup cancelled" : pipe_->connection_error};
}
std::string ProbeChannel::connection_error() const {
  std::lock_guard guard(pipe_->mutex); return pipe_->connection_error;
}
TransportChannel::Result ProbeChannel::Read(base::span<char> bytes) {
  std::unique_lock guard(pipe_->mutex);
  const size_t count = std::min(bytes.size(), pipe_->incoming.size());
  if (count) {
    bytes.first(count).copy_from(base::span(pipe_->incoming).first(count));
    pipe_->incoming.erase(0, count);
    const bool drained = pipe_->incoming.empty();
    guard.unlock();
    if (drained) pipe_->Wake();
    return {Status::kOk, count, 0};
  }
  return {pipe_->closed ? Status::kClosed : Status::kWouldBlock, 0, 0};
}
TransportChannel::Result ProbeChannel::Write(base::span<const char> bytes) {
  std::unique_lock guard(pipe_->mutex);
  if (pipe_->closed) return {Status::kClosed, 0, 0};
  const size_t count = std::min(bytes.size(), kBufferLimit - pipe_->outgoing.size());
  if (!count) return {Status::kWouldBlock, 0, 0};
  pipe_->outgoing.append(bytes.data(), count);
  guard.unlock();
  pipe_->Wake();
  return {Status::kOk, count, 0};
}
TransportChannel::Ready ProbeChannel::Wait(bool read, bool write, int timeout_ms) {
  std::unique_lock lock(pipe_->mutex);
  pipe_->changed.wait_for(lock, std::chrono::milliseconds(std::clamp(timeout_ms, 0, 100)), [&] {
    return pipe_->closed || (read && !pipe_->incoming.empty()) || (write && pipe_->outgoing.size() < kBufferLimit);
  });
  return {read && (pipe_->closed || !pipe_->incoming.empty()),
          write && (pipe_->closed || pipe_->outgoing.size() < kBufferLimit), 0};
}
void ProbeChannel::Close() {
  { std::lock_guard guard(pipe_->mutex);
    pipe_->closed = true; pipe_->close_requested = true; pipe_->changed.notify_all(); }
  pipe_->Wake();
}
const char* ProbeChannel::name() const { return "BLUETOOTH"; }
}
struct BootstrapProbe::Impl {
  std::string endpoint;
  bool attempted = false, missing_logged = false, stopped = false;
  bool reading = false, writing = false;
  bool bluetooth_closing = false;
  base::TimeTicks bluetooth_close_started;
  uint64_t diagnostic_session = 0;
  scoped_refptr<device::BluetoothAdapter> adapter;
  scoped_refptr<device::BluetoothSocket> socket;
  std::string classic_address;
  std::unique_ptr<device::BluetoothDiscoverySession> classic_discovery;
  base::RepeatingTimer classic_poll;
  base::OneShotTimer classic_timeout;
  base::OneShotTimer service_retry;
  bool service_retry_used = false;
  std::shared_ptr<Pipe> pipe = std::make_shared<Pipe>();
  std::thread worker;
  HotspotConnector hotspot;
  bool normal_transfer = false;
  scoped_refptr<device::BluetoothSocket> pending_socket;
  std::optional<Peer> pending_peer;
  bool connection_ready = true;
  base::OneShotTimer deadline;
  base::WeakPtrFactory<Impl> weak{this};
  explicit Impl(bool command_line_probe = true) {
    auto runner = base::SingleThreadTaskRunner::GetCurrentDefault();
    auto callback = base::BindRepeating(&Impl::OnWake, weak.GetWeakPtr());
    pipe->post_pump = [runner, callback] { runner->PostTask(FROM_HERE, callback); };
    if (!command_line_probe) return;
    const auto* command = base::CommandLine::ForCurrentProcess();
    if (!command->HasSwitch("quick-share-bootstrap-endpoint")) return;
    const auto value = command->GetSwitchValueASCII("quick-share-bootstrap-endpoint");
    if (value.size() != 8 || !base::HexStringToString(value, &endpoint) || endpoint.size() != 4) {
      endpoint.clear();
      LOG(WARNING) << "crQuickShare: Bootstrap probe disabled; reason=invalid_endpoint_hex";
    } else {
#if BUILDFLAG(IS_LINUX)
      if (command->HasSwitch("quick-share-upgrade-interface")) {
        hotspot = MakeLinuxHotspotConnector(command->GetSwitchValueASCII("quick-share-upgrade-interface"));
        if (!hotspot) {
          endpoint.clear();
          LOG(WARNING) << "crQuickShare: Bootstrap probe disabled; reason=invalid_upgrade_interface";
          return;
        }
      }
#endif
      LOG(INFO) << "crQuickShare: Bootstrap probe armed; endpoint_id=" << value
                << "; medium=BLUETOOTH; one_attempt=1; attachments_sent=0";
    }
  }
  ~Impl() { Stop("destroyed"); }
  void Stop(const char* reason) {
    if (stopped || !attempted) return;
    stopped = true; weak.InvalidateWeakPtrs(); deadline.Stop();
    service_retry.Stop();
    classic_poll.Stop(); classic_timeout.Stop(); classic_discovery.reset();
    bool done;
    { std::lock_guard guard(pipe->mutex); pipe->closed = true; pipe->cancelled = true; done = pipe->done; pipe->changed.notify_all(); }
    if (pending_socket) { pending_socket->Disconnect(base::DoNothing()); pending_socket.reset(); }
    if (socket) { socket->Disconnect(base::DoNothing()); socket.reset(); }
    // NetworkManager rollback is bounded but may take seconds. It owns no UI
    // objects; cancellation must not block the UI while restoring the network.
    if (worker.joinable()) { if (done) worker.join(); else worker.detach(); }
    adapter.reset();
    LOG(INFO) << "crQuickShare: Bootstrap probe stopped; reason=" << reason;
  }
  void Failed(const std::string& message) {
    if (bluetooth_closing) return;
    {
      std::unique_lock guard(pipe->mutex);
      if (pipe->close_requested) {
        // Optional shutdown bytes can race the peer closing the old socket.
        // Do not cancel the worker that now owns the replacement channel.
        pipe->outgoing.clear(); writing = false; pipe->changed.notify_all();
        guard.unlock();
        pipe->Wake();
        LOG(INFO) << "crQuickShare: Bootstrap Bluetooth shutdown write ended; probe_continues=1";
        return;
      }
    }
    LOG(WARNING) << "crQuickShare: Bootstrap Bluetooth error; connected=" << bool(socket) << "; reason=" << message;
    if (!socket) {
      std::lock_guard guard(pipe->mutex);
      pipe->connection_error = "Bluetooth connection failed: " + message;
    }
    Stop(socket ? "bluetooth_io_failed" : "bluetooth_connect_failed");
  }
  void FindClassicDevice() {
    if (!adapter || !adapter->IsPresent() || !adapter->IsPowered()) {
      Failed("Bluetooth adapter unavailable during classic discovery"); return;
    }
    auto* target = adapter->GetDevice(classic_address);
    if (!target) return;
    classic_poll.Stop(); classic_timeout.Stop();
    LOG(INFO) << "crQuickShare: Bootstrap classic discovery matched; address_source=protocol";
    ConnectService(target);
  }
  void ConnectDeviceFailed(const std::string& message) {
    // Older BlueZ installations may not expose Adapter1.ConnectDevice.
    // Inquiry creates the Device1 object without pairing or guessing an address.
    if (message.find("ConnectDevice") == std::string::npos ||
        (message.find("doesn't exist") == std::string::npos &&
         message.find("UnknownMethod") == std::string::npos)) {
      Failed(message); return;
    }
    LOG(INFO) << "crQuickShare: Bootstrap classic discovery fallback; reason=connect_device_unavailable; timeout_ms=8000";
    classic_timeout.Start(FROM_HERE, base::Seconds(8), base::BindOnce(
        &Impl::Failed, weak.GetWeakPtr(), "Classic Bluetooth target not found before discovery timeout"));
    auto filter = std::make_unique<device::BluetoothDiscoveryFilter>(device::BLUETOOTH_TRANSPORT_CLASSIC);
    adapter->StartDiscoverySessionWithFilter(std::move(filter), "crQuickShare bootstrap",
        base::BindOnce([](base::WeakPtr<Impl> self, std::unique_ptr<device::BluetoothDiscoverySession> discovery) {
          if (!self) return;
          self->classic_discovery = std::move(discovery);
          self->classic_poll.Start(FROM_HERE, base::Milliseconds(100),
              base::BindRepeating(&Impl::FindClassicDevice, self));
          self->FindClassicDevice();
        }, weak.GetWeakPtr()),
        base::BindOnce(&Impl::Failed, weak.GetWeakPtr(), "Classic Bluetooth discovery could not start"));
  }
  void ServiceConnectFailed(const std::string& message) {
#if BUILDFLAG(IS_LINUX)
    bool can_retry = false;
    {
      std::lock_guard guard(pipe->mutex);
      can_retry = normal_transfer && !service_retry_used && !socket && !stopped &&
          !pipe->closed && !pipe->done && !pipe->cancelled && !pipe->connected &&
          message.find("br-connection-canceled") != std::string::npos;
    }
    if (can_retry) {
      service_retry_used = true;
      LOG(INFO) << "crQuickShare: Bootstrap service retry scheduled; attempt=2; delay_ms=500; reason=" << message;
      service_retry.Start(FROM_HERE, base::Milliseconds(500),
          base::BindOnce(&Impl::RetryService, weak.GetWeakPtr()));
      return;
    }
#endif
    Failed(message);
  }
  void RetryService() {
    {
      std::lock_guard guard(pipe->mutex);
      if (pipe->closed || pipe->done || pipe->cancelled || pipe->connected) return;
    }
    if (!adapter || !adapter->IsPresent() || !adapter->IsPowered()) {
      Failed("Bluetooth adapter unavailable during service retry");
      return;
    }
    auto* target = adapter->GetDevice(classic_address);
    if (!target) { Failed("Bluetooth device unavailable during service retry"); return; }
    ConnectService(target);
  }
  void ConnectService(device::BluetoothDevice* device) {
    if (!device) { Stop("device_unavailable"); return; }
    LOG(INFO) << "crQuickShare: Bootstrap service connect requested; service_uuid=" << kServiceUuid;
    // Disconnect even if a successful asynchronous result arrives after timeout.
    device->ConnectToServiceInsecurely(device::BluetoothUUID(kServiceUuid),
        base::BindOnce([](base::WeakPtr<Impl> self, scoped_refptr<device::BluetoothSocket> connected) {
          if (self) self->Connected(std::move(connected));
          else connected->Disconnect(base::DoNothing());
        }, weak.GetWeakPtr()), base::BindOnce(&Impl::ServiceConnectFailed, weak.GetWeakPtr()));
  }
  void Observe(scoped_refptr<device::BluetoothAdapter> value, const Peer& peer) {
    if (endpoint.empty() || peer.id != endpoint || attempted || !value || !value->IsPowered()) return;
    if (peer.classic_bluetooth_address.empty() || peer.name.empty()) {
      if (!missing_logged) {
        missing_logged = true;
        LOG(INFO) << "crQuickShare: Bootstrap waiting for connection entry; classic_address_present="
                  << !peer.classic_bluetooth_address.empty() << "; public_name_present=" << !peer.name.empty();
      }
      return;
    }
    attempted = true; adapter = std::move(value);
    classic_address = peer.classic_bluetooth_address;
    LOG(INFO) << "crQuickShare: Bootstrap connection entry; endpoint_id=" << base::HexEncode(peer.id)
              << "; classic_address=" << peer.classic_bluetooth_address << "; address_source=protocol";
    deadline.Start(FROM_HERE, base::Seconds(hotspot ? 120 : 45), base::BindOnce(&Impl::Stop, weak.GetWeakPtr(), "deadline"));
#if BUILDFLAG(IS_WIN)
    auto connecting = device::BluetoothSocketWin::CreateBluetoothSocket(
        base::SingleThreadTaskRunner::GetCurrentDefault(), device::BluetoothSocketThread::Get());
    pending_socket = connecting;
    connecting->ConnectByAddress(peer.classic_bluetooth_address, device::BluetoothUUID(kServiceUuid),
        base::BindOnce([](base::WeakPtr<Impl> self, scoped_refptr<device::BluetoothSocket> connected) {
          // Stop() disconnects pending_socket when cancelling this operation;
          // do not disconnect it twice.
          if (self) { self->pending_socket.reset(); self->Connected(std::move(connected)); }
        }, weak.GetWeakPtr(), connecting), base::BindOnce(&Impl::Failed, weak.GetWeakPtr()));
#else
    if (auto* device = adapter->GetDevice(peer.classic_bluetooth_address)) ConnectService(device);
    else adapter->ConnectDevice(peer.classic_bluetooth_address, std::nullopt,
        base::BindOnce(&Impl::ConnectService, weak.GetWeakPtr()), base::BindOnce(&Impl::ConnectDeviceFailed, weak.GetWeakPtr()));
#endif
  }
  void Connected(scoped_refptr<device::BluetoothSocket> connected) {
    {
      std::unique_lock guard(pipe->mutex);
      if (pipe->closed || pipe->done) {
        guard.unlock();
        connected->Disconnect(base::DoNothing());
        return;
      }
      pipe->connected = true;
      pipe->changed.notify_all();
    }
    socket = std::move(connected);
    classic_discovery.reset();
    LOG(INFO) << "crQuickShare: Bootstrap service connected; medium=BLUETOOTH";
    if (normal_transfer) deadline.Stop();  // The session owns transfer deadlines.
    else worker = std::thread([shared = pipe, connector = hotspot] {
      QuickShareService::ProbeBootstrap(std::make_unique<ProbeChannel>(shared), [shared] {
        std::lock_guard guard(shared->mutex); return shared->sent_bytes;
      }, connector, [shared] { std::lock_guard guard(shared->mutex); return shared->cancelled; });
      shared->Finished();
    });
    pipe->Wake();
  }
  void OnWake() {
    // Clear before examining queues. A concurrent writer can then enqueue the
    // next wake, so coalescing cannot lose work at the end of this invocation.
    { std::lock_guard guard(pipe->mutex); pipe->pump_posted = false; }
    Pump();
  }
  void ConnectionReady() {
    connection_ready = true;
    pipe->Wake();
  }
  void Pump() {
    if (stopped) return;
    bool done;
    { std::lock_guard guard(pipe->mutex); done = pipe->done; }
    if (done) { Stop("transfer_finished"); return; }
    if (pending_peer) {
      bool start_connect;
      { std::lock_guard guard(pipe->mutex); start_connect = pipe->setup_requested && !pipe->closed; }
      if (start_connect && connection_ready) {
        if (!adapter || !adapter->IsPowered()) { Stop("bluetooth_unavailable"); return; }
        auto peer = std::move(*pending_peer); pending_peer.reset();
        // Source preflight has completed; handshake waits for this socket.
        {
          std::lock_guard guard(pipe->mutex);
          pipe->gatt_released = true;
          pipe->connect_started = base::TimeTicks::Now();
          pipe->changed.notify_all();
        }
        attempted = false;
        Observe(adapter, peer);
      }
    }
    if (!socket && !bluetooth_closing) return;
    bool receive = false, finished = false, close_bluetooth = false;
    scoped_refptr<net::StringIOBuffer> buffer;
    {
      std::lock_guard guard(pipe->mutex);
      finished = pipe->done && pipe->outgoing.empty() && !writing;
      close_bluetooth = pipe->close_requested && pipe->outgoing.empty() && !writing && !bluetooth_closing;
      if (!finished) {
        receive = !pipe->done && !pipe->closed && !reading && pipe->incoming.empty();
        if (!writing && !pipe->outgoing.empty()) buffer = base::MakeRefCounted<net::StringIOBuffer>(pipe->outgoing);
      }
    }
    if (finished) { Stop("finished"); return; }
    if (close_bluetooth) {
      bluetooth_closing = true;
      bluetooth_close_started = base::TimeTicks::Now();
      LOG(INFO) << "crQuickShare: Bootstrap Bluetooth socket close requested; pending_bytes=0; session="
                << diagnostic_session << "; read_pending=" << reading;
      socket->Disconnect(base::BindOnce([](base::WeakPtr<Impl> self) {
        if (!self) return;
        self->socket.reset();
        LOG(INFO) << "crQuickShare: Bootstrap Bluetooth socket closed; probe_continues=1; session="
                  << self->diagnostic_session << "; close_elapsed_ms="
                  << (base::TimeTicks::Now() - self->bluetooth_close_started).InMilliseconds();
      }, weak.GetWeakPtr()));
      return;
    }
    if (bluetooth_closing) return;
    if (buffer) {
      writing = true;
      const int size = buffer->size();
      socket->Send(buffer, size, base::BindOnce(&Impl::Sent, weak.GetWeakPtr(), size),
                   base::BindOnce(&Impl::Failed, weak.GetWeakPtr()));
    }
    if (stopped) return;
    if (receive) {
      reading = true;
      socket->Receive(static_cast<int>(kBufferLimit), base::BindOnce(&Impl::Received, weak.GetWeakPtr()),
          base::BindOnce(&Impl::ReadFailed, weak.GetWeakPtr()));
    }
  }
  void Sent(int requested, int count) {
    if (count <= 0 || count > requested) { Stop("invalid_send_result"); return; }
    std::unique_lock guard(pipe->mutex);
    pipe->sent_bytes += static_cast<uint64_t>(count);
    pipe->outgoing.erase(0, static_cast<size_t>(count)); writing = false; pipe->changed.notify_all();
    guard.unlock();
    pipe->Wake();
  }
  void Received(int count, scoped_refptr<net::IOBuffer> buffer) {
    if (bluetooth_closing) {
      LOG(INFO) << "crQuickShare: Bluetooth receive after close requested; session="
                << diagnostic_session << "; bytes=" << count;
      return;
    }
    if (count == 0) {
      reading = true;
      std::lock_guard guard(pipe->mutex);
      pipe->closed = true; pipe->changed.notify_all();
      LOG(INFO) << "crQuickShare: Bootstrap Bluetooth read ended; eof=1";
      return;
    }
    if (count <= 0 || count > static_cast<int>(kBufferLimit) || !buffer || count > buffer->size()) {
      Stop("invalid_receive_result"); return;
    }
    std::lock_guard guard(pipe->mutex);
    pipe->incoming.assign(buffer->data(), static_cast<size_t>(count)); reading = false; pipe->changed.notify_all();
  }
  void ReadFailed(device::BluetoothSocket::ErrorReason reason, const std::string& message) {
    if (bluetooth_closing) {
      LOG(INFO) << "crQuickShare: Bluetooth read error after close requested; session="
                << diagnostic_session << "; reason=" << static_cast<int>(reason);
      return;
    }
    LOG(INFO) << "crQuickShare: Bootstrap Bluetooth read ended; reason=" << static_cast<int>(reason)
              << "; detail=" << message;
    reading = true;
    std::lock_guard guard(pipe->mutex); pipe->closed = true; pipe->changed.notify_all();
  }
};
BootstrapProbe::BootstrapProbe(bool command_line_probe) : impl_(std::make_unique<Impl>(command_line_probe)) {}
BootstrapProbe::~BootstrapProbe() = default;
void BootstrapProbe::Observe(scoped_refptr<device::BluetoothAdapter> adapter, const Peer& peer) {
  impl_->Observe(std::move(adapter), peer);
}
void BootstrapProbe::Cancel() { impl_->Stop("discovery_stopped"); }
bool BootstrapProbe::CanProbe(const Peer& peer) const {
  return !impl_->attempted && !impl_->endpoint.empty() && peer.id == impl_->endpoint &&
      !peer.name.empty() && !peer.classic_bluetooth_address.empty();
}
bool BootstrapProbe::active() const { return impl_->attempted && !impl_->stopped; }
bool BootstrapProbe::Receive(scoped_refptr<device::BluetoothSocket> socket, QuickShareService& service, HotspotConnector hotspot) {
  if (!socket) return false;
  if (active()) { socket->Disconnect(base::DoNothing()); return false; }
  impl_ = std::make_unique<Impl>(false);
  impl_->attempted = true; impl_->normal_transfer = true;
  impl_->socket = std::move(socket);
  auto shared = impl_->pipe;
  if (!service.ReceiveBluetooth(std::make_unique<ProbeChannel>(shared),
      [shared] { std::lock_guard guard(shared->mutex); return shared->sent_bytes; },
      [shared] { std::lock_guard guard(shared->mutex); return shared->cancelled; },
      [shared] { shared->Finished(); }, std::move(hotspot))) {
    impl_->Stop("incoming_rejected");
    return false;
  }
  impl_->diagnostic_session = service.GetSnapshot().session;
  LOG(INFO) << "crQuickShare: Bluetooth incoming socket ready";
  impl_->pipe->Wake();
  return true;
}
bool BootstrapProbe::Send(scoped_refptr<device::BluetoothAdapter> adapter, const Peer& peer,
                          QuickShareService& service, std::vector<base::FilePath> paths,
                          HotspotConnector hotspot, std::function<void(base::OnceClosure)> when_connection_ready) {
  const auto reject = [](const char* reason) {
    LOG(WARNING) << "crQuickShare: Send rejected before session; component=bluetooth_sender; reason=" << reason;
    return false;
  };
  if (active()) return reject("sender_busy");
  if (!adapter) return reject("adapter_missing");
  if (!adapter->IsPresent()) return reject("adapter_not_present");
  if (!adapter->IsPowered()) return reject("adapter_not_powered");
  if (!peer.ble_only || peer.id.size() != 4 || peer.name.empty() || peer.classic_bluetooth_address.empty())
    return reject("invalid_bluetooth_peer");
  impl_ = std::make_unique<Impl>(false);
  impl_->endpoint = peer.id;
  impl_->hotspot = std::move(hotspot);
  auto shared = impl_->pipe;
  shared->setup_required = true;
  if (!service.SendBluetooth(peer, std::move(paths), std::make_unique<ProbeChannel>(shared),
      [shared] { std::lock_guard guard(shared->mutex); return shared->sent_bytes; }, impl_->hotspot,
      [shared] { std::lock_guard guard(shared->mutex); return shared->cancelled; },
      [shared] { shared->Finished(); })) {
    impl_->Stop("service_rejected");
    return reject("service_rejected");
  }
  impl_->normal_transfer = true;
  impl_->adapter = std::move(adapter);
  impl_->pending_peer = peer;
  impl_->attempted = true;
  if (when_connection_ready) {
    impl_->connection_ready = false;
    when_connection_ready(base::BindOnce(&Impl::ConnectionReady, impl_->weak.GetWeakPtr()));
  }
  impl_->pipe->Wake();
  return true;
}
}
