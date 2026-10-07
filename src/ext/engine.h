// The follow engine: owns the WING client, the current project's bindings and the global
// settings, and applies console state to REAPER tracks. Everything here runs on REAPER's main
// thread (timer callback, UI, actions); only WingClient has its own thread.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../wing/wing_client.h"
#include "../wing/wing_model.h"
#include "reaper_api.h"

namespace wf {

struct Settings {
  std::string host;
  bool connect = false;      // user wants to talk to the console
  bool subscribe = true;
  int pollMs = 500;
  bool logOsc = false;
  FollowSwitches sw;         // master / mute / fader quick switches
  bool windowOpen = false;
  // Defaults for new mappings (dock "add" row and freshly inserted strip FX).
  BindKind addKind = BindKind::Output;
  std::string addGroup = "CRD";  // for Input/Output kinds
  SrcType addStripType = SrcType::Ch;
};

// Status of one binding as shown in the UI and the strip FX.
enum class BindingStatus : int { Unknown = 0, Following = 1, Paused = 2, NoConsole = 3, Unresolved = 4, Off = 5 };

class Engine {
 public:
  static Engine& Get();

  void Init();
  void Shutdown();
  void OnTimer();  // ~30 Hz from REAPER's "timer" hook
  void OnProjectLoad() { reloadRequested_ = true; }

  // ---- settings / quick switches ----
  const Settings& GetSettings() const { return settings_; }
  void SetHost(const std::string& host);
  void SetConnect(bool on);
  void SetMaster(bool on);
  void SetFollowMute(bool on);
  void SetFollowFader(bool on);
  void SetSubscribe(bool on);
  void SetPollMs(int ms);
  void SetLogOsc(bool on);
  void SetWindowOpen(bool open);
  void SetAddDefaults(BindKind kind, const std::string& group, SrcType stripType);
  void Resync();

  // ---- bindings (current project) ----
  const std::vector<Binding>& Bindings() const { return bindings_; }
  const Binding* FindBinding(uint32_t id) const;
  uint32_t AddBinding(const std::string& trackGuid, const FollowTarget& target);
  void UpdateBinding(const Binding& b);  // matched by id
  void RemoveBindings(const std::vector<uint32_t>& ids);
  void SetAllEnabled(bool on);            // "Enable all" / "Disable all"
  // Adds bindings for the selected tracks, numbering targets upward from firstIndex.
  int AddForSelectedTracks(const FollowTarget& first);
  // Adds bindings for the selected tracks, target number = REAPER track number (Dante N -> track N).
  int AddSelectedByTrackNumber(const FollowTarget& proto);
  // Toggles "enabled" on every binding of the selected tracks (all on if any is off).
  void ToggleSelectedTracks();
  std::vector<uint32_t> SelectedTrackBindingIds();
  void AddStripFx(const std::vector<uint32_t>& ids);  // insert the mixer-strip FX for bindings

  // ---- views ----
  MediaTrack* FindTrack(const std::string& guid);
  std::string TrackGuid(MediaTrack* tr);
  Resolution ResolveBinding(const Binding& b) const { return Resolve(b.target, state_); }
  BindingStatus StatusOf(const Binding& b) const;
  const WingState& State() const { return state_; }
  const ClientStatus& ConsoleStatus() const { return status_; }
  WingClient* Client() { return client_.get(); }

  // Bumped whenever the binding list / console values / status change (UI refresh triggers).
  uint32_t BindingsSerial() const { return bindingsSerial_; }
  uint32_t ValuesSerial() const { return valuesSerial_; }

  // Called when toggle-able state changes so toolbar buttons can refresh.
  void (*onToggleStateChanged)() = nullptr;

 private:
  void LoadSettings();
  void SaveSettings();
  void PushClientConfig();
  void CheckProject();
  void LoadBindings();
  void SaveBindings();
  void BindingsChanged(bool save);
  void UpdateInterest();
  void ApplyAll();
  void ForgetApplied(const std::string& trackGuid);  // next apply sets the track even if unchanged
  void SyncStripFx();
  void Toggled();

  std::unique_ptr<WingClient> client_;
  Settings settings_;
  ReaProject* project_ = nullptr;
  bool reloadRequested_ = true;
  std::vector<Binding> bindings_;
  uint32_t nextId_ = 1;
  WingState state_;
  ClientStatus status_;
  uint32_t bindingsSerial_ = 1, valuesSerial_ = 1;
  bool applyNeeded_ = true;
  int fxSyncCountdown_ = 0;

  struct Applied {
    bool hasVol = false;
    double vol = 0;
    bool hasMute = false;
    bool mute = false;
  };
  std::map<std::string, Applied> applied_;  // by track GUID

  // Last parameter values seen on each linked strip FX (by FX GUID) to tell who changed what.
  struct FxParams {
    double v[7] = {0};
    bool operator==(const FxParams& o) const;
  };
  static FxParams ParamsOf(const Binding& b);
  std::map<std::string, FxParams> fxSeen_;
};

// Strip FX identity.
extern const char* const kStripFxAddName;  // for TrackFX_AddByName
bool InstallStripFx();                     // writes the JSFX into <resource>/Effects when outdated

void LogLine(const std::string& s);

}  // namespace wf
