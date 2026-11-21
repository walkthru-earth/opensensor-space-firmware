#include "abort_signal.h"
#include "archive_sync.h"
#include "os_logger_list_job.h"
#include "os_logger_transfer_gate.h"
#include "storage_governor.h"
#include "sync_codec.h"

#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace opensensor::sync;
std::size_t slot(ble::Link link, std::uint8_t peer = 0) {
  return link == ble::Link::Ble ? 0 : 1 + peer;
}
// abort::sequences and abort::stopped are process-wide; every ABORT case
// starts from a clean slate so earlier cases cannot leak a pending abort.
void reset_abort() {
  for (auto &sequence : ossync::abort::sequences)
    sequence.store(0);
  for (auto &entry : ossync::abort::stopped)
    entry = ossync::abort::Stopped{};
}
// What a transport task does when an ABORT arrives on (link, peer).
ble::ControlRequest stamp_abort(ble::Link link, std::uint8_t peer,
                                std::uint32_t generation) {
  ble::ControlRequest marker;
  marker.link = link;
  marker.peer = peer;
  marker.link_generation = generation;
  marker.bytes[0] = ble::kOpAbort;
  marker.length = 1;
  ossync::abort::stamp(marker);
  ossync::abort::commit(marker);
  return marker;
}
unsigned descriptor_count() {
  unsigned count = 0;
  for (int descriptor = 0; descriptor < 1024; ++descriptor)
    if (::fcntl(descriptor, F_GETFD) >= 0)
      ++count;
  return count;
}
// Fills every descriptor below the highest one in use and lowers the soft
// limit to it, so the next open fails with EMFILE until restore().
struct DescriptorLimit {
  rlimit original{};
  std::vector<int> fillers;
  DescriptorLimit() {
    assert(::getrlimit(RLIMIT_NOFILE, &original) == 0);
    int highest = 0;
    for (int descriptor = 0; descriptor < 1024; ++descriptor)
      if (::fcntl(descriptor, F_GETFD) >= 0)
        highest = descriptor;
    for (int descriptor = 0; descriptor < highest; ++descriptor)
      if (::fcntl(descriptor, F_GETFD) < 0) {
        assert(::dup2(0, descriptor) == descriptor);
        fillers.push_back(descriptor);
      }
    rlimit lowered = original;
    lowered.rlim_cur = static_cast<rlim_t>(highest + 1);
    assert(::setrlimit(RLIMIT_NOFILE, &lowered) == 0);
  }
  ~DescriptorLimit() {
    assert(::setrlimit(RLIMIT_NOFILE, &original) == 0);
    for (const int descriptor : fillers)
      ::close(descriptor);
  }
};
struct Frame {
  ble::Link link;
  std::uint8_t peer;
  std::vector<std::uint8_t> bytes;
};
struct Error {
  ble::Link link;
  std::uint8_t peer;
  ble::Op op;
  ble::Error code;
  std::string detail;
};
struct Fixture {
  std::string directory;
  std::vector<std::uint8_t> bytes;
  std::vector<Frame> frames;
  std::vector<Error> errors;
  std::array<std::uint32_t, 1 + ble::kMaxLanClients> generation{1, 1, 1, 1};
  std::array<std::uint16_t, 1 + ble::kMaxLanClients> payload{517, 1024, 1024,
                                                             1024};
  std::vector<std::string> names{"sample.parquet"};
  unsigned partials = 70000;
  unsigned list_repeats = 1;
  unsigned listed = 0, progress = 0, locks = 0, lock_depth = 0;
  unsigned checks = 0, buffer_requests = 0;
  std::int64_t clock_us = 0;
  std::array<std::array<char, ossync::ArchiveSession::kReadBufferBytes>,
             ossync::ArchiveSession::kReadHandles>
      buffers{};
  bool complete = true;
  bool cache_hit = false;
  // Makes the finalized hook fail as if a read hit an I/O error.
  bool check_io_error = false;
  bool listing_complete = true;
  // Resumable fake walk over list_repeats x names. list_open is true while
  // the walk holds its directories between steps.
  std::size_t list_cursor = 0, list_reads = 0, list_steps = 0;
  std::size_t list_cancels = 0, list_budget_max = 0;
  bool list_open = false;
  // Runs between steps, as the storage worker's sample pass does.
  void (*between_steps)(Fixture &) = nullptr;
  // The preempted hook reports true while preempt is set, or once this many
  // frames were delivered (0 never).
  bool preempt = false;
  std::size_t preempt_frame = 0;
  std::size_t refuse_frame = 0;
  std::size_t disconnect_frame = 0;
  // Stamp an ABORT on (abort_link, abort_peer) as this frame is delivered.
  std::size_t abort_frame = 0;
  ble::Link abort_link = ble::Link::Ble;
  std::uint8_t abort_peer = 0;
  std::vector<std::string> logs;
  // LIST_SINCE hook. Records the cursor and answers since_result.
  ossync::ListCursor since_cursor{};
  ossync::ListSinceResult since_result{};
  unsigned since_begins = 0;
  // The logger's battery close gate, run in front of the transport as the
  // firmware deliver hook does. The intent is raised once raise_close_after
  // frames were delivered, so 0 raises it before the first frame.
  oslogger::power::EmergencyClose *close = nullptr;
  std::size_t raise_close_after = 0;

  explicit Fixture(const char *path) : directory(path), bytes(20000) {
    for (std::size_t i = 0; i < bytes.size(); ++i)
      bytes[i] = static_cast<std::uint8_t>(i);
    std::memcpy(bytes.data(), "PAR1", 4);
    put_u32(bytes.data() + bytes.size() - 8, 32);
    std::memcpy(bytes.data() + bytes.size() - 4, "PAR1", 4);
    FILE *file = std::fopen((directory + "/sample.parquet").c_str(), "wb");
    assert(file);
    assert(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    assert(std::fclose(file) == 0);
  }
  void clear() {
    frames.clear();
    errors.clear();
    refuse_frame = 0;
    disconnect_frame = 0;
    abort_frame = 0;
    abort_link = ble::Link::Ble;
    abort_peer = 0;
    logs.clear();
  }
  // The fake walk behind both LIST and LIST_SINCE.
  static ossync::ListStatus walk(void *ctx, bool begin, unsigned budget,
                                 ossync::FileEmitter emit, void *out,
                                 unsigned &partials) {
    auto &f = *static_cast<Fixture *>(ctx);
    assert(budget > 0);
    ++f.list_steps;
    if (budget > f.list_budget_max)
      f.list_budget_max = budget;
    if (begin) {
      f.list_cursor = 0;
      f.list_open = true;
    }
    assert(f.list_open);
    const std::size_t total = f.list_repeats * f.names.size();
    for (unsigned read = 0; read < budget; ++read) {
      if (f.list_cursor == total) {
        f.list_open = false;
        if (!f.listing_complete)
          return ossync::ListStatus::Failed;
        partials += f.partials;
        return ossync::ListStatus::Complete;
      }
      ++f.list_reads;
      const auto &name = f.names[f.list_cursor++ % f.names.size()];
      if (!emit(out, name.c_str(), f.bytes.size())) {
        f.list_open = false;
        return ossync::ListStatus::Stopped;
      }
    }
    return ossync::ListStatus::More;
  }
  ossync::ArchiveHooks archive() {
    return {
        this, &Fixture::walk,
        [](void *ctx, const char *name, char *path, std::size_t size) {
          auto &f = *static_cast<Fixture *>(ctx);
          if (!safe_parquet_name(name))
            return false;
          const int n =
              std::snprintf(path, size, "%s/%s", f.directory.c_str(), name);
          return n > 0 && static_cast<std::size_t>(n) < size;
        },
        // Reads the stream OPEN handed over, as the device owner does on a
        // cache miss. The session must rewind it before READ.
        [](void *ctx, const char *, FILE *file, ossync::FileCheck &check) {
          auto &f = *static_cast<Fixture *>(ctx);
          assert(f.lock_depth == 0 && file);
          ++f.checks;
          std::vector<std::uint8_t> data(f.bytes.size() + 1);
          const std::size_t count =
              std::fread(data.data(), 1, data.size(), file);
          check.size = static_cast<std::uint32_t>(count);
          check.crc =
              crc32_update(0xffffffffU, data.data(), count) ^ 0xffffffffU;
          check.cached = f.cache_hit;
          if (f.check_io_error) {
            check.io_error = true;
            return false;
          }
          return f.complete && count >= 12 &&
                 parquet_complete(count, data.data(), data.data() + count - 8);
        },
        [](void *) { return std::uint32_t(123); },
        [](void *) { return std::uint32_t(45); },
        [](void *ctx) { ++static_cast<Fixture *>(ctx)->listed; },
        [](void *ctx) {
          auto &f = *static_cast<Fixture *>(ctx);
          assert(f.lock_depth++ == 0);
          ++f.locks;
        },
        [](void *ctx) {
          auto &f = *static_cast<Fixture *>(ctx);
          assert(f.lock_depth-- == 1);
        },
        [](void *ctx, const char *line) {
          static_cast<Fixture *>(ctx)->logs.push_back(line);
        },
        [](void *ctx, std::size_t index) -> void * {
          auto &f = *static_cast<Fixture *>(ctx);
          assert(index < f.buffers.size() && f.lock_depth == 1);
          ++f.buffer_requests;
          return f.buffers[index].data();
        },
        [](void *ctx) { return static_cast<Fixture *>(ctx)->clock_us += 250; },
        [](void *ctx) {
          auto &f = *static_cast<Fixture *>(ctx);
          ++f.list_cancels;
          f.list_open = false;
        },
        [](void *ctx) {
          const auto &f = *static_cast<Fixture *>(ctx);
          // The firmware hook reads the battery close, as the gate does.
          return f.preempt ||
                 (f.preempt_frame && f.frames.size() >= f.preempt_frame) ||
                 (f.close && f.close->preempt(0));
        },
        [](void *ctx, bool begin, const ossync::ListCursor &cursor,
           ossync::ListSinceResult &result, unsigned budget,
           ossync::FileEmitter emit, void *out, unsigned &partials) {
          auto &f = *static_cast<Fixture *>(ctx);
          if (begin) {
            ++f.since_begins;
            f.since_cursor = cursor;
            result = f.since_result;
          }
          return walk(ctx, begin, budget, emit, out, partials);
        }};
  }
  ossync::ArchiveTransport transport() {
    return {this,
            [](void *ctx, ble::Link link, std::uint8_t peer) {
              return static_cast<Fixture *>(ctx)->generation[slot(link, peer)];
            },
            [](void *ctx, ble::Link link, std::uint8_t peer) {
              return static_cast<Fixture *>(ctx)->payload[slot(link, peer)];
            },
            [](void *ctx, ble::Link link, std::uint8_t peer,
               std::uint32_t generation, const std::uint8_t *data,
               std::size_t size) {
              auto &f = *static_cast<Fixture *>(ctx);
              if (f.close && f.frames.size() == f.raise_close_after)
                f.close->request();
              if (f.close &&
                  oslogger::power::refuses_frame(*f.close, data, size, 0))
                return false;
              if (generation != f.generation[slot(link, peer)])
                return false;
              f.frames.push_back({link, peer, {data, data + size}});
              if (f.disconnect_frame && f.frames.size() == f.disconnect_frame)
                ++f.generation[slot(link, peer)];
              if (f.abort_frame && f.frames.size() == f.abort_frame)
                stamp_abort(f.abort_link, f.abort_peer,
                            f.generation[slot(f.abort_link, f.abort_peer)]);
              return !f.refuse_frame || f.frames.size() != f.refuse_frame;
            },
            [](void *ctx, ble::Link link, std::uint8_t peer,
               std::uint32_t generation, ble::Op op, ble::Error error,
               const char *detail) {
              auto &f = *static_cast<Fixture *>(ctx);
              assert(3 + (detail ? std::strlen(detail) : 0) <=
                     f.payload[slot(link, peer)]);
              if (generation != f.generation[slot(link, peer)])
                return false;
              f.errors.push_back({link, peer, op, error, detail ? detail : ""});
              return true;
            },
            [](void *ctx) { ++static_cast<Fixture *>(ctx)->progress; }};
  }
};
// Handles a request as the storage worker does, then runs a LIST job it
// started to its end, one bounded step per worker pass.
bool serve(ossync::ArchiveSession &session, const ble::ControlRequest &r,
           bool storage_ready, Fixture *f = nullptr) {
  const bool handled = session.handle(r, storage_ready);
  while (session.listing()) {
    session.step(storage_ready);
    if (f && f->between_steps)
      f->between_steps(*f);
  }
  return handled;
}
ble::ControlRequest request(Fixture &f, ble::Op op,
                            ble::Link link = ble::Link::Ble,
                            std::uint8_t peer = 0) {
  ble::ControlRequest r;
  r.link = link;
  r.peer = peer;
  r.link_generation = f.generation[slot(link, peer)];
  r.bytes[0] = op;
  r.length = 1;
  return r;
}
std::uint16_t open(Fixture &f, ossync::ArchiveSession &session,
                   ble::Link link = ble::Link::Ble,
                   const char *name = "sample.parquet", std::uint8_t peer = 0) {
  f.clear();
  auto r = request(f, ble::kOpOpen, link, peer);
  std::memcpy(r.bytes + 1, name, std::strlen(name));
  r.length += std::strlen(name);
  assert(session.handle(r, true));
  assert(f.errors.empty());
  assert(f.frames.size() == 1 && f.frames[0].link == link &&
         f.frames[0].peer == peer);
  const auto &frame = f.frames[0].bytes;
  assert(frame[0] == ble::kFrameOpened &&
         frame.size() == 11 + std::strlen(name));
  assert(get_u32(frame.data() + 3) == f.bytes.size());
  assert(
      get_u32(frame.data() + 7) ==
      (crc_update(0xffffffffU, f.bytes.data(), f.bytes.size()) ^ 0xffffffffU));
  assert(std::memcmp(frame.data() + 11, name, std::strlen(name)) == 0);
  return get_u16(frame.data() + 1);
}
ble::ControlRequest read(Fixture &f, std::uint16_t handle, std::uint32_t offset,
                         std::uint32_t count, ble::Link link = ble::Link::Ble,
                         std::uint8_t peer = 0) {
  auto r = request(f, ble::kOpRead, link, peer);
  r.length = 11;
  put_u16(r.bytes + 1, handle);
  put_u32(r.bytes + 3, offset);
  put_u32(r.bytes + 7, count);
  return r;
}
void expect_error(const Fixture &f, ble::Error code,
                  ble::Link link = ble::Link::Ble, std::uint8_t peer = 0) {
  assert(f.errors.size() == 1 && f.errors.back().code == code &&
         f.errors.back().link == link && f.errors.back().peer == peer);
  assert(f.frames.empty());
}
void verify_read(Fixture &f, std::uint16_t handle, std::uint32_t offset,
                 std::uint32_t count, std::size_t max_frame,
                 ble::Link link = ble::Link::Ble, std::uint8_t peer = 0) {
  assert(f.errors.empty());
  std::vector<std::uint8_t> received;
  for (std::size_t i = 0; i + 1 < f.frames.size(); ++i) {
    const auto &frame = f.frames[i];
    assert(frame.link == link && frame.peer == peer &&
           frame.bytes.size() <= max_frame);
    assert(frame.bytes[0] == ble::kFrameChunk);
    assert(get_u16(frame.bytes.data() + 1) == handle);
    assert(get_u32(frame.bytes.data() + 3) == offset + received.size());
    received.insert(received.end(), frame.bytes.begin() + 7, frame.bytes.end());
  }
  assert(received.size() == count);
  assert(std::memcmp(received.data(), f.bytes.data() + offset, count) == 0);
  const auto &end = f.frames.back();
  assert(end.link == link && end.peer == peer && end.bytes.size() == 8 &&
         end.bytes[0] == ble::kFrameReadEnd && end.bytes[7] == 0);
  assert(get_u16(end.bytes.data() + 1) == handle);
  assert(get_u32(end.bytes.data() + 3) == offset + count);
}

// ABORT (abort_signal.h): transports stamp each request, an ABORT advances the
// link's sequence, and the worker stops what the ABORT overtook.
void abort_cases(Fixture &f, ossync::ArchiveSession &session) {
  namespace abort = ossync::abort;
  const auto stamped = [&](ble::ControlRequest r) {
    abort::stamp(r);
    abort::commit(r);
    return r;
  };
  const auto expect_aborted = [&](ble::Op op, ble::Link link,
                                  std::uint8_t peer) {
    assert(f.errors.size() == 1 && f.errors[0].op == op &&
           f.errors[0].code == ble::kErrAborted && f.errors[0].link == link &&
           f.errors[0].peer == peer && f.errors[0].detail.empty());
  };
  f.payload = {517, 1024, 1024, 1024};
  f.list_repeats = 5;
  const unsigned listed = f.listed;

  // A LIST queued before an ABORT on its link never starts: ERROR aborted
  // only, then the marker names LIST once and a second marker names nothing.
  reset_abort();
  f.clear();
  const auto queued = stamped(request(f, ble::kOpList));
  stamp_abort(ble::Link::Ble, 0, f.generation[0]);
  assert(serve(session, queued, true));
  expect_aborted(ble::kOpList, ble::Link::Ble, 0);
  assert(f.frames.empty() && f.logs.empty() && f.listed == listed);
  auto marker = stamp_abort(ble::Link::Ble, 0, f.generation[0]);
  assert(abort::take_stopped(marker) == ble::kOpList);
  assert(abort::take_stopped(marker) == 0);

  // ABORT mid LIST: FILE frames stop, ERROR aborted and no LIST_END.
  for (const auto link : {ble::Link::Ble, ble::Link::Lan}) {
    reset_abort();
    f.clear();
    f.abort_frame = 2;
    f.abort_link = link;
    serve(session, stamped(request(f, ble::kOpList, link)), true);
    assert(f.frames.size() == 2);
    for (const auto &frame : f.frames)
      assert(frame.bytes[0] == ble::kFrameFile);
    expect_aborted(ble::kOpList, link, 0);
    assert(f.listed == listed);
    assert(f.logs.size() == 1);
    assert(f.logs[0].find("LIST ABORT") != std::string::npos &&
           f.logs[0].find("reason=aborted") != std::string::npos &&
           f.logs[0].find("after=2") != std::string::npos &&
           f.logs[0].find(link == ble::Link::Lan ? "link=lan" : "link=ble") !=
               std::string::npos);
    marker = stamp_abort(link, 0, f.generation[slot(link)]);
    assert(abort::take_stopped(marker) == ble::kOpList);
  }
  // The transport refusing the frame the ABORT overtook is still aborted, not
  // busy.
  reset_abort();
  f.clear();
  f.abort_frame = f.refuse_frame = 3;
  serve(session, stamped(request(f, ble::kOpList)), true);
  assert(f.frames.size() == 3);
  expect_aborted(ble::kOpList, ble::Link::Ble, 0);
  assert(f.logs.size() == 1 &&
         f.logs[0].find("after=2 reason=aborted") != std::string::npos);

  // ABORT mid READ: CHUNKs stop, ERROR aborted and no READ_END, both when the
  // overtaken frame was delivered and when the transport refused it.
  reset_abort();
  const auto handle = open(f, session, ble::Link::Lan, "sample.parquet", 2);
  for (const std::size_t refuse : {std::size_t(0), std::size_t(2)}) {
    reset_abort();
    f.clear();
    f.abort_frame = 2;
    f.refuse_frame = refuse;
    f.abort_link = ble::Link::Lan;
    f.abort_peer = 2;
    serve(session, stamped(read(f, handle, 0, 8000, ble::Link::Lan, 2)), true);
    assert(f.frames.size() == 2);
    for (const auto &frame : f.frames)
      assert(frame.bytes[0] == ble::kFrameChunk && frame.peer == 2);
    expect_aborted(ble::kOpRead, ble::Link::Lan, 2);
    marker =
        stamp_abort(ble::Link::Lan, 2, f.generation[slot(ble::Link::Lan, 2)]);
    assert(abort::take_stopped(marker) == ble::kOpRead);
  }
  // A READ queued before the ABORT never sends a CHUNK.
  reset_abort();
  f.clear();
  const auto queued_read = stamped(read(f, handle, 0, 100, ble::Link::Lan, 2));
  stamp_abort(ble::Link::Lan, 2, f.generation[slot(ble::Link::Lan, 2)]);
  serve(session, queued_read, true);
  expect_aborted(ble::kOpRead, ble::Link::Lan, 2);
  assert(f.frames.empty());
  // The handle survives the ABORT; a fresh READ after it runs normally.
  f.clear();
  serve(session, stamped(read(f, handle, 10, 3000, ble::Link::Lan, 2)), true);
  verify_read(f, handle, 10, 3000, 1024, ble::Link::Lan, 2);

  // Stale marker: the stop was recorded under generation g, the marker
  // arrives for the replacement connection g+1 and names nothing.
  reset_abort();
  f.clear();
  const auto old_list = stamped(request(f, ble::kOpList, ble::Link::Lan, 1));
  stamp_abort(ble::Link::Lan, 1, f.generation[slot(ble::Link::Lan, 1)]);
  serve(session, old_list, true);
  expect_aborted(ble::kOpList, ble::Link::Lan, 1);
  marker =
      stamp_abort(ble::Link::Lan, 1, f.generation[slot(ble::Link::Lan, 1)] + 1);
  assert(abort::take_stopped(marker) == 0);

  // Abort slots are per link and per LAN peer: an ABORT on BLE or on another
  // LAN peer neither stops nor is named by this peer's LIST and READ.
  reset_abort();
  f.clear();
  const auto other_list = stamped(request(f, ble::kOpList, ble::Link::Lan, 2));
  stamp_abort(ble::Link::Ble, 0, f.generation[0]);
  stamp_abort(ble::Link::Lan, 0, f.generation[slot(ble::Link::Lan, 0)]);
  stamp_abort(ble::Link::Lan, 1, f.generation[slot(ble::Link::Lan, 1)]);
  serve(session, other_list, true);
  assert(f.errors.empty() && f.frames.size() == 6 &&
         f.frames.back().bytes[0] == ble::kFrameListEnd &&
         get_u32(f.frames.back().bytes.data() + 1) == 5);
  f.clear();
  f.abort_frame = 1;
  f.abort_link = ble::Link::Lan;
  f.abort_peer = 1;
  serve(session, stamped(read(f, handle, 0, 3000, ble::Link::Lan, 2)), true);
  verify_read(f, handle, 0, 3000, 1024, ble::Link::Lan, 2);
  f.clear();
  f.abort_frame = 1;
  f.abort_link = ble::Link::Ble;
  serve(session, stamped(request(f, ble::kOpList, ble::Link::Lan, 2)), true);
  assert(f.errors.empty() && f.frames.back().bytes[0] == ble::kFrameListEnd);
  for (std::uint8_t peer = 0; peer < 3; ++peer) {
    const auto link = peer ? ble::Link::Lan : ble::Link::Ble;
    const auto other = peer ? std::uint8_t(peer - 1) : std::uint8_t(0);
    marker = stamp_abort(link, other, f.generation[slot(link, other)]);
    assert(abort::take_stopped(marker) == 0);
  }
  reset_abort();
  f.clear();
  f.list_repeats = 1;
}
// READ windows reuse the descriptor OPEN created, release_files() drops
// descriptors but keeps handles, and descriptor exhaustion is OpenFailed.
void persistent_cases(Fixture &f, ossync::ArchiveSession &session,
                      unsigned idle_descriptors) {
  const std::string path = f.directory + "/sample.parquet";
  const std::string moved = f.directory + "/moved.parquet";
  const unsigned checks = f.checks, buffers = f.buffer_requests;
  const auto lan = open(f, session, ble::Link::Lan, "sample.parquet", 1);
  const auto &handle = lan;
  assert(f.checks == checks + 1 && f.buffer_requests == buffers + 1);
  assert(f.logs.size() == 1 &&
         f.logs[0].find("crc=miss open_us=250 ") != std::string::npos);
  // With the path gone, READ still serves every window from the open stream.
  assert(std::rename(path.c_str(), moved.c_str()) == 0);
  for (std::uint32_t offset = 0; offset < f.bytes.size(); offset += 4096) {
    const std::uint32_t count =
        f.bytes.size() - offset < 4096 ? f.bytes.size() - offset : 4096;
    f.clear();
    serve(session, read(f, handle, offset, count, ble::Link::Lan, 1), true);
    verify_read(f, handle, offset, count, 1024, ble::Link::Lan, 1);
    assert(descriptor_count() == idle_descriptors + 1);
  }
  // release_files() closes the stream; a READ while the path is gone fails
  // and the handle stays valid for a retry once the path is back.
  session.release_files();
  assert(descriptor_count() == idle_descriptors);
  f.clear();
  serve(session, read(f, handle, 0, 10, ble::Link::Lan, 1), true);
  assert(f.errors.empty() && f.frames.size() == 1 &&
         f.frames[0].bytes[0] == ble::kFrameReadEnd &&
         f.frames[0].bytes[7] == ble::kErrOpenFailed);
  assert(descriptor_count() == idle_descriptors);
  assert(std::rename(moved.c_str(), path.c_str()) == 0);
  f.clear();
  serve(session, read(f, handle, 333, 1500, ble::Link::Lan, 1), true);
  verify_read(f, handle, 333, 1500, 1024, ble::Link::Lan, 1);
  assert(descriptor_count() == idle_descriptors + 1);
  assert(f.checks == checks + 1 && f.buffer_requests == buffers + 2);

  // A cached CRC is reported in the OPEN log line.
  f.cache_hit = true;
  open(f, session);
  assert(f.logs.size() == 1 &&
         f.logs[0].find("crc=hit open_us=250 ") != std::string::npos);
  f.cache_hit = false;
  assert(descriptor_count() == idle_descriptors + 2);

  // A failed check and a missing path leave no descriptor behind.
  f.clear();
  f.complete = false;
  auto rejected = request(f, ble::kOpOpen);
  const char *name = "sample.parquet";
  std::memcpy(rejected.bytes + 1, name, std::strlen(name));
  rejected.length += std::strlen(name);
  serve(session, rejected, true);
  expect_error(f, ble::kErrNotFinalized);
  f.complete = true;
  assert(descriptor_count() == idle_descriptors + 1);
  f.clear();
  const unsigned before_missing = f.checks;
  auto missing = request(f, ble::kOpOpen);
  const char *absent = "absent.parquet";
  std::memcpy(missing.bytes + 1, absent, std::strlen(absent));
  missing.length += std::strlen(absent);
  serve(session, missing, true);
  expect_error(f, ble::kErrNotFinalized);
  assert(f.checks == before_missing);
  assert(descriptor_count() == idle_descriptors + 1);

  // A full descriptor table is OpenFailed, not NotFinalized, and the open
  // LAN handle keeps reading through its existing descriptor.
  {
    DescriptorLimit limit;
    f.clear();
    serve(session, rejected, true);
    expect_error(f, ble::kErrOpenFailed);
    assert(f.logs.size() == 1 && f.logs[0].find("BLE OPEN FAIL errno=") == 0);
    f.clear();
    serve(session, read(f, lan, 4000, 2000, ble::Link::Lan, 1), true);
    verify_read(f, lan, 4000, 2000, 1024, ble::Link::Lan, 1);
  }
  session.close();
  assert(descriptor_count() == idle_descriptors);
}
// remounted() closes every stream and the next READ on each handle checks
// the reopened file again once mounted() follows. Same bytes keep serving. A
// proven change is ERROR bad handle and the handle is gone. An unmounted
// volume or an I/O error is READ_END status 4 and the recheck stays pending.
void write_file(const std::string &path,
                const std::vector<std::uint8_t> &data) {
  FILE *file = std::fopen(path.c_str(), "wb");
  assert(file);
  assert(std::fwrite(data.data(), 1, data.size(), file) == data.size());
  assert(std::fclose(file) == 0);
}
std::vector<std::uint8_t> other_parquet(std::size_t size) {
  std::vector<std::uint8_t> data(size, 0x5a);
  std::memcpy(data.data(), "PAR1", 4);
  put_u32(data.data() + size - 8, 32);
  std::memcpy(data.data() + size - 4, "PAR1", 4);
  return data;
}
void expect_bad_handle_read(Fixture &f, ossync::ArchiveSession &session,
                            std::uint16_t handle) {
  f.clear();
  serve(session, read(f, handle, 0, 10), true);
  assert(f.errors.size() == 1 && f.errors[0].op == ble::kOpRead);
  expect_error(f, ble::kErrBadHandle);
}
// A remount the board completed, as probe_storage reports it.
void remount(ossync::ArchiveSession &session) {
  session.remounted();
  session.mounted();
}
// READ_END status 4 at offset with no CHUNK, the retryable open failure.
void expect_open_failed_read(Fixture &f, ossync::ArchiveSession &session,
                             std::uint16_t handle, std::uint32_t offset) {
  f.clear();
  serve(session, read(f, handle, offset, 10), true);
  assert(f.errors.empty() && f.frames.size() == 1);
  const auto &end = f.frames[0].bytes;
  assert(end.size() == 8 && end[0] == ble::kFrameReadEnd &&
         get_u16(end.data() + 1) == handle &&
         get_u32(end.data() + 3) == offset && end[7] == ble::kErrOpenFailed);
}
void remount_cases(Fixture &f, ossync::ArchiveSession &session,
                   unsigned idle_descriptors) {
  const std::string path = f.directory + "/sample.parquet";
  unsigned checks = f.checks;

  // Identical bytes after a remount: one check, then plain descriptor reuse.
  auto handle = open(f, session);
  assert(f.checks == ++checks);
  remount(session);
  assert(descriptor_count() == idle_descriptors);
  f.clear();
  serve(session, read(f, handle, 100, 3000), true);
  verify_read(f, handle, 100, 3000, 512);
  assert(f.checks == ++checks);
  assert(f.logs.size() == 2 &&
         f.logs[0].find("BLE READ RECHECK handle=") == 0 &&
         f.logs[0].find(" result=same check_us=") != std::string::npos);
  f.clear();
  serve(session, read(f, handle, 5000, 2000), true);
  verify_read(f, handle, 5000, 2000, 512);
  assert(f.checks == checks);
  // A later release_files() on the same mount reopens without a check.
  session.release_files();
  f.clear();
  serve(session, read(f, handle, 0, 10), true);
  verify_read(f, handle, 0, 10, 512);
  assert(f.checks == checks && descriptor_count() == idle_descriptors + 1);

  // Repeated remounts need one check, and each new remount needs another.
  // The flag has no counter, so no epoch can wrap back to a match.
  for (int round = 0; round < 2; ++round) {
    session.remounted();
    session.remounted();
    remount(session);
    f.clear();
    serve(session, read(f, handle, 64, 64), true);
    verify_read(f, handle, 64, 64, 512);
    assert(f.checks == ++checks);
  }

  // A full descriptor table after a remount is a retryable READ_END, and the
  // check still runs once the file opens again.
  remount(session);
  {
    DescriptorLimit limit;
    f.clear();
    serve(session, read(f, handle, 0, 10), true);
    assert(f.errors.empty() && f.frames.size() == 1 &&
           f.frames[0].bytes[0] == ble::kFrameReadEnd &&
           f.frames[0].bytes[7] == ble::kErrOpenFailed);
    assert(f.checks == checks);
  }
  f.clear();
  serve(session, read(f, handle, 0, 10), true);
  verify_read(f, handle, 0, 10, 512);
  assert(f.checks == ++checks);

  // Different bytes and size at the same path drop the handle.
  remount(session);
  write_file(path, other_parquet(f.bytes.size() + 777));
  expect_bad_handle_read(f, session, handle);
  assert(f.checks == ++checks);
  assert(f.logs.size() == 1 &&
         f.logs[0].find(" result=changed ") != std::string::npos);
  assert(descriptor_count() == idle_descriptors);
  expect_bad_handle_read(f, session, handle); // gone, not retried
  assert(f.checks == checks);
  f.clear();
  auto close_request = request(f, ble::kOpClose);
  close_request.length = 3;
  put_u16(close_request.bytes + 1, handle);
  serve(session, close_request, true);
  expect_error(f, ble::kErrBadHandle);
  write_file(path, f.bytes);

  // Same size but different bytes is a CRC mismatch, also bad handle.
  handle = open(f, session);
  assert(f.checks == ++checks);
  remount(session);
  write_file(path, other_parquet(f.bytes.size()));
  expect_bad_handle_read(f, session, handle);
  assert(f.checks == ++checks);
  write_file(path, f.bytes);

  // A file that is no longer finalized drops the handle.
  handle = open(f, session);
  assert(f.checks == ++checks);
  remount(session);
  f.complete = false;
  expect_bad_handle_read(f, session, handle);
  assert(f.checks == ++checks);
  f.complete = true;

  // A file deleted while the card was out drops the handle without a check.
  handle = open(f, session);
  assert(f.checks == ++checks);
  remount(session);
  assert(std::remove(path.c_str()) == 0);
  expect_bad_handle_read(f, session, handle);
  assert(f.checks == checks && descriptor_count() == idle_descriptors);
  write_file(path, f.bytes);

  // The other slots are checked independently, a fresh OPEN needs no recheck.
  const auto lan = open(f, session, ble::Link::Lan, "sample.parquet", 2);
  handle = open(f, session);
  checks += 2;
  remount(session);
  handle = open(f, session);
  assert(f.checks == ++checks);
  f.clear();
  serve(session, read(f, handle, 0, 10), true);
  verify_read(f, handle, 0, 10, 512);
  assert(f.checks == checks);
  f.clear();
  serve(session, read(f, lan, 0, 10, ble::Link::Lan, 2), true);
  verify_read(f, lan, 0, 10, 1024, ble::Link::Lan, 2);
  assert(f.checks == ++checks);
  session.close();
  assert(descriptor_count() == idle_descriptors);

  // CLOSE on a handle still waiting for its recheck answers CLOSED and frees
  // the slot without a check.
  handle = open(f, session);
  assert(f.checks == ++checks);
  remount(session);
  f.clear();
  close_request = request(f, ble::kOpClose);
  close_request.length = 3;
  put_u16(close_request.bytes + 1, handle);
  serve(session, close_request, true);
  assert(f.errors.empty() && f.frames.size() == 1 &&
         f.frames[0].bytes[0] == ble::kFrameClosed &&
         get_u16(f.frames[0].bytes.data() + 1) == handle);
  assert(f.checks == checks && descriptor_count() == idle_descriptors);
  expect_bad_handle_read(f, session, handle);
  assert(f.checks == checks);

  // A generation change while a recheck is pending clears the slot. The old
  // handle answers bad handle without a check and a fresh OPEN needs none.
  handle = open(f, session);
  assert(f.checks == ++checks);
  remount(session);
  ++f.generation[slot(ble::Link::Ble)];
  expect_bad_handle_read(f, session, handle);
  assert(f.checks == checks && descriptor_count() == idle_descriptors);
  handle = open(f, session);
  assert(f.checks == ++checks);
  f.clear();
  serve(session, read(f, handle, 0, 10), true);
  verify_read(f, handle, 0, 10, 512);
  assert(f.checks == checks);

  // A volume that did not mount again answers every path as missing, so the
  // file is moved away here too. READ is READ_END status 4 without opening
  // anything, and the recheck waits, also across another failed remount.
  const std::string away = path + ".away";
  session.remounted();
  assert(std::rename(path.c_str(), away.c_str()) == 0);
  expect_open_failed_read(f, session, handle, 100);
  expect_open_failed_read(f, session, handle, 100);
  session.remounted();
  expect_open_failed_read(f, session, handle, 100);
  assert(f.checks == checks && descriptor_count() == idle_descriptors);
  // The same card mounts again unchanged and the handle serves.
  assert(std::rename(away.c_str(), path.c_str()) == 0);
  session.mounted();
  f.clear();
  serve(session, read(f, handle, 100, 3000), true);
  verify_read(f, handle, 100, 3000, 512);
  assert(f.checks == ++checks);

  // An I/O error during the recheck is READ_END status 4. The handle stays,
  // each retry checks again, and a clean check serves.
  remount(session);
  f.check_io_error = true;
  expect_open_failed_read(f, session, handle, 0);
  assert(f.checks == ++checks && descriptor_count() == idle_descriptors);
  assert(f.logs.size() == 1 &&
         f.logs[0].find(" result=io_error check_us=") != std::string::npos);
  expect_open_failed_read(f, session, handle, 0);
  assert(f.checks == ++checks);
  f.check_io_error = false;
  f.clear();
  serve(session, read(f, handle, 0, 10), true);
  verify_read(f, handle, 0, 10, 512);
  assert(f.checks == ++checks);

  // A refused CHUNK, as a critical battery close preempts it, comes after
  // the recheck. A same result is proof, so the handle keeps serving and the
  // retry needs no second check.
  remount(session);
  f.clear();
  f.refuse_frame = 1;
  session.handle(read(f, handle, 0, 3000), true);
  assert(f.checks == ++checks && f.errors.empty() && f.frames.size() == 2);
  {
    const auto &end = f.frames[1].bytes;
    assert(end.size() == 8 && end[0] == ble::kFrameReadEnd &&
           get_u32(end.data() + 3) == 0 && end[7] == ble::kErrBusy);
  }
  f.clear();
  session.handle(read(f, handle, 0, 3000), true);
  verify_read(f, handle, 0, 3000, 512);
  assert(f.checks == checks);
  // A change proven before the first CHUNK ends the handle with ERROR bad
  // handle, a frame the preempt never refuses.
  remount(session);
  write_file(path, other_parquet(f.bytes.size()));
  f.clear();
  f.refuse_frame = 1;
  session.handle(read(f, handle, 0, 10), true);
  expect_error(f, ble::kErrBadHandle);
  assert(f.checks == ++checks);
  write_file(path, f.bytes);

  // A close raised while a READ is open, with storage in Retry. The READ
  // ends busy at its first CHUNK, then the worker's writable() probe
  // finalizes, remounts and reports remounted() and mounted(). The retried
  // READ rechecks before any byte instead of serving the old stream, so
  // unchanged bytes serve and a changed file ends the handle.
  for (const bool changed : {false, true}) {
    oslogger::power::EmergencyClose close;
    handle = open(f, session);
    assert(f.checks == ++checks);
    f.clear();
    f.close = &close;
    f.raise_close_after = 0;
    session.handle(read(f, handle, 0, 3000), true);
    assert(f.errors.empty() && f.frames.size() == 1);
    {
      const auto &end = f.frames[0].bytes;
      assert(end.size() == 8 && end[0] == ble::kFrameReadEnd &&
             get_u32(end.data() + 3) == 0 && end[7] == ble::kErrBusy);
    }
    assert(f.checks == checks);
    assert(close.due(0));
    remount(session); // the Retry probe inside writable()
    assert(close.finish(oslogger::power::CloseOutcome::Closed, 0) ==
           oslogger::power::Next::Done);
    f.close = nullptr;
    if (changed) {
      write_file(path, other_parquet(f.bytes.size()));
      expect_bad_handle_read(f, session, handle);
      assert(f.checks == ++checks);
      write_file(path, f.bytes);
    } else {
      f.clear();
      session.handle(read(f, handle, 0, 3000), true);
      verify_read(f, handle, 0, 3000, 512);
      assert(f.checks == ++checks);
      assert(f.logs.size() == 2 &&
             f.logs[0].find(" result=same check_us=") != std::string::npos);
    }
  }
  session.close();
  assert(descriptor_count() == idle_descriptors);
}
} // namespace

// LIST runs as a job of bounded steps (issue #33). handle() starts it and
// the storage worker calls step() once per pass.
void list_job_cases(Fixture &f, ossync::ArchiveSession &session) {
  namespace abort = ossync::abort;
  constexpr auto kStep = ossync::ArchiveSession::kListStepEntries;
  const auto stamped = [](ble::ControlRequest r) {
    abort::stamp(r);
    abort::commit(r);
    return r;
  };
  const auto file_names = [&] {
    std::vector<std::string> names;
    for (const auto &frame : f.frames)
      if (frame.bytes[0] == ble::kFrameFile)
        names.emplace_back(frame.bytes.begin() + 5, frame.bytes.end());
    return names;
  };
  const unsigned listed = f.listed;
  f.payload = {517, 1024, 1024, 1024};

  // handle() only starts the job. Nothing is read before the first step.
  reset_abort();
  f.clear();
  f.list_repeats = 1000;
  f.list_reads = f.list_steps = f.list_cancels = 0;
  const auto list = stamped(request(f, ble::kOpList));
  assert(session.handle(list, true));
  assert(session.listing() && f.list_reads == 0 && f.frames.empty());
  const auto *tag = session.list_tag();
  assert(tag && tag->link == ble::Link::Ble && tag->peer == 0 &&
         tag->generation == f.generation[0]);
  session.step(true);
  assert(session.listing() && f.list_reads == kStep &&
         f.frames.size() == kStep && f.list_open);

  // ABORT between steps stops the walk before its next read.
  stamp_abort(ble::Link::Ble, 0, f.generation[0]);
  const std::size_t reads = f.list_reads;
  session.step(true);
  assert(!session.listing() && !session.list_tag());
  assert(f.list_reads == reads && !f.list_open && f.list_cancels == 1);
  assert(f.frames.size() == kStep && f.errors.size() == 1 &&
         f.errors[0].code == ble::kErrAborted &&
         f.errors[0].op == ble::kOpList);
  auto marker = stamp_abort(ble::Link::Ble, 0, f.generation[0]);
  assert(abort::take_stopped(marker) == ble::kOpList);

  // ABORT during a step stops it at the next entry, within one step.
  reset_abort();
  f.clear();
  f.abort_frame = kStep + 7;
  f.abort_link = ble::Link::Lan;
  f.abort_peer = 1;
  std::size_t reads_at_abort = 0;
  f.list_reads = 0;
  session.handle(stamped(request(f, ble::kOpList, ble::Link::Lan, 1)), true);
  while (session.listing()) {
    session.step(true);
    if (!reads_at_abort && f.frames.size() >= f.abort_frame)
      reads_at_abort = f.frames.size();
  }
  assert(reads_at_abort == kStep + 7);
  assert(f.list_reads - reads_at_abort <= kStep && !f.list_open);
  assert(f.errors.size() == 1 && f.errors[0].code == ble::kErrAborted &&
         f.errors[0].link == ble::Link::Lan && f.errors[0].peer == 1);
  marker =
      stamp_abort(ble::Link::Lan, 1, f.generation[slot(ble::Link::Lan, 1)]);
  assert(abort::take_stopped(marker) == ble::kOpList);

  // The emitter refusing a frame stops the walk at that entry.
  reset_abort();
  f.clear();
  f.list_reads = 0;
  f.refuse_frame = kStep + 5;
  serve(session, request(f, ble::kOpList), true);
  assert(f.list_reads == kStep + 5 && !f.list_open);
  assert(f.frames.size() == kStep + 5 && f.errors.size() == 1 &&
         f.errors[0].code == ble::kErrBusy && f.errors[0].detail == "refused");
  assert(f.listed == listed);

  // A preemption, for example a critical battery close, ends the job with
  // ERROR busy before its next read, and leaves no ABORT marker behind.
  reset_abort();
  f.clear();
  f.list_reads = f.list_cancels = 0;
  session.handle(request(f, ble::kOpList), true);
  session.step(true);
  assert(session.listing() && f.list_reads == kStep);
  f.preempt = true;
  session.step(true);
  f.preempt = false;
  assert(!session.listing() && f.list_reads == kStep && !f.list_open &&
         f.list_cancels == 1);
  assert(f.frames.size() == kStep && f.errors.size() == 1 &&
         f.errors[0].code == ble::kErrBusy && f.errors[0].op == ble::kOpList &&
         f.errors[0].detail == "preempted");
  assert(f.listed == listed);
  marker = stamp_abort(ble::Link::Ble, 0, f.generation[0]);
  assert(abort::take_stopped(marker) == 0);

  // A preemption raised during a step stops it at the next FILE frame.
  reset_abort();
  f.clear();
  f.list_reads = 0;
  f.preempt_frame = kStep + 3;
  serve(session, request(f, ble::kOpList), true);
  f.preempt_frame = 0;
  assert(f.list_reads == kStep + 4 && !f.list_open);
  assert(f.frames.size() == kStep + 3 && f.errors.size() == 1 &&
         f.errors[0].code == ble::kErrBusy &&
         f.errors[0].detail == "preempted");

  // An ABORT wins over a preemption that is pending at the same time.
  reset_abort();
  f.clear();
  session.handle(stamped(request(f, ble::kOpList)), true);
  session.step(true);
  stamp_abort(ble::Link::Ble, 0, f.generation[0]);
  f.preempt = true;
  session.step(true);
  f.preempt = false;
  assert(!session.listing() && f.errors.size() == 1 &&
         f.errors[0].code == ble::kErrAborted);
  marker = stamp_abort(ble::Link::Ble, 0, f.generation[0]);
  assert(abort::take_stopped(marker) == ble::kOpList);
  reset_abort();

  // Bounded steps list exactly what one unbounded pass lists, in order.
  f.clear();
  f.names = {"a.parquet", "b.parquet", "c.parquet", "d.parquet", "e.parquet"};
  f.list_repeats = 41;
  std::vector<std::string> unbounded;
  {
    unsigned partials = 0;
    const auto hooks = f.archive();
    const ossync::FileEmitter collect = [](void *out, const char *name,
                                           std::uint32_t) {
      static_cast<std::vector<std::string> *>(out)->push_back(name);
      return true;
    };
    assert(hooks.list(hooks.context, true, UINT32_MAX, collect, &unbounded,
                      partials) == ossync::ListStatus::Complete);
  }
  f.list_steps = 0;
  serve(session, request(f, ble::kOpList), true);
  assert(file_names() == unbounded && unbounded.size() == 205);
  assert(f.list_steps == (205 + kStep) / kStep);
  assert(f.frames.back().bytes[0] == ble::kFrameListEnd &&
         get_u32(f.frames.back().bytes.data() + 1) == 205);
  assert(f.errors.empty() && f.listed == listed + 1);
  f.names = {"sample.parquet"};

  // A huge directory never holds the worker for more than one step. The
  // worker's sample pass runs between steps and every frame is progress.
  f.clear();
  f.list_repeats = 10000;
  f.list_steps = f.list_budget_max = 0;
  f.progress = 0;
  static std::size_t passes;
  passes = 0;
  f.between_steps = [](Fixture &) { ++passes; };
  serve(session, request(f, ble::kOpList), true, &f);
  f.between_steps = nullptr;
  assert(f.list_budget_max == kStep);
  assert(f.list_steps == (10000 + kStep) / kStep && passes == f.list_steps);
  assert(f.progress == f.frames.size() && f.frames.size() == 10001);
  assert(f.listed == listed + 2);

  // While the job runs, requests on its link and peer wait, a LIST from
  // another link or peer does not, and other links are served between steps.
  f.clear();
  f.list_repeats = 100;
  session.handle(request(f, ble::kOpList, ble::Link::Lan, 0), true);
  session.step(true);
  assert(session.must_wait_for_list(
      request(f, ble::kOpStatus, ble::Link::Lan, 0)));
  assert(!session.must_wait_for_list(request(f, ble::kOpList)));
  assert(
      session.must_wait_for_list(request(f, ble::kOpList, ble::Link::Lan, 0)));
  auto marker_request = request(f, ble::kOpAbort, ble::Link::Lan, 0);
  assert(session.must_wait_for_list(marker_request));
  assert(!session.must_wait_for_list(request(f, ble::kOpStatus)));
  assert(
      !session.must_wait_for_list(request(f, ble::kOpRead, ble::Link::Lan, 2)));
  const auto other = open(f, session, ble::Link::Lan, "sample.parquet", 2);
  f.clear();
  session.handle(read(f, other, 0, 100, ble::Link::Lan, 2), true);
  assert(session.listing());
  assert(!f.frames.empty() && f.frames.front().peer == 2);
  // A LIST from another link or peer is refused at once, the job continues.
  f.clear();
  session.handle(request(f, ble::kOpList), true);
  assert(f.errors.size() == 1 && f.errors[0].code == ble::kErrBusy &&
         f.errors[0].link == ble::Link::Ble && f.errors[0].detail == "list");
  assert(session.listing());
  f.clear();
  while (session.listing())
    session.step(true);
  assert(f.errors.empty() && f.frames.back().bytes[0] == ble::kFrameListEnd &&
         f.frames.back().peer == 0 &&
         get_u32(f.frames.back().bytes.data() + 1) == 100);
  auto close_other = request(f, ble::kOpClose, ble::Link::Lan, 2);
  close_other.length = 3;
  put_u16(close_other.bytes + 1, other);
  serve(session, close_other, true);

  // A peer that left gets no frames, its walk just closes.
  f.clear();
  f.list_cancels = 0;
  session.handle(request(f, ble::kOpList, ble::Link::Lan, 1), true);
  session.step(true);
  const std::size_t sent = f.frames.size();
  ++f.generation[slot(ble::Link::Lan, 1)];
  session.step(true);
  assert(!session.listing() && !f.list_open && f.list_cancels == 1);
  assert(f.frames.size() == sent && f.errors.empty());

  // A step without storage and an erase end the job with ERROR storage, a
  // remount with ERROR busy "remount" since it is transient. Each closes the
  // walk. The worker passes storage_ready false only around a failed
  // remount.
  for (int cause = 0; cause < 3; ++cause) {
    f.clear();
    session.handle(request(f, ble::kOpList), true);
    session.step(true);
    assert(f.list_open);
    if (cause == 0)
      session.step(false);
    else if (cause == 1)
      session.remounted();
    else
      session.close();
    if (cause == 1)
      session.mounted();
    assert(!session.listing() && !f.list_open);
    assert(f.errors.size() == 1 && f.errors[0].op == ble::kOpList &&
           f.errors[0].link == ble::Link::Ble);
    if (cause == 1)
      assert(f.errors[0].code == ble::kErrBusy &&
             f.errors[0].detail == "remount");
    else
      assert(f.errors[0].code == ble::kErrStorage);
  }
  f.list_repeats = 1;
  reset_abort();
  f.clear();
}

// The storage worker glue around a LIST job (os_logger_list_job.h), driven
// with the real planner, governor and session as the worker runs them.
void worker_glue_cases(Fixture &f, ossync::ArchiveSession &session) {
  namespace listjob = oslogger::listjob;
  namespace storage = oslogger::storage;
  using listjob::StoragePass;
  constexpr auto kStep = ossync::ArchiveSession::kListStepEntries;
  reset_abort();
  f.payload = {517, 1024, 1024, 1024};

  // One worker pass of service_storage on a board with a remount hook. A
  // Probe remounts, which the session sees as remounted() then mounted().
  struct Storage {
    storage::Governor governor;
    listjob::StoragePlanner planner;
    unsigned remounts = 0, kept = 0, rechecks = 0;
    std::uint64_t free_bytes = 0;
    StoragePass pass(ossync::ArchiveSession &session, std::int64_t now,
                     bool owner = false) {
      listjob::StorageInputs in;
      in.probe_due = governor.probe_due(now);
      in.can_probe = true;
      in.recheck_due = governor.recheck_due(now);
      in.full = governor.state() == storage::State::Full;
      in.retry = governor.state() == storage::State::Retry;
      in.remount = true;
      in.files_closed = true;
      in.listing = session.listing();
      in.owner = owner;
      const auto next = planner.next(in);
      if (next == StoragePass::Probe) {
        ++remounts;
        session.remounted();
        session.mounted();
        governor.probe_succeeded(free_bytes, now);
      } else if (next == StoragePass::KeepMount) {
        ++kept;
        governor.probe_succeeded(free_bytes, now);
      } else if (next == StoragePass::Recheck) {
        ++rechecks;
        governor.observe_free(free_bytes, now);
      }
      return next;
    }
  };

  // A Full card held across several rechecks does not remount mid LIST. The
  // walk ends with LIST_END and the skipped remount runs after it.
  {
    f.clear();
    f.list_repeats = 12 * kStep;
    f.list_cancels = 0;
    Storage card;
    card.governor.configure(1ULL << 30);
    card.governor.observe_free(0, 0);
    assert(card.governor.state() == storage::State::Full);
    session.handle(request(f, ble::kOpList), true);
    std::int64_t now = 0;
    while (session.listing()) {
      now += 20;
      card.pass(session, now);
      session.step(true);
    }
    assert(now / storage::kRecheckSeconds >= 3 && card.rechecks >= 3);
    assert(card.remounts == 0 && card.planner.deferred());
    assert(card.governor.state() == storage::State::Full);
    assert(f.errors.empty() && f.list_cancels == 1 && !f.list_open);
    assert(f.frames.back().bytes[0] == ble::kFrameListEnd &&
           get_u32(f.frames.back().bytes.data() + 1) == 12 * kStep);
    assert(card.pass(session, now + 1) == StoragePass::Probe);
    assert(card.remounts == 1 && !card.planner.deferred());
    // Without a job the Full recheck remounts as before.
    assert(card.pass(session, now + 2) == StoragePass::Idle);
    assert(card.pass(session, now + 1 + storage::kRecheckSeconds) ==
           StoragePass::Probe);
  }

  // The same remount without the planner's deferral ends the walk with
  // ERROR busy "remount", which is what the deferral prevents.
  {
    f.clear();
    f.list_repeats = 4 * kStep;
    session.handle(request(f, ble::kOpList), true);
    session.step(true);
    session.remounted();
    session.mounted();
    assert(!session.listing() && f.errors.size() == 1 &&
           f.errors[0].code == ble::kErrBusy &&
           f.errors[0].detail == "remount");
  }

  // A Retry probe due mid LIST keeps the mount. An owner action such as
  // FLUSH still remounts and ends the job with ERROR busy "remount".
  {
    f.clear();
    f.list_repeats = 4 * kStep;
    Storage card;
    card.free_bytes = 1ULL << 30;
    card.governor.configure(1ULL << 30);
    card.governor.write_failed(0);
    assert(card.governor.state() == storage::State::Retry);
    session.handle(request(f, ble::kOpList), true);
    session.step(true);
    assert(card.pass(session, 5) == StoragePass::Idle);
    card.governor.write_failed(10);
    card.governor.request_probe();
    assert(card.pass(session, 11) == StoragePass::KeepMount);
    assert(card.kept == 1 && card.remounts == 0 && card.planner.deferred());
    session.step(true);
    assert(session.listing() && f.errors.empty());
    card.governor.write_failed(12);
    card.governor.request_probe();
    assert(card.pass(session, 13, true) == StoragePass::Probe);
    assert(!session.listing() && f.errors.size() == 1 &&
           f.errors[0].code == ble::kErrBusy &&
           f.errors[0].detail == "remount" && !card.planner.deferred());
  }

  // A Retry card whose kept probe failed again remounts as soon as the job
  // ends, without waiting for its backoff.
  {
    listjob::StoragePlanner planner;
    listjob::StorageInputs in;
    in.probe_due = in.can_probe = in.retry = in.remount = true;
    in.listing = true;
    assert(planner.next(in) == StoragePass::KeepMount && planner.deferred());
    in.probe_due = false;
    assert(planner.next(in) == StoragePass::Idle && planner.deferred());
    in.listing = false;
    assert(planner.next(in) == StoragePass::Probe && !planner.deferred());
    assert(planner.next(in) == StoragePass::Idle);
    // A board without a remount hook probes as before, a job or not.
    in.remount = false;
    in.probe_due = in.listing = true;
    assert(planner.next(in) == StoragePass::Probe && !planner.deferred());
    // A Full card with an open writer file only rechecks.
    in = listjob::StorageInputs{};
    in.recheck_due = in.full = in.remount = in.can_probe = true;
    assert(planner.next(in) == StoragePass::Recheck);
    // A deferred remount the card no longer needs is dropped.
    in.files_closed = in.listing = true;
    assert(planner.next(in) == StoragePass::Recheck && planner.deferred());
    in = listjob::StorageInputs{};
    in.can_probe = in.remount = in.files_closed = true;
    assert(planner.next(in) == StoragePass::Idle && !planner.deferred());
  }

  // A critical battery close that falls due between steps. The worker's
  // close pass first ends the job with ERROR busy "preempted", then the
  // close attempt runs its Retry remount with the walk's directories
  // already closed, so the phone never sees ERROR storage.
  {
    struct CloseCard {
      Fixture &f;
      ossync::ArchiveSession &session;
      unsigned remounts = 0;
      bool has_data() const { return true; }
      bool writable() {
        assert(!f.list_open && !session.listing());
        ++remounts;
        session.remounted();
        session.mounted();
        return true;
      }
      bool finalize() { return true; }
    };
    oslogger::power::EmergencyClose close;
    CloseCard card{f, session};
    f.clear();
    f.list_repeats = 4 * kStep;
    f.list_cancels = 0;
    session.handle(request(f, ble::kOpList, ble::Link::Lan, 1), true);
    session.step(true);
    assert(session.listing() && f.list_open);
    const std::size_t sent = f.frames.size();
    // Nothing is due, so the pass leaves the job alone.
    assert(!listjob::preempt_for_close(session, close, 0));
    assert(!oslogger::power::service(close, card, 0).attempted);
    close.request();
    assert(listjob::preempt_for_close(session, close, 0));
    const auto pass = oslogger::power::service(close, card, 0);
    assert(pass.attempted &&
           pass.outcome == oslogger::power::CloseOutcome::Closed);
    assert(card.remounts == 1 && !f.list_open && f.list_cancels == 1);
    assert(f.frames.size() == sent && f.errors.size() == 1 &&
           f.errors[0].code == ble::kErrBusy &&
           f.errors[0].detail == "preempted" &&
           f.errors[0].link == ble::Link::Lan && f.errors[0].peer == 1);
    // An ABORT already pending still wins over the close.
    f.clear();
    session.handle(request(f, ble::kOpList), true);
    session.step(true);
    stamp_abort(ble::Link::Ble, 0, f.generation[0]);
    close.request();
    assert(listjob::preempt_for_close(session, close, 0));
    assert(!session.listing() && f.errors.size() == 1 &&
           f.errors[0].code == ble::kErrAborted);
    reset_abort();
    assert(close.due(0));
    close.finish(oslogger::power::CloseOutcome::Closed, 0);
  }

  // The queue glue. A request the job holds parks outside the queue, so an
  // unrelated READ behind it runs between steps, and a LIST from another
  // link is refused at once. The parked request runs after LIST_END.
  {
    const auto reader = open(f, session, ble::Link::Lan, "sample.parquet", 2);
    f.clear();
    f.list_repeats = 6 * kStep;
    std::deque<ble::ControlRequest> queue;
    ble::ControlRequest parked{};
    bool has_parked = false;
    session.handle(request(f, ble::kOpList, ble::Link::Lan, 0), true);
    auto same_peer = request(f, ble::kOpOpen, ble::Link::Lan, 0);
    std::memcpy(same_peer.bytes + 1, "sample.parquet", 14);
    same_peer.length += 14;
    queue.push_back(same_peer);
    queue.push_back(read(f, reader, 0, 100, ble::Link::Lan, 2));
    queue.push_back(request(f, ble::kOpList));
    std::vector<listjob::Take> takes;
    std::size_t list_end = 0, opened = 0, read_at = 0, busy_at = 0;
    for (unsigned pass = 0; pass < 100; ++pass) {
      if (session.listing())
        session.step(true);
      const bool has_head = !queue.empty();
      const auto take =
          listjob::take(session.listing(), has_parked, has_head,
                        has_head && session.must_wait_for_list(queue.front()));
      takes.push_back(take);
      if (take == listjob::Take::Wait) {
        if (!session.listing())
          break;
        continue;
      }
      if (take == listjob::Take::Park) {
        parked = queue.front();
        queue.pop_front();
        has_parked = true;
        continue;
      }
      if (take == listjob::Take::Parked) {
        has_parked = false;
        session.handle(parked, true);
        continue;
      }
      const auto r = queue.front();
      queue.pop_front();
      session.handle(r, true);
      if (r.bytes[0] == ble::kOpRead)
        read_at = f.frames.size();
      else
        busy_at = f.errors.size();
    }
    for (std::size_t i = 0; i < f.frames.size(); ++i) {
      const auto &frame = f.frames[i];
      if (frame.peer == 0 && frame.bytes[0] == ble::kFrameListEnd)
        list_end = i + 1;
      if (frame.peer == 0 && frame.bytes[0] == ble::kFrameOpened)
        opened = i + 1;
    }
    assert(takes[0] == listjob::Take::Park && takes[1] == listjob::Take::Head &&
           takes[2] == listjob::Take::Head);
    assert(read_at && read_at < list_end && f.frames[read_at - 1].peer == 2);
    assert(busy_at == 1 && f.errors[0].link == ble::Link::Ble &&
           f.errors[0].code == ble::kErrBusy && f.errors[0].detail == "list");
    assert(list_end && opened == list_end + 1 && f.frames.size() == opened);
    assert(queue.empty() && !has_parked && !session.listing());
    // A second request from the job's peer behind a parked one stays at the
    // head until the job ends, the documented bound.
    assert(listjob::take(true, true, true, true) == listjob::Take::Wait);
    assert(listjob::take(true, true, true, false) == listjob::Take::Head);
    assert(listjob::take(true, false, false, false) == listjob::Take::Wait);
    assert(listjob::take(false, true, true, false) == listjob::Take::Parked);
    assert(listjob::take(false, false, true, true) == listjob::Take::Head);
    const auto parked_handle = get_u16(f.frames[opened - 1].bytes.data() + 1);
    for (const auto &[link_peer, handle] :
         {std::pair<std::uint8_t, std::uint16_t>{0, parked_handle},
          std::pair<std::uint8_t, std::uint16_t>{2, reader}}) {
      auto done = request(f, ble::kOpClose, ble::Link::Lan, link_peer);
      done.length = 3;
      put_u16(done.bytes + 1, handle);
      serve(session, done, true);
    }
  }

  // Serial `parquet list` walks the whole archive and is refused during a
  // job. `parquet get` opens one file and is not.
  assert(listjob::serial_walks_archive("parquet list"));
  assert(!listjob::serial_walks_archive("parquet get sample.parquet"));
  assert(!listjob::serial_walks_archive("parquet"));
  assert(!listjob::serial_walks_archive(nullptr));

  f.list_repeats = 1;
  reset_abort();
  f.clear();
}

ble::ControlRequest since_request(Fixture &f, const ossync::ListCursor &cursor,
                                  ble::Link link = ble::Link::Ble) {
  auto r = request(f, ble::kOpListSince, link);
  ossync::encode_cursor(cursor,
                        std::span<std::uint8_t, ossync::kListCursorBytes>(
                            r.bytes + 1, ossync::kListCursorBytes));
  r.length = ossync::kListSinceRequestBytes;
  return r;
}

// LIST_SINCE shares the LIST job. It sends the same FILE frames, then a
// LIST_SINCE_END whose first 15 bytes are LIST_END.
void list_since_cases(Fixture &f, ossync::ArchiveSession &session) {
  namespace abort = ossync::abort;
  static_assert(ble::kOpListSince == 0x10 && ble::kFrameListSinceEnd == 0x4e);
  reset_abort();
  f.payload = {517, 1024, 1024, 1024};
  f.list_repeats = 3;
  const ossync::ListCursor cursor{
      .mount_epoch = 0xa1b2c3d4,
      .day = 20260315,
      .boot = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
      .seq = 0x0102030405060708ULL};
  f.since_result = {
      .flags = ossync::kListSinceUnsyncedFull,
      .next = {.mount_epoch = 0xa1b2c3d4,
               .day = 20260316,
               .boot = {0xf0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0f},
               .seq = 42}};
  for (const auto link : {ble::Link::Ble, ble::Link::Lan}) {
    f.clear();
    const unsigned listed = f.listed;
    f.since_begins = 0;
    assert(serve(session, since_request(f, cursor, link), true));
    assert(f.errors.empty() && f.since_begins == 1);
    assert(f.since_cursor.mount_epoch == cursor.mount_epoch &&
           f.since_cursor.day == cursor.day &&
           f.since_cursor.boot == cursor.boot &&
           f.since_cursor.seq == cursor.seq);
    assert(f.frames.size() == 4 && f.listed == listed + 1);
    for (std::size_t i = 0; i < 3; ++i)
      assert(f.frames[i].bytes[0] == ble::kFrameFile &&
             f.frames[i].link == link);
    const auto &end = f.frames.back().bytes;
    assert(end.size() == 48 && end[0] == ble::kFrameListSinceEnd);
    assert(get_u32(end.data() + 1) == 3 && get_u16(end.data() + 5) == 65535);
    assert(get_u32(end.data() + 7) == 123 && get_u32(end.data() + 11) == 45);
    assert(end[15] == ossync::kListSinceUnsyncedFull);
    assert(get_u32(end.data() + 16) == 0xa1b2c3d4);
    assert(get_u32(end.data() + 20) == 20260316);
    assert(end[24] == 0xf0 && end[39] == 0x0f);
    assert(get_u32(end.data() + 40) == 42 && get_u32(end.data() + 44) == 0);
  }

  // Plain LIST still ends with the 15 byte LIST_END.
  f.clear();
  serve(session, request(f, ble::kOpList), true);
  assert(f.frames.back().bytes.size() == 15 &&
         f.frames.back().bytes[0] == ble::kFrameListEnd);

  // A cursor of the wrong length or with an impossible day is malformed.
  const auto malformed = [&](ble::ControlRequest r) {
    f.clear();
    const unsigned listed = f.listed;
    f.since_begins = 0;
    serve(session, r, true);
    assert(!session.listing() && f.frames.empty() && f.since_begins == 0);
    assert(f.errors.size() == 1 && f.errors[0].op == ble::kOpListSince &&
           f.errors[0].code == ble::kErrMalformed &&
           f.errors[0].detail == "cursor");
    assert(f.listed == listed);
  };
  auto r = since_request(f, cursor);
  r.length = 32;
  malformed(r);
  r = since_request(f, cursor);
  r.length = 34;
  malformed(r);
  malformed(request(f, ble::kOpListSince));
  for (const std::uint32_t day :
       {20191231U, 21010101U, 20260230U, 20261301U, 20260100U, 1U}) {
    auto bad = cursor;
    bad.day = day;
    malformed(since_request(f, bad));
  }
  // Day 0 is no floor, and an all zero cursor is a valid first request.
  f.clear();
  serve(session, since_request(f, ossync::ListCursor{}), true);
  assert(f.errors.empty() && f.frames.back().bytes.size() == 48);

  // Storage, busy, preempted and a link too small for LIST_SINCE_END.
  f.clear();
  serve(session, since_request(f, cursor), false);
  assert(f.errors.size() == 1 && f.errors[0].op == ble::kOpListSince &&
         f.errors[0].code == ble::kErrStorage);
  f.clear();
  assert(session.handle(request(f, ble::kOpList), true) && session.listing());
  session.handle(since_request(f, cursor), true);
  assert(f.errors.size() == 1 && f.errors[0].op == ble::kOpListSince &&
         f.errors[0].code == ble::kErrBusy && f.errors[0].detail == "list");
  while (session.listing())
    session.step(true);
  f.clear();
  f.preempt = true;
  serve(session, since_request(f, cursor), true);
  f.preempt = false;
  assert(f.errors.size() == 1 && f.errors[0].code == ble::kErrBusy &&
         f.errors[0].op == ble::kOpListSince &&
         f.errors[0].detail == "preempted");
  for (const std::uint16_t small : {47, 20}) {
    f.clear();
    f.payload[0] = small;
    serve(session, since_request(f, cursor), true);
    assert(!session.listing() && f.frames.empty());
    assert(f.errors.size() == 1 && f.errors[0].code == ble::kErrBusy &&
           f.errors[0].op == ble::kOpListSince && f.errors[0].detail == "mtu");
  }
  // A link that shrank during the walk gets ERROR mtu, not a cut frame.
  f.payload[0] = 517;
  f.clear();
  const unsigned listed = f.listed;
  session.handle(since_request(f, cursor), true);
  f.payload[0] = 47;
  while (session.listing())
    session.step(true);
  assert(f.frames.size() == 3 && f.listed == listed);
  assert(f.errors.size() == 1 && f.errors[0].op == ble::kOpListSince &&
         f.errors[0].detail == "mtu");
  f.payload[0] = 517;

  // ABORT stops it with ERROR aborted naming LIST_SINCE.
  f.clear();
  f.list_repeats = 1000;
  auto stamped = since_request(f, cursor);
  abort::stamp(stamped);
  abort::commit(stamped);
  session.handle(stamped, true);
  session.step(true);
  stamp_abort(ble::Link::Ble, 0, f.generation[0]);
  session.step(true);
  assert(!session.listing());
  assert(f.errors.size() == 1 && f.errors[0].code == ble::kErrAborted &&
         f.errors[0].op == ble::kOpListSince);
  auto marker = stamp_abort(ble::Link::Ble, 0, f.generation[0]);
  assert(abort::take_stopped(marker) == ble::kOpListSince);
  reset_abort();
  // A walk error is a storage error, never a short success.
  f.clear();
  f.list_repeats = 2;
  f.listing_complete = false;
  serve(session, since_request(f, cursor), true);
  f.listing_complete = true;
  assert(f.errors.size() == 1 && f.errors[0].code == ble::kErrStorage &&
         f.errors[0].op == ble::kOpListSince);
  f.list_repeats = 1;

  // A board without the hook answers ERROR unknown op, the phone's signal
  // to fall back to LIST.
  auto hooks = f.archive();
  hooks.list_since = nullptr;
  ossync::ArchiveSession bare(hooks, f.transport());
  f.clear();
  assert(serve(bare, since_request(f, cursor), true));
  assert(!bare.listing() && f.frames.empty());
  assert(f.errors.size() == 1 && f.errors[0].op == ble::kOpListSince &&
         f.errors[0].code == ble::kErrUnknownOp);
  f.clear();
  serve(bare, request(f, ble::kOpList), true);
  assert(f.errors.empty() && f.frames.back().bytes.size() == 15);
}

// A STAC sidecar next to its dated Parquet (#143), served through the same
// session as the Parquet. The hooks apply the owner's rules from sync_codec.h
// as os_logger.cpp does: resolve takes a Parquet name or the sidecar name of
// a dated one, and the finalized hook checks a sidecar's Parquet beside it,
// its size bounds and its first and last byte.
void sidecar_cases(const std::string &root) {
  const std::string directory = root + "/sidecar";
  const std::string day = "thing=t1/year=2026/month=11/day=21";
  std::string made = directory;
  assert(::mkdir(made.c_str(), 0700) == 0);
  for (const char *part : {"/thing=t1", "/year=2026", "/month=11", "/day=21"}) {
    made += part;
    assert(::mkdir(made.c_str(), 0700) == 0);
  }
  const std::string stem =
      day + "/data_0910_0123456789abcdef0123456789abcdef_1-90-0";
  const std::string parquet = stem + ".parquet", json = stem + ".json";
  Fixture f(directory.c_str());
  write_file(directory + "/" + parquet, f.bytes);
  const std::string text = R"({"type":"Feature","id":"x","properties":{}})";
  const std::vector<std::uint8_t> sidecar(text.begin(), text.end());
  write_file(directory + "/" + json, sidecar);
  f.names = {parquet, json};
  auto hooks = f.archive();
  hooks.resolve = [](void *ctx, const char *name, char *path,
                     std::size_t size) {
    auto &self = *static_cast<Fixture *>(ctx);
    if (!safe_parquet_name(name) && !safe_sidecar_name(name))
      return false;
    const int n =
        std::snprintf(path, size, "%s/%s", self.directory.c_str(), name);
    return n > 0 && static_cast<std::size_t>(n) < size;
  };
  hooks.finalized = [](void *ctx, const char *path, FILE *file,
                       ossync::FileCheck &check) {
    auto &self = *static_cast<Fixture *>(ctx);
    const std::string_view full{path};
    if (!full.ends_with(kSidecarSuffix))
      return false; // the Parquet cases live in main()
    const std::string relative{full.substr(self.directory.size() + 1)};
    std::array<char, 385> sibling{};
    struct stat info{};
    if (!sidecar_parquet_name(relative.c_str(), sibling) ||
        ::stat((self.directory + "/" + sibling.data()).c_str(), &info) != 0 ||
        !S_ISREG(info.st_mode))
      return false;
    std::vector<std::uint8_t> data(kMaxSidecarBytes + 2);
    const std::size_t count = std::fread(data.data(), 1, data.size(), file);
    check.size = static_cast<std::uint32_t>(count);
    check.crc = crc32_update(0xffffffffU, data.data(), count) ^ 0xffffffffU;
    return count > 0 && sidecar_complete(count, data[0], data[count - 1]);
  };
  ossync::ArchiveSession session(hooks, f.transport());

  // LIST names the sidecar right after its Parquet, and LIST_END counts
  // every FILE frame, sidecars included.
  for (const auto link : {ble::Link::Ble, ble::Link::Lan}) {
    f.clear();
    assert(serve(session, request(f, ble::kOpList, link), true));
    assert(f.errors.empty() && f.frames.size() == 3);
    for (std::size_t i = 0; i < 2; ++i) {
      const auto &frame = f.frames[i].bytes;
      assert(frame[0] == ble::kFrameFile);
      assert(std::string(frame.begin() + 5, frame.end()) == f.names[i]);
    }
    assert(f.frames[2].bytes[0] == ble::kFrameListEnd &&
           get_u32(f.frames[2].bytes.data() + 1) == 2);
  }

  const auto open_name = [&](const std::string &name, ble::Link link) {
    f.clear();
    auto r = request(f, ble::kOpOpen, link);
    std::memcpy(r.bytes + 1, name.data(), name.size());
    r.length += name.size();
    assert(session.handle(r, true));
  };
  // OPEN and READ serve the sidecar bytes with their size and CRC-32.
  for (const auto link : {ble::Link::Ble, ble::Link::Lan}) {
    open_name(json, link);
    assert(f.errors.empty() && f.frames.size() == 1);
    const auto &opened = f.frames[0].bytes;
    assert(opened[0] == ble::kFrameOpened &&
           get_u32(opened.data() + 3) == sidecar.size());
    assert(get_u32(opened.data() + 7) ==
           (crc_update(0xffffffffU, sidecar.data(), sidecar.size()) ^
            0xffffffffU));
    const auto handle = get_u16(opened.data() + 1);
    f.clear();
    serve(session,
          read(f, handle, 0, static_cast<std::uint32_t>(sidecar.size()), link),
          true);
    std::vector<std::uint8_t> received;
    for (std::size_t i = 0; i + 1 < f.frames.size(); ++i) {
      assert(f.frames[i].bytes[0] == ble::kFrameChunk);
      received.insert(received.end(), f.frames[i].bytes.begin() + 7,
                      f.frames[i].bytes.end());
    }
    assert(f.errors.empty() && received == sidecar);
    assert(f.frames.back().bytes[0] == ble::kFrameReadEnd &&
           f.frames.back().bytes[7] == 0);
  }

  // Names that are never served: a partial, a traversal, an unsynced or a
  // set aside sidecar, and a json beside nothing dated.
  write_file(directory + "/" + json + ".partial", sidecar);
  const std::vector<std::string> refused = {
      json + ".partial",
      day + "/../" + json,
      "../" + json,
      "thing=t1/unsynced/boot=0123456789abcdef0123456789abcdef/"
      "data_unsynced_0123456789abcdef0123456789abcdef_1-90-0.json",
      "flash-conflict/" + json,
      "quarantine/" + json,
      "legacy-parquet/" + json,
      "sample.json"};
  for (const auto &name : refused) {
    open_name(name, ble::Link::Ble);
    expect_error(f, ble::kErrInvalidName);
  }

  // A sidecar whose Parquet is gone is not a finalized file.
  const std::string lone =
      day + "/data_0920_0123456789abcdef0123456789abcdef_91-180-0.json";
  write_file(directory + "/" + lone, sidecar);
  open_name(lone, ble::Link::Ble);
  expect_error(f, ble::kErrNotFinalized);
  // A missing sidecar beside a real Parquet.
  std::rename((directory + "/" + json).c_str(),
              (directory + "/" + json + ".moved").c_str());
  open_name(json, ble::Link::Ble);
  expect_error(f, ble::kErrNotFinalized);

  // Over the writer's largest Item, or not one JSON object, is refused.
  std::vector<std::uint8_t> oversize(kMaxSidecarBytes + 1, ' ');
  oversize.front() = '{';
  oversize.back() = '}';
  write_file(directory + "/" + json, oversize);
  open_name(json, ble::Link::Ble);
  expect_error(f, ble::kErrNotFinalized);
  oversize.pop_back();
  oversize.back() = '}';
  write_file(directory + "/" + json, oversize);
  open_name(json, ble::Link::Ble);
  assert(f.errors.empty() && f.frames.size() == 1 &&
         get_u32(f.frames[0].bytes.data() + 3) == kMaxSidecarBytes);
  for (const std::string &bad : {std::string("{\"a\":1"), std::string("[]"),
                                 std::string("{}\n"), std::string("{")}) {
    write_file(directory + "/" + json,
               std::vector<std::uint8_t>(bad.begin(), bad.end()));
    open_name(json, ble::Link::Ble);
    expect_error(f, ble::kErrNotFinalized);
  }
}

int main(int argc, char **argv) {
  assert(argc == 2);
  Fixture f(argv[1]);
  ossync::ArchiveSession session(f.archive(), f.transport());

  assert(!serve(session, request(f, ble::kOpStatus), true));
  assert(serve(session, request(f, ble::kOpList), false));
  expect_error(f, ble::kErrStorage);
  f.clear();
  assert(serve(session, request(f, ble::kOpList), true));
  assert(f.frames.size() == 2 && f.listed == 1 && f.progress == 2);
  const auto &list = f.frames.back().bytes;
  assert(list.size() == 15 && list[0] == ble::kFrameListEnd);
  assert(get_u32(list.data() + 1) == 1 && get_u16(list.data() + 5) == 65535);
  assert(get_u32(list.data() + 7) == 123 && get_u32(list.data() + 11) == 45);
  f.clear();
  f.refuse_frame = 1;
  serve(session, request(f, ble::kOpList), true);
  assert(f.frames.size() == 1 && f.listed == 1); // no LIST_END after refusal
  // A refusal without ABORT still ends LIST with a terminal ERROR busy, so
  // the client never waits for a LIST_END that will not come.
  assert(f.errors.size() == 1 && f.errors[0].op == ble::kOpList &&
         f.errors[0].code == ble::kErrBusy && f.errors[0].detail == "refused");
  assert(f.logs.size() == 1 &&
         f.logs[0].find("LIST ABORT link=ble after=0 reason=refused") !=
             std::string::npos);
  f.clear();
  f.payload[0] = 18;
  serve(session, request(f, ble::kOpList), true);
  expect_error(f, ble::kErrBusy);
  assert(f.listed == 1); // no false empty listing or cleared new-files hint
  f.payload[0] = 517;

  // SD failure after FILE frames is an error, never a successful prefix.
  for (const auto link : {ble::Link::Ble, ble::Link::Lan}) {
    f.clear();
    f.listing_complete = false;
    serve(session, request(f, ble::kOpList, link), true);
    assert(f.frames.size() == 1 && f.frames[0].bytes[0] == ble::kFrameFile);
    assert(f.errors.size() == 1 && f.errors[0].op == ble::kOpList &&
           f.errors[0].code == ble::kErrStorage && f.errors[0].link == link);
    assert(f.listed == 1);
  }
  f.listing_complete = true;
  f.clear();
  f.refuse_frame = 2; // LIST_END delivery fails after a successful traversal.
  serve(session, request(f, ble::kOpList), true);
  assert(f.frames.size() == 2 && f.listed == 1);
  f.clear();
  serve(session, request(f, ble::kOpList, ble::Link::Lan), true);
  assert(f.frames.size() == 2 && f.listed == 2 && f.errors.empty());
  f.clear();
  f.list_repeats = 70000; // Proto 3 LIST_END count is u32, past the old cap.
  serve(session, request(f, ble::kOpList, ble::Link::Lan), true);
  assert(f.frames.size() == 70001 && f.listed == 3 && f.errors.empty());
  assert(f.frames.back().bytes[0] == ble::kFrameListEnd &&
         f.frames.back().bytes.size() == 15 &&
         get_u32(f.frames.back().bytes.data() + 1) == 70000);
  f.list_repeats = 1;
  // A critical battery close raised mid LIST stops it before the next FILE
  // through the session's preempted hook with ERROR busy "preempted". The
  // gate passes FILE frames and refuses only READ CHUNK frames. The worker
  // runs the close, then a retried LIST completes.
  {
    oslogger::power::EmergencyClose close;
    f.clear();
    f.list_repeats = 3;
    f.close = &close;
    f.raise_close_after = 1;
    serve(session, request(f, ble::kOpList, ble::Link::Lan), true);
    assert(f.frames.size() == 2 && f.frames[1].bytes[0] == ble::kFrameFile);
    assert(f.errors.size() == 1 && f.errors[0].op == ble::kOpList &&
           f.errors[0].code == ble::kErrBusy &&
           f.errors[0].detail == "preempted" &&
           f.errors[0].link == ble::Link::Lan);
    // A LIST while the close is still due is answered at once, no job.
    f.clear();
    serve(session, request(f, ble::kOpList), true);
    assert(!session.listing() && f.frames.empty() && f.errors.size() == 1 &&
           f.errors[0].code == ble::kErrBusy &&
           f.errors[0].detail == "preempted");
    const unsigned listed_before = f.listed;
    assert(close.due(0) && close.finish(oslogger::power::CloseOutcome::Closed,
                                        0) == oslogger::power::Next::Done);
    f.clear();
    f.raise_close_after = static_cast<std::size_t>(-1);
    serve(session, request(f, ble::kOpList, ble::Link::Lan), true);
    assert(f.frames.size() == 4 && f.errors.empty() &&
           f.listed == listed_before + 1 &&
           f.frames.back().bytes[0] == ble::kFrameListEnd);
    f.close = nullptr;
    f.raise_close_after = 0;
    f.list_repeats = 1;
  }
  list_job_cases(f, session);
  list_since_cases(f, session);
  worker_glue_cases(f, session);
  sidecar_cases(argv[1]);

  auto handle = open(f, session);
  f.clear();
  f.payload[0] = 24; // LIST fits, but OPENED has a larger header
  auto too_small = request(f, ble::kOpOpen);
  const char *file_name = "sample.parquet";
  std::memcpy(too_small.bytes + 1, file_name, std::strlen(file_name));
  too_small.length += std::strlen(file_name);
  serve(session, too_small, true);
  expect_error(f, ble::kErrBusy);
  f.payload[0] = 517;
  f.clear();
  serve(session, read(f, handle, 0, 10), true);
  verify_read(f, handle, 0, 10, 512); // rejected OPEN preserves prior handle
  f.clear();
  serve(session, read(f, handle, 0, UINT32_MAX), true);
  verify_read(f, handle, 0, ble::kMaxRead, 512);
  f.clear();
  serve(session, read(f, handle, f.bytes.size() - 15, 200), true);
  verify_read(f, handle, f.bytes.size() - 15, 15, 512);
  f.clear();
  serve(session, read(f, handle, f.bytes.size() + 1, 1), true);
  expect_error(f, ble::kErrRange);
  f.clear();
  serve(session, read(f, handle, 0, 10, ble::Link::Lan), true);
  expect_error(f, ble::kErrBadHandle, ble::Link::Lan);
  ++f.generation[1]; // another transport reconnect cannot kill a BLE handle
  f.clear();
  serve(session, read(f, handle, 7, 22), true);
  verify_read(f, handle, 7, 22, 512);
  ++f.generation[0];
  f.clear();
  serve(session, read(f, handle, 0, 10), true);
  expect_error(f, ble::kErrBadHandle);

  handle = open(f, session, ble::Link::Lan);
  f.clear();
  serve(session, read(f, handle, 123, 2040, ble::Link::Lan), true);
  verify_read(f, handle, 123, 2040, 1024, ble::Link::Lan);
  f.clear();
  auto malformed = read(f, handle, 0, 10, ble::Link::Lan);
  --malformed.length;
  serve(session, malformed, true);
  expect_error(f, ble::kErrMalformed, ble::Link::Lan);
  f.clear();
  f.refuse_frame = 2;
  serve(session, read(f, handle, 0, 4000, ble::Link::Lan), true);
  assert(f.frames.size() == 3 &&
         f.frames.back().bytes[0] == ble::kFrameReadEnd);
  assert(get_u32(f.frames.back().bytes.data() + 3) == 1017);
  assert(f.frames.back().bytes[7] == ble::kErrBusy);
  f.clear();
  serve(session, read(f, handle, 1017, 4000 - 1017, ble::Link::Lan), true);
  verify_read(f, handle, 1017, 4000 - 1017, 1024, ble::Link::Lan);

  f.clear();
  auto close = request(f, ble::kOpClose, ble::Link::Lan);
  close.length = 3;
  put_u16(close.bytes + 1, handle);
  serve(session, close, true);
  assert(f.frames.size() == 1 && f.frames[0].bytes[0] == ble::kFrameClosed);
  f.clear();
  serve(session, read(f, handle, 0, 10, ble::Link::Lan), true);
  expect_error(f, ble::kErrBadHandle, ble::Link::Lan);

  f.clear();
  auto invalid = request(f, ble::kOpOpen);
  const char *name = "../secret.parquet";
  std::memcpy(invalid.bytes + 1, name, std::strlen(name));
  invalid.length += std::strlen(name);
  serve(session, invalid, true);
  expect_error(f, ble::kErrInvalidName);
  f.clear();
  f.complete = false;
  const char *valid = "sample.parquet";
  std::memcpy(invalid.bytes + 1, valid, std::strlen(valid));
  invalid.length = 1 + std::strlen(valid);
  serve(session, invalid, true);
  expect_error(f, ble::kErrNotFinalized);
  f.complete = true;
  handle = open(f, session);
  f.clear();
  f.payload[0] = 7;
  serve(session, read(f, handle, 0, 10), true);
  assert(f.frames.size() == 1 && f.frames[0].bytes[0] == ble::kFrameReadEnd &&
         f.frames[0].bytes[7] == ble::kErrBusy);
  session.close();
  assert(f.lock_depth == 0 && f.locks > 0);

  // All authenticated peers retain independent handles, even when their
  // requests are interleaved on the sole storage worker.
  // Each logical handle holds exactly one descriptor until it is released.
  f.payload[0] = 517;
  const unsigned idle_descriptors = descriptor_count();
  const auto ble_handle = open(f, session);
  assert(descriptor_count() == idle_descriptors + 1);
  std::array<std::uint16_t, ble::kMaxLanClients> lan_handles{};
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer) {
    lan_handles[peer] =
        open(f, session, ble::Link::Lan, "sample.parquet", peer);
    assert(descriptor_count() == idle_descriptors + 2 + peer);
  }
  constexpr unsigned all_slots = ossync::ArchiveSession::kReadHandles;
  static_assert(all_slots == 1 + ble::kMaxLanClients, "one slot per peer");
  assert(descriptor_count() == idle_descriptors + all_slots);
  for (std::uint8_t peer = 0; peer < ble::kMaxLanClients; ++peer) {
    f.clear();
    serve(session,
          read(f, lan_handles[peer], peer * 100, 2000, ble::Link::Lan, peer),
          true);
    verify_read(f, lan_handles[peer], peer * 100, 2000, 1024, ble::Link::Lan,
                peer);
    assert(descriptor_count() == idle_descriptors + all_slots);
    f.clear();
    serve(session, request(f, ble::kOpList, ble::Link::Lan, peer), true);
    assert(f.frames.size() == 2);
    for (const auto &frame : f.frames)
      assert(frame.link == ble::Link::Lan && frame.peer == peer);
  }
  f.clear();
  serve(session, read(f, ble_handle, 37, 900), true);
  verify_read(f, ble_handle, 37, 900, 512);

  // A peer cannot READ or CLOSE another peer's handle.
  f.clear();
  serve(session, read(f, lan_handles[0], 0, 10, ble::Link::Lan, 1), true);
  expect_error(f, ble::kErrBadHandle, ble::Link::Lan, 1);
  auto wrong_close = request(f, ble::kOpClose, ble::Link::Lan, 1);
  wrong_close.length = 3;
  put_u16(wrong_close.bytes + 1, lan_handles[0]);
  f.clear();
  serve(session, wrong_close, true);
  expect_error(f, ble::kErrBadHandle, ble::Link::Lan, 1);

  // OPEN and CLOSE invalidate only the requesting slot. A re-OPEN replaces the
  // slot's descriptor rather than adding one.
  const auto reopened = open(f, session, ble::Link::Lan, "sample.parquet", 0);
  assert(reopened != lan_handles[0]);
  assert(descriptor_count() == idle_descriptors + all_slots);
  auto own_close = request(f, ble::kOpClose, ble::Link::Lan, 0);
  own_close.length = 3;
  put_u16(own_close.bytes + 1, reopened);
  f.clear();
  serve(session, own_close, true);
  assert(f.frames.size() == 1 && f.frames[0].peer == 0 &&
         f.frames[0].bytes[0] == ble::kFrameClosed);
  assert(descriptor_count() == idle_descriptors + all_slots - 1);
  f.clear();
  serve(session, read(f, lan_handles[1], 91, 10, ble::Link::Lan, 1), true);
  verify_read(f, lan_handles[1], 91, 10, 1024, ble::Link::Lan, 1);

  // Stale queued OPEN/CLOSE requests cannot mutate a replacement peer.
  auto stale_open = request(f, ble::kOpOpen, ble::Link::Lan, 1);
  const char *sample = "sample.parquet";
  std::memcpy(stale_open.bytes + 1, sample, std::strlen(sample));
  stale_open.length += std::strlen(sample);
  auto stale_close = request(f, ble::kOpClose, ble::Link::Lan, 1);
  stale_close.length = 3;
  put_u16(stale_close.bytes + 1, lan_handles[1]);
  ++f.generation[slot(ble::Link::Lan, 1)];
  session.reconcile();
  assert(descriptor_count() == idle_descriptors + all_slots - 2);
  const auto replacement = open(f, session, ble::Link::Lan, sample, 1);
  assert(descriptor_count() == idle_descriptors + all_slots - 1);
  f.clear();
  serve(session, stale_open, true);
  serve(session, stale_close, true);
  assert(f.frames.empty() && f.errors.empty());
  serve(session, read(f, replacement, 5, 75, ble::Link::Lan, 1), true);
  verify_read(f, replacement, 5, 75, 1024, ble::Link::Lan, 1);
  f.clear();
  serve(session, read(f, lan_handles[2], 80, 32, ble::Link::Lan, 2), true);
  verify_read(f, lan_handles[2], 80, 32, 1024, ble::Link::Lan, 2);
  f.clear();
  serve(session, read(f, ble_handle, 90, 14), true);
  verify_read(f, ble_handle, 90, 14, 512);

  // Disconnect during a multi-frame READ stops delivery immediately, including
  // READ_END: that frame must never leak into the slot's next connection.
  f.clear();
  f.disconnect_frame = 1;
  serve(session, read(f, replacement, 0, 3000, ble::Link::Lan, 1), true);
  assert(f.frames.size() == 1 && f.frames[0].bytes[0] == ble::kFrameChunk);
  assert(f.errors.empty());
  // The next request reconciles the generation change and drops the stream.
  f.clear();
  serve(session, read(f, replacement, 0, 10, ble::Link::Lan, 1), true);
  expect_error(f, ble::kErrBadHandle, ble::Link::Lan, 1);
  assert(descriptor_count() == idle_descriptors + all_slots - 2);
  f.clear();
  serve(session, read(f, lan_handles[2], 0, 10, ble::Link::Lan, 2), true);
  verify_read(f, lan_handles[2], 0, 10, 1024, ble::Link::Lan, 2);
  session.close();
  assert(f.lock_depth == 0);
  assert(descriptor_count() == idle_descriptors);

  persistent_cases(f, session, idle_descriptors);
  remount_cases(f, session, idle_descriptors);
  abort_cases(f, session);
  session.close();
  assert(descriptor_count() == idle_descriptors);

  // File sync is read-only: the synthetic archive remains byte-for-byte equal.
  FILE *file = std::fopen((f.directory + "/sample.parquet").c_str(), "rb");
  assert(file);
  std::vector<std::uint8_t> after(f.bytes.size());
  assert(std::fread(after.data(), 1, after.size(), file) == after.size());
  assert(std::fclose(file) == 0 && after == f.bytes);
}
