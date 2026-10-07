#include "td/actor/actor.h"
#include "td/utils/Time.h"
#include <cassert>
#include <cstdio>
#include <vector>

// Real TDLib scheduler, with the Vita cooperative branch enabled on the host.
// Advance TDLib's test clock instead of relying on host CPU speed or sleeps.
class Worker final : public td::Actor {
 public:
  explicit Worker(std::vector<int>* seen) : seen_(seen) {}
  void work(int value) {
    seen_->push_back(value);
    td::Time::jump_in_future(td::Time::now() + 0.003);
  }
 private:
  std::vector<int>* seen_;
};
class Timer final : public td::Actor {
 public:
  explicit Timer(int* count) : count_(count) {}
  void arm() { set_timeout_in(0); }
  void timeout_expired() final { ++*count_; }
 private:
  int* count_;
};

int main() {
  td::Scheduler scheduler;
  scheduler.init(0, {}, nullptr);
  auto guard = scheduler.get_guard();
  std::vector<int> a, b;
  int timers = 0;
  auto first = td::create_actor<Worker>("first", &a);
  auto second = td::create_actor<Worker>("second", &b);
  auto timer = td::create_actor<Timer>("timer", &timers);
  for (int i = 0; i < 8; ++i) scheduler.run_no_guard(td::Timestamp::in(0));
  td::send_closure(timer, &Timer::arm);
  for (int i = 0; i < 80; ++i) td::send_closure_later(first, &Worker::work, i);
  for (int i = 0; i < 4; ++i) td::send_closure_later(second, &Worker::work, i);
  scheduler.run_no_guard(td::Timestamp::in(1));
  assert(a.size() + b.size() == 1); // positive timeout must also respect budget
  assert(timers == 1);             // timers cannot starve behind mailbox traffic
  scheduler.run_no_guard(td::Timestamp::in(0));
  assert(a.size() == 1 && b.size() == 1); // unvisited actor precedes requeued actor
  for (int i = 0; i < 120; ++i) scheduler.run_no_guard(td::Timestamp::in(0));
  assert(a.size() == 80 && b.size() == 4);
  for (int i = 0; i < 80; ++i) assert(a[i] == i);
  for (int i = 0; i < 4; ++i) assert(b[i] == i);

  // Destroy an actor while it has remaining messages after a budget boundary.
  td::send_closure_later(first, &Worker::work, 80);
  td::send_closure_later(first, &Worker::work, 81);
  scheduler.run_no_guard(td::Timestamp::in(0));
  first.reset();
  for (int i = 0; i < 8; ++i) scheduler.run_no_guard(td::Timestamp::in(0));
  second.reset(); timer.reset();
  for (int i = 0; i < 8; ++i) scheduler.run_no_guard(td::Timestamp::in(0));
  std::puts("TDLib cooperative scheduler tests passed");
}
