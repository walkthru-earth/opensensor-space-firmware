#include "epaper_ui/panel.h"

namespace opensensor::epd {

// These words reach the serial log and the bench parsers, keep them stable.
const char *to_string(Refresh r) {
  switch (r) {
  case Refresh::Full:
    return "full";
  case Refresh::Fast:
    return "fast";
  case Refresh::Partial:
    return "partial";
  }
  return "unknown";
}

const char *to_string(Error e) {
  switch (e) {
  case Error::NotReady:
    return "not-ready";
  case Error::AlreadyStarted:
    return "already-started";
  case Error::Gpio:
    return "gpio";
  case Error::SpiBus:
    return "spi-bus";
  case Error::SpiDevice:
    return "spi-device";
  case Error::NoMemory:
    return "no-memory";
  case Error::UnsupportedController:
    return "unsupported-controller";
  case Error::BusyTimeout:
    return "busy-timeout";
  case Error::Spi:
    return "spi";
  case Error::BadFrame:
    return "bad-frame";
  case Error::BadWindow:
    return "bad-window";
  case Error::Unsupported:
    return "unsupported";
  }
  return "unknown";
}

} // namespace opensensor::epd
