#include "wing_client.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>

#include "udp.h"

namespace wf {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kSubscribeRenew = std::chrono::milliseconds(7000);  // console drops it after 10 s
constexpr auto kHeartbeat = std::chrono::milliseconds(2000);
constexpr auto kRespondingTimeout = std::chrono::milliseconds(4500);
constexpr auto kReopenDelay = std::chrono::milliseconds(3000);
constexpr auto kDiscoveryDuration = std::chrono::milliseconds(1600);
constexpr int kImmediatePerTick = 16;  // ~1600 queries/s max for bursts after (re)connect
constexpr int kTickMs = 10;

bool StartsWith(const std::string& s, const char* prefix) {
  return s.compare(0, std::strlen(prefix), prefix) == 0;
}

}  // namespace

bool ParseConsoleInfo(const std::string& csv, DiscoveredConsole* out) {
  if (!StartsWith(csv, "WING,")) return false;
  std::vector<std::string> f;
  size_t start = 0;
  for (;;) {
    size_t p = csv.find(',', start);
    f.push_back(csv.substr(start, p == std::string::npos ? std::string::npos : p - start));
    if (p == std::string::npos) break;
    start = p + 1;
  }
  if (f.size() < 3) return false;
  DiscoveredConsole c;
  c.ip = f[1];
  c.name = f[2];
  if (f.size() > 3) c.model = f[3];
  if (f.size() > 4) c.serial = f[4];
  if (f.size() > 5) c.firmware = f[5];
  *out = c;
  return true;
}

WingClient::WingClient() {
  NetInit();
  thread_ = std::thread([this] { Run(); });
}

WingClient::~WingClient() {
  quit_ = true;
  if (thread_.joinable()) thread_.join();
  NetShutdown();
}

void WingClient::SetConfig(const ClientConfig& cfg) {
  std::lock_guard<std::mutex> lock(mu_);
  if (cfg == cfg_) return;
  cfg_ = cfg;
  cfgSerial_++;
}

void WingClient::SetActive(bool active) {
  std::lock_guard<std::mutex> lock(mu_);
  active_ = active;
  status_.active = active;
}

void WingClient::SetInterest(const std::set<std::string>& fast, const std::set<std::string>& slow) {
  std::lock_guard<std::mutex> lock(mu_);
  if (fast == fast_ && slow == slow_) return;
  fast_ = fast;
  slow_ = slow;
  interestSerial_++;
}

void WingClient::RequestResync() {
  std::lock_guard<std::mutex> lock(mu_);
  resyncRequested_ = true;
}

bool WingClient::FetchUpdates(WingState* state) {
  std::map<std::string, OscMessage> got;
  {
    std::lock_guard<std::mutex> lock(mu_);
    got.swap(pending_);
  }
  bool changed = false;
  for (auto& kv : got) changed |= state->Apply(kv.second);
  return changed;
}

ClientStatus WingClient::GetStatus() {
  std::lock_guard<std::mutex> lock(mu_);
  return status_;
}

void WingClient::StartDiscovery() {
  std::lock_guard<std::mutex> lock(mu_);
  discoveryRequested_ = true;
  discovering_ = true;
}

bool WingClient::IsDiscovering() {
  std::lock_guard<std::mutex> lock(mu_);
  return discovering_;
}

std::vector<DiscoveredConsole> WingClient::GetDiscovered() {
  std::lock_guard<std::mutex> lock(mu_);
  return discovered_;
}

void WingClient::DrainLog(std::vector<std::string>* out) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& l : log_) out->push_back(std::move(l));
  log_.clear();
}

void WingClient::Log(const std::string& line) {
  std::lock_guard<std::mutex> lock(mu_);
  if (log_.size() < 2000) log_.push_back(line);
}

void WingClient::NoteConsoleInfo(const std::string& csv, bool fromDiscovery) {
  DiscoveredConsole c;
  if (!ParseConsoleInfo(csv, &c)) return;
  std::lock_guard<std::mutex> lock(mu_);
  if (fromDiscovery) {
    for (const DiscoveredConsole& d : discovered_) {
      if (d.ip == c.ip) return;
    }
    discovered_.push_back(c);
  } else {
    status_.consoleInfo = csv;
  }
}

void WingClient::HandlePacket(const uint8_t* data, int len, bool fromDiscovery) {
  // The native discovery reply (port 2222) is a bare "WING,..." datagram, not OSC.
  if (len >= 5 && std::memcmp(data, "WING,", 5) == 0) {
    NoteConsoleInfo(std::string(reinterpret_cast<const char*>(data), strnlen(reinterpret_cast<const char*>(data), len)),
                    fromDiscovery);
    return;
  }
  std::vector<OscMessage> msgs;
  if (!OscDecode(data, static_cast<size_t>(len), msgs)) return;
  bool logOsc;
  {
    std::lock_guard<std::mutex> lock(mu_);
    logOsc = cfg_.logOsc;
  }
  for (OscMessage& m : msgs) {
    if (logOsc) Log("WING -> " + OscToString(m));
    bool isInfo = false;
    for (const OscArg& a : m.args) {
      if (a.type == 's' && StartsWith(a.s, "WING,")) {
        NoteConsoleInfo(a.s, fromDiscovery);
        isInfo = true;
        break;
      }
    }
    if (isInfo || fromDiscovery) continue;
    std::lock_guard<std::mutex> lock(mu_);
    if (fast_.count(m.address) || slow_.count(m.address)) pending_[m.address] = std::move(m);
  }
}

void WingClient::Run() {
  UdpSocket sock, disco;
  UdpEndpoint console;
  ClientConfig cfg;
  uint32_t seenCfg = ~0u, seenInterest = ~0u;
  std::vector<std::string> fastList, slowList;
  std::set<std::string> queried;
  std::deque<std::string> immediate;
  size_t fastCursor = 0, slowCursor = 0;
  double fastCredit = 0, slowCredit = 0;
  Clock::time_point now = Clock::now(), lastTick = now, lastRx{}, nextSub = now, nextHb = now,
                    nextOpen = now, discoStart{};
  bool responding = false;
  int discoSends = 0;
  std::vector<uint8_t> buf(65536);

  auto send = [&](const std::string& addr) {
    std::vector<uint8_t> pkt = OscEncodeQuery(addr);
    sock.SendTo(console, pkt.data(), pkt.size());
  };
  auto setStatus = [&](auto fn) {
    std::lock_guard<std::mutex> lock(mu_);
    fn(status_);
  };

  while (!quit_) {
    now = Clock::now();
    bool active, resync = false, startDisco = false, hostChanged = false, newInterest = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (cfgSerial_ != seenCfg) {
        hostChanged = cfg.host != cfg_.host || cfg.port != cfg_.port;
        cfg = cfg_;
        seenCfg = cfgSerial_;
      }
      active = active_;
      if (interestSerial_ != seenInterest) {
        fastList.assign(fast_.begin(), fast_.end());
        slowList.assign(slow_.begin(), slow_.end());
        seenInterest = interestSerial_;
        newInterest = true;
      }
      if (resyncRequested_) resync = true, resyncRequested_ = false;
      if (discoveryRequested_) {
        startDisco = true;
        discoveryRequested_ = false;
        discovered_.clear();
      }
    }

    bool wantOpen = active && !cfg.host.empty();
    if (sock.IsOpen() && (!wantOpen || hostChanged)) {
      sock.Close();
      responding = false;
      setStatus([](ClientStatus& s) { s.responding = false, s.consoleInfo.clear(); });
      nextOpen = now;
    }
    if (wantOpen && !sock.IsOpen() && now >= nextOpen) {
      std::string err;
      if (!ResolveIPv4(cfg.host, cfg.port, &console)) {
        err = "Cannot resolve '" + cfg.host + "'";
      } else if (!sock.Open(false, &err)) {
        err = "Network error: " + err;
      }
      if (!err.empty()) {
        nextOpen = now + kReopenDelay;
      } else {
        queried.clear();
        immediate.clear();
        resync = true;
        nextSub = nextHb = now;
        Log("WING Follow: talking to " + console.IpString() + ":" + std::to_string(cfg.port));
      }
      setStatus([&](ClientStatus& s) { s.error = err; });
    }

    if (sock.IsOpen()) {
      if (resync) {
        immediate.clear();
        immediate.insert(immediate.end(), fastList.begin(), fastList.end());
        immediate.insert(immediate.end(), slowList.begin(), slowList.end());
        queried.clear();
      } else if (newInterest) {
        // Query newly added addresses right away so routing resolves quickly.
        for (const auto* list : {&fastList, &slowList}) {
          for (const std::string& a : *list) {
            if (!queried.count(a)) immediate.push_back(a);
          }
        }
      }

      if (cfg.subscribe && now >= nextSub) {
        send("/*S");
        nextSub = now + kSubscribeRenew;
      }
      if (now >= nextHb) {
        send("/?");
        nextHb = now + kHeartbeat;
      }

      for (int n = 0; n < kImmediatePerTick && !immediate.empty(); n++) {
        send(immediate.front());
        queried.insert(immediate.front());
        immediate.pop_front();
      }

      // Spread the periodic re-queries evenly over their interval instead of bursting.
      double dtMs = std::chrono::duration<double, std::milli>(now - lastTick).count();
      auto spread = [&](std::vector<std::string>& list, size_t& cursor, double& credit, int periodMs) {
        if (list.empty()) return;
        credit += list.size() * dtMs / std::max(periodMs, 20);
        credit = std::min(credit, static_cast<double>(list.size()));
        while (credit >= 1.0) {
          send(list[cursor++ % list.size()]);
          credit -= 1.0;
        }
      };
      spread(fastList, fastCursor, fastCredit, cfg.pollMs);
      spread(slowList, slowCursor, slowCredit, cfg.slowPollMs);

      bool nowResponding = lastRx != Clock::time_point{} && now - lastRx < kRespondingTimeout;
      if (nowResponding != responding) {
        responding = nowResponding;
        if (responding) {
          Log("WING Follow: console is responding");
          // Values may have changed while we were away: refresh everything.
          std::lock_guard<std::mutex> lock(mu_);
          resyncRequested_ = true;
          status_.connectSerial++;
        } else {
          Log("WING Follow: console stopped responding");
        }
        setStatus([&](ClientStatus& s) { s.responding = responding; });
      }
    } else if (responding) {
      responding = false;
      setStatus([](ClientStatus& s) { s.responding = false; });
    }
    lastTick = now;

    // Discovery: broadcast both the native "WING?" (port 2222) and OSC "/?" (port 2223).
    if (startDisco) {
      std::string err;
      if (disco.Open(true, &err)) {
        discoStart = now;
        discoSends = 0;
      } else {
        std::lock_guard<std::mutex> lock(mu_);
        discovering_ = false;
      }
    }
    if (disco.IsOpen()) {
      if (discoSends < 3 && now >= discoStart + std::chrono::milliseconds(400 * discoSends)) {
        disco.SendTo(BroadcastEndpoint(kWingDiscoveryPort), "WING?", 5);
        std::vector<uint8_t> q = OscEncodeQuery("/?");
        disco.SendTo(BroadcastEndpoint(kWingOscPort), q.data(), q.size());
        discoSends++;
      }
      if (now >= discoStart + kDiscoveryDuration) {
        disco.Close();
        std::lock_guard<std::mutex> lock(mu_);
        discovering_ = false;
      }
    }

    UdpSocket* socks[2] = {&sock, &disco};
    WaitReadable(socks, 2, kTickMs);

    UdpEndpoint from;
    int n;
    while (sock.IsOpen() && (n = sock.RecvFrom(buf.data(), buf.size(), &from)) > 0) {
      if (from.addr != console.addr) continue;
      lastRx = Clock::now();
      HandlePacket(buf.data(), n, false);
    }
    while (disco.IsOpen() && (n = disco.RecvFrom(buf.data(), buf.size(), &from)) > 0) {
      HandlePacket(buf.data(), n, true);
    }
  }
}

}  // namespace wf
