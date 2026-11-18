#include "boot_self_test.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace opensensor::selftest {
namespace {
struct Named {
  Check check;
  const char *fail;
  const char *timeout;
};
} // namespace

Outcome decide(const Inputs &inputs) {
  const Named checks[] = {
      {.check = inputs.sd, .fail = "sd", .timeout = "sd-timeout"},
      {.check = inputs.sensor, .fail = "sensor", .timeout = "sensor-timeout"},
      {.check = inputs.radio, .fail = "radio", .timeout = "radio-timeout"},
  };
  const auto first = [&](Check state) {
    return std::ranges::find_if(
        checks, [state](const Named &item) { return item.check == state; });
  };
  if (const auto *failed = first(Check::Fail); failed != std::end(checks))
    return {.verdict = Verdict::Unhealthy, .reason = failed->fail};
  if (const auto *pending = first(Check::Pending); pending != std::end(checks))
    return inputs.elapsed_ms >= inputs.deadline_ms
               ? Outcome{.verdict = Verdict::Unhealthy,
                         .reason = pending->timeout}
               : Outcome{.verdict = Verdict::Pending, .reason = "pending"};
  return {.verdict = Verdict::Healthy, .reason = "ok"};
}

bool should_mark_valid(Verdict verdict, OtaState state) {
  return verdict == Verdict::Healthy &&
         (state == OtaState::New || state == OtaState::PendingVerify ||
          state == OtaState::Undefined);
}

const char *name(Check check) {
  switch (check) {
  case Check::Pending:
    return "pending";
  case Check::Pass:
    return "pass";
  case Check::Fail:
    return "fail";
  }
  return "?";
}

const char *name(Verdict verdict) {
  switch (verdict) {
  case Verdict::Pending:
    return "pending";
  case Verdict::Healthy:
    return "healthy";
  case Verdict::Unhealthy:
    return "unhealthy";
  }
  return "?";
}

const char *name(OtaState state) {
  switch (state) {
  case OtaState::Unknown:
    return "none";
  case OtaState::New:
    return "new";
  case OtaState::PendingVerify:
    return "pending-verify";
  case OtaState::Valid:
    return "valid";
  case OtaState::Invalid:
    return "invalid";
  case OtaState::Aborted:
    return "aborted";
  case OtaState::Undefined:
    return "undefined";
  }
  return "?";
}

const char *name(Mark mark) {
  switch (mark) {
  case Mark::Skipped:
    return "skipped";
  case Mark::Marked:
    return "marked";
  case Mark::Failed:
    return "failed";
  }
  return "?";
}

std::size_t format_line(const Inputs &inputs, const Outcome &outcome,
                        OtaState state, Mark mark, std::span<char> out) {
  if (out.empty())
    return 0;
  const int written = std::snprintf(
      out.data(), out.size(),
      "SELFTEST result=%s reason=%s sd=%s sensor=%s radio=%s elapsed_ms=%lld "
      "ota_state=%s mark_valid=%s\n",
      name(outcome.verdict), outcome.reason, name(inputs.sd),
      name(inputs.sensor), name(inputs.radio),
      static_cast<long long>(inputs.elapsed_ms), name(state), name(mark));
  if (written <= 0 || static_cast<std::size_t>(written) >= out.size()) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<std::size_t>(written);
}

} // namespace opensensor::selftest
