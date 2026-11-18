#pragma once

// BLE link authentication policy (issue #51). Pure decisions with no host
// stack or RTOS dependency, so ble_sync.cpp and the host tests share them.
//
// NimBLE's security state for the connection is the truth. The transport keeps
// an atomic copy so the response gates need no host lock per frame, and that
// copy used to change only on CONNECT and ENC_CHANGE. On some bonded
// reconnects ENC_CHANGE never reached the GAP callback, so the copy stayed
// false while every GATT access saw a secure link. The first LIST was then
// refused with ERROR 7 and that ERROR was dropped by the same stale gate.
//
// Now every host task place that reads the live state (CONNECT, ENC_CHANGE,
// SUBSCRIBE and each GATT access) latches the copy, and each gate falls back
// to the live state before it refuses. An insecure link is still refused,
// never with 7. NimBLE's ATT permission check refuses it first, with the same
// rule as secure() in this configuration (ENC flags in every mode, AUTHEN and
// sm_sc_only, which also demands a 16 byte key, in the passkey modes). That
// ATT error is what makes the phone pair. ERROR 14 is a guarded fallback in
// case the two checks ever diverge.

#include "ble_sync.h"

#include <cstdint>
#include <span>
#include <utility>

namespace ble {
// One live reading of NimBLE's security state on the host task. True when it
// is the first proof that the current link is secure, so the transport sets
// its flag, logs BLE BONDED once and starts the link sequence.
[[nodiscard]] constexpr bool should_latch(bool current_link, bool cached,
                                          bool secure_now) {
  return current_link && secure_now && !cached;
}

// The ERROR 14 refusal: ERROR type, any op, code kErrAuth. It names a fixed
// protocol code and constant detail, so it carries no secret.
[[nodiscard]] constexpr bool
is_auth_refusal(std::span<const std::uint8_t> frame) {
  return frame.size() >= 3 && frame[0] == kFrameError && frame[2] == kErrAuth;
}

// The response gates' send or refuse decision. A frame goes out when the
// cached flag says the link is authenticated, when it is the ERROR 14 refusal
// (the same way the LAN link answers a bad token before it closes), or else
// when NimBLE's live state says the link is secure. Refusing on the cached
// flag alone is the issue #51 bug.
[[nodiscard]] constexpr bool gate_allows(bool cached, bool auth_error,
                                         bool live_secure) {
  return cached || auth_error || live_secure;
}

// gate_allows() for the transport. `live` runs only when nothing cheaper
// decided, so a healthy link pays no host lock per frame.
template <class Live>
[[nodiscard]] bool may_send(bool cached, std::span<const std::uint8_t> frame,
                            Live &&live) {
  const bool auth_error = is_auth_refusal(frame);
  return gate_allows(cached, auth_error, !cached && !auth_error && live());
}

// ENC_CHANGE after a link was already latched by a via= proof (or a second
// encryption on the same link) changes nothing, so it logs no second BLE
// BONDED line and does not restart the link sequence.
[[nodiscard]] constexpr bool enc_change_announces(bool was_cached,
                                                  bool secure_now) {
  return secure_now && !was_cached;
}

// The ERROR that answers a Control write the transport does not hand to the
// worker. Authentication is decided first, so a link that is not secure gets
// 14 and code 7 only ever means the command queue was full.
struct Refusal {
  Error code;
  const char *detail; // constant, secret free
};
[[nodiscard]] constexpr Refusal control_refusal(bool secure_now) {
  return secure_now ? Refusal{.code = kErrBusy, .detail = "queue-full"}
                    : Refusal{.code = kErrAuth, .detail = "auth"};
}
// Subscriptions that arrive before CONNECT (bench 2026-10-18, Waveshare,
// macOS with a stored bond). NimBLE 6.1 reports a peripheral's CONNECT only
// after its remote version and feature reads finish
// (ble_gap_rx_rd_rem_sup_feat_complete in ble_gap.c), while the central may
// encrypt in between. Then ENC_CHANGE and ble_gatts_bonding_restored() run
// first, and the bonded CCCDs come back as SUBSCRIBE events with reason
// RESTORE for a handle the transport does not know yet. A client with a bond
// never writes those CCCDs again, so dropping the events left `response`
// unsubscribed for the whole link. Every FILE frame was refused and the LIST
// ended ERROR 7 "refused", which could not be delivered either.
//
// What the transport does with one SUBSCRIBE event.
enum class SubscribeRoute : std::uint8_t {
  Apply, // the current link, same as a live CCCD write
  Hold,  // no link yet, its CONNECT is still to come
  Drop,  // another handle while a link is up
};
[[nodiscard]] constexpr SubscribeRoute route_subscribe(bool connected,
                                                       bool current_handle) {
  using enum SubscribeRoute;
  return connected ? (current_handle ? Apply : Drop) : Hold;
}

// The held subscribe state of one not yet connected handle, the last value
// per characteristic. NimBLE has no public read of a link's live CCCD flags,
// and the bond store holds what was persisted, not what the link uses, so the
// events themselves are the record. Host task only, no heap.
struct PendingCccd {
  static constexpr unsigned kMax = 8;
  static constexpr std::uint16_t kNone = 0xffff; // BLE_HS_CONN_HANDLE_NONE

  std::uint16_t held = kNone;
  std::uint8_t seen = 0;
  std::uint8_t notify_bits = 0;

  void hold(std::uint16_t handle, unsigned index, bool notify) {
    if (index >= kMax)
      return;
    // Only one link is pending at a time. A new handle replaces the old one.
    if (handle != held)
      *this = PendingCccd{.held = handle};
    const auto bit = static_cast<std::uint8_t>(1U << index);
    seen |= bit;
    notify_bits = static_cast<std::uint8_t>(notify ? notify_bits | bit
                                                   : notify_bits & ~bit);
  }
  // At CONNECT. Hands each held state for `handle` to apply(index, notify),
  // then forgets everything. Returns how many were applied.
  template <class Apply> unsigned replay(std::uint16_t handle, Apply &&apply) {
    const auto pending = *this;
    clear();
    if (pending.held != handle)
      return 0;
    unsigned applied = 0;
    for (unsigned index = 0; index < kMax; ++index) {
      const auto bit = 1U << index;
      if (pending.seen & bit) {
        apply(index, (pending.notify_bits & bit) != 0);
        ++applied;
      }
    }
    return applied;
  }
  void clear() { *this = PendingCccd{}; }
};

// Why a response gate refused a frame. Disconnected and Stale are the normal
// end of a session. Unsubscribed and Insecure are faults the bench must see,
// so the transport logs them once per session as BLE REFUSE reason=...
// before the worker's ERROR and LIST ABORT lines.
enum class GateRefusal : std::uint8_t {
  None,
  Disconnected,
  Stale,
  Unsubscribed,
  Insecure,
  Deferred, // os-lan only, see response_refusal_now()
};
// `allowed` is may_send_frame() and runs only when nothing cheaper refused.
template <class Allowed>
[[nodiscard]] GateRefusal response_refusal(bool connected, bool current_session,
                                           bool subscribed, Allowed &&allowed) {
  using enum GateRefusal;
  if (!connected)
    return Disconnected;
  if (!current_session)
    return Stale;
  if (!subscribed)
    return Unsubscribed;
  return std::forward<Allowed>(allowed)() ? None : Insecure;
}
// The gate for the os-lan path, which must never wait (#178). NimBLE's live
// state takes the host lock, so this never asks it. A frame the cached flag
// or the ERROR 14 rule allows goes out. Where only the live state could allow
// it, the frame is Deferred, not refused, so a secure link whose flag missed
// ENC_CHANGE is sent once the host task latches the flag on the next access,
// subscribe or ENC_CHANGE (#51). The other refusals end the reply at once.
[[nodiscard]] inline GateRefusal
response_refusal_now(bool connected, bool current_session, bool subscribed,
                     bool cached, std::span<const std::uint8_t> frame) {
  const auto refusal =
      response_refusal(connected, current_session, subscribed, [&] {
        return gate_allows(cached, is_auth_refusal(frame), false);
      });
  return refusal == GateRefusal::Insecure ? GateRefusal::Deferred : refusal;
}
[[nodiscard]] constexpr bool refusal_is_fault(GateRefusal refusal) {
  return refusal == GateRefusal::Unsubscribed ||
         refusal == GateRefusal::Insecure;
}
[[nodiscard]] constexpr const char *refusal_name(GateRefusal refusal) {
  switch (refusal) {
  case GateRefusal::None:
    return "none";
  case GateRefusal::Disconnected:
    return "disconnected";
  case GateRefusal::Stale:
    return "stale";
  case GateRefusal::Unsubscribed:
    return "unsubscribed";
  case GateRefusal::Insecure:
    return "insecure";
  case GateRefusal::Deferred:
    return "deferred";
  }
  return "unknown";
}
} // namespace ble
