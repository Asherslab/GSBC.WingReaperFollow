#include "engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace wf {

extern const char kWingFollowJsfx[];  // generated from src/jsfx/wing_follow.jsfx

const char* const kStripFxAddName = "JS:WING Follow/wing_follow.jsfx";

namespace {

const char* const kSection = "WingFollow";  // global ext state and project ext state section
const char* const kStripFxIdent = "wing_follow";

// Strip FX parameter indices (slider order in wing_follow.jsfx).
enum FxParam { kFxKind = 0, kFxGroup, kFxNum, kFxEnabled, kFxMute, kFxFader, kFxOffset,
               kFxConfigured, kFxStatus, kFxResolved };

std::string GetSetting(const char* key, const char* def) {
  const char* v = GetExtState(kSection, key);
  return v && *v ? v : def;
}
bool GetSettingBool(const char* key, bool def) { return atoi(GetSetting(key, def ? "1" : "0").c_str()) != 0; }
void PutSetting(const char* key, const std::string& v) { SetExtState(kSection, key, v.c_str(), true); }
void PutSettingBool(const char* key, bool v) { PutSetting(key, v ? "1" : "0"); }

bool VolumeDiffers(double a, double b) {
  if (a == b) return false;
  if (a <= 0 || b <= 0) return true;
  return std::fabs(20.0 * std::log10(a / b)) > 0.005;
}

int GroupIndex(const std::string& key) {
  const auto& g = IoGroups();
  for (size_t i = 0; i < g.size(); i++) {
    if (key == g[i].key) return static_cast<int>(i);
  }
  return -1;
}

// Writing FX parameters while a track is in touch/latch/write mode would record automation.
bool AutomationSafe(MediaTrack* tr) {
  int ovr = GetGlobalAutomationOverride();
  int mode = ovr >= 0 ? ovr : GetTrackAutomationMode(tr);
  return mode <= 1 || mode == 5;  // trim/read, read, bypass
}

}  // namespace

void LogLine(const std::string& s) { ShowConsoleMsg((s + "\n").c_str()); }

bool Engine::FxParams::operator==(const FxParams& o) const {
  for (int i = 0; i < 7; i++) {
    if (std::fabs(v[i] - o.v[i]) > 1e-4) return false;
  }
  return true;
}

Engine::FxParams Engine::ParamsOf(const Binding& b) {
  FxParams p;
  p.v[kFxKind] = static_cast<int>(b.target.kind);
  p.v[kFxGroup] = b.target.kind == BindKind::Strip ? static_cast<int>(b.target.strip.type)
                                                   : GroupIndex(b.target.io.group);
  p.v[kFxNum] = b.target.kind == BindKind::Strip ? b.target.strip.index : b.target.io.index;
  p.v[kFxEnabled] = b.enabled;
  p.v[kFxMute] = b.followMute;
  p.v[kFxFader] = b.followFader;
  p.v[kFxOffset] = b.offsetCentiDb / 100.0;
  return p;
}

Engine& Engine::Get() {
  static Engine e;
  return e;
}

// ---- lifecycle ----------------------------------------------------------------------------------

void Engine::Init() {
  LoadSettings();
  client_.reset(new WingClient());
  PushClientConfig();
  client_->SetActive(settings_.connect);
  if (!InstallStripFx()) LogLine("WING Follow: could not install the mixer-strip JSFX into the Effects folder");
}

void Engine::Shutdown() {
  client_.reset();  // joins the network thread
}

void Engine::LoadSettings() {
  settings_.host = GetSetting("host", "");
  settings_.connect = GetSettingBool("connect", false);
  settings_.subscribe = GetSettingBool("subscribe", true);
  settings_.pollMs = std::max(20, atoi(GetSetting("poll_ms", "500").c_str()));
  settings_.logOsc = false;  // never persist debug logging
  int k = atoi(GetSetting("add_kind", "2").c_str());
  settings_.addKind = k == 0 ? BindKind::Strip : k == 1 ? BindKind::Input : BindKind::Output;
  settings_.addGroup = GetSetting("add_group", "CRD");
  if (GroupIndex(settings_.addGroup) < 0) settings_.addGroup = "CRD";
  SrcType t;
  settings_.addStripType = SrcTypeFromKey(GetSetting("add_strip", "ch"), &t) ? t : SrcType::Ch;
}

void Engine::SaveSettings() {
  PutSetting("host", settings_.host);
  PutSettingBool("connect", settings_.connect);
  PutSettingBool("subscribe", settings_.subscribe);
  PutSetting("poll_ms", std::to_string(settings_.pollMs));
  PutSetting("add_kind", std::to_string(static_cast<int>(settings_.addKind)));
  PutSetting("add_group", settings_.addGroup);
  PutSetting("add_strip", GetSrcTypeInfo(settings_.addStripType).key);
}

void Engine::PushClientConfig() {
  if (!client_) return;
  ClientConfig c;
  c.host = settings_.host;
  c.subscribe = settings_.subscribe;
  c.pollMs = settings_.pollMs;
  c.logOsc = settings_.logOsc;
  client_->SetConfig(c);
}

void Engine::Toggled() {
  SaveSettings();
  valuesSerial_++;
  if (onToggleStateChanged) onToggleStateChanged();
}

// ---- settings -----------------------------------------------------------------------------------

void Engine::SetHost(const std::string& host) {
  if (host == settings_.host) return;
  settings_.host = host;
  state_.Clear();  // values from another console are meaningless
  applied_.clear();
  PushClientConfig();
  Toggled();
}

void Engine::SetConnect(bool on) {
  settings_.connect = on;
  if (client_) client_->SetActive(on);
  Toggled();
}

void Engine::SwitchesChanged(bool turnedOn) {
  if (turnedOn) applied_.clear();  // bring every followed track in line right away
  applyNeeded_ = true;
  valuesSerial_++;
  if (onToggleStateChanged) onToggleStateChanged();
}

void Engine::SetMaster(bool on) {
  settings_.sw.master = on;
  PutProjectValue("master", on, true);
  SwitchesChanged(on);
}

void Engine::SetFollowMute(bool on) {
  settings_.sw.mute = on;
  PutProjectValue("follow_mute", on, true);
  SwitchesChanged(on);
}

void Engine::SetFollowFader(bool on) {
  settings_.sw.fader = on;
  PutProjectValue("follow_fader", on, true);
  SwitchesChanged(on);
}

void Engine::SetSubscribe(bool on) {
  settings_.subscribe = on;
  PushClientConfig();
  Toggled();
}

void Engine::SetPollMs(int ms) {
  settings_.pollMs = std::max(20, std::min(ms, 10000));
  PushClientConfig();
  SaveSettings();
}

void Engine::SetLogOsc(bool on) {
  settings_.logOsc = on;
  PushClientConfig();
}

void Engine::SetWindowOpen(bool open) {
  settings_.windowOpen = open;
  // Remembered with the project, but not worth a "save changes?" prompt on its own.
  PutProjectValue("window_open", open, false);
  if (onToggleStateChanged) onToggleStateChanged();
}

void Engine::SetAddDefaults(BindKind kind, const std::string& group, SrcType stripType) {
  settings_.addKind = kind;
  if (GroupIndex(group) >= 0) settings_.addGroup = group;
  settings_.addStripType = stripType;
  SaveSettings();
}

void Engine::Resync() {
  if (client_) client_->RequestResync();
  applied_.clear();
  applyNeeded_ = true;
}

// ---- project / persistence ----------------------------------------------------------------------

void Engine::CheckProject() {
  char file[4096] = "";
  ReaProject* cur = EnumProjects(-1, file, sizeof(file));
  if (cur != project_ || projectFile_ != file || reloadRequested_) {
    // Another tab, another file, or a fresh project (e.g. from the default template).
    project_ = cur;
    projectFile_ = file;
    reloadRequested_ = false;
    applied_.clear();
    fxSeen_.clear();
    bindings_.clear();
    MigrateLegacyBindings();
    LoadBindings();
    LoadProjectSwitches(true);
    stateCount_ = project_ ? GetProjectStateChangeCount(project_) : 0;
    BindingsChanged(false);
    return;
  }
  // Undo/redo, track deletion, pasted or imported tracks: re-read what the tracks carry.
  int sc = project_ ? GetProjectStateChangeCount(project_) : 0;
  if (sc != stateCount_) {
    stateCount_ = sc;
    if (LoadBindings()) BindingsChanged(false);
    LoadProjectSwitches(false);
  }
}

int Engine::GetProjectValue(const char* key, int def) {
  char buf[32] = "";
  if (!project_ || GetProjExtState(project_, kSection, key, buf, sizeof(buf)) <= 0 || !buf[0]) return def;
  return atoi(buf);
}

void Engine::PutProjectValue(const char* key, bool on, bool markDirty) {
  if (!project_) return;
  SetProjExtState(project_, kSection, key, on ? "1" : "0");
  if (markDirty) MarkProjectDirty(project_);
}

void Engine::LoadProjectSwitches(bool applyWindow) {
  FollowSwitches sw;
  sw.master = GetProjectValue("master", 0) != 0;
  sw.mute = GetProjectValue("follow_mute", 1) != 0;
  sw.fader = GetProjectValue("follow_fader", 1) != 0;
  if (sw.master != settings_.sw.master || sw.mute != settings_.sw.mute || sw.fader != settings_.sw.fader) {
    settings_.sw = sw;
    SwitchesChanged(true);
  }
  if (applyWindow) {
    int open = GetProjectValue("window_open", -1);  // -1: project never said, leave it as is
    if (open >= 0 && onWindowStateWanted) onWindowStateWanted(open != 0);
  }
}

namespace {

const char* const kTrackExtKey = "P_EXT:WingFollow";

std::string ReadTrackBindings(MediaTrack* tr) {
  static char buf[65536];
  buf[0] = 0;
  if (!GetSetMediaTrackInfo_String(tr, kTrackExtKey, buf, false)) return "";
  return buf;
}

std::string BindingsKey(const Binding& b) {
  return b.trackGuid + "|" + b.target.Label() + "|" + b.fxGuid;
}

}  // namespace

// Mappings live on the tracks themselves (P_EXT), so they travel with project templates, track
// templates, copies and imports, and come back with undo.
bool Engine::LoadBindings() {
  std::map<std::string, uint32_t> oldIds;
  for (const Binding& b : bindings_) oldIds[BindingsKey(b)] = b.id;

  std::vector<Binding> loaded;
  if (project_) {
    int n = CountTracks(project_);
    for (int i = 0; i < n; i++) {
      MediaTrack* tr = GetTrack(project_, i);
      std::string data = ReadTrackBindings(tr);
      if (data.empty()) continue;
      std::string guid = TrackGuid(tr);
      size_t start = 0;
      while (start <= data.size()) {
        size_t end = data.find(';', start);
        if (end == std::string::npos) end = data.size();
        Binding b;
        if (ParseBinding(data.substr(start, end - start), &b)) {
          b.trackGuid = guid;  // the stored GUID may be stale (template, copied track)
          auto it = oldIds.find(BindingsKey(b));
          b.id = it != oldIds.end() ? it->second : nextId_++;
          loaded.push_back(b);
        }
        start = end + 1;
      }
    }
  }

  bool same = loaded.size() == bindings_.size();
  for (size_t i = 0; same && i < loaded.size(); i++) {
    same = loaded[i].id == bindings_[i].id && SerializeBinding(loaded[i]) == SerializeBinding(bindings_[i]);
  }
  if (same) return false;
  bindings_.swap(loaded);
  return true;
}

void Engine::SaveBindings() {
  if (!project_) return;
  std::map<std::string, std::string> perTrack;
  for (const Binding& b : bindings_) {
    std::string& v = perTrack[b.trackGuid];
    if (!v.empty()) v += ";";
    v += SerializeBinding(b);
  }
  std::vector<std::pair<MediaTrack*, std::string>> writes;
  int n = CountTracks(project_);
  for (int i = 0; i < n; i++) {
    MediaTrack* tr = GetTrack(project_, i);
    auto it = perTrack.find(TrackGuid(tr));
    std::string want = it != perTrack.end() ? it->second : "";
    if (ReadTrackBindings(tr) != want) writes.emplace_back(tr, want);
  }
  if (writes.empty()) return;
  // Its own undo step: otherwise undoing something unrelated later would restore an older copy of
  // the track and silently drop this change.
  Undo_BeginBlock2(project_);
  for (auto& w : writes) GetSetMediaTrackInfo_String(w.first, kTrackExtKey, const_cast<char*>(w.second.c_str()), true);
  Undo_EndBlock2(project_, "WING Follow: change mappings", UNDO_STATE_TRACKCFG);
  MarkProjectDirty(project_);
  stateCount_ = GetProjectStateChangeCount(project_);  // our own write, nothing to reload
}

// Projects saved by 0.1.x kept mappings in project ext state keyed by track GUID: move them onto
// their tracks.
void Engine::MigrateLegacyBindings() {
  if (!project_) return;
  std::vector<std::string> keys;
  std::vector<Binding> legacy;
  char key[64], val[1024];
  for (int i = 0; EnumProjExtState(project_, kSection, i, key, sizeof(key), val, sizeof(val)); i++) {
    if (key[0] != 'b') continue;
    keys.push_back(key);
    Binding b;
    if (ParseBinding(val, &b)) legacy.push_back(b);
  }
  if (keys.empty()) return;
  for (const std::string& k : keys) SetProjExtState(project_, kSection, k.c_str(), "");
  LoadBindings();  // anything already on tracks
  for (Binding& b : legacy) {
    if (!FindTrack(b.trackGuid)) continue;
    bool dupe = false;
    for (const Binding& x : bindings_) dupe |= x.trackGuid == b.trackGuid && x.target == b.target;
    if (dupe) continue;
    b.id = nextId_++;
    bindings_.push_back(b);
  }
  SaveBindings();
  LogLine("WING Follow: moved this project's mappings onto its tracks (save the project to keep this).");
}

void Engine::BindingsChanged(bool save) {
  if (save) SaveBindings();
  bindingsSerial_++;
  applyNeeded_ = true;
  UpdateInterest();
}

// ---- bindings -----------------------------------------------------------------------------------

const Binding* Engine::FindBinding(uint32_t id) const {
  for (const Binding& b : bindings_) {
    if (b.id == id) return &b;
  }
  return nullptr;
}

uint32_t Engine::AddBinding(const std::string& trackGuid, const FollowTarget& target) {
  if (!target.IsValid()) return 0;
  for (const Binding& b : bindings_) {
    if (b.trackGuid == trackGuid && b.target == target) return b.id;  // already mapped
  }
  Binding b;
  b.id = nextId_++;
  b.trackGuid = trackGuid;
  b.target = target;
  bindings_.push_back(b);
  ForgetApplied(trackGuid);
  BindingsChanged(true);
  return b.id;
}

void Engine::UpdateBinding(const Binding& nb) {
  for (Binding& b : bindings_) {
    if (b.id != nb.id) continue;
    if (!nb.target.IsValid()) return;
    b = nb;
    ForgetApplied(b.trackGuid);
    BindingsChanged(true);
    return;
  }
}

void Engine::RemoveBindings(const std::vector<uint32_t>& ids) {
  size_t before = bindings_.size();
  bindings_.erase(std::remove_if(bindings_.begin(), bindings_.end(),
                                 [&](const Binding& b) {
                                   bool rm = std::find(ids.begin(), ids.end(), b.id) != ids.end();
                                   if (rm) ForgetApplied(b.trackGuid);
                                   return rm;
                                 }),
                  bindings_.end());
  if (bindings_.size() != before) BindingsChanged(true);
}

void Engine::SetAllEnabled(bool on) {
  for (Binding& b : bindings_) b.enabled = on;
  applied_.clear();
  BindingsChanged(true);
  if (onToggleStateChanged) onToggleStateChanged();
}

int Engine::AddForSelectedTracks(const FollowTarget& first) {
  if (!project_) return 0;
  int added = 0;
  int n = CountSelectedTracks(project_);
  for (int i = 0; i < n; i++) {
    MediaTrack* tr = GetSelectedTrack(project_, i);
    FollowTarget t = first;
    if (t.kind == BindKind::Strip) t.strip.index += i;
    else t.io.index += i;
    if (!t.IsValid()) break;
    if (AddBinding(TrackGuid(tr), t)) added++;
  }
  return added;
}

int Engine::AddSelectedByTrackNumber(const FollowTarget& proto) {
  if (!project_) return 0;
  int added = 0;
  int n = CountSelectedTracks(project_);
  for (int i = 0; i < n; i++) {
    MediaTrack* tr = GetSelectedTrack(project_, i);
    int num = static_cast<int>(GetMediaTrackInfo_Value(tr, "IP_TRACKNUMBER"));
    FollowTarget t = proto;
    if (t.kind == BindKind::Strip) t.strip.index = num;
    else t.io.index = num;
    if (t.IsValid() && AddBinding(TrackGuid(tr), t)) added++;
  }
  return added;
}

std::vector<uint32_t> Engine::SelectedTrackBindingIds() {
  std::vector<uint32_t> ids;
  if (!project_) return ids;
  std::set<std::string> guids;
  int n = CountSelectedTracks(project_);
  for (int i = 0; i < n; i++) guids.insert(TrackGuid(GetSelectedTrack(project_, i)));
  for (const Binding& b : bindings_) {
    if (guids.count(b.trackGuid)) ids.push_back(b.id);
  }
  return ids;
}

void Engine::ToggleSelectedTracks() {
  std::vector<uint32_t> ids = SelectedTrackBindingIds();
  bool anyOff = false;
  for (uint32_t id : ids) anyOff |= !FindBinding(id)->enabled;
  for (Binding& b : bindings_) {
    if (std::find(ids.begin(), ids.end(), b.id) == ids.end()) continue;
    b.enabled = anyOff;
    ForgetApplied(b.trackGuid);
  }
  if (!ids.empty()) BindingsChanged(true);
}

// ---- tracks -------------------------------------------------------------------------------------

std::string Engine::TrackGuid(MediaTrack* tr) {
  char buf[64] = "";
  if (tr) guidToString(GetTrackGUID(tr), buf);
  return buf;
}

MediaTrack* Engine::FindTrack(const std::string& guid) {
  if (!project_ || guid.empty()) return nullptr;
  int n = CountTracks(project_);
  for (int i = 0; i < n; i++) {
    MediaTrack* tr = GetTrack(project_, i);
    if (TrackGuid(tr) == guid) return tr;
  }
  return nullptr;
}

void Engine::ForgetApplied(const std::string& trackGuid) {
  applied_.erase(trackGuid);
  applyNeeded_ = true;
}

// ---- following ----------------------------------------------------------------------------------

BindingStatus Engine::StatusOf(const Binding& b) const {
  if (!b.enabled || (!b.followMute && !b.followFader)) return BindingStatus::Off;
  if (!settings_.sw.master) return BindingStatus::Paused;
  if (!status_.responding) return BindingStatus::NoConsole;
  if (!Resolve(b.target, state_).ok) return BindingStatus::Unresolved;
  return BindingStatus::Following;
}

void Engine::UpdateInterest() {
  if (!client_) return;
  std::set<std::string> fast, slow;
  for (const Binding& b : bindings_) CollectInterest(b.target, state_, &fast, &slow);
  client_->SetInterest(fast, slow);
}

void Engine::ApplyAll() {
  if (!project_) return;
  std::map<std::string, std::vector<const Binding*>> byTrack;
  for (const Binding& b : bindings_) byTrack[b.trackGuid].push_back(&b);

  // One pass over the project's tracks instead of a GUID search per binding.
  int n = CountTracks(project_);
  for (int i = 0; i < n; i++) {
    MediaTrack* tr = GetTrack(project_, i);
    std::string guid = TrackGuid(tr);
    auto it = byTrack.find(guid);
    if (it == byTrack.end()) continue;

    TrackTarget t = ComputeTrackTarget(it->second, state_, settings_.sw);
    Applied& a = applied_[guid];
    if (t.setVolume) {
      if (!a.hasVol || VolumeDiffers(a.vol, t.volume)) {
        CSurf_OnVolumeChangeEx(tr, t.volume, false, false);
        CSurf_SetSurfaceVolume(tr, t.volume, nullptr);
        a.hasVol = true;
        a.vol = t.volume;
      }
    } else {
      a.hasVol = false;
    }
    if (t.setMute) {
      if (!a.hasMute || a.mute != t.mute) {
        CSurf_OnMuteChangeEx(tr, t.mute ? 1 : 0, false);
        CSurf_SetSurfaceMute(tr, t.mute, nullptr);
        a.hasMute = true;
        a.mute = t.mute;
      }
    } else {
      a.hasMute = false;
    }
  }
}

void Engine::OnTimer() {
  if (!client_) return;
  CheckProject();

  std::vector<std::string> lines;
  client_->DrainLog(&lines);
  // REAPER pops its console window open on every message, so only talk when asked to.
  if (settings_.logOsc) {
    for (const std::string& l : lines) LogLine(l);
  }

  ClientStatus st = client_->GetStatus();
  if (st.connectSerial != status_.connectSerial) {
    applied_.clear();  // re-assert everything after a (re)connect
    applyNeeded_ = true;
  }
  if (st.responding != status_.responding || st.consoleInfo != status_.consoleInfo ||
      st.error != status_.error || st.active != status_.active) {
    valuesSerial_++;
  }
  status_ = st;

  // Routing answers can reveal new addresses to follow; re-derive interest at most 5x/s.
  using Clock = std::chrono::steady_clock;
  static Clock::time_point lastInterest;
  static bool interestDirty = false;
  if (client_->FetchUpdates(&state_)) {
    valuesSerial_++;
    applyNeeded_ = true;
    interestDirty = true;
  }
  if (interestDirty && Clock::now() - lastInterest > std::chrono::milliseconds(200)) {
    interestDirty = false;
    lastInterest = Clock::now();
    UpdateInterest();
  }

  if (applyNeeded_) {
    applyNeeded_ = false;
    ApplyAll();
  }

  if (--fxSyncCountdown_ <= 0) {
    fxSyncCountdown_ = 6;  // ~5 Hz
    SyncStripFx();
  }
}

// ---- mixer-strip FX -----------------------------------------------------------------------------

namespace {

bool IsStripFx(MediaTrack* tr, int fx) {
  char buf[1024] = "";
  if (TrackFX_GetNamedConfigParm(tr, fx, "fx_ident", buf, sizeof(buf))) return strstr(buf, kStripFxIdent) != nullptr;
  // Older REAPER without fx_ident: fall back to the (default) FX name.
  return TrackFX_GetFXName(tr, fx, buf, sizeof(buf)) && strstr(buf, "WING Follow (mixer strip)") != nullptr;
}

void ReadFx(MediaTrack* tr, int fx, double* v, int count) {
  double mn, mx;
  for (int i = 0; i < count; i++) v[i] = TrackFX_GetParam(tr, fx, i, &mn, &mx);
}

}  // namespace

void Engine::SyncStripFx() {
  if (!project_) return;

  auto applyParams = [](const FxParams& p, Binding* b) {
    FollowTarget t;
    int kind = static_cast<int>(std::lround(p.v[kFxKind]));
    int grp = static_cast<int>(std::lround(p.v[kFxGroup]));
    int num = static_cast<int>(std::lround(p.v[kFxNum]));
    if (kind == 0) {
      if (grp < 0 || grp >= static_cast<int>(SrcType::Count)) return false;
      t.kind = BindKind::Strip;
      t.strip = Source{static_cast<SrcType>(grp), num};
    } else {
      if (grp < 0 || grp >= static_cast<int>(IoGroups().size())) return false;
      t.kind = kind == 1 ? BindKind::Input : BindKind::Output;
      t.io = IoRef{IoGroups()[grp].key, num};
    }
    if (!t.IsValid()) return false;
    b->target = t;
    b->enabled = p.v[kFxEnabled] >= 0.5;
    b->followMute = p.v[kFxMute] >= 0.5;
    b->followFader = p.v[kFxFader] >= 0.5;
    b->offsetCentiDb = static_cast<int>(std::lround(p.v[kFxOffset] * 100.0));
    return true;
  };
  auto writeParams = [](MediaTrack* tr, int fx, const FxParams& p) {
    for (int i = 0; i < 7; i++) TrackFX_SetParam(tr, fx, i, p.v[i]);
    TrackFX_SetParam(tr, fx, kFxConfigured, 1.0);
  };

  bool changed = false;
  std::set<std::string> present;
  std::set<std::string> tracksPresent;
  int nt = CountTracks(project_);

  // Where each strip FX lives. A copied track carries its mappings (with the original FX's GUID)
  // while the original FX still exists elsewhere: unlink such stale copies so they re-link to
  // their own track's FX below instead of being mistaken for an FX that moved.
  std::map<std::string, std::string> fxTrack;
  for (int ti = 0; ti < nt; ti++) {
    MediaTrack* tr = GetTrack(project_, ti);
    std::string tg = TrackGuid(tr);
    int nfx = TrackFX_GetCount(tr);
    for (int fx = 0; fx < nfx; fx++) {
      if (!IsStripFx(tr, fx)) continue;
      char g[64] = "";
      guidToString(TrackFX_GetFXGUID(tr, fx), g);
      fxTrack[g] = tg;
    }
  }
  for (Binding& b : bindings_) {
    auto it = fxTrack.find(b.fxGuid);
    if (b.fxGuid.empty() || it == fxTrack.end() || it->second == b.trackGuid) continue;
    for (const Binding& x : bindings_) {
      if (&x != &b && x.fxGuid == b.fxGuid && x.trackGuid == it->second) {
        b.fxGuid.clear();
        changed = true;
        break;
      }
    }
  }

  for (int ti = 0; ti < nt; ti++) {
    MediaTrack* tr = GetTrack(project_, ti);
    std::string trackGuid = TrackGuid(tr);
    tracksPresent.insert(trackGuid);
    int nfx = TrackFX_GetCount(tr);
    std::set<std::string> fxOnTrack;
    for (int fx = 0; fx < nfx; fx++) {
      char g[64] = "";
      guidToString(TrackFX_GetFXGUID(tr, fx), g);
      fxOnTrack.insert(g);
    }
    for (int fx = 0; fx < nfx; fx++) {
      if (!IsStripFx(tr, fx)) continue;
      char fxGuid[64] = "";
      guidToString(TrackFX_GetFXGUID(tr, fx), fxGuid);
      present.insert(fxGuid);

      double raw[10];
      ReadFx(tr, fx, raw, 10);
      FxParams cur;
      std::copy(raw, raw + 7, cur.v);
      bool safe = AutomationSafe(tr);

      Binding* b = nullptr;
      for (Binding& x : bindings_) {
        if (x.fxGuid == fxGuid && (!b || x.trackGuid == trackGuid)) b = &x;
      }

      if (!b) {
        // An FX we don't know: added by hand, or on a copied/imported track. Adopt it.
        Binding nb;
        nb.trackGuid = trackGuid;
        nb.fxGuid = fxGuid;
        nb.fxOwned = true;
        if (raw[kFxConfigured] < 0.5) {
          // Fresh instance: default to the "add" settings, numbered after the track.
          int trackNum = static_cast<int>(GetMediaTrackInfo_Value(tr, "IP_TRACKNUMBER"));
          nb.target.kind = settings_.addKind;
          if (nb.target.kind == BindKind::Strip) {
            nb.target.strip = Source{settings_.addStripType, trackNum};
          } else {
            nb.target.io = IoRef{settings_.addGroup, trackNum};
          }
          if (!nb.target.IsValid()) nb.target = FollowTarget{};  // CH 1
          if (!safe) continue;  // try again once the track leaves write mode
          cur = ParamsOf(nb);
          writeParams(tr, fx, cur);
        } else if (!applyParams(cur, &nb)) {
          continue;
        } else {
          // A copied track (or a new project from a template) brings its mappings along, but the
          // FX gets a new GUID: re-link to the matching mapping instead of adding a duplicate.
          Binding* same = nullptr;
          for (Binding& x : bindings_) {
            if (x.trackGuid == trackGuid && x.target == nb.target &&
                (x.fxGuid.empty() || !fxOnTrack.count(x.fxGuid))) {
              same = &x;
            }
          }
          if (same) {
            same->fxGuid = fxGuid;
            fxSeen_[fxGuid] = cur;
            changed = true;
            continue;
          }
        }
        nb.id = nextId_++;
        bindings_.push_back(nb);
        fxSeen_[fxGuid] = cur;
        ForgetApplied(trackGuid);
        changed = true;
        continue;
      }

      if (b->trackGuid != trackGuid) {  // FX moved to another track
        ForgetApplied(b->trackGuid);
        b->trackGuid = trackGuid;
        ForgetApplied(trackGuid);
        changed = true;
      }
      FxParams fromBinding = ParamsOf(*b);
      auto seen = fxSeen_.find(fxGuid);
      if (seen == fxSeen_.end() || !(cur == seen->second)) {
        // First look since load, or the user clicked the strip: the FX is the truth.
        if (!(cur == fromBinding) && applyParams(cur, b)) {
          ForgetApplied(trackGuid);
          changed = true;
        }
        fxSeen_[fxGuid] = cur;
      } else if (!(cur == fromBinding) && safe) {
        // Edited in the window: push to the strip.
        writeParams(tr, fx, fromBinding);
        fxSeen_[fxGuid] = fromBinding;
      }

      // Status shown in the strip.
      Resolution r = Resolve(b->target, state_);
      double status = static_cast<double>(StatusOf(*b));
      double resolved = r.ok ? (static_cast<int>(r.strip.type) + 1) * 100 + r.strip.index : 0;
      bool stale = std::fabs(raw[kFxStatus] - status) > 0.01 || std::fabs(raw[kFxResolved] - resolved) > 0.01;
      if (safe && stale) {
        TrackFX_SetParam(tr, fx, kFxStatus, status);
        TrackFX_SetParam(tr, fx, kFxResolved, resolved);
      }
    }
  }

  // Linked FX that vanished while its track still exists: the user removed it (or undid adding
  // it). A mapping that the FX created goes with it; one made in the window just loses its view.
  size_t before = bindings_.size();
  for (Binding& b : bindings_) {
    if (b.fxGuid.empty() || present.count(b.fxGuid) || !tracksPresent.count(b.trackGuid)) continue;
    if (!b.fxOwned) {
      b.fxGuid.clear();
      changed = true;
    }
  }
  bindings_.erase(std::remove_if(bindings_.begin(), bindings_.end(),
                                 [&](const Binding& b) {
                                   bool gone = b.fxOwned && !b.fxGuid.empty() && !present.count(b.fxGuid) &&
                                               tracksPresent.count(b.trackGuid);
                                   if (gone) ForgetApplied(b.trackGuid);
                                   return gone;
                                 }),
                  bindings_.end());
  if (bindings_.size() != before) changed = true;

  if (changed) BindingsChanged(true);
}

void Engine::AddStripFx(const std::vector<uint32_t>& ids) {
  if (!project_) return;
  Undo_BeginBlock2(project_);
  int added = 0;
  for (Binding& b : bindings_) {
    if (std::find(ids.begin(), ids.end(), b.id) == ids.end() || !b.fxGuid.empty()) continue;
    MediaTrack* tr = FindTrack(b.trackGuid);
    if (!tr) continue;
    int fx = TrackFX_AddByName(tr, kStripFxAddName, false, -1);
    if (fx < 0) fx = TrackFX_AddByName(tr, "JS:WING Follow/wing_follow", false, -1);
    if (fx < 0) {
      LogLine("WING Follow: could not insert the strip FX (is Effects/WING Follow/wing_follow.jsfx present?)");
      break;
    }
    char fxGuid[64] = "";
    guidToString(TrackFX_GetFXGUID(tr, fx), fxGuid);
    b.fxGuid = fxGuid;
    FxParams p = ParamsOf(b);
    for (int i = 0; i < 7; i++) TrackFX_SetParam(tr, fx, i, p.v[i]);
    TrackFX_SetParam(tr, fx, kFxConfigured, 1.0);
    fxSeen_[fxGuid] = p;
    added++;
  }
  Undo_EndBlock2(project_, "WING Follow: add mixer-strip FX", -1);
  if (added) {
    BindingsChanged(true);
    static bool hinted = false;
    if (!hinted) {
      hinted = true;
      LogLine("WING Follow: added the strip FX. To see it inside the mixer strip, right-click it in "
              "the mixer's FX list and enable \"Show embedded UI\".");
    }
  }
}

// ---- JSFX install -------------------------------------------------------------------------------

bool InstallStripFx() {
  std::string dir = std::string(GetResourcePath()) + "/Effects/WING Follow";
  std::string path = dir + "/wing_follow.jsfx";
  const std::string want(kWingFollowJsfx);

  if (FILE* f = fopen(path.c_str(), "rb")) {
    std::string have;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) have.append(buf, n);
    fclose(f);
    if (have == want) return true;
  }
  RecursiveCreateDirectory(dir.c_str(), 0);
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(want.data(), 1, want.size(), f) == want.size();
  fclose(f);
  return ok;
}

}  // namespace wf
