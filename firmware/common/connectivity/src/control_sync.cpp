#include "control_sync.h"
#include "abort_signal.h"
#include "debug_log.h"
#include "descriptor_chunk.h"
#include "device_config.h"
#include "disconnect.h"
#include "wifi_link.h"
#include <sync_codec.h>

#include <cstdio>
#include <cstring>

namespace ossync {
namespace {
using namespace opensensor::sync;
constexpr std::size_t kPayloadCapacity = 1024;

void send_config(ble::Link link, ble::Op op, const ControlReplies &replies,
                 std::uint8_t page = 0) {
  const lan::Status wifi = lan::status();
  config::WifiView view{wifi.state,   wifi.ip,   wifi.rssi,        wifi.mac,
                        wifi.clients, wifi.host, ble::link().bonds};
  std::uint8_t frame[2 + ble::kMaxJson];
  config::PageInputs inputs{};
  if (page == 1 && replies.gnss_cell)
    inputs = {.gnss = true, .gnss_cell = replies.gnss_cell(replies.context)};
  std::size_t length =
      page ? config::build_page(reinterpret_cast<char *>(frame + 2),
                                ble::kMaxJson, page, inputs)
           : config::build_json(reinterpret_cast<char *>(frame + 2),
                                ble::kMaxJson, view);
  if (page == 2 && length >= 2) {
    const auto time = opensensor::network_time::status();
    char age[24] = "null";
    if (time.has_anchor)
      std::snprintf(age, sizeof(age), "%llu",
                    static_cast<unsigned long long>(time.age_s));
    const auto prefix = length - 2; // append inside the existing ntp object
    const int added = std::snprintf(
        reinterpret_cast<char *>(frame + 2) + prefix, ble::kMaxJson - prefix,
        ",\"state\":\"%s\",\"running\":%u,\"anchored\":%u,\"age_s\":%s,"
        "\"stale\":%u,\"source\":%ld,\"last_public\":%u,\"sync_count\":%lu,"
        "\"error\":%d}}",
        time.state, time.running ? 1U : 0U, time.has_anchor ? 1U : 0U, age,
        time.stale ? 1U : 0U, static_cast<long>(time.source),
        time.public_active ? 1U : 0U,
        static_cast<unsigned long>(time.sync_count), time.error);
    length =
        added > 0 && static_cast<std::size_t>(added) < ble::kMaxJson - prefix
            ? prefix + static_cast<std::size_t>(added)
            : 0;
  }
  if (!length) {
    replies.error(replies.context, link, op, ble::kErrMalformed, "json");
    return;
  }
  frame[0] = ble::kFrameConfig;
  frame[1] = config::reboot_required() ? 1 : 0;
  const auto payload_max = replies.payload_max(replies.context, link);
  if (link == ble::Link::Lan || 2 + length <= payload_max) {
    replies.respond(replies.context, link, frame, 2 + length);
    return;
  }
  constexpr std::size_t kHeader = 6;
  if (payload_max <= kHeader) {
    replies.error(replies.context, link, op, ble::kErrBusy, "mtu");
    return;
  }
  std::uint8_t chunk[ble::kMaxFrame];
  chunk[0] = ble::kFrameConfigChunk;
  chunk[1] = frame[1];
  put_u16(chunk + 2, static_cast<std::uint16_t>(length));
  constexpr std::size_t kSliceCap = 128;
  const auto slice_max =
      payload_max - kHeader < kSliceCap ? payload_max - kHeader : kSliceCap;
  for (std::size_t offset = 0; offset < length; offset += slice_max) {
    const auto count =
        length - offset < slice_max ? length - offset : slice_max;
    put_u16(chunk + 4, static_cast<std::uint16_t>(offset));
    std::memcpy(chunk + kHeader, frame + 2 + offset, count);
    if (!replies.respond(replies.context, link, chunk, kHeader + count))
      return;
  }
}

std::size_t reply_cap(ble::Link link, const ControlReplies &replies) {
  const std::size_t payload_max = replies.payload_max(replies.context, link);
  const std::size_t cap =
      link == ble::Link::Ble ? ble::kMaxFrame : kPayloadCapacity;
  return payload_max < cap ? payload_max : cap;
}

// Streams straight from the ring by cursor; no ring-sized copy. LOG_TAIL's
// u16 request clips the reply at 65535 bytes, so `sent` fits LOG_END's u16.
// An ABORT ends the stream with ERROR aborted and no LOG_END.
void send_log_tail(const ble::ControlRequest &request, std::uint16_t max_bytes,
                   const ControlReplies &replies) {
  const ble::Link link = request.link;
  const auto stop = [&] {
    replies.error(replies.context, link, ble::kOpLogTail, ble::kErrAborted,
                  nullptr);
    abort::note_stopped(request, ble::kOpLogTail);
  };
  std::uint32_t total = 0;
  auto cursor = oslog.tail_cursor(max_bytes, total);
  const std::size_t payload_max = reply_cap(link, replies);
  std::uint8_t frame[1 + kPayloadCapacity];
  std::size_t sent = 0;
  if (payload_max > 1) {
    const std::size_t slice_max = payload_max - 1;
    while (cursor.remaining) {
      if (abort::pending(request)) {
        stop();
        return;
      }
      const std::size_t slice =
          oslog.read(cursor, reinterpret_cast<char *>(frame + 1), slice_max);
      if (!slice)
        break;
      frame[0] = ble::kFrameLog;
      if (!replies.respond(replies.context, link, frame, 1 + slice)) {
        if (abort::pending(request)) {
          stop();
          return;
        }
        break;
      }
      sent += slice;
    }
  }
  std::uint8_t end[7];
  end[0] = ble::kFrameLogEnd;
  put_u32(end + 1, total);
  put_u16(end + 5, static_cast<std::uint16_t>(sent));
  replies.respond(replies.context, link, end, sizeof(end));
}

void send_diag(ble::Link link, const ControlReplies &replies) {
  const std::size_t capacity = reply_cap(link, replies);
  std::uint8_t frame[kPayloadCapacity];
  const std::size_t length = ble::encode_diag_snapshot(frame, capacity);
  if (!length) {
    replies.error(replies.context, link, ble::kOpGetDiag, ble::kErrBusy, "mtu");
    return;
  }
  replies.respond(replies.context, link, frame, length);
}

// One chunk per request from .rodata into this stack frame. No heap and no
// copy beyond the frame, the same budget as send_diag.
void send_descriptor(ble::Link link, std::span<const std::uint8_t> body,
                     const ControlReplies &replies) {
  std::uint8_t frame[kPayloadCapacity];
  const auto encoded = ble::wire::encode_descriptor_chunk(
      replies.descriptor, body,
      std::span<std::uint8_t>(frame, reply_cap(link, replies)));
  if (encoded) {
    replies.respond(replies.context, link, frame, *encoded);
    return;
  }
  struct Reply {
    ble::Error code;
    const char *detail;
  };
  const Reply reply = [&]() -> Reply {
    using enum ble::wire::DescriptorError;
    switch (encoded.error()) {
    case Malformed:
      return {.code = ble::kErrMalformed, .detail = "descriptor"};
    case ZeroLength:
      return {.code = ble::kErrMalformed, .detail = "length"};
    case Unconfigured:
      return {.code = ble::kErrNoDescriptor, .detail = nullptr};
    case Range:
      return {.code = ble::kErrRange, .detail = "offset"};
    case Mtu:
      break;
    }
    return {.code = ble::kErrBusy, .detail = "mtu"};
  }();
  replies.error(replies.context, link, ble::kOpGetDescriptor, reply.code,
                reply.detail);
}
} // namespace

bool handle_common_control(const ble::ControlRequest &request,
                           const ControlReplies &replies) {
  if (!request.length || request.length > ble::kMaxControlBytes ||
      !replies.respond || !replies.error || !replies.payload_max)
    return false;
  const std::uint8_t *body = request.bytes + 1;
  const std::size_t length = request.length - 1;
  switch (request.bytes[0]) {
  case ble::kOpGetConfig:
    if (length > 1 || (length == 1 && body[0] > 4))
      replies.error(replies.context, request.link, ble::kOpGetConfig,
                    ble::kErrMalformed, "page");
    else
      send_config(request.link, ble::kOpGetConfig, replies,
                  length ? body[0] : 0);
    return true;
  case ble::kOpSetConfig: {
    char bad_key[48];
    config::Actions actions;
    if (!config::apply_lines(reinterpret_cast<const char *>(body), length,
                             bad_key, sizeof(bad_key), actions)) {
      replies.error(replies.context, request.link, ble::kOpSetConfig,
                    ble::kErrInvalidConfig, bad_key);
      return true;
    }
    if (actions.clear_bonds)
      ble::clear_bonds();
    if (actions.wifi_changed)
      lan::apply_settings();
    else if (actions.ntp_changed)
      lan::apply_time_settings();
    send_config(request.link, ble::kOpSetConfig, replies);
    // After the reply, so a LAN caller reads CONFIG before BYE token-rotated.
    if (actions.rotate_token)
      lan::drop_session();
    return true;
  }
  case ble::kOpGetDiag:
    if (length)
      replies.error(replies.context, request.link, ble::kOpGetDiag,
                    ble::kErrMalformed, nullptr);
    else
      send_diag(request.link, replies);
    return true;
  case ble::kOpAbort: {
    // The worker runs one command at a time. Anything this ABORT stopped
    // already ended with ERROR aborted; the marker names it, or 0 when idle.
    if (length) {
      replies.error(replies.context, request.link, ble::kOpAbort,
                    ble::kErrMalformed, nullptr);
      return true;
    }
    std::uint8_t frame[ble::kAbortedBytes];
    replies.respond(replies.context, request.link, frame,
                    ble::encode_aborted(frame, sizeof(frame),
                                        abort::take_stopped(request)));
    return true;
  }
  case ble::kOpLogTail:
    if (length != 2)
      replies.error(replies.context, request.link, ble::kOpLogTail,
                    ble::kErrMalformed, nullptr);
    else
      send_log_tail(request, get_u16(body), replies);
    return true;
  case ble::kOpGetToken: {
    if (request.link != ble::Link::Ble) {
      replies.error(replies.context, request.link, ble::kOpGetToken,
                    ble::kErrNotOnThisLink, "ble-only");
      return true;
    }
    const config::Settings settings = config::get();
    std::uint8_t frame[3 + config::kTokenBytes];
    frame[0] = ble::kFrameToken;
    put_u16(frame + 1, config::kLanPort);
    std::memcpy(frame + 3, settings.token, config::kTokenBytes);
    replies.respond(replies.context, request.link, frame, sizeof(frame));
    oslog.println("LAN TOKEN issued=ble");
    return true;
  }
  case ble::kOpGetDescriptor:
    send_descriptor(request.link, {body, length}, replies);
    return true;
  case ble::kOpWifiScan:
    // Normal scans are dispatched on the LAN task. A queued scan means it
    // already had one pending, so answer busy on the originating transport.
    replies.error(replies.context, request.link, ble::kOpWifiScan,
                  ble::kErrBusy, "scan-pending");
    return true;
  default:
    return false;
  }
}
} // namespace ossync
