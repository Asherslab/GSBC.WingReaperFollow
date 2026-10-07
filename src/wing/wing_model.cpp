#include "wing_model.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wf {

namespace {

const SrcTypeInfo kSrcTypes[] = {
    {SrcType::Ch, "ch", "CH", 40, true, true},
    {SrcType::Aux, "aux", "AUX", 8, true, true},
    {SrcType::Bus, "bus", "BUS", 16, true, true},
    {SrcType::Main, "main", "MAIN", 4, true, true},
    {SrcType::Mtx, "mtx", "MTX", 8, true, true},
    {SrcType::Dca, "dca", "DCA", 16, true, true},
    {SrcType::MuteGroup, "mgrp", "MGRP", 8, false, true},
};
static_assert(sizeof(kSrcTypes) / sizeof(kSrcTypes[0]) == static_cast<size_t>(SrcType::Count),
              "kSrcTypes must list every SrcType in order");

bool ParseInt(const std::string& s, int* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  long v = std::strtol(s.c_str(), &end, 10);
  if (*end) return false;
  *out = static_cast<int>(v);
  return true;
}

// Locale-independent parse of display strings like "-3.0", "+2.5", "-oo", "-3.0dB".
bool ParseDbString(const std::string& in, double* out) {
  std::string s;
  for (char c : in) {
    if (c != ' ') s += c;
  }
  if (s == "-oo" || s == "-inf" || s == "-INF") {
    *out = kWingMinusInfDb;
    return true;
  }
  size_t i = 0;
  bool neg = false;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
  double v = 0;
  bool digits = false;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0'), digits = true;
  if (i < s.size() && s[i] == '.') {
    i++;
    double scale = 0.1;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      v += (s[i++] - '0') * scale;
      scale /= 10;
      digits = true;
    }
  }
  if (!digits) return false;
  std::string rest = s.substr(i);
  if (!rest.empty() && rest != "dB" && rest != "db") return false;
  *out = neg ? -v : v;
  return true;
}

// Strip types a routing "grp" string can name (in /io/out and /io/user).
bool StripFromRoutingGroup(const std::string& grp, bool allowChAux, SrcType* out) {
  if (grp == "BUS") return *out = SrcType::Bus, true;
  if (grp == "MAIN") return *out = SrcType::Main, true;
  if (grp == "MTX") return *out = SrcType::Mtx, true;
  if (allowChAux && grp == "CH") return *out = SrcType::Ch, true;
  if (allowChAux && grp == "AUX") return *out = SrcType::Aux, true;
  return false;
}

std::string OutAddr(const IoRef& o, const char* leaf) {
  return "/io/out/" + o.group + "/" + std::to_string(o.index) + "/" + leaf;
}
std::string InAddr(const IoRef& i, const char* leaf) {
  return "/io/in/" + i.group + "/" + std::to_string(i.index) + "/" + leaf;
}
std::string UserAddr(int n, const char* leaf) {
  return "/io/user/" + std::to_string(n) + "/" + leaf;
}

// Channels that can take an input source: CH 1..40 and AUX 1..8.
template <typename F>
void ForEachInputStrip(F f) {
  for (SrcType t : {SrcType::Ch, SrcType::Aux}) {
    for (int i = 1; i <= GetSrcTypeInfo(t).count; i++) f(Source{t, i});
  }
}

const char* const kConnLeaves[] = {"in/conn/grp", "in/conn/in", "in/conn/altgrp", "in/conn/altin",
                                   "in/set/altsrc"};

}  // namespace

// ---- Strips -------------------------------------------------------------------------------------

const SrcTypeInfo& GetSrcTypeInfo(SrcType t) {
  int i = static_cast<int>(t);
  if (i < 0 || i >= static_cast<int>(SrcType::Count)) i = 0;
  return kSrcTypes[i];
}

bool SrcTypeFromKey(const std::string& key, SrcType* out) {
  for (const SrcTypeInfo& info : kSrcTypes) {
    if (key == info.key) {
      *out = info.type;
      return true;
    }
  }
  return false;
}

bool Source::IsValid() const {
  int t = static_cast<int>(type);
  return t >= 0 && t < static_cast<int>(SrcType::Count) && index >= 1 &&
         index <= GetSrcTypeInfo(type).count;
}

std::string Source::Base() const {
  return std::string("/") + GetSrcTypeInfo(type).key + "/" + std::to_string(index);
}

std::string Source::Label() const {
  return std::string(GetSrcTypeInfo(type).label) + " " + std::to_string(index);
}

std::string AddressFor(const Source& s, Param p) {
  switch (p) {
    case Param::Fader: return s.Base() + "/fdr";
    case Param::Mute: return s.Base() + "/mute";
    case Param::Name: return s.Base() + "/name";
  }
  return s.Base();
}

// ---- I/O ----------------------------------------------------------------------------------------

const std::vector<IoGroupInfo>& IoGroups() {
  // Counts from the WING OSC documentation's io tree (full-size WING).
  static const std::vector<IoGroupInfo> groups = {
      {"LCL", "Local", 8, 8},        {"AUX", "Aux", 8, 8},
      {"A", "AES50 A", 48, 48},      {"B", "AES50 B", 48, 48},
      {"C", "AES50 C", 48, 48},      {"SC", "StageConnect", 32, 32},
      {"USB", "USB", 48, 48},        {"CRD", "Card", 64, 64},
      {"MOD", "Module", 64, 64},     {"PLAY", "Player", 4, 0},
      {"REC", "Recorder", 0, 4},     {"AES", "AES/EBU", 2, 2},
      {"USR", "User signal", 24, 0}, {"OSC", "Oscillator", 2, 0},
  };
  return groups;
}

const IoGroupInfo* FindIoGroup(const std::string& key) {
  for (const IoGroupInfo& g : IoGroups()) {
    if (key == g.key) return &g;
  }
  return nullptr;
}

std::string IoRef::Label() const { return group + "." + std::to_string(index); }

bool FollowTarget::IsValid() const {
  switch (kind) {
    case BindKind::Strip: return strip.IsValid();
    case BindKind::Input: {
      const IoGroupInfo* g = FindIoGroup(io.group);
      return g && io.index >= 1 && io.index <= g->inputs;
    }
    case BindKind::Output: {
      const IoGroupInfo* g = FindIoGroup(io.group);
      return g && io.index >= 1 && io.index <= g->outputs;
    }
  }
  return false;
}

std::string FollowTarget::Label() const {
  switch (kind) {
    case BindKind::Strip: return strip.Label();
    case BindKind::Input: return "IN " + io.Label();
    case BindKind::Output: return "OUT " + io.Label();
  }
  return "?";
}

bool FollowTarget::operator==(const FollowTarget& o) const {
  if (kind != o.kind) return false;
  return kind == BindKind::Strip ? strip == o.strip : io == o.io;
}

// ---- Values -------------------------------------------------------------------------------------

bool DecodeFaderDb(const OscMessage& msg, double* db) {
  // ",sff": display string, raw position 0..1, value in dB -> the last float is dB.
  std::vector<float> floats;
  const std::string* str = nullptr;
  for (const OscArg& a : msg.args) {
    if (a.type == 'f') floats.push_back(a.f);
    else if (a.type == 's' && !str) str = &a.s;
  }
  double v;
  if (floats.size() >= 2) {
    v = floats.back();
  } else if (str && ParseDbString(*str, &v)) {
    // display string carries the dB value
  } else if (floats.size() == 1) {
    v = floats[0];  // a lone float on an fdr leaf is dB per the WING docs
  } else {
    return false;
  }
  if (!std::isfinite(v)) return false;
  if (v < kWingMinusInfDb) v = kWingMinusInfDb;
  *db = v;
  return true;
}

bool DecodeInt(const OscMessage& msg, int* value) {
  for (const OscArg& a : msg.args) {
    if (a.type == 'i') return *value = a.i, true;
  }
  for (const OscArg& a : msg.args) {
    if (a.type == 's' && ParseInt(a.s, value)) return true;
  }
  for (const OscArg& a : msg.args) {
    if (a.type == 'f') return *value = static_cast<int>(std::lround(a.f)), true;
  }
  return false;
}

bool DecodeBool(const OscMessage& msg, bool* value) {
  for (const OscArg& a : msg.args) {
    if (a.type == 'i') return *value = a.i != 0, true;
  }
  for (const OscArg& a : msg.args) {
    if (a.type == 'f') return *value = a.f >= 0.5f, true;
  }
  for (const OscArg& a : msg.args) {
    if (a.type != 's') continue;
    if (a.s == "1" || a.s == "ON" || a.s == "on") return *value = true, true;
    if (a.s == "0" || a.s == "OFF" || a.s == "off") return *value = false, true;
  }
  return false;
}

bool DecodeString(const OscMessage& msg, std::string* value) {
  for (const OscArg& a : msg.args) {
    if (a.type == 's') return *value = a.s, true;
  }
  return false;
}

bool WingState::Apply(const OscMessage& msg) {
  auto it = values_.find(msg.address);
  if (it == values_.end()) {
    values_.emplace(msg.address, msg);
    return true;
  }
  // Compare the decoded meaning, not the raw bytes (the raw 0..1 float can jitter).
  const OscMessage& old = it->second;
  bool same = old.args.size() == msg.args.size();
  for (size_t i = 0; same && i < msg.args.size(); i++) {
    const OscArg& a = old.args[i];
    const OscArg& b = msg.args[i];
    same = a.type == b.type && a.i == b.i && a.s == b.s && std::fabs(a.f - b.f) < 1e-5f;
  }
  if (same) return false;
  it->second = msg;
  return true;
}

bool WingState::GetFaderDb(const Source& s, double* db) const {
  auto it = values_.find(AddressFor(s, Param::Fader));
  return it != values_.end() && DecodeFaderDb(it->second, db);
}

bool WingState::GetMute(const Source& s, bool* muted) const {
  auto it = values_.find(AddressFor(s, Param::Mute));
  return it != values_.end() && DecodeBool(it->second, muted);
}

std::string WingState::GetName(const Source& s) const {
  std::string n;
  GetString(AddressFor(s, Param::Name), &n);
  return n;
}

bool WingState::GetString(const std::string& addr, std::string* out) const {
  auto it = values_.find(addr);
  return it != values_.end() && DecodeString(it->second, out);
}

bool WingState::GetInt(const std::string& addr, int* out) const {
  auto it = values_.find(addr);
  return it != values_.end() && DecodeInt(it->second, out);
}

bool WingState::GetBool(const std::string& addr, bool* out) const {
  auto it = values_.find(addr);
  return it != values_.end() && DecodeBool(it->second, out);
}

// ---- Resolution ---------------------------------------------------------------------------------

namespace {

bool IsStereoSource(const IoRef& src, const WingState& st) {
  std::string mode;
  return st.GetString(InAddr(src, "mode"), &mode) && (mode == "ST" || mode == "MS" || mode == "M/S");
}

Resolution ResolveInput(const IoRef& src, const WingState& st, const std::string& prefix) {
  Resolution r;
  // A stereo source occupies two slots but is patched by its first one (e.g. USB 1/2 -> USB.1).
  IoRef pairStart{src.group, src.index - 1};
  bool secondOfPair = src.index > 1 && IsStereoSource(pairStart, st);

  int missing = 0;
  std::vector<Source> users;
  ForEachInputStrip([&](const Source& ch) {
    std::string base = ch.Base() + "/";
    bool alt = false;
    st.GetBool(base + "in/set/altsrc", &alt);
    std::string grp;
    int in = 0;
    bool haveGrp = st.GetString(base + (alt ? "in/conn/altgrp" : "in/conn/grp"), &grp);
    bool haveIn = st.GetInt(base + (alt ? "in/conn/altin" : "in/conn/in"), &in);
    if (!haveGrp || !haveIn) {
      missing++;
      return;
    }
    if (grp != src.group) return;
    if (in == src.index || (secondOfPair && in == pairStart.index)) users.push_back(ch);
  });

  if (users.empty()) {
    r.detail = prefix + src.Label() + (missing ? ": waiting for console routing"
                                               : ": not patched to any channel");
    return r;
  }
  r.ok = true;
  r.strip = users[0];  // CH before AUX, lowest number first
  users.erase(users.begin());
  r.alsoUsedBy = users;
  r.detail = prefix + src.Label() + " > " + r.strip.Label();
  if (!r.alsoUsedBy.empty()) r.detail += " (+" + std::to_string(r.alsoUsedBy.size()) + " more)";
  return r;
}

// Follows an output's routing. Returns false with detail set if unresolved.
Resolution ResolveOutput(const IoRef& out, const WingState& st) {
  Resolution r;
  std::string prefix = out.Label() + " > ";
  std::string grp;
  int in = 0;
  if (!st.GetString(OutAddr(out, "grp"), &grp) || !st.GetInt(OutAddr(out, "in"), &in)) {
    r.detail = out.Label() + ": waiting for console routing";
    return r;
  }
  SrcType stripType;
  if (grp == "OFF" || grp.empty()) {
    r.detail = out.Label() + ": output not routed";
    return r;
  }
  // "AUX" in /io/out is the aux *input* group (handled below), so only CH counts as a channel here.
  if (StripFromRoutingGroup(grp, false, &stripType) || (grp == "CH" && (stripType = SrcType::Ch, true))) {
    r.strip = Source{stripType, in};
    r.ok = r.strip.IsValid();
    r.detail = prefix + r.strip.Label();
    return r;
  }
  if (grp == "USR") {
    // User signal: follow its own routing one more hop (commonly a channel direct out).
    std::string ugrp;
    int uin = 0;
    std::string uprefix = prefix + "USR." + std::to_string(in) + " > ";
    if (!st.GetString(UserAddr(in, "grp"), &ugrp) || !st.GetInt(UserAddr(in, "in"), &uin)) {
      r.detail = uprefix + "waiting for console routing";
      return r;
    }
    if (StripFromRoutingGroup(ugrp, true, &stripType)) {
      r.strip = Source{stripType, uin};
      r.ok = r.strip.IsValid();
      r.detail = uprefix + r.strip.Label();
      return r;
    }
    const IoGroupInfo* g = FindIoGroup(ugrp);
    if (g && g->inputs > 0) return ResolveInput(IoRef{ugrp, uin}, st, uprefix);
    r.detail = uprefix + ugrp + ": not a followable signal";
    return r;
  }
  const IoGroupInfo* g = FindIoGroup(grp);
  if (g && g->inputs > 0) return ResolveInput(IoRef{grp, in}, st, prefix);
  r.detail = prefix + grp + "." + std::to_string(in) + ": not a followable signal";
  return r;
}

void AddStripInterest(const Source& s, std::set<std::string>* fast) {
  const SrcTypeInfo& info = GetSrcTypeInfo(s.type);
  if (info.hasFader) fast->insert(AddressFor(s, Param::Fader));
  if (info.hasMute) fast->insert(AddressFor(s, Param::Mute));
  fast->insert(AddressFor(s, Param::Name));
}

void AddInputInterest(const IoRef& src, std::set<std::string>* slow) {
  ForEachInputStrip([&](const Source& ch) {
    for (const char* leaf : kConnLeaves) slow->insert(ch.Base() + "/" + leaf);
  });
  slow->insert(InAddr(src, "name"));
  if (src.index > 1) slow->insert(InAddr(IoRef{src.group, src.index - 1}, "mode"));
}

}  // namespace

Resolution Resolve(const FollowTarget& t, const WingState& st) {
  Resolution r;
  if (!t.IsValid()) {
    r.detail = "invalid target";
    return r;
  }
  switch (t.kind) {
    case BindKind::Strip:
      r.ok = true;
      r.strip = t.strip;
      r.detail = t.strip.Label();
      return r;
    case BindKind::Input: return ResolveInput(t.io, st, "");
    case BindKind::Output: return ResolveOutput(t.io, st);
  }
  return r;
}

void CollectInterest(const FollowTarget& t, const WingState& st, std::set<std::string>* fast,
                     std::set<std::string>* slow) {
  if (!t.IsValid()) return;
  if (t.kind == BindKind::Input) AddInputInterest(t.io, slow);
  if (t.kind == BindKind::Output) {
    slow->insert(OutAddr(t.io, "grp"));
    slow->insert(OutAddr(t.io, "in"));
    std::string grp;
    int in = 0;
    if (st.GetString(OutAddr(t.io, "grp"), &grp) && st.GetInt(OutAddr(t.io, "in"), &in)) {
      if (grp == "USR") {
        slow->insert(UserAddr(in, "grp"));
        slow->insert(UserAddr(in, "in"));
        std::string ugrp;
        int uin = 0;
        const IoGroupInfo* g = nullptr;
        if (st.GetString(UserAddr(in, "grp"), &ugrp) && st.GetInt(UserAddr(in, "in"), &uin) &&
            (g = FindIoGroup(ugrp)) && g->inputs > 0) {
          AddInputInterest(IoRef{ugrp, uin}, slow);
        }
      } else if (const IoGroupInfo* g = FindIoGroup(grp)) {
        if (g->inputs > 0) AddInputInterest(IoRef{grp, in}, slow);
      }
    }
  }
  Resolution r = Resolve(t, st);
  if (r.ok) AddStripInterest(r.strip, fast);
}

// ---- Bindings -----------------------------------------------------------------------------------

double DbToReaperVolume(double db) {
  if (db <= kWingMinusInfDb + 0.001) return 0.0;
  if (db > kReaperMaxDb) db = kReaperMaxDb;
  return std::pow(10.0, db / 20.0);
}

std::string SerializeBinding(const Binding& b) {
  const char* kind = "strip";
  std::string a;
  int idx = 0;
  switch (b.target.kind) {
    case BindKind::Strip:
      kind = "strip";
      a = GetSrcTypeInfo(b.target.strip.type).key;
      idx = b.target.strip.index;
      break;
    case BindKind::Input:
      kind = "in";
      a = b.target.io.group;
      idx = b.target.io.index;
      break;
    case BindKind::Output:
      kind = "out";
      a = b.target.io.group;
      idx = b.target.io.index;
      break;
  }
  char buf[320];
  snprintf(buf, sizeof(buf), "2|%s|%s|%s|%d|%d|%d|%d|%d", b.trackGuid.c_str(), kind, a.c_str(), idx,
           b.enabled ? 1 : 0, b.followMute ? 1 : 0, b.followFader ? 1 : 0, b.offsetCentiDb);
  std::string s = buf;
  if (!b.fxGuid.empty()) s += "|" + b.fxGuid + (b.fxOwned ? "|1" : "|0");
  return s;
}

bool ParseBinding(const std::string& s, Binding* out) {
  std::vector<std::string> f;
  size_t start = 0;
  for (;;) {
    size_t p = s.find('|', start);
    f.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
    if (p == std::string::npos) break;
    start = p + 1;
  }
  // Fields 10-11 are the optional strip FX guid and owned flag; anything after is ignored.
  if (f.size() < 9 || f[0] != "2") return false;
  Binding b;
  b.id = out->id;
  b.trackGuid = f[1];
  if (b.trackGuid.size() < 32) return false;
  int v[5];
  for (int i = 0; i < 5; i++) {
    if (!ParseInt(f[4 + i], &v[i])) return false;
  }
  if (f[2] == "strip") {
    b.target.kind = BindKind::Strip;
    if (!SrcTypeFromKey(f[3], &b.target.strip.type)) return false;
    b.target.strip.index = v[0];
  } else if (f[2] == "in" || f[2] == "out") {
    b.target.kind = f[2] == "in" ? BindKind::Input : BindKind::Output;
    b.target.io.group = f[3];
    b.target.io.index = v[0];
  } else {
    return false;
  }
  if (!b.target.IsValid()) return false;
  b.enabled = v[1] != 0;
  b.followMute = v[2] != 0;
  b.followFader = v[3] != 0;
  b.offsetCentiDb = v[4];
  if (f.size() > 9 && f[9].size() >= 32 && f[9][0] == '{') {
    b.fxGuid = f[9];
    b.fxOwned = f.size() > 10 && f[10] == "1";
  }
  *out = b;
  return true;
}

TrackTarget ComputeTrackTarget(const std::vector<const Binding*>& bindings, const WingState& st,
                               const FollowSwitches& sw) {
  TrackTarget t;
  if (!sw.master) return t;
  double sumDb = 0.0;
  bool minusInf = false;
  for (const Binding* b : bindings) {
    if (!b->enabled) continue;
    Resolution r = Resolve(b->target, st);
    if (!r.ok) continue;
    const SrcTypeInfo& info = GetSrcTypeInfo(r.strip.type);

    double db;
    if (sw.fader && b->followFader && info.hasFader && st.GetFaderDb(r.strip, &db)) {
      t.setVolume = true;
      if (db <= kWingMinusInfDb + 0.001) minusInf = true;
      else sumDb += db + b->OffsetDb();
    }
    bool muted;
    if (sw.mute && b->followMute && info.hasMute && st.GetMute(r.strip, &muted)) {
      t.setMute = true;
      t.mute = t.mute || muted;
    }
  }
  if (t.setVolume) {
    t.sumDb = minusInf ? kWingMinusInfDb : sumDb;
    t.volume = DbToReaperVolume(t.sumDb);
  }
  return t;
}

}  // namespace wf
