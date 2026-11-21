#include "os_logger_copy_candidates.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <string>
#include <vector>

// The copy back candidate list of #215. Every check compares the list with
// the copy order a full walk picks, one name at a time, as copy_back_next()
// did before the list.
namespace {
namespace cc = opensensor::logger::copy_candidates;
namespace fc = opensensor::logger::flash_copy;

using Names = std::vector<std::string>;

// The order copy back takes: the first of `names` by copy_before, again and
// again, which is what one walk per step picked.
Names walk_order(Names names) {
  std::sort(names.begin(), names.end(),
            [](const std::string &a, const std::string &b) {
              return fc::copy_before(a.c_str(), b.c_str());
            });
  return names;
}

template <std::size_t N> Names drain(cc::List<N> &list) {
  Names out;
  while (const char *name = list.front()) {
    out.emplace_back(name);
    list.pop_front();
  }
  return out;
}

// Copies every name in order with walks of a list of N, the way the worker
// does: walk, take until empty, walk again.
template <std::size_t N> std::pair<Names, std::size_t> copy_all(Names names) {
  cc::List<N> list;
  Names copied;
  std::size_t walks = 0;
  for (;;) {
    list.clear();
    ++walks;
    for (const auto &name : names)
      list.offer(name.c_str());
    if (list.empty())
      break;
    while (const char *name = list.front()) {
      copied.emplace_back(name);
      std::erase(names, std::string{name});
      list.pop_front();
    }
  }
  return {copied, walks};
}

const Names kMixed = {
    "thing=t/year=2026/month=11/day=21/b-2-2.json",
    "quarantine/thing=t/a-1.partial",
    "thing=t/year=2026/month=11/day=20/a-1-1.parquet",
    "flash-conflict/thing=t/year=2026/month=11/day=19/c-3-3.parquet",
    "thing=t/year=2026/month=11/day=21/b-2-2.parquet",
    "thing=t/year=2026/month=11/day=20/a-1-1.json",
    "quarantine/thing=t/z-9.partial",
    "thing=t/year=2026/month=11/day=22/d-4-4.parquet",
};

void test_empty() {
  cc::List<4> list;
  assert(list.empty());
  assert(list.size() == 0);
  assert(list.front() == nullptr);
  list.pop_front(); // a no-op on an empty list
  assert(list.empty());
}

void test_keeps_copy_order() {
  cc::List<16> list;
  for (const auto &name : kMixed)
    assert(list.offer(name.c_str()));
  assert(list.size() == kMixed.size());
  assert(drain(list) == walk_order(kMixed));
}

void test_parquet_before_sidecar() {
  cc::List<2> list;
  list.offer("x/a.json");
  list.offer("x/b.parquet");
  list.offer("quarantine/a.partial");
  assert(std::string{list.front()} == "x/b.parquet");
  list.pop_front();
  assert(std::string{list.front()} == "x/a.json");
}

void test_full_list_keeps_the_first() {
  // Offered in reverse, so every offer goes before the last entry.
  Names reversed = walk_order(kMixed);
  std::reverse(reversed.begin(), reversed.end());
  cc::List<3> list;
  for (const auto &name : reversed)
    list.offer(name.c_str());
  assert(list.size() == 3);
  const Names order = walk_order(kMixed);
  assert(drain(list) == Names(order.begin(), order.begin() + 3));
}

void test_would_keep() {
  cc::List<2> list;
  assert(list.would_keep("a.parquet"));
  assert(!list.would_keep(nullptr));
  list.offer("b.parquet");
  list.offer("c.parquet");
  // Full: only a name before the last entry is kept.
  assert(list.would_keep("a.parquet"));
  assert(!list.would_keep("d.parquet"));
  assert(!list.would_keep("a.json"));
  assert(!list.offer("d.parquet"));
  assert(list.size() == 2);
  // A list being taken from takes no new names until it is cleared.
  list.pop_front();
  assert(!list.would_keep("a.parquet"));
  assert(!list.offer("a.parquet"));
  list.clear();
  assert(list.empty());
  assert(list.offer("a.parquet"));
}

void test_name_bounds() {
  cc::List<2> list;
  std::string longest(cc::List<2>::kNameBytes - 1, 'a');
  longest.replace(longest.size() - 8, 8, ".parquet");
  assert(list.offer(longest.c_str()));
  std::string too_long(cc::List<2>::kNameBytes, 'a');
  too_long.replace(too_long.size() - 8, 8, ".parquet");
  assert(!list.would_keep(too_long.c_str()));
  assert(!list.offer(too_long.c_str()));
  assert(list.size() == 1);
  assert(std::string{list.front()} == longest);
}

void test_same_order_as_a_walk_per_step() {
  // Many files and a small list: the copies come in the order a walk per
  // step gives, with one walk per kCapacity files and a last empty walk.
  Names names;
  for (int at = 0; at < 50; ++at) {
    const std::string stem =
        "thing=t/year=2026/month=11/day=" + std::to_string(10 + at % 17) +
        "/f-" + std::to_string(at * 7919 % 1000);
    names.push_back(stem + ".parquet");
    names.push_back(stem + ".json");
    if (at % 9 == 0)
      names.push_back("quarantine/" + stem + ".partial");
  }
  const auto [copied, walks] = copy_all<4>(names);
  assert(copied == walk_order(names));
  assert(walks == (names.size() + 3) / 4 + 1);
  // A list of one is a walk per file, the old behaviour.
  const auto [single, every] = copy_all<1>(names);
  assert(single == walk_order(names));
  assert(every == names.size() + 1);
}

void test_clear_refills_after_a_change() {
  // A name that appears after the walk is not taken until the next fill.
  cc::List<4> list;
  list.offer("b.parquet");
  list.offer("c.parquet");
  list.pop_front();
  list.clear();
  for (const char *name : {"a.parquet", "c.parquet"})
    list.offer(name);
  assert(std::string{list.front()} == "a.parquet");
}

} // namespace

int main() {
  test_empty();
  test_keeps_copy_order();
  test_parquet_before_sidecar();
  test_full_list_keeps_the_first();
  test_would_keep();
  test_name_bounds();
  test_same_order_as_a_walk_per_step();
  test_clear_refills_after_a_change();
  return 0;
}
