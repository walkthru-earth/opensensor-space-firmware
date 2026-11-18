// Exercise the real Wi-Fi transport over host TCP sockets. Wi-Fi/mDNS and
// task creation are faked; framing, authentication, routing and sockets are
// not.
#include "../firmware/common/connectivity/src/wifi_link.cpp"
#include "archive_sync.h"
#include "os_logger_transfer_gate.h"
#include "sync_codec.h"
#include <esp_netif_sntp.h>

#include <cassert>
#include <chrono>
#include <csignal>
#include <thread>
#include <vector>

namespace opensensor::console {
std::size_t write(const std::uint8_t *, std::size_t length) { return length; }
} // namespace opensensor::console

namespace {
config::Settings fixture_settings;
std::vector<ble::ControlRequest> requests;
bool accept_requests = true;
std::uint32_t ble_reply_generation = 0;
std::vector<std::vector<std::uint8_t>> ble_frames;
// Frames the fake BLE notify queue takes before it reports Full, or -1 for
// no limit. Closed stands for a peer that left.
int ble_credits = -1;
bool ble_closed = false;
bool enqueue(const ble::ControlRequest &request) {
  if (!accept_requests)
    return false;
  requests.push_back(request);
  return true;
}

int connect_phone() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert(fd >= 0);
  timeval timeout{1, 0};
  assert(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ==
         0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(config::kLanPort);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(::connect(fd, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) == 0);
  lan::accept_client();
  return fd;
}

// A LIST job meets a full LAN ring and the LAN task accepts an ABORT while
// the worker waits for space. The worker's wait step checks the job's stamp,
// as the logger's does, so deliver gives up within one wait step and the
// job ends with ERROR aborted after one directory entry.
struct ListJobProbe {
  ossync::ArchiveSession *session = nullptr;
  unsigned waits = 0, reads = 0;
  std::vector<ble::Error> errors;
};
ListJobProbe list_probe;

void list_job_abort_on_full_ring() {
  const std::uint32_t sequence = ossync::abort::sequences[1].load();
  list_probe = ListJobProbe{};
  // Fill what the bulk frames left, down to a FILE frame's size.
  const std::uint8_t filler[14] = {ble::kFrameFile};
  while (lan::enqueue_response(filler, sizeof(filler), 0) ==
         ossync::EnqueueResult::Ok) {
  }
  ossync::ArchiveHooks hooks{};
  hooks.list = [](void *, bool, unsigned budget, ossync::FileEmitter emit,
                  void *out, unsigned &) {
    for (unsigned read = 0; read < budget; ++read) {
      ++list_probe.reads;
      if (!emit(out, "a.parquet", 1))
        return ossync::ListStatus::Stopped;
    }
    return ossync::ListStatus::More;
  };
  ossync::ArchiveTransport transport{};
  transport.generation = [](void *, ble::Link, std::uint8_t peer) {
    return lan::connection_generation(peer);
  };
  transport.payload_max = [](void *, ble::Link, std::uint8_t peer) {
    return lan::payload_max(peer);
  };
  transport.respond = [](void *, ble::Link, std::uint8_t peer,
                         std::uint32_t generation, const std::uint8_t *frame,
                         std::size_t length) {
    return lan::deliver(
        frame, length, peer, generation,
        [](void *) {
          if (list_probe.waits++ == 0) {
            ble::ControlRequest marker;
            marker.link = ble::Link::Lan;
            marker.peer = 0;
            marker.bytes[0] = ble::kOpAbort;
            marker.length = 1;
            ossync::abort::stamp(marker);
            ossync::abort::commit(marker);
          }
          const auto *tag = list_probe.session->list_tag();
          return !(tag && ossync::abort::pending(*tag));
        },
        nullptr);
  };
  transport.error = [](void *, ble::Link, std::uint8_t, std::uint32_t,
                       ble::Op op, ble::Error code, const char *) {
    assert(op == ble::kOpList);
    list_probe.errors.push_back(code);
    return true;
  };
  ossync::ArchiveSession session(hooks, transport);
  list_probe.session = &session;
  ble::ControlRequest list;
  list.link = ble::Link::Lan;
  list.peer = 0;
  list.link_generation = lan::connection_generation(0);
  list.bytes[0] = ble::kOpList;
  list.length = 1;
  ossync::abort::stamp(list);
  assert(session.handle(list, true) && session.listing());
  const auto started = std::chrono::steady_clock::now();
  session.step(true);
  const auto waited = std::chrono::steady_clock::now() - started;
  assert(!session.listing() && list_probe.reads == 1 && list_probe.waits == 1);
  assert(list_probe.errors.size() == 1 &&
         list_probe.errors[0] == ble::kErrAborted);
  assert(waited < std::chrono::milliseconds(lan::options.wait_step_ms + 200));
  ble::ControlRequest marker;
  marker.link = ble::Link::Lan;
  marker.link_generation = list.link_generation;
  assert(ossync::abort::take_stopped(marker) == ble::kOpList);
  ossync::abort::sequences[1].store(sequence);
}

// Client sockets are non-blocking, as the LAN task's select loop expects, so
// wait for loopback delivery before reading like that loop would.
void handle_bytes(std::uint8_t peer) {
  const int fd = lan::clients[peer].fd.load();
  if (fd >= 0) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(fd, &readable);
    timeval timeout{1, 0};
    ::select(fd + 1, &readable, nullptr, nullptr, &timeout);
  }
  lan::handle_client_bytes(peer);
}

void write_bytes(int fd, const std::uint8_t *data, std::size_t length) {
  assert(::send(fd, data, length, 0) == static_cast<int>(length));
}

void read_exact(int fd, std::uint8_t *data, std::size_t length) {
  while (length) {
    const auto count = ::recv(fd, data, length, 0);
    assert(count > 0);
    data += count;
    length -= count;
  }
}

// Stands in for the LAN task's writable wakeup: send what each ring holds.
void pump() {
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer)
    if (lan::clients[peer].fd.load() >= 0 && !lan::clients[peer].write_shut)
      lan::flush(peer, fake_now_us);
}

std::vector<std::uint8_t> read_frame(int fd) {
  pump();
  std::uint8_t prefix[2];
  read_exact(fd, prefix, sizeof(prefix));
  const auto length = unsigned(prefix[0]) | (unsigned(prefix[1]) << 8);
  assert(length > 0 && length <= lan::kPayloadMax);
  std::vector<std::uint8_t> frame(length);
  read_exact(fd, frame.data(), frame.size());
  return frame;
}

void authenticate(int fd, std::uint8_t peer, bool fragmented = false) {
  std::uint8_t bytes[lan::kHandshakeBytes];
  std::memcpy(bytes, "OSS1", 4);
  std::memcpy(bytes + 4, fixture_settings.token, config::kTokenBytes);
  if (fragmented) {
    write_bytes(fd, bytes, 7);
    handle_bytes(peer);
    assert(lan::payload_max(peer) == 0);
    write_bytes(fd, bytes + 7, sizeof(bytes) - 7);
  } else {
    write_bytes(fd, bytes, sizeof(bytes));
  }
  handle_bytes(peer);
  const auto hello = read_frame(fd);
  assert(hello[0] == ble::kFrameHello && hello[1] == ble::kProtocolVersion);
  assert(lan::payload_max(peer) == lan::kPayloadMax);
}

void request_status(int fd, std::uint8_t peer) {
  const std::uint8_t bytes[] = {1, 0, ble::kOpStatus};
  write_bytes(fd, bytes, sizeof(bytes));
  handle_bytes(peer);
}

// The device says why before it closes: one BYE frame, then EOF from its
// half-close. Cached STATUS/LIVE pushes queued ahead of it are skipped. The
// phone then closes, and the slot finishes on that EOF (peer < 0 for a
// refused socket that never had a slot).
std::vector<std::uint8_t> expect_bye(int fd, ble::DisconnectReason reason,
                                     int peer) {
  auto frame = read_frame(fd);
  while (frame[0] == ble::kFrameStatus || frame[0] == ble::kFrameLive)
    frame = read_frame(fd);
  assert(frame.size() == ble::kByeBytes && frame[0] == ble::kFrameBye);
  assert(frame[1] == static_cast<std::uint8_t>(reason));
  std::uint8_t byte;
  assert(::recv(fd, &byte, 1, 0) == 0);
  ::close(fd);
  if (peer >= 0 && lan::clients[peer].fd.load() >= 0) {
    assert(lan::clients[peer].closing.load());
    const int server_fd = lan::clients[peer].fd.load();
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(server_fd, &readable);
    timeval wait{1, 0};
    assert(::select(server_fd + 1, &readable, nullptr, nullptr, &wait) > 0);
    lan::drain_closing_input(static_cast<std::uint8_t>(peer));
    assert(lan::clients[peer].fd.load() < 0);
  }
  return frame;
}

std::size_t count_frames(const std::vector<std::uint8_t> &stream,
                         std::uint8_t type) {
  std::size_t count = 0;
  for (std::size_t at = 0; at + 2 < stream.size();) {
    const std::size_t length = stream[at] | (stream[at + 1] << 8);
    count += stream[at + 2] == type;
    at += 2 + length;
  }
  return count;
}

// Stands in for the LAN task's scan steps: start, the driver's SCAN_DONE
// event, then collection on the next loop.
void scan_now() {
  lan::start_scan();
  if (lan::scan_running) {
    assert(!fake_scan_blocking);
    fake_scan_done();
    lan::poll_scan();
  }
  assert(!lan::scan_running && !lan::scan_pending.load());
}

int socket_option(int fd, int level, int name) {
  int value = -1;
  socklen_t length = sizeof(value);
  assert(::getsockopt(fd, level, name, &value, &length) == 0);
  return value;
}

ble::DisconnectEvent expect_event(ble::DisconnectReason reason,
                                  std::uint8_t link) {
  std::uint8_t raw[DebugLog::kFallbackEventSlots][DebugLog::kEventBytes];
  const auto count = oslog.events(raw, DebugLog::kFallbackEventSlots);
  assert(count > 0);
  const auto event = ble::decode_event(raw[count - 1]);
  assert(event.reason == static_cast<std::uint8_t>(reason));
  assert(event.link == link);
  assert(ble::disconnect_stats().last == reason);
  return event;
}
} // namespace

namespace config {
Settings get() { return fixture_settings; }
} // namespace config
namespace ble {
const char *local_name() { return "OS-fixture"; }
const char *info_json() { return "{\"thing\":\"fixture\"}"; }
ossync::EnqueueResult try_send_response(const std::uint8_t *frame,
                                        std::size_t length,
                                        std::uint32_t generation) {
  if (ble_closed)
    return ossync::EnqueueResult::Closed;
  if (ble_credits == 0)
    return ossync::EnqueueResult::Full;
  if (ble_credits > 0)
    --ble_credits;
  ble_frames.emplace_back(frame, frame + length);
  ble_reply_generation = generation;
  return ossync::EnqueueResult::Ok;
}
} // namespace ble

int main() {
  lan::TimeAnchor time_anchor;
  assert(!lan::take_time_anchor(time_anchor));
  timeval actual_time{1800000000, 123456};
  std::signal(SIGPIPE, SIG_IGN);
  auto ap = [](const char *ssid, std::int8_t rssi, wifi_auth_mode_t auth,
               std::uint8_t channel) {
    wifi_ap_record_t record{};
    std::snprintf(reinterpret_cast<char *>(record.ssid), sizeof(record.ssid),
                  "%s", ssid);
    record.rssi = rssi;
    record.authmode = auth;
    record.primary = channel;
    return record;
  };
  const ble::Identity identity{"fixture", "device", "boot", "schema",
                               0,         "",       "test"};
  fake_init_result = ESP_FAIL;
  assert(!lan::begin("os-fixture", identity, enqueue));
  assert(!lan::station_netif && !lan::wifi_initialized);
  assert(!lan::request_scan(ble::Link::Ble, 1));
  fake_init_result = ESP_OK;
  fake_pm_create_result = ESP_ERR_NOT_SUPPORTED;
  fake_task_result = 0;
  assert(!lan::begin("os-fixture", identity, enqueue));
  assert(!lan::station_netif && !lan::wifi_initialized &&
         !fake_station_events && !fake_ip_events && fake_wifi_deinits == 1);
  assert(!lan::request_scan(ble::Link::Ble, 1));
  fake_task_result = pdPASS;
  assert(lan::begin("os-fixture", identity, enqueue));
  // An image without PM_ENABLE still starts; only power save follows peers.
  assert(!lan::active_lock);
  assert(!lan::begin("os-fixture", identity, enqueue));
  // Shared defaults: the LAN task outranks the storage worker (3).
  assert(fake_task_priority == 4 && fake_task_stack == 8192);
  fake_pm_create_result = ESP_OK;
  assert(esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "os-lan",
                            &lan::active_lock) == ESP_OK);
  assert(fake_wifi_storage == WIFI_STORAGE_RAM);
  assert(!fake_wifi_started);
  lan::settings = fixture_settings;
  lan::connected = true;
  lan::start_server();
  if (lan::listen_fd < 0)
    std::fprintf(stderr, "LAN fixture listener failed: errno=%d (%s)\n", errno,
                 std::strerror(errno));
  assert(lan::listen_fd >= 0);
  assert(fake_mdns_started && fake_mdns_host == "os-fixture" &&
         fake_mdns_instance == "OS-fixture" && fake_mdns_service == "_ossync" &&
         fake_mdns_transport == "_tcp" && fake_mdns_port == config::kLanPort);
  assert(fake_mdns_txt ==
         (std::map<std::string, std::string>{{"proto", "3"},
                                             {"thing", "fixture"},
                                             {"dev", "device"},
                                             {"fw", "test"}}));
  int phones[ble::kMaxLanClients];
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer) {
    phones[peer] = connect_phone();
    authenticate(phones[peer], peer, peer == 1);
  }
  // Every accepted socket probes a silent phone: 30 s idle, then 3 probes
  // 5 s apart, so a dead peer closes in 45 s instead of the 300 s idle limit.
  {
    const int fd = lan::clients[0].fd.load();
    assert(socket_option(fd, SOL_SOCKET, SO_KEEPALIVE) != 0);
    assert(socket_option(fd, IPPROTO_TCP, TCP_KEEPIDLE) == 30);
    assert(socket_option(fd, IPPROTO_TCP, TCP_KEEPINTVL) == 5);
    assert(socket_option(fd, IPPROTO_TCP, TCP_KEEPCNT) == 3);
    assert(socket_option(fd, IPPROTO_TCP, TCP_NODELAY) != 0);
  }
  assert(lan::status().clients == 3 && lan::status().authenticated);
  const auto disconnects_before = ble::disconnect_stats().count;
  const int fourth = connect_phone();
  const auto busy = read_frame(fourth);
  assert(busy[0] == ble::kFrameError && busy[2] == ble::kErrBusy);
  expect_bye(fourth, ble::DisconnectReason::SessionsFull, -1);
  expect_event(ble::DisconnectReason::SessionsFull, ble::kEventLinkRefused);
  assert(ble::disconnect_stats().count == disconnects_before + 1);
  assert(lan::status().clients == 3);

  // Each parsed request and response stays on the phone that sent it.
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer) {
    request_status(phones[peer], peer);
    assert(requests.back().peer == peer);
    const std::uint8_t frame[] = {ble::kFrameStatus, peer};
    assert(lan::enqueue_response(frame, sizeof(frame), peer,
                                 requests.back().link_generation) ==
           ossync::EnqueueResult::Ok);
    assert(read_frame(phones[peer]) ==
           std::vector<std::uint8_t>(frame, frame + 2));
  }
  // The task waits on socket readiness, rather than sleeping after each
  // request. With sessions open each wait is capped at kActiveWaitUs so a
  // worker push into an empty ring leaves promptly; arriving bytes are still
  // picked up well before the requested 500 ms, including on a nonzero slot.
  for (const std::uint8_t peer : {std::uint8_t(0), std::uint8_t(2)}) {
    const auto before = requests.size();
    std::thread phone([&, peer] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      const std::uint8_t bytes[] = {1, 0, ble::kOpStatus};
      write_bytes(phones[peer], bytes, sizeof(bytes));
    });
    const auto started = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration elapsed{};
    while (requests.size() == before &&
           elapsed < std::chrono::milliseconds(400)) {
      lan::poll_server(500000);
      elapsed = std::chrono::steady_clock::now() - started;
    }
    phone.join();
    assert(requests.size() == before + 1 && requests.back().peer == peer);
    assert(elapsed < std::chrono::milliseconds(400));
  }
  // Socket waiting is bounded when no request arrives (radio housekeeping
  // and idle expiry must still run); no spin loop replaces the fixed sleep.
  // A session is open, so the wait is the active cap, not the 500 ms asked.
  const auto idle_started = std::chrono::steady_clock::now();
  lan::poll_server(500000);
  const auto idle_wait = std::chrono::steady_clock::now() - idle_started;
  assert(idle_wait >= std::chrono::milliseconds(8) &&
         idle_wait < std::chrono::milliseconds(300));

  // TCP can split a full-size body at any byte or coalesce several frames.
  std::vector<std::uint8_t> full(2 + ble::kMaxControlBytes, 0x55);
  full[0] = 0;
  full[1] = 2;
  full[2] = ble::kOpList;
  const auto before_fragment = requests.size();
  write_bytes(phones[0], full.data(), 9);
  handle_bytes(0);
  assert(requests.size() == before_fragment);
  write_bytes(phones[0], full.data() + 9, full.size() - 9);
  handle_bytes(0);
  assert(requests.back().length == ble::kMaxControlBytes &&
         requests.back().bytes[511] == 0x55);
  const std::uint8_t coalesced[] = {1, 0, ble::kOpStatus, 1, 0, ble::kOpClose};
  const auto before_coalesced = requests.size();
  write_bytes(phones[2], coalesced, sizeof(coalesced));
  handle_bytes(2);
  assert(requests.size() == before_coalesced + 2 && requests.back().peer == 2);
  accept_requests = false;
  request_status(phones[2], 2);
  const auto queue_busy = read_frame(phones[2]);
  assert(queue_busy[0] == ble::kFrameError && queue_busy[1] == ble::kOpStatus &&
         queue_busy[2] == ble::kErrBusy);
  accept_requests = true;

  // ABORT advances its link's sequence before it is queued, so the worker
  // sees every earlier request on that link as aborted and no other link's.
  request_status(phones[2], 2);
  const auto before_abort = requests.back();
  const std::uint8_t abort_bytes[] = {1, 0, ble::kOpAbort};
  write_bytes(phones[2], abort_bytes, sizeof(abort_bytes));
  handle_bytes(2);
  assert(requests.back().bytes[0] == ble::kOpAbort &&
         ossync::abort::pending(before_abort) &&
         !ossync::abort::pending(requests.back()));
  request_status(phones[0], 0);
  assert(!ossync::abort::pending(requests.back()));

  lan::publish_status("{}", 2);
  lan::publish_live("{\"x\":1}", 7);
  lan::push_documents();
  for (int fd : phones) {
    assert(read_frame(fd)[0] == ble::kFrameStatus);
    assert(read_frame(fd)[0] == ble::kFrameLive);
  }

  const auto old_generation = lan::connection_generation(1);
  const auto other_generation = lan::connection_generation(2);
  ::close(phones[1]);
  handle_bytes(1);
  assert(lan::status().clients == 2);
  assert(expect_event(ble::DisconnectReason::PeerClosed, 2).generation ==
         old_generation);
  assert(lan::connection_generation(2) == other_generation);
  phones[1] = connect_phone();
  authenticate(phones[1], 1);
  const std::uint8_t response[] = {ble::kFrameClosed, 42};
  assert(lan::enqueue_response(response, sizeof(response), 1, old_generation) ==
         ossync::EnqueueResult::Closed);
  assert(lan::enqueue_error(ble::kOpClose, ble::kErrBusy, "stale", 1,
                            old_generation) == ossync::EnqueueResult::Closed);
  assert(lan::enqueue_response(response, sizeof(response), 1,
                               lan::connection_generation(1)) ==
         ossync::EnqueueResult::Ok);
  assert(read_frame(phones[1]) ==
         std::vector<std::uint8_t>(response, response + 2));
  // An oversize frame is refused without dropping a healthy client, even with
  // a stale errno left over from earlier socket work.
  {
    const std::vector<std::uint8_t> oversize(lan::kPayloadMax + 1, 0x5a);
    const auto clients = lan::status().clients;
    errno = EPIPE;
    assert(lan::enqueue_response(oversize.data(), oversize.size(), 1,
                                 lan::connection_generation(1)) ==
           ossync::EnqueueResult::Rejected);
    assert(
        lan::enqueue_response(response, 0, 1, lan::connection_generation(1)) ==
        ossync::EnqueueResult::Rejected);
    assert(lan::status().clients == clients && !lan::clients[1].drop.load());
    assert(lan::enqueue_response(response, sizeof(response), 1,
                                 lan::connection_generation(1)) ==
           ossync::EnqueueResult::Ok);
    assert(read_frame(phones[1]) ==
           std::vector<std::uint8_t>(response, response + 2));
  }

  // Malformed/auth-failing clients cannot terminate their neighbors.
  const std::uint8_t malformed[] = {1, 2}; // 513 exceeds bounded control size
  write_bytes(phones[0], malformed, sizeof(malformed));
  handle_bytes(0);
  assert(read_frame(phones[0])[2] == ble::kErrMalformed);
  expect_bye(phones[0], ble::DisconnectReason::Malformed, 0);
  expect_event(ble::DisconnectReason::Malformed, 1);
  phones[0] = connect_phone();
  std::uint8_t invalid[lan::kHandshakeBytes]{};
  write_bytes(phones[0], invalid, sizeof(invalid));
  handle_bytes(0);
  assert(read_frame(phones[0])[2] == ble::kErrAuth);
  expect_bye(phones[0], ble::DisconnectReason::AuthFailed, 0);
  expect_event(ble::DisconnectReason::AuthFailed, 1);
  assert(lan::status().clients == 2);
  phones[0] = connect_phone();
  fake_now_us += lan::kHandshakeUs + 1;
  // Keep authenticated neighbors alive while the unauthenticated slot expires.
  request_status(phones[1], 1);
  request_status(phones[2], 2);
  lan::poll_server();
  assert(lan::clients[0].closing.load() && lan::payload_max(0) == 0 &&
         lan::status().clients == 2);
  expect_bye(phones[0], ble::DisconnectReason::HandshakeTimeout, 0);
  expect_event(ble::DisconnectReason::HandshakeTimeout, 1);
  // Reconnecting slot 1 received its cached initial snapshots on poll.
  assert(read_frame(phones[1])[0] == ble::kFrameStatus);
  assert(read_frame(phones[1])[0] == ble::kFrameLive);

  // A send failure marks only its own slot, then poll closes it.
  linger reset{1, 0};
  assert(::setsockopt(phones[1], SOL_SOCKET, SO_LINGER, &reset,
                      sizeof(reset)) == 0);
  ::close(phones[1]);
  // Wait until the local socket receives the reset before exercising send.
  fd_set reset_readable;
  FD_ZERO(&reset_readable);
  const auto server_fd = lan::clients[1].fd.load();
  FD_SET(server_fd, &reset_readable);
  timeval reset_wait{1, 0};
  assert(::select(server_fd + 1, &reset_readable, nullptr, nullptr,
                  &reset_wait) > 0);
  // The worker only queues; the LAN task's send meets the reset.
  for (unsigned attempt = 0; attempt < 4 && lan::clients[1].fd.load() >= 0;
       ++attempt) {
    lan::enqueue_response(response, sizeof(response), 1);
    lan::poll_server();
  }
  assert(lan::clients[1].fd.load() < 0);
  assert(lan::status().clients == 1 && lan::payload_max(2) == lan::kPayloadMax);
  assert(expect_event(ble::DisconnectReason::SendError, 2).err != 0);
  lan::drop_session();
  assert(lan::enqueue_response(response, sizeof(response), 2) ==
         ossync::EnqueueResult::Closed);
  assert(lan::payload_max(2) == 0);
  lan::poll_server();
  expect_bye(phones[2], ble::DisconnectReason::TokenRotated, 2);
  assert(lan::status().clients == 0 && !lan::status().client);
  expect_event(ble::DisconnectReason::TokenRotated, 3);
  // An owner-requested reboot names itself before the sockets go away.
  const int rebooting = connect_phone();
  authenticate(rebooting, 0);
  lan::drop_session(ble::DisconnectReason::Reboot);
  lan::poll_server();
  expect_bye(rebooting, ble::DisconnectReason::Reboot, 0);
  expect_event(ble::DisconnectReason::Reboot, 1);
  const int idle = connect_phone();
  authenticate(idle, 0);
  const auto idle_generation = lan::connection_generation(0);
  fake_now_us += lan::kIdleUs + 1;
  lan::poll_server();
  const auto bye = expect_bye(idle, ble::DisconnectReason::IdleTimeout, 0);
  assert(!lan::status().client &&
         lan::connection_generation(0) != idle_generation);
  assert(opensensor::sync::get_u32(bye.data() + 2) == idle_generation);
  assert(opensensor::sync::get_u32(bye.data() + 10) == fake_now_us / 1000000);
  assert(expect_event(ble::DisconnectReason::IdleTimeout, 1).generation ==
         idle_generation);

  // A valid frame that can never fit a ring smaller than ring_bytes closes
  // its peer as tx-overflow instead of leaving the worker waiting.
  const int oversized = connect_phone();
  authenticate(oversized, 0);
  const ossync::FrameRing full_ring = lan::clients[0].tx;
  std::uint8_t tiny[64];
  lan::clients[0].tx.attach(tiny, sizeof(tiny));
  std::vector<std::uint8_t> large(lan::kPayloadMax, 0x11);
  assert(lan::enqueue_response(large.data(), large.size(), 0) ==
         ossync::EnqueueResult::Closed);
  assert(lan::payload_max(0) == 0);
  lan::poll_server();
  expect_bye(oversized, ble::DisconnectReason::TxOverflow, 0);
  expect_event(ble::DisconnectReason::TxOverflow, 1);
  lan::clients[0].tx = full_ring;

  // A LAN scan runs on os-lan, the only sender. A full ring is flushed once
  // and the reply retried. A ring the socket cannot drain closes the peer as
  // tx-overflow instead of stalling the scan.
  {
    const int scanner = connect_phone();
    authenticate(scanner, 0);
    const ossync::FrameRing scan_ring = lan::clients[0].tx;
    std::uint8_t small_ring[24];
    lan::clients[0].tx.attach(small_ring, sizeof(small_ring));
    std::uint8_t filler[20] = {ble::kFrameLive};
    wifi_ap_record_t record{};
    std::memcpy(record.ssid, "fixture", 8);
    record.rssi = -40;
    record.authmode = WIFI_AUTH_WPA2_PSK;
    record.primary = 3;
    // While a scan reply holds the reserve, the worker's pushes see Full
    // and cannot refill the space the scan's flush frees.
    lan::clients[0].scan_reserve = true;
    assert(lan::enqueue_response(filler, sizeof(filler), 0) ==
           ossync::EnqueueResult::Full);
    lan::clients[0].scan_reserve = false;
    assert(lan::enqueue_response(filler, sizeof(filler), 0) ==
           ossync::EnqueueResult::Ok);
    fake_scan_records = {record};
    assert(lan::request_scan(ble::Link::Lan, lan::connection_generation(0), 0));
    scan_now();
    assert(read_frame(scanner).size() == sizeof(filler));
    const auto found = read_frame(scanner);
    assert(found.size() == 11 && found[0] == ble::kFrameWifiAp &&
           found[3] == 3 && std::memcmp(found.data() + 4, "fixture", 7) == 0);
    assert(read_frame(scanner) ==
           (std::vector<std::uint8_t>{ble::kFrameWifiScanEnd, 1, 0, 0}));
    assert(!lan::clients[0].drop.load() && !lan::clients[0].scan_reserve);

    // Stall the socket. Queue frames the size of the scan reply and pump
    // until three rounds in a row move nothing, so a flush makes no progress.
    const int scan_fd = lan::clients[0].fd.load();
    int tiny_buffer = 4096;
    assert(::setsockopt(scanner, SOL_SOCKET, SO_RCVBUF, &tiny_buffer,
                        sizeof(tiny_buffer)) == 0);
    assert(::setsockopt(scan_fd, SOL_SOCKET, SO_SNDBUF, &tiny_buffer,
                        sizeof(tiny_buffer)) == 0);
    static std::uint8_t stall_ring[4096];
    lan::clients[0].tx.attach(stall_ring, sizeof(stall_ring));
    std::uint8_t same_size[11] = {ble::kFrameLive};
    for (unsigned round = 0, stable = 0; stable < 3; ++round) {
      assert(round < 100000);
      while (lan::enqueue_response(same_size, sizeof(same_size), 0) ==
             ossync::EnqueueResult::Ok) {
      }
      const auto sent = lan::clients[0].tx.consumed_total();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      pump();
      stable = lan::clients[0].tx.consumed_total() == sent ? stable + 1 : 0;
    }
    fake_scan_records = {record};
    assert(lan::request_scan(ble::Link::Lan, lan::connection_generation(0), 0));
    scan_now();
    assert(lan::clients[0].drop.load() &&
           lan::clients[0].drop_reason ==
               static_cast<std::uint8_t>(ble::DisconnectReason::TxOverflow));
    ::close(scanner);
    for (int tick = 0; tick < 50 && lan::clients[0].fd.load() >= 0; ++tick) {
      fake_now_us += 100000;
      lan::poll_server(10000);
    }
    assert(lan::clients[0].fd.load() < 0);
    expect_event(ble::DisconnectReason::TxOverflow, 1);
    lan::clients[0].tx = scan_ring;
  }

  // A peer that stops reading fills only its own ring. The worker never
  // blocks on its socket, other peers keep receiving, at most one STATUS and
  // one LIVE wait in the ring, and after slow_peer_ms without progress the
  // peer gets BYE slow-peer at the next frame boundary, then EOF.
  const int slow = connect_phone();
  authenticate(slow, 0);
  const int fast = connect_phone();
  authenticate(fast, 1);
  int small = 4096;
  assert(::setsockopt(slow, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)) == 0);
  const int slow_fd = lan::clients[0].fd.load();
  assert(::setsockopt(slow_fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) ==
         0);
  std::vector<std::uint8_t> bulk(lan::kPayloadMax, 0x5a);
  bulk[0] = ble::kFrameChunk;
  auto &slow_ring = lan::clients[0].tx;
  for (unsigned attempt = 0; attempt < 4096 && slow_ring.used() < 4096;
       ++attempt) {
    assert(lan::enqueue_response(bulk.data(), bulk.size(), 0) ==
           ossync::EnqueueResult::Ok);
    lan::poll_server();
  }
  assert(slow_ring.used() >= 4096);
  const auto frames_before_pushes = slow_ring.frames();
  for (int round = 0; round < 5; ++round) {
    char status[16];
    const int length =
        std::snprintf(status, sizeof(status), "{\"n\":%d}", round);
    lan::publish_status(status, static_cast<std::size_t>(length));
    lan::publish_live(status, static_cast<std::size_t>(length));
    lan::poll_server();
  }
  assert(slow_ring.frames() <= frames_before_pushes + 2);
  // Keep the ring full until the kernel buffers stop taking bytes too.
  auto top_up = [&] {
    while (lan::enqueue_response(bulk.data(), bulk.size(), 0) ==
           ossync::EnqueueResult::Ok) {
    }
  };
  for (int quiet = 0, round = 0; quiet < 3 && round < 400; ++round) {
    top_up();
    const auto sent_before = slow_ring.consumed_total();
    lan::poll_server(20000);
    quiet = slow_ring.consumed_total() == sent_before ? quiet + 1 : 0;
  }
  top_up();
  assert(lan::enqueue_response(bulk.data(), bulk.size(), 0) ==
         ossync::EnqueueResult::Full);
  // One worker wait step is bounded by wait_step_ms; the hook runs between
  // steps and can stop the wait (ABORT).
  unsigned steps = 0;
  const auto wait_started = std::chrono::steady_clock::now();
  assert(!lan::deliver(
      bulk.data(), bulk.size(), 0, lan::connection_generation(0),
      [](void *context) {
        ++*static_cast<unsigned *>(context);
        return false;
      },
      &steps));
  const auto waited = std::chrono::steady_clock::now() - wait_started;
  assert(steps == 1 &&
         waited < std::chrono::milliseconds(lan::options.wait_step_ms + 200));
  list_job_abort_on_full_ring();
  // A LAN task that stops draining never runs the slow peer rule, and the
  // worker's wait step keeps its heartbeat fresh. The wait still gives up
  // after 2 * slow_peer_ms without a space wake, and the peer stays open.
  // The clock is put back after, since no LAN tick ran meanwhile.
  const std::int64_t frozen_at = fake_now_us;
  steps = 0;
  assert(!lan::deliver(
      bulk.data(), bulk.size(), 0, lan::connection_generation(0),
      [](void *context) {
        ++*static_cast<unsigned *>(context);
        fake_now_us += lan::slow_peer_us;
        return true;
      },
      &steps));
  assert(steps >= 2 && steps <= 4);
  assert(fake_now_us - frozen_at >= 2 * lan::slow_peer_us);
  assert(lan::payload_max(0) == lan::kPayloadMax);
  fake_now_us = frozen_at;
  // A due battery close ends a data frame's wait at the next step through
  // the logger's own gate, long before the slow peer bound, while the peer
  // still never drains.
  struct CloseWait {
    oslogger::power::EmergencyClose close;
    const std::vector<std::uint8_t> *frame = nullptr;
    unsigned steps = 0;
    unsigned raise_at = 0;
  };
  std::vector<std::uint8_t> chunk_frame = bulk;
  chunk_frame[0] = ble::kFrameChunk;
  const auto gate_step = [](void *context) {
    auto &wait = *static_cast<CloseWait *>(context);
    if (++wait.steps == wait.raise_at)
      wait.close.request();
    const auto &frame = *wait.frame;
    return oslogger::power::keep_waiting(
        wait.close, false,
        oslogger::power::data_frame(frame.data(), frame.size()),
        fake_now_us / 1000);
  };
  for (const unsigned raise_at : {1u, 2u}) {
    CloseWait wait;
    wait.frame = &chunk_frame;
    wait.raise_at = raise_at;
    const auto started = std::chrono::steady_clock::now();
    assert(!lan::deliver(chunk_frame.data(), chunk_frame.size(), 0,
                         lan::connection_generation(0), gate_step, &wait));
    assert(wait.steps == raise_at);
    assert(
        std::chrono::steady_clock::now() - started <
        std::chrono::milliseconds(raise_at * lan::options.wait_step_ms + 200));
    assert(fake_now_us == frozen_at);
  }
  // A terminal frame is never cut short by the close, only by ABORT or the
  // slow peer bound.
  {
    CloseWait wait;
    std::vector<std::uint8_t> end_frame = bulk;
    end_frame[0] = ble::kFrameReadEnd;
    wait.frame = &end_frame;
    wait.close.request();
    assert(oslogger::power::keep_waiting(
        wait.close, false,
        oslogger::power::data_frame(end_frame.data(), end_frame.size()), 0));
    assert(!oslogger::power::keep_waiting(wait.close, true, false, 0));
  }
  const std::uint8_t to_fast[] = {ble::kFrameClosed, 7};
  assert(lan::enqueue_response(to_fast, sizeof(to_fast), 1) ==
         ossync::EnqueueResult::Ok);
  lan::poll_server();
  auto fast_frame = read_frame(fast);
  while (fast_frame[0] == ble::kFrameStatus || fast_frame[0] == ble::kFrameLive)
    fast_frame = read_frame(fast);
  assert(fast_frame == std::vector<std::uint8_t>(to_fast, to_fast + 2));
  // Any progress restarts the slow-peer clock, and host loopback buffers can
  // keep growing, so step half the budget at a time with the ring full. The
  // close comes only after a whole budget without one accepted byte.
  std::int64_t stalled_since = -1;
  for (int round = 0; round < 64 && !lan::clients[0].closing.load(); ++round) {
    top_up();
    stalled_since = lan::clients[0].stalled_since_us;
    fake_now_us += lan::slow_peer_us / 2;
    lan::poll_server();
  }
  assert(stalled_since >= 0 &&
         fake_now_us - stalled_since >= lan::slow_peer_us);
  assert(lan::clients[0].closing.load() && lan::payload_max(0) == 0 &&
         lan::payload_max(1) == lan::kPayloadMax);
  std::vector<std::uint8_t> slow_stream;
  std::thread slow_reader([&] {
    std::uint8_t chunk[4096];
    ssize_t got;
    while ((got = ::recv(slow, chunk, sizeof(chunk), 0)) > 0)
      slow_stream.insert(slow_stream.end(), chunk, chunk + got);
    ::close(slow);
  });
  const auto drain_started = std::chrono::steady_clock::now();
  while (lan::clients[0].fd.load() >= 0 &&
         std::chrono::steady_clock::now() - drain_started <
             std::chrono::seconds(5))
    lan::poll_server(10000);
  slow_reader.join();
  assert(lan::clients[0].fd.load() < 0);
  // Every frame arrived whole and the last one is BYE slow-peer.
  std::size_t at = 0, last = 0;
  while (at < slow_stream.size()) {
    last = at;
    at += 2 + (slow_stream[at] | (slow_stream[at + 1] << 8));
  }
  assert(at == slow_stream.size() && slow_stream.size() > ble::kByeBytes);
  assert(slow_stream[last] == ble::kByeBytes &&
         slow_stream[last + 2] == ble::kFrameBye &&
         slow_stream[last + 3] ==
             static_cast<std::uint8_t>(ble::DisconnectReason::SlowPeer));
  assert(count_frames(slow_stream, ble::kFrameBye) == 1);
  const auto slow_event = expect_event(ble::DisconnectReason::SlowPeer, 1);
  assert(slow_event.err == EAGAIN && slow_event.queued_frames > 0);
  lan::drop_session(ble::DisconnectReason::ServerStop);
  lan::poll_server();
  expect_bye(fast, ble::DisconnectReason::ServerStop, 1);
  lan::poll_server();

  // A reset peer fails recv. The event keeps the errno.
  const int reset_peer = connect_phone();
  authenticate(reset_peer, 0);
  const auto reset_fd = lan::clients[0].fd.load();
  assert(::setsockopt(reset_peer, SOL_SOCKET, SO_LINGER, &reset,
                      sizeof(reset)) == 0);
  ::close(reset_peer);
  fd_set reset_ready;
  FD_ZERO(&reset_ready);
  FD_SET(reset_fd, &reset_ready);
  timeval reset_timeout{1, 0};
  assert(::select(reset_fd + 1, &reset_ready, nullptr, nullptr,
                  &reset_timeout) > 0);
  handle_bytes(0);
  assert(lan::clients[0].fd.load() < 0);
  assert(expect_event(ble::DisconnectReason::RecvError, 1).err == ECONNRESET);

  // lwIP reports expired keepalive or retransmission as ECONNABORTED, other
  // stacks as ETIMEDOUT. Either closes KeepaliveFailed, without a BYE that
  // the dead socket could not carry. A reset stays a receive error.
  assert(
      lan::transport_reason(ECONNABORTED, ble::DisconnectReason::RecvError) ==
      ble::DisconnectReason::KeepaliveFailed);
  assert(lan::transport_reason(ETIMEDOUT, ble::DisconnectReason::SendError) ==
         ble::DisconnectReason::KeepaliveFailed);
  assert(lan::transport_reason(ECONNRESET, ble::DisconnectReason::RecvError) ==
         ble::DisconnectReason::RecvError);
  {
    const int silent = connect_phone();
    authenticate(silent, 0);
    lan::begin_close(0, ble::DisconnectReason::KeepaliveFailed, ECONNABORTED);
    assert(lan::clients[0].fd.load() < 0);
    std::uint8_t byte;
    assert(::recv(silent, &byte, 1, 0) == 0);
    ::close(silent);
    assert(expect_event(ble::DisconnectReason::KeepaliveFailed, 1).err ==
           ECONNABORTED);
  }

  // A rotation requested between poll_server and a handshake refuses HELLO.
  // The new peer learns the rotation, not a generic send error.
  const int racing = connect_phone();
  std::uint8_t handshake[lan::kHandshakeBytes];
  std::memcpy(handshake, "OSS1", 4);
  std::memcpy(handshake + 4, fixture_settings.token, config::kTokenBytes);
  write_bytes(racing, handshake, sizeof(handshake));
  lan::drop_session();
  handle_bytes(0);
  assert(lan::clients[0].closing.load());
  expect_bye(racing, ble::DisconnectReason::TokenRotated, 0);
  expect_event(ble::DisconnectReason::TokenRotated, 1);
  lan::poll_server();
  assert(!lan::drop_requested.load());

  fixture_settings.wifi_on = true;
  std::memcpy(fixture_settings.ssid, "fixture", 8);
  fixture_settings.lan_on = false;
  lan::apply_settings();
  lan::poll_radio();
  assert(lan::listen_fd < 0);
  lan::poll_radio();
  assert(lan::connected && lan::listen_fd < 0);
  lan::poll_radio(); // asynchronous DNS result consumed on the next tick
  assert(fake_ntp_starts > 0 && fake_time_callback != nullptr);
  // Association without a successful SNTP callback is not time evidence.
  assert(!lan::take_time_anchor(time_anchor));
  fake_now_us += 123456;
  fake_time_callback(&actual_time);
  assert(lan::take_time_anchor(time_anchor));
  assert(time_anchor.monotonic_us == fake_now_us &&
         time_anchor.seconds == actual_time.tv_sec &&
         time_anchor.subsecond_us == 123456);
  assert(!lan::take_time_anchor(time_anchor));
  const auto starts_before_reconnect = fake_ntp_starts;
  const auto stops_before_disconnect = fake_ntp_stops;
  fake_wifi_autoconnect = false;
  fake_wifi_lost();
  lan::poll_radio();
  assert(!lan::connected && fake_ntp_stops == stops_before_disconnect + 1);
  fake_wifi_autoconnect = true;
  fake_wifi_got_ip();
  lan::poll_radio();
  lan::poll_radio();
  assert(lan::connected && fake_ntp_starts == starts_before_reconnect + 1);
  // Reassociation restarts SNTP but never manufactures a new clock anchor.
  assert(!lan::take_time_anchor(time_anchor));
  // Updating only the time policy does not reconnect Wi-Fi or discard an
  // existing successful anchor; owner tasks reconcile it independently.
  const auto connects_before_time_settings = fake_wifi_connects;
  fixture_settings.ntp.enabled = false;
  lan::apply_time_settings();
  lan::poll_radio();
  assert(
      lan::connected && fake_wifi_connects == connects_before_time_settings &&
      !fake_time_callback && opensensor::network_time::status().has_anchor &&
      std::strcmp(opensensor::network_time::status().state, "disabled") == 0);
  fixture_settings.ntp.enabled = true;
  lan::apply_time_settings();
  lan::poll_radio();
  lan::poll_radio();
  assert(lan::connected &&
         fake_wifi_connects == connects_before_time_settings &&
         fake_time_callback && !lan::take_time_anchor(time_anchor));
  fixture_settings.lan_on = true;
  lan::apply_settings();
  lan::poll_radio();
  lan::poll_radio();
  assert(lan::listen_fd >= 0);
  // Power follows authenticated peers: modem sleep and DFS while idle, then
  // PS NONE and the CPU_FREQ_MAX lock from the first peer to the last.
  lan::update_power();
  assert(fake_wifi_power_save == WIFI_PS_MIN_MODEM && fake_pm_lock.held == 0 &&
         fake_pm_lock.type == ESP_PM_CPU_FREQ_MAX);
  int lost = connect_phone();
  lan::update_power();
  assert(fake_wifi_power_save == WIFI_PS_MIN_MODEM); // not authenticated yet
  authenticate(lost, 0);
  lan::update_power();
  assert(fake_wifi_power_save == WIFI_PS_NONE && fake_pm_lock.held == 1);
  const auto power_changes = fake_power_save_changes;
  const int second = connect_phone();
  authenticate(second, 1);
  lan::update_power();
  assert(fake_power_save_changes == power_changes && fake_pm_lock.held == 1);

  // A BLE scan reply never waits on the LAN task (#178). While the BLE
  // notify queue or the worker's response lock is busy the reply waits in its
  // outbox, a second scan is refused, and a LAN peer's requests and replies
  // keep flowing. The frames then leave in order on later steps.
  {
    fake_scan_records = {ap("fixture-ble", -45, WIFI_AUTH_WPA2_PSK, 9),
                         ap("fixture-two", -70, WIFI_AUTH_OPEN, 2)};
    ble_frames.clear();
    ble_credits = 0;
    assert(lan::request_scan(ble::Link::Ble, 31));
    lan::lan_step();
    assert(lan::scan_running);
    fake_scan_done();
    lan::lan_step();
    assert(!lan::scan_running && lan::scan_job.active &&
           lan::scan_pending.load() && ble_frames.empty());
    assert(!lan::request_scan(ble::Link::Ble, 32));
    for (int round = 0; round < 3; ++round) {
      const auto before = requests.size();
      const std::uint8_t bytes[] = {1, 0, ble::kOpStatus};
      write_bytes(second, bytes, sizeof(bytes));
      for (int step = 0; step < 50 && requests.size() == before; ++step)
        lan::lan_step();
      assert(requests.size() == before + 1 && requests.back().peer == 1);
      const std::uint8_t reply[] = {ble::kFrameChunk,
                                    static_cast<std::uint8_t>(round)};
      assert(lan::enqueue_response(reply, sizeof(reply), 1,
                                   requests.back().link_generation) ==
             ossync::EnqueueResult::Ok);
      auto frame = read_frame(second);
      while (frame[0] == ble::kFrameStatus || frame[0] == ble::kFrameLive)
        frame = read_frame(second);
      assert(frame == std::vector<std::uint8_t>(reply, reply + 2));
      assert(lan::scan_job.active && ble_frames.empty());
    }
    ble_credits = 1;
    lan::lan_step();
    assert(lan::scan_job.active && ble_frames.size() == 1);
    ble_credits = -1;
    lan::lan_step();
    assert(!lan::scan_job.active && !lan::scan_pending.load());
    assert(ble_reply_generation == 31 && ble_frames.size() == 3);
    assert(ble_frames[0][0] == ble::kFrameWifiAp && ble_frames[0][3] == 9 &&
           std::memcmp(ble_frames[0].data() + 4, "fixture-ble", 11) == 0);
    assert(ble_frames[1][0] == ble::kFrameWifiAp && ble_frames[1][3] == 2);
    assert(ble_frames[2] ==
           (std::vector<std::uint8_t>{ble::kFrameWifiScanEnd, 2, 0, 0}));

    // A reply with no frame taken for kScanReplyStallUs is dropped, and the
    // next scan may start.
    fake_scan_records = {ap("fixture-ble", -45, WIFI_AUTH_WPA2_PSK, 9)};
    ble_frames.clear();
    ble_credits = 0;
    assert(lan::request_scan(ble::Link::Ble, 33));
    lan::lan_step();
    fake_scan_done();
    lan::lan_step();
    assert(lan::scan_job.active && ble_frames.empty());
    lan::pump_scan_reply(lan::scan_job.progress_us + lan::kScanReplyStallUs -
                         1);
    assert(lan::scan_job.active);
    lan::pump_scan_reply(lan::scan_job.progress_us + lan::kScanReplyStallUs);
    assert(!lan::scan_job.active && !lan::scan_pending.load());
    char log[DebugLog::kFallbackRingBytes];
    std::uint32_t logged;
    const auto length = oslog.tail(log, sizeof(log), logged);
    assert(
        std::string(log, length).find("WIFI SCAN REPLY STALLED sent=0 of=2") !=
        std::string::npos);

    // A BLE peer that left ends the reply at once, with nothing sent.
    fake_scan_records = {ap("fixture-ble", -45, WIFI_AUTH_WPA2_PSK, 9)};
    ble_credits = -1;
    ble_closed = true;
    assert(lan::request_scan(ble::Link::Ble, 34));
    lan::lan_step();
    fake_scan_done();
    lan::lan_step();
    assert(!lan::scan_job.active && !lan::scan_pending.load() &&
           ble_frames.empty());
    ble_closed = false;
  }

  // A scan runs while peers are served. The driver scan is non-blocking,
  // settings wait for it, and requests and replies keep flowing on slot 1
  // until WIFI_EVENT_SCAN_DONE lets the next loop report to slot 0.
  fake_scan_records = {ap("fixture-ap", -50, WIFI_AUTH_WPA2_PSK, 3)};
  const auto scan_starts = fake_scan_starts;
  assert(lan::request_scan(ble::Link::Lan, lan::connection_generation(0), 0));
  lan::lan_step();
  assert(lan::scan_running && fake_scan_starts == scan_starts + 1 &&
         !fake_scan_blocking);
  lan::apply_settings();
  for (int round = 0; round < 3; ++round) {
    const auto before = requests.size();
    const std::uint8_t bytes[] = {1, 0, ble::kOpStatus};
    write_bytes(second, bytes, sizeof(bytes));
    for (int step = 0; step < 50 && requests.size() == before; ++step)
      lan::lan_step();
    assert(requests.size() == before + 1 && requests.back().peer == 1);
    const std::uint8_t reply[] = {ble::kFrameClosed,
                                  static_cast<std::uint8_t>(round)};
    assert(lan::enqueue_response(reply, sizeof(reply), 1,
                                 requests.back().link_generation) ==
           ossync::EnqueueResult::Ok);
    auto frame = read_frame(second);
    while (frame[0] == ble::kFrameStatus || frame[0] == ble::kFrameLive)
      frame = read_frame(second);
    assert(frame == std::vector<std::uint8_t>(reply, reply + 2));
    assert(lan::scan_running && lan::reapply.load());
  }
  fake_scan_done();
  lan::lan_step();
  assert(!lan::scan_running && !lan::scan_pending.load());
  {
    auto frame = read_frame(lost);
    while (frame[0] == ble::kFrameStatus || frame[0] == ble::kFrameLive)
      frame = read_frame(lost);
    assert(frame[0] == ble::kFrameWifiAp && frame[3] == 3);
    assert(read_frame(lost) ==
           (std::vector<std::uint8_t>{ble::kFrameWifiScanEnd, 1, 0, 0}));
  }
  // The deferred reapply runs once the scan ended: unchanged settings
  // restart the server, closing both peers, and power returns to idle.
  lan::lan_step();
  assert(!lan::reapply.load());
  expect_bye(second, ble::DisconnectReason::ServerStop, 1);
  expect_bye(lost, ble::DisconnectReason::ServerStop, 0);
  lan::update_power();
  assert(fake_wifi_power_save == WIFI_PS_MIN_MODEM && fake_pm_lock.held == 0);
  lan::poll_radio();
  lan::poll_radio();
  assert(lan::listen_fd >= 0);

  // A scan that never reports SCAN_DONE is stopped after kScanTimeoutUs and
  // answers unavailable; a failed SCAN_DONE status answers the same.
  ble_frames.clear();
  assert(lan::request_scan(ble::Link::Ble, 77));
  lan::start_scan();
  assert(lan::scan_running);
  lan::poll_scan();
  assert(lan::scan_running && ble_frames.empty());
  fake_now_us += lan::kScanTimeoutUs;
  const auto scan_stops = fake_scan_stops;
  lan::poll_scan();
  assert(!lan::scan_running && fake_scan_stops == scan_stops + 1 &&
         ble_frames ==
             (std::vector<std::vector<std::uint8_t>>{
                 {ble::kFrameWifiScanEnd, 0, 0, ble::kErrWifiUnavailable}}));
  ble_frames.clear();
  // The SCAN_DONE that the driver posts for the stopped scan arrives late and
  // must not complete the next scan.
  fake_scan_done(1);
  assert(!lan::scan_done);
  assert(lan::request_scan(ble::Link::Ble, 78));
  lan::start_scan();
  fake_scan_done(1);
  lan::poll_scan();
  assert(!lan::scan_running &&
         ble_frames ==
             (std::vector<std::vector<std::uint8_t>>{
                 {ble::kFrameWifiScanEnd, 0, 0, ble::kErrWifiUnavailable}}));
  ble_frames.clear();

  // Losing the station closes sessions with WifiLost; reapplying unchanged
  // LAN settings restarts the server with ServerStop.
  lost = connect_phone();
  authenticate(lost, 0);
  fake_wifi_lost();
  lan::poll_radio();
  expect_bye(lost, ble::DisconnectReason::WifiLost, 0);
  expect_event(ble::DisconnectReason::WifiLost, 1);
  fake_wifi_got_ip();
  lan::poll_radio();
  lan::poll_radio();
  assert(lan::listen_fd >= 0);
  const int restarted = connect_phone();
  authenticate(restarted, 0);
  lan::apply_settings();
  lan::poll_radio();
  expect_bye(restarted, ble::DisconnectReason::ServerStop, 0);
  expect_event(ble::DisconnectReason::ServerStop, 1);
  lan::poll_radio();
  lan::poll_radio();
  assert(lan::listen_fd >= 0);
  // SET_CONFIG that rotates the token and changes Wi-Fi drops the session,
  // then reapplies. poll_radio runs first and must still name the rotation.
  const int rotated = connect_phone();
  authenticate(rotated, 0);
  lan::drop_session();
  lan::apply_settings();
  lan::poll_radio();
  assert(!lan::drop_requested.load());
  expect_bye(rotated, ble::DisconnectReason::TokenRotated, 0);
  expect_event(ble::DisconnectReason::TokenRotated, 1);
  lan::poll_radio();
  lan::poll_radio();
  assert(lan::listen_fd >= 0);
  const int restored = connect_phone();
  authenticate(restored, 0);
  fixture_settings.lan_on = false;
  lan::apply_settings();
  lan::poll_radio();
  assert(lan::listen_fd < 0);
  expect_bye(restored, ble::DisconnectReason::LanDisabled, 0);
  expect_event(ble::DisconnectReason::LanDisabled, 1);
  lan::stop_server(ble::DisconnectReason::ServerStop);

  // Native station events drive link state; failed association keeps the
  // existing bounded 30 second connect / 30 second retry cadence.
  fake_wifi_autoconnect = false;
  lan::apply_settings();
  lan::poll_radio();
  assert(std::strcmp(lan::status().state, "connecting") == 0 &&
         fake_wifi_power_save == WIFI_PS_MIN_MODEM);
  const auto attempts = fake_wifi_connects;
  fake_now_us += lan::kConnectTimeoutUs + 1;
  lan::poll_radio();
  assert(std::strcmp(lan::status().state, "failed") == 0);
  fake_now_us += lan::kRetryUs - 1;
  lan::poll_radio();
  assert(fake_wifi_connects == attempts);
  ++fake_now_us;
  lan::poll_radio();
  assert(fake_wifi_connects == attempts + 1);
  assert(!lan::take_time_anchor(time_anchor));

  // A start error must retry as well, even though the radio never started.
  fake_start_result = ESP_FAIL;
  lan::apply_settings();
  lan::poll_radio();
  assert(!lan::radio_on && std::strcmp(lan::status().state, "failed") == 0);
  fake_start_result = ESP_OK;
  fake_now_us += lan::kRetryUs;
  lan::poll_radio();
  assert(lan::radio_on && std::strcmp(lan::status().state, "connecting") == 0);

  fixture_settings.wifi_on = false;
  lan::apply_settings();
  lan::poll_radio();
  assert(!fake_wifi_started && !fake_mdns_started &&
         std::strcmp(lan::status().state, "off") == 0);

  // Off-radio scans start/stop temporarily, deduplicate SSIDs by strongest
  // RSSI, encode security/channel, preserve 32-byte names and skip hidden APs.
  fake_scan_records = {
      ap("", -20, WIFI_AUTH_OPEN, 1),
      ap("fixture-ap", -85, WIFI_AUTH_WPA2_PSK, 1),
      ap("fixture-ap", -35, WIFI_AUTH_WPA3_PSK, 6),
      ap("12345678901234567890123456789012", -128, WIFI_AUTH_OPEN, 11)};
  ble_frames.clear();
  assert(lan::request_scan(ble::Link::Ble, 123));
  assert(!lan::request_scan(ble::Link::Ble, 456));
  scan_now();
  assert(ble_reply_generation == 123 && ble_frames.size() == 3);
  assert(ble_frames[0][0] == ble::kFrameWifiAp &&
         static_cast<std::int8_t>(ble_frames[0][1]) == -35 &&
         ble_frames[0][2] == 6 && ble_frames[0][3] == 6);
  assert(ble_frames[1].size() == 36 &&
         static_cast<std::int8_t>(ble_frames[1][1]) == -127);
  assert(ble_frames[2] ==
         (std::vector<std::uint8_t>{ble::kFrameWifiScanEnd, 2, 0, 0}));
  assert(fake_scan_records.empty() && !fake_wifi_started && !lan::radio_on);

  // Bounded scans release the complete driver list; failure emits one end
  // frame and also frees results so repeated scans cannot retain AP memory.
  for (unsigned i = 0; i < 80; ++i) {
    const std::string name = "ap" + std::to_string(i);
    fake_scan_records.push_back(ap(name.c_str(), -60, WIFI_AUTH_WPA2_PSK, 1));
  }
  ble_frames.clear();
  scan_now();
  assert(ble_frames.size() == lan::kMaxScanEntries + 1 &&
         ble_frames.back()[1] == lan::kMaxScanEntries &&
         fake_scan_records.empty());
  fake_scan_result = ESP_FAIL;
  const auto clears_before_failure = fake_scan_clears;
  fake_scan_records.push_back(ap("unavailable", -50, WIFI_AUTH_OPEN, 1));
  ble_frames.clear();
  scan_now();
  assert(ble_frames ==
         (std::vector<std::vector<std::uint8_t>>{
             {ble::kFrameWifiScanEnd, 0, 0, ble::kErrWifiUnavailable}}));
  assert(fake_scan_records.empty() &&
         fake_scan_clears == clears_before_failure + 1 && !fake_wifi_started);
  fake_scan_result = ESP_OK;

  // The driver sees credentials in RAM only; diagnostics never include PSK.
  // This is a generated fixture value, never a real owner's network secret.
  std::memset(fixture_settings.ssid, 's', config::kSsidMax);
  fixture_settings.ssid[config::kSsidMax] = '\0';
  std::memset(fixture_settings.psk, 'z', config::kPskMax);
  fixture_settings.psk[config::kPskMax] = '\0';
  fixture_settings.wifi_on = true;
  lan::apply_settings();
  lan::poll_radio();
  assert(std::memcmp(fake_wifi_config.sta.ssid, fixture_settings.ssid,
                     config::kSsidMax) == 0);
  assert(fake_wifi_config.sta.threshold.authmode == WIFI_AUTH_WPA2_PSK);
  assert(std::memcmp(fake_wifi_config.sta.password, fixture_settings.psk,
                     config::kPskMax) == 0 &&
         fake_wifi_config.sta.password[config::kPskMax] == 0);
  char log[DebugLog::kFallbackRingBytes];
  std::uint32_t logged;
  const auto length = oslog.tail(log, sizeof(log), logged);
  assert(std::string(log, length).find(fixture_settings.psk) ==
         std::string::npos);
  fixture_settings.wifi_on = false;
  lan::apply_settings();
  lan::poll_radio();
  lan::release_station();
}
