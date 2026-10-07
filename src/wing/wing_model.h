// WING addressing and the REAPER-independent follow model.
//
// A binding says "this REAPER track follows <something> on the WING". That something is one of:
//   - Strip:  a WING strip directly (CH, AUX, BUS, MAIN, MTX, DCA, mute group).
//   - Input:  a WING input source (e.g. A.5, LCL.3). Resolved to whichever channel currently
//             uses that source as its (main or alt) input, so re-patching on the desk is followed.
//   - Output: a WING output (e.g. CRD.12 = card/Dante output 12). Resolved through the console's
//             output routing to the source feeding it, then on to the channel using that source
//             (or directly to a BUS/MAIN/MTX strip if the output is fed from one).
//
// Faders and mutes only exist on strips; Input/Output bindings end up following a strip.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "osc.h"

namespace wf {

// ---- Strips -------------------------------------------------------------------------------------

enum class SrcType : int { Ch = 0, Aux, Bus, Main, Mtx, Dca, MuteGroup, Count };

struct SrcTypeInfo {
  SrcType type;
  const char* key;    // OSC path segment and serialization key, e.g. "ch"
  const char* label;  // UI label, e.g. "CH"
  int count;          // strips on a full-size WING
  bool hasFader;
  bool hasMute;
};

const SrcTypeInfo& GetSrcTypeInfo(SrcType t);
bool SrcTypeFromKey(const std::string& key, SrcType* out);

struct Source {
  SrcType type = SrcType::Ch;
  int index = 1;  // 1-based, as printed on the console

  bool operator<(const Source& o) const {
    return type != o.type ? type < o.type : index < o.index;
  }
  bool operator==(const Source& o) const { return type == o.type && index == o.index; }
  bool operator!=(const Source& o) const { return !(*this == o); }
  bool IsValid() const;
  std::string Base() const;   // "/ch/1" (WING does not zero-pad)
  std::string Label() const;  // "CH 1"
};

enum class Param { Fader, Mute, Name };
std::string AddressFor(const Source& s, Param p);

// ---- I/O (sources and outputs) ------------------------------------------------------------------

struct IoGroupInfo {
  const char* key;    // "A", "CRD", ...
  const char* label;  // "AES50 A", "Card", ...
  int inputs;         // number of input sources in this group (0 = not an input group)
  int outputs;        // number of outputs in this group (0 = not an output group)
};

const std::vector<IoGroupInfo>& IoGroups();
const IoGroupInfo* FindIoGroup(const std::string& key);

struct IoRef {
  std::string group;  // "A"
  int index = 1;      // 1-based
  bool operator<(const IoRef& o) const {
    return group != o.group ? group < o.group : index < o.index;
  }
  bool operator==(const IoRef& o) const { return group == o.group && index == o.index; }
  std::string Label() const;  // "A.5"
};

// ---- What a binding follows ---------------------------------------------------------------------

enum class BindKind : int { Strip = 0, Input, Output };

struct FollowTarget {
  BindKind kind = BindKind::Strip;
  Source strip;  // kind == Strip
  IoRef io;      // kind == Input / Output

  bool IsValid() const;
  std::string Label() const;  // "CH 1", "IN A.5", "OUT CRD.12"
  bool operator==(const FollowTarget& o) const;
  bool operator!=(const FollowTarget& o) const { return !(*this == o); }
};

// ---- Console state ------------------------------------------------------------------------------

// Last-known values of the OSC addresses we care about, keyed by address.
class WingState {
 public:
  // Stores the message if it is newer information; returns true if the value changed.
  bool Apply(const OscMessage& msg);
  void Clear() { values_.clear(); }

  bool Has(const std::string& addr) const { return values_.count(addr) != 0; }
  bool GetFaderDb(const Source& s, double* db) const;
  bool GetMute(const Source& s, bool* muted) const;
  std::string GetName(const Source& s) const;
  bool GetString(const std::string& addr, std::string* out) const;
  bool GetInt(const std::string& addr, int* out) const;
  bool GetBool(const std::string& addr, bool* out) const;

  const std::map<std::string, OscMessage>& Values() const { return values_; }

 private:
  std::map<std::string, OscMessage> values_;
};

// Value decoding, exposed for tests. WING replies ",sff" (display, raw 0..1, dB) for faders and
// ",sfi" (display, raw, int) for switches; strings/enums come back as ",s".
bool DecodeFaderDb(const OscMessage& msg, double* db);
bool DecodeBool(const OscMessage& msg, bool* value);
bool DecodeInt(const OscMessage& msg, int* value);
bool DecodeString(const OscMessage& msg, std::string* value);

// Result of resolving a FollowTarget against the console's current routing.
struct Resolution {
  bool ok = false;
  Source strip;             // the strip that is followed, when ok
  std::string detail;       // "A.5 -> CH 12" / "not patched to any channel" / "waiting for console"
  std::vector<Source> alsoUsedBy;  // other channels using the same source (lowest one is followed)
};

Resolution Resolve(const FollowTarget& t, const WingState& st);

// OSC addresses the client must query/keep fresh for a target. "fast" = the values being followed
// (fader, mute, names); "slow" = routing information used to resolve Input/Output targets.
void CollectInterest(const FollowTarget& t, const WingState& st, std::set<std::string>* fast,
                     std::set<std::string>* slow);

// ---- Bindings -----------------------------------------------------------------------------------

constexpr double kWingMinusInfDb = -144.0;
constexpr double kReaperMaxDb = 12.0;
double DbToReaperVolume(double db);  // -inf (<= -144) -> 0.0

struct Binding {
  uint32_t id = 0;         // runtime-only, stable while loaded (UI selection, FX link)
  std::string trackGuid;   // "{XXXXXXXX-...}" as produced by REAPER's guidToString
  FollowTarget target;
  bool enabled = true;
  bool followMute = true;
  bool followFader = true;
  int offsetCentiDb = 0;   // added to the WING fader value; integer to avoid locale issues
  std::string fxGuid;      // optional "WING Follow" strip FX showing this binding in the mixer
  bool fxOwned = false;    // created by adding that FX: removing the FX removes the mapping

  double OffsetDb() const { return offsetCentiDb / 100.0; }
};

// "2|{guid}|strip|ch|12|1|1|1|-350"   (version|guid|kind|a|b|enabled|mute|fader|offset centi-dB)
// "2|{guid}|out|CRD|12|1|1|1|0|{fx guid}|1" (FX guid + owned flag only when a strip FX is linked)
std::string SerializeBinding(const Binding& b);
bool ParseBinding(const std::string& s, Binding* b);

struct FollowSwitches {
  bool master = false;  // "Follow WING"
  bool mute = true;     // global: follow mutes at all
  bool fader = true;    // global: follow faders at all
};

// What a REAPER track should be set to. setVolume/setMute false == leave the track alone.
struct TrackTarget {
  bool setVolume = false;
  double volume = 1.0;  // REAPER linear gain
  double sumDb = 0.0;   // for display
  bool setMute = false;
  bool mute = false;
};

// Combines all bindings for one REAPER track the way the console combines them:
//  - fader: dB of every participating strip (plus offsets) are summed, e.g. channel + DCA;
//           -inf anywhere gives -inf;
//  - mute:  muted if ANY participating strip is muted (channel mute, DCA mute, mute group).
// A binding participates only when master, the global switch, the binding and its per-parameter
// flag are all on, its target resolves, and the console value has been received.
TrackTarget ComputeTrackTarget(const std::vector<const Binding*>& bindings, const WingState& st,
                               const FollowSwitches& sw);

}  // namespace wf
