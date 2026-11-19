#include "network_time.h"
#include "utc_clock.h"

#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <lwip/dns.h>
#include <lwip/ip_addr.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace opensensor::network_time {
namespace {
constexpr std::int64_t kDnsTimeoutUs = 30000000;
constexpr std::int64_t kResponseTimeoutUs = 15000000;
constexpr std::int64_t kInitRetryUs = 30000000;
constexpr std::uint32_t kRetryMaxSeconds = 300;
constexpr unsigned kNativeSlots = 3;
constexpr unsigned kCandidates = kNativeSlots + kServerCount + 1;
constexpr char kPublicServer[] = "time.cloudflare.com";
Policy configured;
bool valid_policy = true, online = false, initialized = false;
std::uint32_t generation = 1, handled_sync = 0, retry_s = 30;
std::int64_t due_us = 0, deadline_us = 0;
portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;
Status snapshot;
bool accepting = false, pending = false;
enum class Phase { Idle, Resolving, Waiting };
Phase phase = Phase::Idle;
struct Candidate {
  char name[kServerMax + 1]{};
  bool public_server = false;
};
Candidate candidates[kCandidates];
unsigned candidate_count = 0, candidate_index = 0;
ip_addr_t dhcp_servers[kNativeSlots]{};
unsigned dhcp_count = 0;
// Native lwIP's DNS callback cannot be cancelled. Never recycle a live query;
// stale completions only free their own bounded context, not current state.
struct Query {
  char name[kServerMax + 1]{};
  ip_addr_t address{};
  std::uint32_t generation = 0;
  bool in_flight = false, ready = false, found = false;
};
Query queries[4];
Query *active_query = nullptr;
char numeric_server[48]{};
esp_err_t tcpip_barrier(void *);

// cppcheck-suppress constParameterCallback
void synchronized(struct timeval *time) {
  const auto mono = esp_timer_get_time();
  if (!time || time->tv_usec < 0 ||
      !utc::accepted_anchor(time->tv_sec,
                            static_cast<std::uint32_t>(time->tv_usec)))
    return;
  portENTER_CRITICAL(&mutex);
  if (accepting) {
    snapshot.last_sync = {static_cast<std::int64_t>(time->tv_sec),
                          static_cast<std::uint32_t>(time->tv_usec), mono};
    snapshot.has_anchor = true;
    snapshot.source = utc::Network;
    ++snapshot.sync_count;
    std::snprintf(snapshot.server, sizeof(snapshot.server), "%s",
                  candidates[candidate_index].name);
    snapshot.public_active = candidates[candidate_index].public_server;
    pending = true;
  }
  portEXIT_CRITICAL(&mutex);
}

void dns_completed(const char *, const ip_addr_t *address, void *argument) {
  auto *query = static_cast<Query *>(argument);
  portENTER_CRITICAL(&mutex);
  query->found = address != nullptr && !ip_addr_isany(address);
  if (query->found)
    query->address = *address;
  query->in_flight = false;
  query->ready = true;
  portEXIT_CRITICAL(&mutex);
}

esp_err_t resolve_on_tcpip(void *argument) {
  auto *query = static_cast<Query *>(argument);
  ip_addr_t result{};
  const auto error =
      dns_gethostbyname(query->name, &result, dns_completed, query);
  if (error != ERR_INPROGRESS)
    dns_completed(query->name, error == ERR_OK ? &result : nullptr, query);
  return ESP_OK;
}

bool valid_server(const char (&server)[kServerMax + 1]) {
  const auto length = strnlen(server, sizeof(server));
  if (length == sizeof(server))
    return false;
  if (!length)
    return true;
  ip_addr_t literal{};
  if (ipaddr_aton(server, &literal)) {
    if (ip_addr_isany(&literal))
      return false;
    return true;
  }
  // Hostnames only, never URL, host:port, whitespace or invalid DNS labels.
  unsigned label_length = 0;
  for (std::size_t i = 0; i < length; ++i) {
    const char c = server[i];
    if (c == '.') {
      if (!label_length || server[i - 1] == '-')
        return false;
      label_length = 0;
    } else {
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || (c == '-' && label_length)))
        return false;
      if (++label_length > 63)
        return false;
    }
  }
  if (server[length - 1] == '-')
    return false;
  return true;
}

bool validate(const Policy &policy) {
  return policy.interval_s >= kMinIntervalSeconds &&
         policy.interval_s <= kMaxIntervalSeconds &&
         std::all_of(std::begin(policy.servers), std::end(policy.servers),
                     valid_server);
}

void set_state(const char *state, int error = 0) {
  portENTER_CRITICAL(&mutex);
  snapshot.state = state;
  snapshot.running = initialized;
  snapshot.error = error;
  portEXIT_CRITICAL(&mutex);
}

void stop() {
  portENTER_CRITICAL(&mutex);
  accepting = false;
  snapshot.running = false;
  portEXIT_CRITICAL(&mutex);
  if (initialized) {
    // Numeric addresses ensure native SNTP has no outstanding DNS callback
    // which could later address a destroyed PCB or a different policy server.
    esp_netif_sntp_deinit();
    initialized = false;
    // Accept fresh lease offers between requests. During a request the
    // selected slots are frozen, so DHCP cannot relabel its response source.
    esp_sntp_servermode_dhcp(configured.enabled && configured.dhcp &&
                             valid_policy);
  }
}

void reset_cycle() {
  stop();
  ++generation;
  active_query = nullptr;
  phase = Phase::Idle;
  candidate_count = candidate_index = 0;
  due_us = 0;
  retry_s = 30;
}

bool same_policy(const Policy &a, const Policy &b) {
  if (a.enabled != b.enabled || a.dhcp != b.dhcp ||
      a.public_fallback != b.public_fallback || a.interval_s != b.interval_s)
    return false;
  for (std::size_t i = 0; i < kServerCount; ++i)
    if (std::memcmp(a.servers[i], b.servers[i], sizeof(a.servers[i])) != 0)
      return false;
  return true;
}

void add_candidate(const char *name, bool public_server = false) {
  if (!name[0])
    return;
  for (unsigned i = 0; i < candidate_count; ++i)
    if (std::strcmp(candidates[i].name, name) == 0)
      return;
  auto &candidate = candidates[candidate_count++];
  std::snprintf(candidate.name, sizeof(candidate.name), "%s", name);
  candidate.public_server = public_server;
}

void build_candidates() {
  candidate_count = candidate_index = 0;
  if (configured.dhcp) {
    for (unsigned i = 0; i < dhcp_count; ++i) {
      char literal[48]{};
      ipaddr_ntoa_r(&dhcp_servers[i], literal, sizeof(literal));
      add_candidate(literal);
    }
  }
  for (const char *server : configured.servers)
    add_candidate(server);
  if (configured.public_fallback)
    add_candidate(kPublicServer, true);
  portENTER_CRITICAL(&mutex);
  snapshot.server_count = candidate_count;
  portEXIT_CRITICAL(&mutex);
}

void next_candidate(int error) {
  stop();
  ++generation;
  active_query = nullptr;
  phase = Phase::Idle;
  ++candidate_index;
  if (candidate_index >= candidate_count) {
    candidate_count = candidate_index = 0;
    due_us = esp_timer_get_time() + std::int64_t(retry_s) * 1000000;
    retry_s = retry_s < kRetryMaxSeconds / 2 ? retry_s * 2 : kRetryMaxSeconds;
    set_state("retry", error);
  }
}

void start_response(const ip_addr_t &address) {
  // A lease renewal can otherwise replace every selected endpoint while
  // SNTP is waiting, making even a valid response carry the wrong provenance.
  esp_sntp_servermode_dhcp(false);
  const auto barrier = esp_netif_tcpip_exec(tcpip_barrier, nullptr);
  if (barrier != ESP_OK) {
    due_us = esp_timer_get_time() + kInitRetryUs;
    phase = Phase::Idle;
    set_state("retry", barrier);
    return;
  }
  ipaddr_ntoa_r(&address, numeric_server, sizeof(numeric_server));
  esp_sntp_config_t native =
      ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(0, ESP_SNTP_SERVER_LIST());
  static_assert(sizeof(native.servers) / sizeof(native.servers[0]) >=
                    kNativeSlots,
                "Enable at least three native SNTP slots");
  // lwIP retains sntp_current_server across stop(). Replicate the selected
  // numeric endpoint into every slot, so that hidden index cannot bypass
  // local-first policy or retain an obsolete configured server.
  native.num_of_servers = sizeof(native.servers) / sizeof(native.servers[0]);
  std::fill(std::begin(native.servers), std::end(native.servers),
            numeric_server);
  native.wait_for_sync = false;
  native.sync_cb = synchronized;
  esp_sntp_set_sync_interval(configured.interval_s * 1000);
  portENTER_CRITICAL(&mutex);
  accepting = true;
  handled_sync = snapshot.sync_count;
  portEXIT_CRITICAL(&mutex);
  const auto result = esp_netif_sntp_init(&native);
  initialized = result == ESP_OK;
  if (!initialized) {
    portENTER_CRITICAL(&mutex);
    accepting = false;
    portEXIT_CRITICAL(&mutex);
    due_us = esp_timer_get_time() + kInitRetryUs;
    phase = Phase::Idle;
    set_state("retry", result);
    return;
  }
  phase = Phase::Waiting;
  deadline_us = esp_timer_get_time() + kResponseTimeoutUs;
  set_state("waiting");
}

void start_candidate() {
  ip_addr_t literal{};
  if (ipaddr_aton(candidates[candidate_index].name, &literal)) {
    start_response(literal);
    return;
  }
  portENTER_CRITICAL(&mutex);
  const auto available =
      std::find_if(std::begin(queries), std::end(queries),
                   [](const Query &q) { return !q.in_flight; });
  Query *query = available == std::end(queries) ? nullptr : available;
  if (query) {
    query->generation = generation;
    query->in_flight = true;
    query->ready = query->found = false;
    std::snprintf(query->name, sizeof(query->name), "%s",
                  candidates[candidate_index].name);
  }
  portEXIT_CRITICAL(&mutex);
  if (!query) {
    due_us = esp_timer_get_time() + kInitRetryUs;
    set_state("retry", ESP_ERR_NO_MEM);
    return;
  }
  active_query = query;
  phase = Phase::Resolving;
  deadline_us = esp_timer_get_time() + kDnsTimeoutUs;
  set_state("resolving");
  const auto result = esp_netif_tcpip_exec(resolve_on_tcpip, query);
  if (result != ESP_OK) {
    portENTER_CRITICAL(&mutex);
    query->in_flight = false;
    portEXIT_CRITICAL(&mutex);
    next_candidate(result);
  }
}

esp_err_t capture_dhcp(void *) {
  dhcp_count = 0;
  for (unsigned i = 0; i < kNativeSlots; ++i) {
    // DHCP writes numeric addresses and clears the associated name. A name
    // still present is our candidate, not fresh DHCP option 42 evidence.
    if (esp_sntp_getservername(i) != nullptr)
      continue;
    const auto *address = esp_sntp_getserver(i);
    if (address && !ip_addr_isany(address))
      dhcp_servers[dhcp_count++] = *address;
  }
  return ESP_OK;
}

esp_err_t tcpip_barrier(void *) { return ESP_OK; }
} // namespace

bool configure(const Policy &policy) {
  const bool valid = validate(policy);
  if (!same_policy(policy, configured) || valid != valid_policy)
    reset_cycle();
  configured = policy;
  valid_policy = valid;
  esp_sntp_servermode_dhcp(policy.enabled && policy.dhcp && valid &&
                           !initialized);
  portENTER_CRITICAL(&mutex);
  snapshot.enabled = policy.enabled;
  snapshot.interval_s = policy.interval_s;
  portEXIT_CRITICAL(&mutex);
  if (!valid)
    set_state("invalid", ESP_ERR_INVALID_ARG);
  else if (!policy.enabled)
    set_state("disabled");
  else if (!online)
    set_state("offline");
  return valid;
}

void prepare_network() {
  reset_cycle();
  online = false;
  dhcp_count = 0;
  for (unsigned i = 0; i < kNativeSlots; ++i)
    esp_sntp_setserver(i, nullptr);
  esp_sntp_servermode_dhcp(configured.enabled && configured.dhcp &&
                           valid_policy);
  // esp_sntp_servermode_dhcp queues a TCP/IP callback; drain it before DHCP
  // starts. This is native esp-netif's start=false preparation without a PCB.
  const auto result = esp_netif_tcpip_exec(tcpip_barrier, nullptr);
  if (!valid_policy)
    set_state("invalid", ESP_ERR_INVALID_ARG);
  else
    set_state(configured.enabled ? "offline" : "disabled", result);
}

void ip_changed() {
  if (!configured.dhcp)
    return;
  // Only owner-task calls this; copying inside TCP/IP avoids retaining a
  // pointer into mutable SDK storage. Keep a previous cache on IP renewal
  // without new option 42, and never cache candidate addresses as DHCP.
  ip_addr_t previous[kNativeSlots];
  std::memcpy(previous, dhcp_servers, sizeof(previous));
  const unsigned previous_count = dhcp_count;
  const auto result = esp_netif_tcpip_exec(capture_dhcp, nullptr);
  if (result != ESP_OK || !dhcp_count) {
    std::memcpy(dhcp_servers, previous, sizeof(previous));
    dhcp_count = previous_count;
  }
  reset_cycle();
}

void set_network_available(bool available) {
  if (online == available)
    return;
  online = available;
  reset_cycle();
  if (!available)
    set_state(configured.enabled ? "offline" : "disabled");
}

void poll() {
  if (!valid_policy || !configured.enabled || !online)
    return;
  const auto now = esp_timer_get_time();
  if (phase == Phase::Waiting) {
    portENTER_CRITICAL(&mutex);
    const bool synced = snapshot.sync_count != handled_sync;
    const auto sync_mono = snapshot.last_sync.monotonic_us;
    portEXIT_CRITICAL(&mutex);
    if (synced) {
      stop();
      phase = Phase::Idle;
      candidate_count = candidate_index = 0;
      retry_s = 30;
      due_us = sync_mono + std::int64_t(configured.interval_s) * 1000000;
      set_state("synchronized");
      return;
    }
    if (now >= deadline_us)
      next_candidate(ESP_ERR_TIMEOUT);
    return;
  }
  if (phase == Phase::Resolving) {
    ip_addr_t address{};
    portENTER_CRITICAL(&mutex);
    const bool ready =
        active_query->ready && active_query->generation == generation;
    const bool found = ready && active_query->found;
    if (found)
      address = active_query->address;
    portEXIT_CRITICAL(&mutex);
    if (found)
      start_response(address);
    else if (ready || now >= deadline_us)
      next_candidate(ESP_ERR_TIMEOUT);
    return;
  }
  if (now < due_us)
    return;
  if (!candidate_count)
    build_candidates();
  if (!candidate_count) {
    set_state("unconfigured");
    return;
  }
  start_candidate();
}

bool take_anchor(Anchor &anchor) {
  portENTER_CRITICAL(&mutex);
  const bool available = pending;
  if (available) {
    anchor = snapshot.last_sync;
    pending = false;
  }
  portEXIT_CRITICAL(&mutex);
  return available;
}

Status status() {
  const auto now = esp_timer_get_time();
  portENTER_CRITICAL(&mutex);
  Status result = snapshot;
  portEXIT_CRITICAL(&mutex);
  if (result.has_anchor && now >= result.last_sync.monotonic_us)
    result.age_s = (now - result.last_sync.monotonic_us) / 1000000;
  result.stale =
      result.has_anchor && result.age_s > result.interval_s + kRetryMaxSeconds;
  if (std::strcmp(result.state, "synchronized") == 0 && result.stale)
    result.state = "stale";
  return result;
}
} // namespace opensensor::network_time
