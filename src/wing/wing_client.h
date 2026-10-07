// Background UDP client for the WING's OSC server (port 2223).
//
// The client knows nothing about REAPER. The main thread tells it which OSC addresses it is
// interested in; the client keeps those fresh (subscription + paced polling) and hands back the
// received values. It only ever sends argument-less messages (queries, /*S, /?), never values.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "osc.h"
#include "wing_model.h"

namespace wf {

constexpr uint16_t kWingOscPort = 2223;
constexpr uint16_t kWingDiscoveryPort = 2222;

struct ClientConfig {
  std::string host;         // console IP (or hostname)
  uint16_t port = kWingOscPort;
  bool subscribe = true;    // keep an OSC subscription (/*S) alive for instant updates
  int pollMs = 500;         // re-query followed values this often (safety net / poll-only mode)
  int slowPollMs = 3000;    // re-query routing information this often
  bool logOsc = false;      // log traffic to the REAPER console

  bool operator==(const ClientConfig& o) const {
    return host == o.host && port == o.port && subscribe == o.subscribe && pollMs == o.pollMs &&
           slowPollMs == o.slowPollMs && logOsc == o.logOsc;
  }
  bool operator!=(const ClientConfig& o) const { return !(*this == o); }
};

struct ClientStatus {
  bool active = false;      // user wants to be connected
  bool responding = false;  // console answered recently
  std::string consoleInfo;  // "WING,192.168.1.62,FOH,ngc-full,S/N,3.0.6" when known
  std::string error;
  uint32_t connectSerial = 0;  // increments each time the console (re)starts responding
};

struct DiscoveredConsole {
  std::string ip, name, model, serial, firmware;
};

class WingClient {
 public:
  WingClient();
  ~WingClient();
  WingClient(const WingClient&) = delete;
  WingClient& operator=(const WingClient&) = delete;

  void SetConfig(const ClientConfig& cfg);
  void SetActive(bool active);
  void SetInterest(const std::set<std::string>& fast, const std::set<std::string>& slow);
  void RequestResync();  // re-query everything of interest now

  // Moves all values received since the last call into *state. Returns true if anything changed.
  bool FetchUpdates(WingState* state);
  ClientStatus GetStatus();

  void StartDiscovery();
  bool IsDiscovering();
  std::vector<DiscoveredConsole> GetDiscovered();

  void DrainLog(std::vector<std::string>* out);

 private:
  void Run();
  void Log(const std::string& line);
  void HandlePacket(const uint8_t* data, int len, bool fromDiscovery);
  void NoteConsoleInfo(const std::string& csv, bool fromDiscovery);

  std::thread thread_;
  std::atomic<bool> quit_{false};

  std::mutex mu_;
  ClientConfig cfg_;
  uint32_t cfgSerial_ = 0;
  bool active_ = false;
  std::set<std::string> fast_, slow_;
  uint32_t interestSerial_ = 0;
  bool resyncRequested_ = false;
  std::map<std::string, OscMessage> pending_;
  ClientStatus status_;
  bool discoveryRequested_ = false;
  bool discovering_ = false;
  std::vector<DiscoveredConsole> discovered_;
  std::vector<std::string> log_;
};

// Parses "WING,<ip>,<name>,<model>,<serial>,<firmware>".
bool ParseConsoleInfo(const std::string& csv, DiscoveredConsole* out);

}  // namespace wf
