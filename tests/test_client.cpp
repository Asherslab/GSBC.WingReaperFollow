// Integration test: WingClient against an in-process fake WING on localhost.
// Checks connection, initial query, subscription pushes, polling fallback when the subscription is
// lost (e.g. taken over by Companion), loss/recovery detection, and that the client never sends a
// value-carrying message to the console.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/wing/osc.h"
#include "../src/wing/udp.h"
#include "../src/wing/wing_client.h"
#include "../src/wing/wing_model.h"

using namespace wf;
using Clock = std::chrono::steady_clock;

static int g_failures = 0;
#define CHECK(cond)                                                                     \
  do {                                                                                  \
    if (!(cond)) {                                                                      \
      g_failures++;                                                                     \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                                   \
  } while (0)

// ---- a minimal fake console ---------------------------------------------------------------------

static void PutStr(std::vector<uint8_t>& b, const std::string& s) {
  b.insert(b.end(), s.begin(), s.end());
  b.push_back(0);
  while (b.size() % 4) b.push_back(0);
}
static void PutBE(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(v >> 24), b.push_back(v >> 16), b.push_back(v >> 8), b.push_back(v);
}
static void PutF(std::vector<uint8_t>& b, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  PutBE(b, v);
}

class FakeWing {
 public:
  FakeWing() {
    std::string err;
    sock_.Open(false, &err);
    port = sock_.LocalPort();
    faders_["/ch/1/fdr"] = -10.0f;
    mutes_["/ch/1/mute"] = 0;
    thread_ = std::thread([this] { Run(); });
  }
  ~FakeWing() {
    quit_ = true;
    thread_.join();
  }

  void SetFader(const std::string& addr, float db) {
    std::lock_guard<std::mutex> l(mu_);
    faders_[addr] = db;
    Push(addr);
  }
  void SetMute(const std::string& addr, int m) {
    std::lock_guard<std::mutex> l(mu_);
    mutes_[addr] = m;
    Push(addr);
  }

  uint16_t port = 0;
  std::atomic<bool> online{true};         // false: ignore everything (console unplugged)
  std::atomic<bool> pushEnabled{true};    // false: subscription taken by another client
  std::atomic<int> subscribes{0}, queries{0}, heartbeats{0}, violations{0};

 private:
  std::vector<uint8_t> Reply(const std::string& addr) {
    std::vector<uint8_t> b;
    PutStr(b, addr);
    if (faders_.count(addr)) {
      float db = faders_[addr];
      PutStr(b, ",sff");
      char s[32];
      snprintf(s, sizeof(s), "%.1f", db);
      PutStr(b, db <= -144 ? "-oo" : s);
      PutF(b, 0.5f);
      PutF(b, db);
    } else if (mutes_.count(addr)) {
      PutStr(b, ",sfi");
      PutStr(b, mutes_[addr] ? "1" : "0");
      PutF(b, static_cast<float>(mutes_[addr]));
      PutBE(b, static_cast<uint32_t>(mutes_[addr]));
    } else {
      b.clear();
    }
    return b;
  }
  void Push(const std::string& addr) {  // mu_ held
    if (!pushEnabled || !online || !haveSub_ || Clock::now() > subUntil_) return;
    std::vector<uint8_t> r = Reply(addr);
    if (!r.empty()) sock_.SendTo(sub_, r.data(), r.size());
  }
  void Run() {
    std::vector<uint8_t> buf(65536);
    while (!quit_) {
      UdpSocket* s[1] = {&sock_};
      WaitReadable(s, 1, 10);
      UdpEndpoint from;
      int n;
      while ((n = sock_.RecvFrom(buf.data(), buf.size(), &from)) > 0) {
        if (!online) continue;
        std::vector<OscMessage> msgs;
        if (!OscDecode(buf.data(), n, msgs)) continue;
        std::lock_guard<std::mutex> l(mu_);
        for (const OscMessage& m : msgs) {
          if (!m.args.empty()) {
            violations++;  // the plugin must never set anything on the console
            continue;
          }
          if (m.address == "/*S") {
            subscribes++;
            haveSub_ = true;
            sub_ = from;
            subUntil_ = Clock::now() + std::chrono::seconds(10);
          } else if (m.address == "/?") {
            heartbeats++;
            std::vector<uint8_t> b;
            PutStr(b, "/?");
            PutStr(b, ",s");
            PutStr(b, "WING,127.0.0.1,FAKE,ngc-full,S000,3.0.6");
            sock_.SendTo(from, b.data(), b.size());
          } else {
            queries++;
            std::vector<uint8_t> r = Reply(m.address);
            if (!r.empty()) sock_.SendTo(from, r.data(), r.size());
          }
        }
      }
    }
  }

  UdpSocket sock_;
  std::thread thread_;
  std::atomic<bool> quit_{false};
  std::mutex mu_;
  std::map<std::string, float> faders_;
  std::map<std::string, int> mutes_;
  bool haveSub_ = false;
  UdpEndpoint sub_;
  Clock::time_point subUntil_;
};

// Polls the client until pred(state) holds or the timeout expires.
template <typename P>
static bool WaitFor(WingClient& c, WingState& st, int timeoutMs, P pred) {
  auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
  while (Clock::now() < end) {
    c.FetchUpdates(&st);
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  c.FetchUpdates(&st);
  return pred();
}

int main() {
  NetInit();
  FakeWing wing;
  CHECK(wing.port != 0);

  WingClient client;
  ClientConfig cfg;
  cfg.host = "127.0.0.1";
  cfg.port = wing.port;
  cfg.subscribe = true;
  cfg.pollMs = 300;
  client.SetConfig(cfg);
  client.SetInterest({"/ch/1/fdr", "/ch/1/mute"}, {});
  client.SetActive(true);

  WingState st;
  Source ch1{SrcType::Ch, 1};
  double db = 0;
  bool muted = true;

  // 1. Initial values arrive via query.
  CHECK(WaitFor(client, st, 2000, [&] { return st.GetFaderDb(ch1, &db) && db == -10.0; }));
  CHECK(st.GetMute(ch1, &muted) && !muted);
  CHECK(WaitFor(client, st, 3000, [&] { return client.GetStatus().responding; }));
  DiscoveredConsole info;
  CHECK(ParseConsoleInfo(client.GetStatus().consoleInfo, &info) && info.name == "FAKE");
  CHECK(wing.subscribes >= 1);

  // 2. Subscription push arrives quickly (well under the poll interval).
  auto t0 = Clock::now();
  wing.SetFader("/ch/1/fdr", -3.0f);
  CHECK(WaitFor(client, st, 1000, [&] { return st.GetFaderDb(ch1, &db) && db == -3.0; }));
  auto pushMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
  std::printf("push latency: %lld ms\n", static_cast<long long>(pushMs));
  CHECK(pushMs < 150);

  // 3. Subscription lost to another app: polling still catches up within ~pollMs.
  wing.pushEnabled = false;
  wing.SetMute("/ch/1/mute", 1);
  t0 = Clock::now();
  CHECK(WaitFor(client, st, 2000, [&] { return st.GetMute(ch1, &muted) && muted; }));
  auto pollMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
  std::printf("poll fallback latency: %lld ms\n", static_cast<long long>(pollMs));
  CHECK(pollMs < 1000);
  wing.pushEnabled = true;

  // 4. Console disappears, then comes back: detected, and serial bumps so the engine re-applies.
  uint32_t serial = client.GetStatus().connectSerial;
  wing.online = false;
  CHECK(WaitFor(client, st, 7000, [&] { return !client.GetStatus().responding; }));
  wing.SetFader("/ch/1/fdr", -20.0f);  // changed while unreachable
  wing.online = true;
  CHECK(WaitFor(client, st, 4000, [&] { return client.GetStatus().responding; }));
  CHECK(client.GetStatus().connectSerial == serial + 1);
  CHECK(WaitFor(client, st, 2000, [&] { return st.GetFaderDb(ch1, &db) && db == -20.0; }));

  // 5. Disconnect stops traffic.
  client.SetActive(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  int q = wing.queries;
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  CHECK(wing.queries == q);

  // The one-way guarantee.
  CHECK(wing.violations == 0);

  std::printf("queries=%d subscribes=%d heartbeats=%d violations=%d\n", wing.queries.load(),
              wing.subscribes.load(), wing.heartbeats.load(), wing.violations.load());
  std::printf("%s\n", g_failures ? "FAILED" : "OK");
  NetShutdown();
  return g_failures ? 1 : 0;
}
