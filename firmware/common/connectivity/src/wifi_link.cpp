#include "wifi_link.h"
#include "abort_signal.h"
#include "debug_log.h"
#include "device_config.h"
#include "disconnect.h"

#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_pm.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <mdns.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace lan {
namespace {
using ble::DisconnectReason;
constexpr TickType_t kTick = pdMS_TO_TICKS(100);
constexpr std::int64_t kConnectTimeoutUs = 30LL * 1000000LL;
constexpr std::int64_t kRetryUs = 30LL * 1000000LL;
constexpr std::int64_t kHandshakeUs = 5LL * 1000000LL;
constexpr std::int64_t kIdleUs = 300LL * 1000000LL;
// Longest a queued reply waits for the LAN task while a session is open.
constexpr int kActiveWaitUs = 10000;
constexpr std::size_t kHandshakeBytes = 4 + config::kTokenBytes;
constexpr std::size_t kMaxScanEntries = 48;
// A scan normally ends within about 3 s. Past this the LAN task stops it.
constexpr std::int64_t kScanTimeoutUs = 15LL * 1000000LL;
// A BLE scan reply that has no frame accepted for this long is dropped. It
// matches the BLE progress timeout, so the host normally closes a stalled link
// first and the reply ends on Closed.
constexpr std::int64_t kScanReplyStallUs = 15LL * 1000000LL;

char host_label[24]{};
ble::Identity device_identity{};
ble::RequestHandler request_handler = nullptr;
Status current;
esp_netif_t *station_netif = nullptr;
esp_event_handler_instance_t wifi_events = nullptr, ip_events = nullptr;
bool wifi_initialized = false;
std::atomic<bool> station_has_ip{false};
std::atomic<unsigned> disconnect_reason{0};
std::atomic<std::uint32_t> station_ip_generation{0};
std::uint32_t handled_ip_generation = 0;
// Set by WIFI_EVENT_SCAN_DONE; the LAN task collects the records. The event
// counts only while scan_armed, so the SCAN_DONE that esp_wifi_scan_stop posts
// after a timeout cannot complete the next scan early.
std::atomic<bool> scan_armed{false};
std::atomic<bool> scan_done{false};
std::atomic<std::uint32_t> scan_done_status{0};

// Startup failures must release their ownership so a later begin can retry.
// The default event loop/netif stack may be shared and remains initialized.
void release_station() {
  if (wifi_events) {
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          wifi_events);
    wifi_events = nullptr;
  }
  if (ip_events) {
    esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID,
                                          ip_events);
    ip_events = nullptr;
  }
  if (wifi_initialized) {
    esp_wifi_deinit();
    wifi_initialized = false;
  }
  if (station_netif) {
    esp_netif_destroy_default_wifi(station_netif);
    station_netif = nullptr;
  }
}

// Event callbacks publish state only. The LAN task owns reconnects, discovery,
// SNTP and sockets; event-task callbacks never touch those resources.
void station_event(void *, esp_event_base_t base, std::int32_t id, void *data) {
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    station_has_ip = true;
    ++station_ip_generation;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    station_has_ip = false;
    disconnect_reason =
        static_cast<wifi_event_sta_disconnected_t *>(data)->reason;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
    if (!scan_armed)
      return;
    scan_done_status =
        data ? static_cast<wifi_event_sta_scan_done_t *>(data)->status : 0;
    scan_done = true;
  } else if ((base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) ||
             (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)) {
    station_has_ip = false;
  }
}

int station_rssi() {
  wifi_ap_record_t ap{};
  return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

void station_mac(char *out, std::size_t size) {
  std::uint8_t mac[6]{};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK)
    std::snprintf(out, size, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
                  mac[2], mac[3], mac[4], mac[5]);
}

portMUX_TYPE status_mutex = portMUX_INITIALIZER_UNLOCKED;
Options options;
std::int64_t slow_peer_us = 15000000LL, close_flush_us = 500000LL;
// Given whenever the LAN task frees ring space or closes a peer, so a worker
// waiting on a full ring retries at once instead of after a whole step.
SemaphoreHandle_t space_event = nullptr;

std::atomic<bool> reapply{true}, time_reapply{false}, drop_requested{false};
std::atomic<std::uint8_t> drop_all_reason{
    static_cast<std::uint8_t>(DisconnectReason::TokenRotated)};
std::atomic<bool> lan_ready{false};
std::atomic<bool> scan_pending{false};
std::atomic<std::uint8_t> scan_link{0};
std::atomic<std::uint32_t> scan_generation{0};
std::atomic<std::uint8_t> scan_peer{0};
std::atomic<std::uint32_t> ui{0}, sessions{0}, bytes_out{0};

// The LAN task alone owns sockets, parser and timing state, and it is the only
// consumer of each ring. Producers (the worker and the LAN task itself) push
// under tx_lock, which also covers socket identity, authentication and
// generation changes, so a reply is validated atomically with its push. The
// lock is held for a ring copy at most, never across socket IO.
struct Client {
  std::atomic<int> fd{-1};
  std::atomic<bool> authenticated{false}, drop{false}, closing{false};
  std::atomic<std::uint32_t> generation{0};
  // Why a producer asked the LAN task to close this slot.
  std::atomic<std::uint8_t> drop_reason{0};
  std::atomic<int> drop_err{0};
  SemaphoreHandle_t tx_lock = nullptr;
  ossync::FrameRing tx;
  // Under tx_lock. While a scan reply retries after a flush, other producers
  // see Full so the worker cannot refill the space the flush just freed.
  bool scan_reserve = false;
  // LAN task only from here on.
  std::uint8_t rx[2 + ble::kMaxControlBytes + kHandshakeBytes]{};
  std::size_t rx_length = 0;
  std::int64_t since_us = 0, last_rx_us = 0;
  // Start of the current period with queued bytes and no send progress.
  std::int64_t stalled_since_us = -1;
  // Latest value pushes: wanted until serialised, then *_end marks the ring
  // byte count after that copy so at most one of each is ever queued.
  bool status_wanted = false, live_wanted = false;
  std::uint64_t status_end = 0, live_end = 0;
  // Close in progress: BYE and pending frames flush until close_deadline_us.
  std::int64_t close_deadline_us = 0;
  DisconnectReason close_reason = DisconnectReason::Unknown;
  int close_err = 0;
  std::uint16_t close_queued = 0;
  bool close_was_authenticated = false, write_shut = false;
};
Client clients[ble::kMaxLanClients];

class TxLock {
public:
  explicit TxLock(Client &client) : client_(client) {
    xSemaphoreTake(client_.tx_lock, portMAX_DELAY);
  }
  ~TxLock() { xSemaphoreGive(client_.tx_lock); }
  TxLock(const TxLock &) = delete;
  TxLock &operator=(const TxLock &) = delete;

private:
  Client &client_;
};

std::uint8_t authenticated_clients() {
  return static_cast<std::uint8_t>(std::count_if(
      std::begin(clients), std::end(clients),
      [](const Client &client) { return client.authenticated.load(); }));
}

bool any_client() {
  return std::any_of(
      std::begin(clients), std::end(clients),
      [](const Client &client) { return client.fd.load() >= 0; });
}

// Latest pushes, copied by the sampling loop and sent by the LAN task.
char status_json[ble::kMaxJson + 16]{};
char live_json[ble::kMaxJson + 16]{};
std::size_t status_length = 0, live_length = 0;
std::atomic<bool> status_dirty{false}, live_dirty{false};
portMUX_TYPE push_mutex = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE scan_mutex = portMUX_INITIALIZER_UNLOCKED;

void touch_ui() { ++ui; }

void set_status(const char *state, bool link_up) {
  // Strings are built before the critical section: no heap use with
  // interrupts masked.
  char ip[16] = "";
  char mac[18] = "";
  int rssi = 0;
  if (link_up) {
    esp_netif_ip_info_t address{};
    if (station_netif &&
        esp_netif_get_ip_info(station_netif, &address) == ESP_OK)
      esp_ip4addr_ntoa(&address.ip, ip, sizeof(ip));
    rssi = station_rssi();
  }
  station_mac(mac, sizeof(mac));
  portENTER_CRITICAL(&status_mutex);
  current.state = state;
  std::memcpy(current.ip, ip, sizeof(ip));
  std::memcpy(current.mac, mac, sizeof(mac));
  current.rssi = rssi;
  std::snprintf(current.host, sizeof(current.host), "%s", host_label);
  current.client = any_client();
  current.clients = authenticated_clients();
  current.authenticated = current.clients > 0;
  current.sessions = sessions.load();
  current.bytes_out = bytes_out.load();
  portEXIT_CRITICAL(&status_mutex);
  touch_ui();
}

// ---- transmit path. Only the LAN task touches sockets.

void signal_space() {
  if (space_event)
    xSemaphoreGive(space_event);
}

// Asks the LAN task to close a slot. Keeps the first reason.
void request_drop(Client &client, DisconnectReason reason, int err) {
  if (client.drop.load())
    return;
  client.drop_reason = static_cast<std::uint8_t>(reason);
  client.drop_err = err;
  client.drop = true;
}

ossync::EnqueueResult push(std::uint8_t peer, const std::uint8_t *frame,
                           std::size_t length,
                           std::uint32_t expected_generation = 0,
                           bool require_authenticated = true,
                           bool scan_reply = false) {
  if (peer >= ble::kMaxLanClients || !clients[peer].tx_lock)
    return ossync::EnqueueResult::Closed;
  // A frame this side cannot encode is a caller bug, not a peer failure: keep
  // the client and refuse the frame without retry.
  if (length == 0 || length > kPayloadMax) {
    oslog.record_only().printf("LAN SEND REJECTED bytes=%u max=%u\n",
                               unsigned(length), unsigned(kPayloadMax));
    return ossync::EnqueueResult::Rejected;
  }
  auto &client = clients[peer];
  TxLock lock(client);
  const bool valid =
      client.fd.load() >= 0 && !client.closing.load() &&
      !drop_requested.load() && !client.drop.load() &&
      (!require_authenticated || client.authenticated.load()) &&
      (!expected_generation || client.generation.load() == expected_generation);
  if (!valid)
    return ossync::EnqueueResult::Closed;
  if (client.scan_reserve && !scan_reply)
    return ossync::EnqueueResult::Full;
  if (!client.tx.fits(length)) {
    // Waiting cannot help a frame larger than the ring. Close the peer so the
    // reply fails visibly instead of looping on Full.
    oslog.record_only().printf("LAN TX OVERFLOW slot=%u bytes=%u ring=%u\n",
                               unsigned(peer), unsigned(length),
                               unsigned(client.tx.capacity()));
    request_drop(client, DisconnectReason::TxOverflow, 0);
    return ossync::EnqueueResult::Closed;
  }
  return client.tx.try_push(frame, length);
}

std::int16_t clamp_err(int err) {
  return static_cast<std::int16_t>(err > 32767    ? 32767
                                   : err < -32768 ? -32768
                                                  : err);
}

// lwIP aborts a peer that stopped answering keepalive probes or
// retransmissions with ERR_ABRT, which a socket call reports as ECONNABORTED.
// Other stacks report ETIMEDOUT. Both mean the phone went silent.
DisconnectReason transport_reason(int err, DisconnectReason other) {
  return err == ECONNABORTED || err == ETIMEDOUT
             ? DisconnectReason::KeepaliveFailed
             : other;
}

std::uint16_t clamp_u16(std::size_t value) {
  return static_cast<std::uint16_t>(value > 65535 ? 65535 : value);
}

// Sends what the socket accepts now. Returns false on a socket error, with
// errno set; would-block is not an error.
bool flush(std::uint8_t peer, std::int64_t now) {
  auto &client = clients[peer];
  const int fd = client.fd.load();
  bool progressed = false;
  for (;;) {
    const std::uint8_t *data = nullptr;
    std::size_t length;
    {
      TxLock lock(client);
      length = client.tx.peek(&data);
    }
    // Producers only write free space, so the peeked span is stable.
    if (!length || fd < 0)
      break;
    const int count = ::send(fd, data, length, MSG_DONTWAIT);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      break;
    if (count <= 0) {
      const int failure = count == 0 ? EPIPE : errno;
      oslog.record_only().printf("LAN SEND FAILED slot=%u errno=%d\n",
                                 unsigned(peer), failure);
      errno = failure;
      return false;
    }
    {
      TxLock lock(client);
      client.tx.consume(static_cast<std::size_t>(count));
    }
    bytes_out += static_cast<std::uint32_t>(count);
    progressed = true;
  }
  if (progressed) {
    client.stalled_since_us = now;
    signal_space();
  }
  return true;
}

bool ring_empty(Client &client) {
  TxLock lock(client);
  return client.tx.empty();
}

// Last step of every close. Records the event the BYE announced.
void finish_close(std::uint8_t peer) {
  auto &client = clients[peer];
  if (client.fd.load() < 0)
    return;
  int fd;
  std::uint32_t generation;
  {
    TxLock lock(client);
    fd = client.fd.exchange(-1);
    client.authenticated = false;
    generation = client.generation.load();
    if (fd >= 0)
      ++client.generation;
    client.tx.reset();
    client.drop = false;
    client.closing = false;
  }
  client.rx_length = 0;
  client.status_wanted = client.live_wanted = false;
  client.stalled_since_us = -1;
  if (fd < 0)
    return;
  if (!client.write_shut)
    ::shutdown(fd, SHUT_WR);
  client.write_shut = false;
  ::close(fd);
  signal_space();
  ble::record_disconnect(
      client.close_reason, static_cast<std::uint8_t>(peer + 1), generation,
      clamp_err(client.close_err), client.close_queued, esp_timer_get_time());
  oslog.record_only().printf(
      "LAN DISCONNECT slot=%u reason=%s err=%d queued=%u authenticated=%s\n",
      unsigned(peer), ble::disconnect_reason_name(client.close_reason),
      client.close_err, unsigned(client.close_queued),
      client.close_was_authenticated ? "true" : "false");
  touch_ui();
}

// Every device-side close: queue BYE, flush it with pending frames for up to
// Options::close_flush_ms, shut the write side and close once the phone
// closes or the budget ends. BYE goes at the next frame boundary ahead of
// unsent frames when those frames are the problem (slow-peer, tx-overflow).
// Otherwise it follows them, so a REBOOTING or ERROR queued just before still
// arrives. A socket that already failed or that the phone closed gets no BYE;
// the event is still recorded for DIAG.
void begin_close(std::uint8_t peer, DisconnectReason reason, int err = 0) {
  auto &client = clients[peer];
  if (client.fd.load() < 0 || client.closing.load())
    return;
  const bool bye = reason != DisconnectReason::PeerClosed &&
                   reason != DisconnectReason::RecvError &&
                   reason != DisconnectReason::SendError &&
                   reason != DisconnectReason::KeepaliveFailed;
  const std::int64_t now = esp_timer_get_time();
  {
    TxLock lock(client);
    client.closing = true;
    client.close_reason = reason;
    client.close_err = err;
    client.close_was_authenticated = client.authenticated.load();
    client.close_queued = clamp_u16(client.tx.frames());
    if (!bye || reason == DisconnectReason::SlowPeer ||
        reason == DisconnectReason::TxOverflow)
      client.tx.drop_unstarted();
    if (bye) {
      std::uint8_t frame[ble::kByeBytes];
      const auto length = ble::encode_bye(
          frame, sizeof(frame), reason, client.generation.load(),
          clamp_err(err), client.close_queued,
          static_cast<std::uint32_t>(now / 1000000));
      if (client.tx.try_push(frame, length) != ossync::EnqueueResult::Ok) {
        // A full ring cannot hold BYE behind its frames. The reason matters
        // more than frames that would not drain within the budget anyway.
        client.tx.drop_unstarted();
        client.tx.try_push(frame, length);
      }
    }
    client.tx.close();
  }
  client.close_deadline_us = now + close_flush_us;
  signal_space();
  if (!bye) {
    finish_close(peer);
    return;
  }
  // Most closes finish here: an idle socket takes BYE at once.
  if (!flush(peer, now)) {
    finish_close(peer);
    return;
  }
  if (ring_empty(client)) {
    ::shutdown(client.fd.load(), SHUT_WR);
    client.write_shut = true;
  }
}

// Closing slots: flush, half-close, then wait for the phone's FIN so unread
// input cannot turn the close into a reset that discards BYE.
void service_closing(std::uint8_t peer, std::int64_t now) {
  auto &client = clients[peer];
  if (!client.closing.load() || client.fd.load() < 0)
    return;
  if (!client.write_shut) {
    if (!flush(peer, now)) {
      finish_close(peer);
      return;
    }
    if (ring_empty(client)) {
      ::shutdown(client.fd.load(), SHUT_WR);
      client.write_shut = true;
    }
  }
  if (now >= client.close_deadline_us)
    finish_close(peer);
}

// Input on a closing slot is discarded; EOF or an error ends the close.
void drain_closing_input(std::uint8_t peer) {
  auto &client = clients[peer];
  std::uint8_t scratch[128];
  const int count = ::recv(client.fd.load(), scratch, sizeof(scratch), 0);
  if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    return;
  if (count <= 0)
    finish_close(peer);
}

// ---- server task state

int listen_fd = -1;
bool radio_on = false, connected = false, mdns_on = false;
std::int64_t connect_started_us = 0, retry_at_us = 0;
config::Settings settings;

// Power follows LAN activity. With an authenticated peer the station leaves
// modem sleep, so ACKs and requests are not held for a DTIM period, and a
// CPU_FREQ_MAX lock holds the DFS maximum. An APB_FREQ_MAX lock would not:
// on the ESP32-S3 it caps the CPU at 80 MHz, the DFS minimum. Idle, the
// station returns to modem sleep and DFS may scale down. LAN task only.
esp_pm_lock_handle_t active_lock = nullptr;
bool power_active = false;

wifi_ps_type_t power_save_mode() {
  return power_active ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM;
}

void update_power() {
  const bool active = authenticated_clients() > 0;
  if (active == power_active)
    return;
  power_active = active;
  const esp_err_t ps = radio_on ? esp_wifi_set_ps(power_save_mode()) : ESP_OK;
  esp_err_t lock = ESP_OK;
  if (active_lock)
    lock = active ? esp_pm_lock_acquire(active_lock)
                  : esp_pm_lock_release(active_lock);
  oslog.record_only().printf("LAN POWER mode=%s ps=%s cpu_lock=%s code=%d/%d\n",
                             active ? "active" : "idle",
                             active ? "none" : "min-modem",
                             !active_lock ? "unavailable"
                             : active     ? "held"
                                          : "released",
                             ps, lock);
}

void start_server() {
  if (listen_fd >= 0 || !settings.lan_on)
    return;
  listen_fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listen_fd < 0) {
    oslog.record_only().printf("LAN ERROR operation=socket errno=%d\n", errno);
    return;
  }
  int reuse = 1;
  ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(config::kLanPort);
  if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) != 0 ||
      ::listen(listen_fd, ble::kMaxLanClients) != 0) {
    oslog.record_only().printf("LAN ERROR operation=bind-listen errno=%d\n",
                               errno);
    ::close(listen_fd);
    listen_fd = -1;
    return;
  }
  char proto[4];
  std::snprintf(proto, sizeof(proto), "%u", unsigned(kProtocolVersion));
  mdns_txt_item_t txt[] = {{"proto", proto},
                           {"thing", device_identity.thing},
                           {"dev", device_identity.device},
                           {"fw", device_identity.firmware}};
  if (mdns_init() == ESP_OK) {
    if (mdns_hostname_set(host_label) == ESP_OK &&
        mdns_instance_name_set(ble::local_name()) == ESP_OK &&
        mdns_service_add(nullptr, "_ossync", "_tcp", config::kLanPort, txt,
                         sizeof(txt) / sizeof(txt[0])) == ESP_OK)
      mdns_on = true;
    else
      mdns_free();
  }
  if (!mdns_on)
    oslog.record_only().println("LAN ERROR operation=mdns");
  portENTER_CRITICAL(&status_mutex);
  current.mdns = mdns_on;
  portEXIT_CRITICAL(&status_mutex);
  oslog.record_only().printf(
      "LAN LISTEN port=%u host=%s.local mdns=%s service=_ossync._tcp\n",
      unsigned(config::kLanPort), host_label, mdns_on ? "true" : "false");
}

void stop_server(DisconnectReason reason) {
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer)
    begin_close(peer, reason);
  // The listener goes away with its sessions, so their BYE flush is bounded
  // here in close_flush_ms rather than by later poll_server ticks.
  constexpr int kStepUs = 50000;
  const auto steps = close_flush_us / kStepUs + 1;
  for (std::int64_t step = 0; step < steps; ++step) {
    fd_set readable, writable;
    FD_ZERO(&readable);
    FD_ZERO(&writable);
    int highest = -1;
    for (auto &client : clients) {
      const int fd = client.fd.load();
      if (fd < 0)
        continue;
      FD_SET(fd, &readable);
      if (!client.write_shut)
        FD_SET(fd, &writable);
      highest = std::max(highest, fd);
    }
    if (highest < 0)
      break;
    timeval wait{0, kStepUs};
    const int ready =
        ::select(highest + 1, &readable, &writable, nullptr, &wait);
    const std::int64_t now = esp_timer_get_time();
    for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer) {
      const int fd = clients[peer].fd.load();
      if (fd < 0)
        continue;
      if (ready > 0 && FD_ISSET(fd, &readable))
        drain_closing_input(peer);
      else
        service_closing(peer, now);
    }
  }
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer)
    finish_close(peer);
  if (mdns_on) {
    mdns_free();
    mdns_on = false;
  }
  if (listen_fd >= 0) {
    ::close(listen_fd);
    listen_fd = -1;
    oslog.record_only().printf("LAN STOP reason=%s\n",
                               ble::disconnect_reason_name(reason));
  }
  portENTER_CRITICAL(&status_mutex);
  current.mdns = false;
  portEXIT_CRITICAL(&status_mutex);
}

void radio_off(const char *reason) {
  opensensor::network_time::set_network_available(false);
  stop_server(DisconnectReason::LanDisabled);
  if (radio_on) {
    esp_wifi_disconnect();
    esp_wifi_stop();
    station_has_ip = false;
    radio_on = false;
    oslog.record_only().printf("WIFI OFF reason=%s\n", reason);
  }
  connected = false;
  set_status("off", false);
}

void radio_connect() {
  // oscfg is the sole persistent settings owner; the driver uses RAM storage.
  // Restart on apply so old association/IP cannot keep an old LAN alive.
  station_has_ip = false;
  opensensor::network_time::prepare_network();
  // Every caller closed the sessions first; drop the active power state with
  // them so the new association starts in modem sleep.
  update_power();
  if (radio_on) {
    esp_wifi_disconnect();
    esp_wifi_stop();
    radio_on = false;
  }
  wifi_config_t station{};
  std::memcpy(station.sta.ssid, settings.ssid,
              std::min(sizeof(station.sta.ssid), std::strlen(settings.ssid)));
  std::memcpy(
      station.sta.password, settings.psk,
      std::min(sizeof(station.sta.password), std::strlen(settings.psk)));
  station.sta.threshold.authmode =
      settings.psk[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &station);
  if (result == ESP_OK)
    result = esp_wifi_start();
  radio_on = result == ESP_OK;
  if (radio_on) {
    esp_wifi_set_ps(power_save_mode());
    result = esp_wifi_connect();
  }
  connect_started_us = esp_timer_get_time();
  retry_at_us = result == ESP_OK ? 0 : connect_started_us + kRetryUs;
  set_status(result == ESP_OK ? "connecting" : "failed", false);
  oslog.record_only().printf("WIFI CONNECT ssid=%s code=%d\n", settings.ssid,
                             result);
}

void apply_time_policy(const config::NtpSettings &policy) {
  opensensor::network_time::Policy time_policy;
  time_policy.enabled = policy.enabled;
  time_policy.dhcp = policy.dhcp;
  time_policy.public_fallback = policy.public_fallback;
  time_policy.interval_s = policy.interval_s;
  static_assert(sizeof(time_policy.servers) == sizeof(policy.servers));
  std::memcpy(time_policy.servers, policy.servers, sizeof(time_policy.servers));
  opensensor::network_time::configure(time_policy);
}

void poll_radio() {
  const std::int64_t now = esp_timer_get_time();
  if (time_reapply.exchange(false)) {
    settings.ntp = config::get().ntp;
    apply_time_policy(settings.ntp);
  }
  if (reapply.exchange(false)) {
    settings = config::get();
    apply_time_policy(settings.ntp);
    // Reconcile lan.on and invalidate old sessions even when the station
    // stays associated with the same network. A drop requested with the same
    // settings write (token rotation, reboot) names the restart instead of
    // ServerStop. When a scan defers this reapply, poll_server consumes that
    // drop first and closes with the same reason.
    const bool lan_kept =
        settings.lan_on && settings.wifi_on && settings.ssid[0];
    const bool dropped = drop_requested.exchange(false);
    stop_server(!lan_kept ? DisconnectReason::LanDisabled
                : dropped
                    ? static_cast<DisconnectReason>(drop_all_reason.load())
                    : DisconnectReason::ServerStop);
    connected = false;
    if (!settings.wifi_on || !settings.ssid[0]) {
      radio_off("settings");
      return;
    }
    radio_connect();
    return;
  }
  if (!settings.wifi_on || !settings.ssid[0])
    return;
  const bool up = station_has_ip.load();
  const auto ip_generation = station_ip_generation.load();
  if (up && ip_generation != handled_ip_generation) {
    handled_ip_generation = ip_generation;
    opensensor::network_time::ip_changed();
  }
  if (up && !connected) {
    connected = true;
    // Nonblocking SNTP: successful responses alone produce an anchor. DNS or
    // internet failure leaves monotonic capture running without fake UTC.
    opensensor::network_time::set_network_available(true);
    set_status("connected", true);
    oslog.record_only().printf("WIFI CONNECTED ssid=%s ip=%s rssi=%d\n",
                               settings.ssid, status().ip, station_rssi());
    start_server();
  } else if (!up && connected) {
    connected = false;
    opensensor::network_time::set_network_available(false);
    stop_server(DisconnectReason::WifiLost);
    oslog.record_only().println("WIFI LOST");
    radio_connect();
  } else if (!up && retry_at_us == 0 &&
             now - connect_started_us > kConnectTimeoutUs) {
    set_status("failed", false);
    retry_at_us = now + kRetryUs;
    oslog.record_only().printf("WIFI FAILED ssid=%s status=%d retry_s=%lld\n",
                               settings.ssid, int(disconnect_reason.load()),
                               static_cast<long long>(kRetryUs / 1000000));
    esp_wifi_disconnect();
    station_has_ip = false;
  } else if (!up && retry_at_us && now >= retry_at_us) {
    radio_connect();
  } else if (up) {
    // Periodic RSSI refresh for the UI/CONFIG document; no UI event, so the
    // display loop's own 10 s cadence (and its serial print) is untouched.
    static std::int64_t last_refresh = 0;
    if (now - last_refresh > 10000000LL) {
      last_refresh = now;
      const int rssi = station_rssi();
      portENTER_CRITICAL(&status_mutex);
      current.rssi = rssi;
      portEXIT_CRITICAL(&status_mutex);
    }
  }
  opensensor::network_time::poll();
}

std::uint8_t auth_code(wifi_auth_mode_t mode) {
  switch (mode) {
  case WIFI_AUTH_OPEN:
    return 0;
  case WIFI_AUTH_WEP:
    return 1;
  case WIFI_AUTH_WPA_PSK:
    return 2;
  case WIFI_AUTH_WPA2_PSK:
    return 3;
  case WIFI_AUTH_WPA_WPA2_PSK:
    return 4;
  case WIFI_AUTH_WPA2_ENTERPRISE:
    return 5;
  case WIFI_AUTH_WPA3_PSK:
    return 6;
  case WIFI_AUTH_WPA2_WPA3_PSK:
    return 7;
  default:
    return 255;
  }
}

// Scans run on os-lan, the only sender, so a full ring cannot drain while a
// scan reply waits. Reserve the ring, flush the peer once and retry, else
// close it as tx-overflow so the scan never stalls the LAN task. The reserve
// keeps the worker from refilling the flushed space before the retry, so a
// peer that drained is never dropped. LAN peers only. A BLE reply goes
// through the scan outbox below and never waits.
bool respond(std::uint32_t link_generation, std::uint8_t peer,
             const std::uint8_t *frame, std::size_t length) {
  auto result = enqueue_response(frame, length, peer, link_generation);
  if (result != ossync::EnqueueResult::Full)
    return result == ossync::EnqueueResult::Ok;
  auto &client = clients[peer];
  {
    TxLock lock(client);
    client.scan_reserve = true;
  }
  if (!flush(peer, esp_timer_get_time())) {
    const int failure = errno;
    {
      TxLock lock(client);
      client.scan_reserve = false;
    }
    signal_space();
    begin_close(peer, DisconnectReason::SendError, failure);
    return false;
  }
  result = push(peer, frame, length, link_generation, true, true);
  {
    TxLock lock(client);
    client.scan_reserve = false;
    if (result == ossync::EnqueueResult::Full) {
      oslog.record_only().printf("LAN SCAN OVERFLOW slot=%u free=%u\n",
                                 unsigned(peer), unsigned(client.tx.free()));
      request_drop(client, DisconnectReason::TxOverflow, 0);
    }
  }
  signal_space();
  return result == ossync::EnqueueResult::Ok;
}

// A scan runs without blocking the LAN task: start_scan asks the driver for
// a non-blocking scan, WIFI_EVENT_SCAN_DONE sets scan_done, and poll_scan
// collects and reports the records on a later loop. Peers keep being served
// in between. Radio reconfiguration waits for the scan to end.
bool scan_running = false, scan_temporary = false;
std::int64_t scan_deadline_us = 0;

// The scan list. Task-owned static storage keeps it off the LAN stack. It
// stays valid until the reply is done, because a second scan waits for that.
wifi_ap_record_t records[kMaxScanEntries];

// The reply of one scan: the WIFI_AP frames, then WIFI_SCAN_END. LAN task
// only. It keeps the index of each reported record and builds a frame when it
// is sent, so the reply adds no frame copies to internal RAM. A LAN reply
// leaves at once. A BLE reply leaves through ble::try_send_response, as many
// frames per LAN step as the notify queue takes, because waiting for a BLE
// credit or for the storage worker's response lock would stop every LAN peer
// (#178). The scan stays pending until its reply is done, so a second scan
// waits.
struct ScanReply {
  std::uint8_t picks[kMaxScanEntries]{};
  std::uint16_t total = 0, next = 0, sent = 0, found = 0;
  std::uint32_t generation = 0;
  std::int64_t progress_us = 0;
  std::uint8_t end_error = 0;
  bool active = false, failed = false;
};
ScanReply scan_job;
using ScanFrame = std::uint8_t[4 + config::kSsidMax];
static_assert(kMaxScanEntries <= 255, "picks hold a record index in one byte");

// Builds frame `index` of the reply. The last one is WIFI_SCAN_END, which
// carries the count of AP frames actually queued, so a reply cut short still
// counts true.
std::size_t reply_frame(std::uint16_t index, ScanFrame &frame) {
  if (index + 1 >= scan_job.total) {
    frame[0] = ble::kFrameWifiScanEnd;
    frame[1] = static_cast<std::uint8_t>(scan_job.sent & 0xff);
    frame[2] = static_cast<std::uint8_t>((scan_job.sent >> 8) & 0xff);
    frame[3] = scan_job.end_error;
    return 4;
  }
  const auto &record = records[scan_job.picks[index]];
  const char *ssid = reinterpret_cast<const char *>(record.ssid);
  const auto length = strnlen(ssid, sizeof(record.ssid));
  frame[0] = ble::kFrameWifiAp;
  frame[1] =
      static_cast<std::uint8_t>(std::max<std::int8_t>(-127, record.rssi));
  frame[2] = auth_code(record.authmode);
  frame[3] = record.primary;
  std::memcpy(frame + 4, ssid, length);
  return 4 + length;
}

void end_scan_reply() {
  scan_job.active = false;
  scan_pending = false;
}

void log_scan_end() {
  if (scan_job.failed)
    return;
  oslog.record_only().printf("WIFI SCAN END found=%u reported=%u\n",
                             unsigned(scan_job.found), unsigned(scan_job.sent));
}

// Queues what the BLE notify queue takes now and returns. Full keeps the rest
// for the next LAN step. Closed (the peer left or the link moved on) skips to
// the end frame, which then fails too and ends the reply.
void pump_scan_reply(std::int64_t now) {
  while (scan_job.active) {
    const bool last = scan_job.next + 1 == scan_job.total;
    ScanFrame frame{};
    const auto length = reply_frame(scan_job.next, frame);
    const auto result =
        ble::try_send_response(frame, length, scan_job.generation);
    if (result == ossync::EnqueueResult::Full) {
      if (now - scan_job.progress_us < kScanReplyStallUs)
        return;
      oslog.record_only().printf("WIFI SCAN REPLY STALLED sent=%u of=%u\n",
                                 unsigned(scan_job.next),
                                 unsigned(scan_job.total));
      end_scan_reply();
      return;
    }
    const bool ok = result == ossync::EnqueueResult::Ok;
    if (ok)
      scan_job.progress_us = now;
    if (last) {
      log_scan_end();
      end_scan_reply();
      return;
    }
    if (ok) {
      ++scan_job.sent;
      ++scan_job.next;
    } else {
      scan_job.next = scan_job.total - 1;
    }
  }
}

void send_lan_scan_reply(std::uint32_t link_generation, std::uint8_t peer) {
  ScanFrame frame{};
  for (; scan_job.next + 1 < scan_job.total; ++scan_job.next) {
    const auto length = reply_frame(scan_job.next, frame);
    if (!respond(link_generation, peer, frame, length))
      break;
    ++scan_job.sent;
  }
  const auto end_length = reply_frame(scan_job.total - 1, frame);
  respond(link_generation, peer, frame, end_length);
  log_scan_end();
  end_scan_reply();
}

void finish_scan(esp_err_t result) {
  const auto link = static_cast<ble::Link>(scan_link.load());
  const auto link_generation = scan_generation.load();
  const auto peer = scan_peer.load();
  std::uint16_t found = kMaxScanEntries;
  std::memset(records, 0, sizeof(records));
  if (result == ESP_OK)
    result = esp_wifi_scan_get_ap_records(&found, records);
  scan_job.total = scan_job.next = scan_job.sent = 0;
  scan_job.found = 0;
  scan_job.failed = result != ESP_OK;
  scan_job.end_error = 0;
  // get_ap_records releases the driver's full list. Release it on errors too.
  if (result != ESP_OK) {
    esp_wifi_clear_ap_list();
    oslog.record_only().printf("WIFI SCAN FAILED code=%d\n", result);
    scan_job.end_error = ble::kErrWifiUnavailable;
  } else {
    scan_job.found = found;
    // Report each SSID once with its strongest access point; skip hidden.
    bool reported[kMaxScanEntries]{};
    for (std::uint16_t i = 0; i < found; ++i) {
      if (reported[i])
        continue;
      const char *ssid = reinterpret_cast<const char *>(records[i].ssid);
      const auto length = strnlen(ssid, sizeof(records[i].ssid));
      if (length == 0 || length > config::kSsidMax)
        continue;
      std::uint16_t best = i;
      for (std::uint16_t j = i + 1; j < found; ++j) {
        if (!reported[j] &&
            std::memcmp(records[j].ssid, records[i].ssid, length + 1) == 0) {
          reported[j] = true;
          if (records[j].rssi > records[best].rssi)
            best = j;
        }
      }
      scan_job.picks[scan_job.total++] = static_cast<std::uint8_t>(best);
    }
  }
  ++scan_job.total; // the end frame
  if (scan_temporary) {
    esp_wifi_stop();
    station_has_ip = false;
  }
  scan_armed = false;
  scan_running = scan_temporary = false;
  if (link != ble::Link::Lan) {
    scan_job.generation = link_generation;
    scan_job.progress_us = esp_timer_get_time();
    scan_job.active = true;
    pump_scan_reply(scan_job.progress_us);
    return;
  }
  send_lan_scan_reply(link_generation, peer);
}

void start_scan() {
  scan_temporary = !radio_on;
  esp_err_t result = ESP_OK;
  if (scan_temporary)
    result = esp_wifi_start();
  oslog.record_only().println("WIFI SCAN BEGIN");
  wifi_scan_config_t scan{};
  scan.show_hidden = false;
  scan_done = false;
  scan_armed = true;
  if (result == ESP_OK)
    result = esp_wifi_scan_start(&scan, false);
  if (result != ESP_OK) {
    finish_scan(result);
    return;
  }
  scan_running = true;
  scan_deadline_us = esp_timer_get_time() + kScanTimeoutUs;
}

void poll_scan() {
  if (!scan_running)
    return;
  if (scan_done.exchange(false)) {
    finish_scan(scan_done_status.load() == 0 ? ESP_OK : ESP_FAIL);
    return;
  }
  if (esp_timer_get_time() < scan_deadline_us)
    return;
  esp_wifi_scan_stop();
  finish_scan(ESP_ERR_TIMEOUT);
}

bool constant_time_equal(const std::uint8_t *a, const std::uint8_t *b,
                         std::size_t length) {
  std::uint8_t diff = 0;
  for (std::size_t i = 0; i < length; ++i)
    diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

bool token_matches(const std::uint8_t *handshake) {
  const config::Settings now = config::get();
  return std::memcmp(handshake, "OSS1", 4) == 0 &&
         constant_time_equal(handshake + 4, now.token, config::kTokenBytes);
}

// Non-blocking as well as MSG_DONTWAIT: some stacks block a send despite the
// flag (macOS loopback did, measured on the host test), and nothing on the LAN
// task may wait for a peer. Keepalive ends a phone that left the network
// without FIN (screen off, out of range) in idle + interval * count seconds
// instead of the five minute idle timeout.
void configure_client_socket(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, (flags < 0 ? 0 : flags) | O_NONBLOCK);
  int on = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &options.keepalive_idle_s,
               sizeof(options.keepalive_idle_s));
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &options.keepalive_interval_s,
               sizeof(options.keepalive_interval_s));
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &options.keepalive_count,
               sizeof(options.keepalive_count));
}

// A refused client never gets a slot or a ring: one non-blocking send carries
// ERROR and BYE, then the socket closes.
void refuse_client(int fd) {
  std::uint8_t buffer[2 + kErrorFrameBytes + 2 + ble::kByeBytes];
  const auto error_length = encode_error(buffer + 2, static_cast<ble::Op>(0),
                                         ble::kErrBusy, "sessions-full");
  buffer[0] = error_length & 0xff;
  buffer[1] = (error_length >> 8) & 0xff;
  std::uint8_t *bye = buffer + 2 + error_length;
  const auto bye_length = ble::encode_bye(
      bye + 2, ble::kByeBytes, DisconnectReason::SessionsFull, 0, 0, 0,
      static_cast<std::uint32_t>(esp_timer_get_time() / 1000000));
  bye[0] = bye_length & 0xff;
  bye[1] = (bye_length >> 8) & 0xff;
  ::send(fd, buffer, 2 + error_length + 2 + bye_length, MSG_DONTWAIT);
  ::shutdown(fd, SHUT_WR);
  ::close(fd);
  ble::record_disconnect(DisconnectReason::SessionsFull, ble::kEventLinkRefused,
                         0, 0, 0, esp_timer_get_time());
}

void accept_client() {
  sockaddr_in address{};
  socklen_t address_length = sizeof(address);
  const int fd = ::accept(listen_fd, reinterpret_cast<sockaddr *>(&address),
                          &address_length);
  if (fd < 0)
    return;
  configure_client_socket(fd);
  char peer_text[INET_ADDRSTRLEN] = "?";
  inet_ntop(AF_INET, &address.sin_addr, peer_text, sizeof(peer_text));
  std::uint8_t peer = 0;
  for (; peer < ble::kMaxLanClients; ++peer)
    if (clients[peer].fd.load() < 0 && clients[peer].tx_lock)
      break;
  if (peer == ble::kMaxLanClients) {
    oslog.record_only().printf("LAN REFUSE peer=%s reason=busy\n", peer_text);
    refuse_client(fd);
    return;
  }
  auto &client = clients[peer];
  client.rx_length = 0;
  client.since_us = client.last_rx_us = esp_timer_get_time();
  client.stalled_since_us = -1;
  client.status_wanted = client.live_wanted = false;
  client.write_shut = false;
  {
    TxLock lock(client);
    client.tx.reset();
    client.drop = false;
    client.closing = false;
    client.authenticated = false;
    ++client.generation;
    client.fd = fd;
  }
  set_status(connected ? "connected" : "connecting", connected);
  oslog.record_only().printf("LAN CONNECT peer=%s slot=%u\n", peer_text,
                             unsigned(peer));
}

bool send_hello(std::uint8_t peer) {
  const char *info = ble::info_json();
  const std::size_t info_length = std::strlen(info);
  std::uint8_t frame[4 + ble::kMaxInfo];
  if (info_length > ble::kMaxInfo)
    return false;
  frame[0] = ble::kFrameHello;
  frame[1] = kProtocolVersion;
  frame[2] = kPayloadMax & 0xff;
  frame[3] = (kPayloadMax >> 8) & 0xff;
  std::memcpy(frame + 4, info, info_length);
  return push(peer, frame, 4 + info_length) == ossync::EnqueueResult::Ok;
}

bool open_session(std::uint8_t peer) {
  auto &client = clients[peer];
  {
    TxLock lock(client);
    ++client.generation;
    client.authenticated = true;
  }
  ++sessions;
  if (!send_hello(peer)) {
    if (client.drop.load())
      begin_close(peer,
                  static_cast<DisconnectReason>(client.drop_reason.load()),
                  client.drop_err.load());
    else if (drop_requested.load())
      // push refuses while a drop is pending, so report the drop's reason.
      begin_close(peer, static_cast<DisconnectReason>(drop_all_reason.load()));
    else
      begin_close(peer, DisconnectReason::TxOverflow);
    return false;
  }
  client.status_wanted = client.live_wanted = true;
  set_status("connected", true);
  oslog.record_only().printf("LAN AUTH result=ok slot=%u session=%lu\n",
                             unsigned(peer),
                             static_cast<unsigned long>(sessions.load()));
  return true;
}

// LAN task frames that are not worker replies: auth and framing errors and
// the queue-full refusal. They may target an unauthenticated slot.
ossync::EnqueueResult queue_error(std::uint8_t peer, ble::Op op,
                                  ble::Error code, const char *detail) {
  std::uint8_t frame[kErrorFrameBytes];
  const auto length = encode_error(frame, op, code, detail);
  return push(peer, frame, length, 0, false);
}

void handle_client_bytes(std::uint8_t peer) {
  auto &client = clients[peer];
  if (client.closing.load()) {
    drain_closing_input(peer);
    return;
  }
  const int count = ::recv(client.fd.load(), client.rx + client.rx_length,
                           sizeof(client.rx) - client.rx_length, 0);
  if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    return;
  if (count <= 0) {
    if (count == 0)
      begin_close(peer, DisconnectReason::PeerClosed);
    else
      begin_close(peer, transport_reason(errno, DisconnectReason::RecvError),
                  errno);
    return;
  }
  client.rx_length += static_cast<std::size_t>(count);
  client.last_rx_us = esp_timer_get_time();
  if (!client.authenticated.load()) {
    if (client.rx_length < kHandshakeBytes)
      return;
    if (!token_matches(client.rx)) {
      oslog.record_only().println("LAN AUTH result=rejected");
      queue_error(peer, static_cast<ble::Op>(0), ble::kErrAuth, "token");
      begin_close(peer, DisconnectReason::AuthFailed);
      return;
    }
    std::memmove(client.rx, client.rx + kHandshakeBytes,
                 client.rx_length - kHandshakeBytes);
    client.rx_length -= kHandshakeBytes;
    if (!open_session(peer))
      return;
  }
  // Framed requests: u16 length + body. Parser storage is bounded per slot.
  for (;;) {
    if (client.rx_length < 2 || client.drop.load() || client.closing.load())
      return;
    const std::size_t body = client.rx[0] | (client.rx[1] << 8);
    if (body == 0 || body > ble::kMaxControlBytes) {
      queue_error(peer, static_cast<ble::Op>(0), ble::kErrMalformed, "length");
      begin_close(peer, DisconnectReason::Malformed);
      return;
    }
    if (client.rx_length < 2 + body)
      return;
    ble::ControlRequest request;
    request.received_mono_us = esp_timer_get_time();
    request.link = ble::Link::Lan;
    request.peer = peer;
    request.link_generation = client.generation.load();
    request.length = static_cast<std::uint16_t>(body);
    std::memcpy(request.bytes, client.rx + 2, body);
    std::memmove(client.rx, client.rx + 2 + body, client.rx_length - 2 - body);
    client.rx_length -= 2 + body;
    oslog.record_only().printf("LAN CMD slot=%u op=0x%02x bytes=%u\n",
                               unsigned(peer), unsigned(request.bytes[0]),
                               unsigned(request.length));
    ossync::abort::stamp(request);
    if (request_handler(request)) {
      ossync::abort::commit(request);
      continue;
    }
    // A refusal that cannot be queued would leave the request unanswered,
    // so close the session visibly instead.
    if (queue_error(peer, static_cast<ble::Op>(request.bytes[0]), ble::kErrBusy,
                    "queue-full") != ossync::EnqueueResult::Ok) {
      oslog.record_only().printf("LAN REFUSAL LOST slot=%u op=0x%02x\n",
                                 unsigned(peer), unsigned(request.bytes[0]));
      begin_close(peer, DisconnectReason::TxOverflow);
      return;
    }
  }
}

// Serialises the newest STATUS or LIVE document when the ring has room and
// the previous copy has left it, so at most one of each is ever queued.
void fill_push(std::uint8_t peer, std::uint8_t type, const char *json,
               const std::size_t &json_length, bool &wanted,
               std::uint64_t &end) {
  auto &client = clients[peer];
  if (!wanted)
    return;
  {
    TxLock lock(client);
    if (client.tx.consumed_total() < end)
      return;
  }
  std::uint8_t frame[1 + ble::kMaxJson + 16];
  std::size_t length;
  portENTER_CRITICAL(&push_mutex);
  length = json_length;
  std::memcpy(frame + 1, json, length);
  portEXIT_CRITICAL(&push_mutex);
  if (!length) {
    wanted = false;
    return;
  }
  frame[0] = type;
  TxLock lock(client);
  if (client.closing.load() || !client.authenticated.load())
    return;
  if (client.tx.try_push(frame, 1 + length) == ossync::EnqueueResult::Ok) {
    end = client.tx.pushed_total();
    wanted = false;
  }
}

void push_documents() {
  const bool new_status = status_dirty.exchange(false);
  const bool new_live = live_dirty.exchange(false);
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer) {
    auto &client = clients[peer];
    if (!client.authenticated.load() || client.closing.load() ||
        client.drop.load())
      continue;
    client.status_wanted |= new_status;
    client.live_wanted |= new_live;
    fill_push(peer, ble::kFrameStatus, status_json, status_length,
              client.status_wanted, client.status_end);
    fill_push(peer, ble::kFrameLive, live_json, live_length, client.live_wanted,
              client.live_end);
  }
}

void poll_server(int wait_us = 0) {
  const bool drop_all = drop_requested.exchange(false);
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer) {
    auto &client = clients[peer];
    if (drop_all)
      begin_close(peer, static_cast<DisconnectReason>(drop_all_reason.load()));
    else if (client.drop.load())
      begin_close(peer,
                  static_cast<DisconnectReason>(client.drop_reason.load()),
                  client.drop_err.load());
  }
  if (listen_fd < 0)
    return;
  push_documents();
  fd_set readable, writable;
  FD_ZERO(&readable);
  FD_ZERO(&writable);
  FD_SET(listen_fd, &readable);
  int highest = listen_fd;
  int fds[ble::kMaxLanClients];
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer) {
    auto &client = clients[peer];
    fds[peer] = client.fd.load();
    if (fds[peer] < 0)
      continue;
    FD_SET(fds[peer], &readable);
    if (!client.write_shut && !ring_empty(client))
      FD_SET(fds[peer], &writable);
    highest = std::max(highest, fds[peer]);
  }
  // Socket readiness wakes the task immediately, but a worker push into an
  // empty ring does not. While a session is open the timeout is kept short so
  // replies leave within kActiveWaitUs. Idle, it only bounds radio/settings,
  // expiry and snapshot housekeeping.
  if (highest != listen_fd && wait_us > kActiveWaitUs)
    wait_us = kActiveWaitUs;
  timeval wait{0, wait_us};
  const int ready = ::select(highest + 1, &readable, &writable, nullptr, &wait);
  if (ready < 0 && wait_us)
    vTaskDelay(kTick); // avoid a busy loop if the socket layer fails
  std::int64_t now = esp_timer_get_time();
  if (ready > 0) {
    for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer)
      if (fds[peer] >= 0 && fds[peer] == clients[peer].fd.load() &&
          FD_ISSET(fds[peer], &writable) && !clients[peer].write_shut &&
          !flush(peer, now)) {
        if (clients[peer].closing.load())
          finish_close(peer);
        else
          begin_close(peer,
                      transport_reason(errno, DisconnectReason::SendError),
                      errno);
      }
    for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer)
      if (fds[peer] >= 0 && fds[peer] == clients[peer].fd.load() &&
          FD_ISSET(fds[peer], &readable))
        handle_client_bytes(peer);
    if (FD_ISSET(listen_fd, &readable))
      accept_client();
  }
  now = esp_timer_get_time();
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer) {
    auto &client = clients[peer];
    if (client.fd.load() < 0)
      continue;
    if (client.closing.load()) {
      service_closing(peer, now);
      continue;
    }
    if (ring_empty(client)) {
      client.stalled_since_us = -1;
    } else if (client.stalled_since_us < 0) {
      client.stalled_since_us = now;
    } else if (now - client.stalled_since_us >= slow_peer_us) {
      oslog.record_only().printf(
          "LAN SLOW PEER slot=%u stalled_ms=%lld\n", unsigned(peer),
          static_cast<long long>((now - client.stalled_since_us) / 1000));
      begin_close(peer, DisconnectReason::SlowPeer, EAGAIN);
      continue;
    }
    if (!client.authenticated.load() && now - client.since_us > kHandshakeUs)
      begin_close(peer, DisconnectReason::HandshakeTimeout);
    else if (now - client.last_rx_us > kIdleUs)
      begin_close(peer, DisconnectReason::IdleTimeout);
  }
}

// One LAN task iteration. Settings and reconnects wait while a scan runs,
// since restarting the station would abort it; peers are served throughout.
void lan_step() {
  if (scan_running) {
    poll_scan();
  } else {
    poll_radio();
    if (scan_job.active)
      pump_scan_reply(esp_timer_get_time());
    else if (scan_pending.load())
      start_scan();
  }
  if (listen_fd >= 0)
    poll_server(100000);
  else
    vTaskDelay(kTick);
  update_power();
}

void lan_task(void *) {
  for (;;)
    lan_step();
}
} // namespace

bool take_time_anchor(TimeAnchor &anchor) {
  return opensensor::network_time::take_anchor(anchor);
}

// Rings live for the program: a failed begin keeps them for the retry.
bool allocate_clients() {
  if (!space_event)
    space_event = xSemaphoreCreateBinary();
  if (!space_event)
    return false;
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer) {
    auto &client = clients[peer];
    if (!client.tx_lock)
      client.tx_lock = xSemaphoreCreateMutex();
    if (!client.tx_lock)
      return false;
    if (client.tx.capacity())
      continue;
    std::size_t bytes = options.ring_bytes;
    auto *storage = static_cast<std::uint8_t *>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!storage) {
      // Without PSRAM a smaller internal ring still carries every reply
      // frame; the worker waits on it more often.
      bytes = std::min<std::size_t>(bytes, 2 + kPayloadMax + 2 * 1024);
      storage = static_cast<std::uint8_t *>(
          heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      oslog.record_only().printf("LAN RING internal slot=%u bytes=%u\n",
                                 unsigned(peer), unsigned(bytes));
    }
    if (!storage)
      return false;
    TxLock lock(client);
    client.tx.attach(storage, bytes);
  }
  return true;
}

bool begin(const char *host, const ble::Identity &identity,
           ble::RequestHandler handler, const Options &transmit) {
  if (!handler || station_netif)
    return false;
  options = transmit;
  slow_peer_us = std::int64_t(options.slow_peer_ms) * 1000;
  close_flush_us = std::int64_t(options.close_flush_ms) * 1000;
  if (!allocate_clients()) {
    oslog.record_only().println("LAN ERROR operation=ring");
    return false;
  }
  device_identity = identity;
  request_handler = handler;
  std::snprintf(host_label, sizeof(host_label), "%s", host);
  esp_err_t result = esp_netif_init();
  if (result != ESP_OK)
    return false;
  result = esp_event_loop_create_default();
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
    return false;
  station_netif = esp_netif_create_default_wifi_sta();
  if (!station_netif)
    return false;
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  result = esp_wifi_init(&init);
  wifi_initialized = result == ESP_OK;
  if (result == ESP_OK)
    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (result == ESP_OK)
    result = esp_wifi_set_mode(WIFI_MODE_STA);
  if (result == ESP_OK)
    result = esp_netif_set_hostname(station_netif, host_label);
  if (result == ESP_OK)
    result = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, station_event, nullptr, &wifi_events);
  if (result == ESP_OK)
    result = esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, station_event, nullptr, &ip_events);
  if (result != ESP_OK) {
    oslog.record_only().printf("LAN ERROR operation=wifi-init code=%d\n",
                               result);
    release_station();
    return false;
  }
  std::snprintf(current.host, sizeof(current.host), "%s", host_label);
  station_mac(current.mac, sizeof(current.mac));
  reapply = true;
  if (!active_lock && esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "os-lan",
                                         &active_lock) != ESP_OK) {
    // Without PM_ENABLE the CPU stays at its boot frequency; only power
    // save follows activity.
    active_lock = nullptr;
  }
  if (xTaskCreate(lan_task, "os-lan", options.task_stack_bytes, nullptr,
                  options.task_priority, nullptr) != pdPASS) {
    oslog.record_only().println("LAN ERROR operation=task");
    release_station();
    return false;
  }
  lan_ready = true;
  oslog.record_only().printf("LAN BEGIN host=%s port=%u\n", host_label,
                             unsigned(config::kLanPort));
  return true;
}

void apply_settings() { reapply = true; }
void apply_time_settings() { time_reapply = true; }

bool request_scan(ble::Link link, std::uint32_t link_generation,
                  std::uint8_t peer) {
  if (!lan_ready.load())
    return false;
  // Both transport tasks may request a scan. Publish its owner before making
  // it visible to the LAN task, while excluding a second concurrent caller.
  portENTER_CRITICAL(&scan_mutex);
  if (scan_pending.load()) {
    portEXIT_CRITICAL(&scan_mutex);
    return false;
  }
  scan_link = static_cast<std::uint8_t>(link);
  scan_generation = link_generation;
  scan_peer = peer;
  scan_pending = true;
  portEXIT_CRITICAL(&scan_mutex);
  return true;
}

void drop_session(ble::DisconnectReason reason) {
  drop_all_reason = static_cast<std::uint8_t>(reason);
  drop_requested = true;
}

ossync::EnqueueResult enqueue_response(const std::uint8_t *frame,
                                       std::size_t length, std::uint8_t peer,
                                       std::uint32_t expected_generation) {
  return push(peer, frame, length, expected_generation);
}

std::size_t encode_error(std::uint8_t *frame, ble::Op op, ble::Error code,
                         const char *detail) {
  frame[0] = ble::kFrameError;
  frame[1] = static_cast<std::uint8_t>(op);
  frame[2] = static_cast<std::uint8_t>(code);
  std::size_t length = 3;
  if (detail) {
    const std::size_t copy = std::min<std::size_t>(std::strlen(detail), 64);
    std::memcpy(frame + 3, detail, copy);
    length += copy;
  }
  oslog.record_only().printf("LAN ERROR op=0x%02x code=%u\n", unsigned(op),
                             unsigned(code));
  return length;
}

ossync::EnqueueResult enqueue_error(ble::Op op, ble::Error code,
                                    const char *detail, std::uint8_t peer,
                                    std::uint32_t expected_generation) {
  std::uint8_t frame[kErrorFrameBytes];
  const auto length = encode_error(frame, op, code, detail);
  return push(peer, frame, length, expected_generation);
}

bool wait_for_space(std::uint32_t timeout_ms) {
  return space_event &&
         xSemaphoreTake(space_event, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool deliver(const std::uint8_t *frame, std::size_t length, std::uint8_t peer,
             std::uint32_t expected_generation, WaitStep between_steps,
             void *context) {
  // The slow peer rule runs in the LAN task, so it cannot end this wait when
  // that task stops draining. Bound it as BLE does: twice the slow peer
  // budget without a space wake from the LAN task gives up. The caller's wait
  // step stamps the worker heartbeat, so without this bound a stuck LAN task
  // would keep the worker looking alive while nothing reaches the card.
  const std::int64_t budget_us = 2LL * options.slow_peer_ms * 1000LL;
  std::int64_t waited_from = -1;
  for (;;) {
    const auto result = push(peer, frame, length, expected_generation);
    if (result != ossync::EnqueueResult::Full)
      return result == ossync::EnqueueResult::Ok;
    const std::int64_t now = esp_timer_get_time();
    if (waited_from < 0) {
      waited_from = now;
    } else if (now - waited_from >= budget_us) {
      oslog.record_only().printf(
          "LAN ERROR operation=deliver slot=%u waited_ms=%lld\n",
          unsigned(peer), static_cast<long long>((now - waited_from) / 1000));
      return false;
    }
    if (wait_for_space(options.wait_step_ms))
      waited_from = esp_timer_get_time();
    if (between_steps && !between_steps(context))
      return false;
  }
}

void publish_status(const char *json, std::size_t length) {
  if (length > ble::kMaxJson + 16)
    return;
  portENTER_CRITICAL(&push_mutex);
  std::memcpy(status_json, json, length);
  status_length = length;
  portEXIT_CRITICAL(&push_mutex);
  status_dirty = true;
}

void publish_live(const char *json, std::size_t length) {
  if (length > ble::kMaxJson + 16)
    return;
  portENTER_CRITICAL(&push_mutex);
  std::memcpy(live_json, json, length);
  live_length = length;
  portEXIT_CRITICAL(&push_mutex);
  live_dirty = true;
}

std::uint16_t payload_max(std::uint8_t peer) {
  return peer < ble::kMaxLanClients && !drop_requested.load() &&
                 clients[peer].fd.load() >= 0 &&
                 clients[peer].authenticated.load() &&
                 !clients[peer].closing.load() && !clients[peer].drop.load()
             ? kPayloadMax
             : 0;
}

std::uint32_t connection_generation(std::uint8_t peer) {
  return peer < ble::kMaxLanClients ? clients[peer].generation.load() : 0;
}

Status status() {
  Status copy;
  portENTER_CRITICAL(&status_mutex);
  copy = current;
  portEXIT_CRITICAL(&status_mutex);
  copy.client = any_client();
  copy.clients = authenticated_clients();
  copy.authenticated = copy.clients > 0;
  copy.sessions = sessions.load();
  copy.bytes_out = bytes_out.load();
  return copy;
}

std::uint32_t ui_generation() { return ui.load(); }
} // namespace lan
