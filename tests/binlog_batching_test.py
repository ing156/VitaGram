"""Check production BinlogActor flush deadlines and forced persistence paths."""
from pathlib import Path
import os
import subprocess
import sys
root, build = map(Path, sys.argv[1:])
source = (root / 'external/tdlib-upstream/tddb/td/db/binlog/ConcurrentBinlog.cpp').read_text()

def method(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end].replace(' final', '') + '\n'

start = source.index('#if TD_VITA\n  // Batch ordinary writes')
end = source.index('#endif', start) + len('#endif')
fixture = r'''
#include <cassert>
#include <cmath>
#include <sstream>
#include <utility>
#include <vector>
#define LOG(level) std::ostringstream()
struct Unit {};
template<class T = Unit> struct Promise {
    bool valid = true;
    explicit operator bool() const { return valid; }
    void set_value(Unit) {}
};
struct Time { static inline double clock = 100; static double now_cached() { return clock; } };
struct Binlog {
    int flushes = 0, syncs = 0, closes = 0;
    double since = 100;
    double need_flush_since() { return since; }
    void flush(const char*) { ++flushes; since = 0; }
    void sync(const char*) { ++syncs; flush("sync"); }
    struct Status { void ensure() {} };
    Status close() { ++closes; sync("close"); return {}; }
};
struct Probe {
    Binlog data;
    Binlog* binlog_ = &data;
    bool force_sync_flag_ = false, lazy_sync_flag_ = false, flush_flag_ = false;
    double wakeup_at_ = 0, alarm = 0;
    std::vector<Promise<>> sync_promises_;
    int completions = 0;
    void set_timeout_at(double value) { alarm = value; }
    void stop() {}
    void set_promises(std::vector<Promise<>>& promises) { completions += promises.size(); promises.clear(); }
'''
fixture += source[start:end] + '\n'
for signature in ['  void wakeup_after(', '  void wakeup_at(', '  void try_flush(',
                  '  void do_immediate_sync(', '  void force_flush(', '  void timeout_expired(', '  void close(Promise']:
    fixture += method(signature)
fixture += r'''
};
int main() {
    Probe p;
    p.try_flush();
    const double delay = TD_VITA ? 0.1 : 0.001;
    assert(p.data.flushes == 0 && p.flush_flag_);
    assert(std::abs(p.alarm - (100 + delay)) < 1e-8);
    Time::clock = 100 + delay / 2;
    p.try_flush(); // More records share the original flush deadline.
    assert(p.data.flushes == 0 && std::abs(p.alarm - (100 + delay)) < 1e-8);
    Time::clock = 100 + delay;
    p.timeout_expired();
    assert(p.data.flushes == 1 && !p.flush_flag_);
    p.data.since = Time::clock;
    p.try_flush();
    p.force_flush();
    assert(p.data.flushes == 2 && !p.flush_flag_);
    Time::clock = p.alarm; p.timeout_expired(); // Consume the timer left pending by force_flush.
    Time::clock = 200;
    p.data.since = 200;
    p.try_flush();
    p.do_immediate_sync(Promise<>{});
    assert(p.force_sync_flag_ && p.alarm > 200 && p.alarm <= 200.003 + 1e-8);
    Time::clock = p.alarm;
    p.timeout_expired();
    assert(p.data.syncs == 1 && p.completions == 1 && !p.force_sync_flag_ && !p.flush_flag_);
    p.data.since = Time::clock;
    p.close(Promise<>{});
    assert(p.data.closes == 1 && p.data.syncs == 2); // Closing still persists without waiting for the batch timer.
}
'''
p = build / 'binlog_batching_test.cpp'
p.write_text(fixture)
for vita in (1, 0):
    binary = build / f'binlog_batching_test-{vita}'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    f'-DTD_VITA={vita}', str(p), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Binlog batching: Vita/host deadlines, shared batching, forced flush, 3 ms sync and close: PASS')
