// Unit tests for the REAPER-independent core: OSC codec, WING value decoding, routing resolution,
// binding serialization and track target combination. No framework, just asserts with messages.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/wing/osc.h"
#include "../src/wing/wing_client.h"
#include "../src/wing/wing_model.h"

using namespace wf;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    g_checks++;                                                           \
    if (!(cond)) {                                                        \
      g_failures++;                                                       \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                     \
  } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

// ---- helpers to build console replies the way the WING sends them -------------------------------

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

static std::vector<uint8_t> Sff(const std::string& addr, const std::string& s, float raw, float db) {
  std::vector<uint8_t> b;
  PutStr(b, addr), PutStr(b, ",sff"), PutStr(b, s), PutF(b, raw), PutF(b, db);
  return b;
}
static std::vector<uint8_t> Sfi(const std::string& addr, const std::string& s, float raw, int v) {
  std::vector<uint8_t> b;
  PutStr(b, addr), PutStr(b, ",sfi"), PutStr(b, s), PutF(b, raw), PutBE(b, static_cast<uint32_t>(v));
  return b;
}
static std::vector<uint8_t> S(const std::string& addr, const std::string& s) {
  std::vector<uint8_t> b;
  PutStr(b, addr), PutStr(b, ",s"), PutStr(b, s);
  return b;
}

static void Feed(WingState& st, const std::vector<uint8_t>& pkt) {
  std::vector<OscMessage> msgs;
  CHECK(OscDecode(pkt.data(), pkt.size(), msgs));
  for (auto& m : msgs) st.Apply(m);
}
static void Fader(WingState& st, const std::string& base, float db) {
  Feed(st, Sff(base + "/fdr", db <= -144 ? "-oo" : std::to_string(db), 0.5f, db));
}
static void Mute(WingState& st, const std::string& base, int m) {
  Feed(st, Sfi(base + "/mute", m ? "1" : "0", static_cast<float>(m), m));
}
// Patch channel N's main input to grp.in (and make every other channel/aux report its patch).
static void PatchAll(WingState& st, std::vector<std::pair<std::string, int>> chIn) {
  for (int t = 0; t < 2; t++) {
    int count = t == 0 ? 40 : 8;
    for (int i = 1; i <= count; i++) {
      std::string base = std::string(t == 0 ? "/ch/" : "/aux/") + std::to_string(i);
      std::string grp = "OFF";
      int in = 1;
      if (t == 0 && i <= static_cast<int>(chIn.size())) grp = chIn[i - 1].first, in = chIn[i - 1].second;
      Feed(st, S(base + "/in/conn/grp", grp));
      Feed(st, Sfi(base + "/in/conn/in", std::to_string(in), 0, in));
      Feed(st, S(base + "/in/conn/altgrp", "OFF"));
      Feed(st, Sfi(base + "/in/conn/altin", "1", 0, 1));
      Feed(st, Sfi(base + "/in/set/altsrc", "0", 0, 0));
    }
  }
}

// ---- tests --------------------------------------------------------------------------------------

static void TestOsc() {
  std::vector<uint8_t> q = OscEncodeQuery("/ch/1/fdr");
  // "/ch/1/fdr\0\0\0" (12) + ",\0\0\0" (4)
  CHECK(q.size() == 16);
  CHECK(std::memcmp(q.data(), "/ch/1/fdr\0\0\0,\0\0\0", 16) == 0);

  std::vector<OscMessage> msgs;
  CHECK(OscDecode(q.data(), q.size(), msgs));
  CHECK(msgs.size() == 1 && msgs[0].address == "/ch/1/fdr" && msgs[0].args.empty());

  // Example straight from the WING OSC doc: /ch/2/fdr ,sff "-2.0" 0.7 -2.0
  std::vector<uint8_t> r = Sff("/ch/2/fdr", "-2.0", 0.7f, -2.0f);
  msgs.clear();
  CHECK(OscDecode(r.data(), r.size(), msgs));
  CHECK(msgs.size() == 1);
  CHECK(msgs[0].args.size() == 3);
  CHECK(msgs[0].args[0].type == 's' && msgs[0].args[0].s == "-2.0");
  CHECK(msgs[0].args[2].type == 'f' && msgs[0].args[2].f == -2.0f);

  // Truncated / garbage packets are rejected.
  msgs.clear();
  CHECK(!OscDecode(r.data(), r.size() - 3, msgs));
  const uint8_t junk[] = {'x', 'y', 0, 0};
  CHECK(!OscDecode(junk, sizeof(junk), msgs));
  CHECK(!OscDecode(nullptr, 0, msgs));

  // Bundles are flattened.
  std::vector<uint8_t> bundle;
  PutStr(bundle, "#bundle");
  PutBE(bundle, 0), PutBE(bundle, 1);
  PutBE(bundle, static_cast<uint32_t>(r.size()));
  bundle.insert(bundle.end(), r.begin(), r.end());
  msgs.clear();
  CHECK(OscDecode(bundle.data(), bundle.size(), msgs));
  CHECK(msgs.size() == 1 && msgs[0].address == "/ch/2/fdr");
}

static void TestDecode() {
  auto one = [](const std::vector<uint8_t>& p) {
    std::vector<OscMessage> m;
    OscDecode(p.data(), p.size(), m);
    return m.at(0);
  };
  double db = 0;
  CHECK(DecodeFaderDb(one(Sff("/ch/1/fdr", "-oo", 0, -144)), &db) && db == -144.0);
  CHECK(DecodeFaderDb(one(Sff("/ch/1/fdr", "3.0", 0.825f, 3.0f)), &db) && db == 3.0);
  // Only a display string.
  CHECK(DecodeFaderDb(one(S("/ch/1/fdr", "-12.5")), &db) && db == -12.5);
  CHECK(DecodeFaderDb(one(S("/ch/1/fdr", "-oo")), &db) && db == -144.0);
  CHECK(!DecodeFaderDb(one(S("/ch/1/fdr", "abc")), &db));

  bool m = false;
  CHECK(DecodeBool(one(Sfi("/ch/1/mute", "1", 1, 1)), &m) && m);
  CHECK(DecodeBool(one(Sfi("/ch/1/mute", "0", 0, 0)), &m) && !m);
  CHECK(DecodeBool(one(S("/ch/1/mute", "ON")), &m) && m);

  CHECK_NEAR(DbToReaperVolume(0), 1.0, 1e-9);
  CHECK_NEAR(DbToReaperVolume(-6.0206), 0.5, 1e-4);
  CHECK(DbToReaperVolume(-144) == 0.0);
  CHECK_NEAR(DbToReaperVolume(40), std::pow(10.0, kReaperMaxDb / 20), 1e-9);
}

static void TestAddresses() {
  CHECK(AddressFor(Source{SrcType::Ch, 1}, Param::Fader) == "/ch/1/fdr");
  CHECK(AddressFor(Source{SrcType::Dca, 16}, Param::Mute) == "/dca/16/mute");
  CHECK(AddressFor(Source{SrcType::MuteGroup, 3}, Param::Mute) == "/mgrp/3/mute");
  CHECK(!Source({SrcType::Ch, 41}).IsValid());
  CHECK(!Source({SrcType::Dca, 0}).IsValid());
  FollowTarget t;
  t.kind = BindKind::Output;
  t.io = {"CRD", 64};
  CHECK(t.IsValid());
  t.io = {"CRD", 65};
  CHECK(!t.IsValid());
  t.io = {"PLAY", 1};  // player is input-only
  CHECK(!t.IsValid());
  t.kind = BindKind::Input;
  CHECK(t.IsValid());
}

static void TestResolve() {
  WingState st;
  FollowTarget in;
  in.kind = BindKind::Input;
  in.io = {"A", 5};

  // Before the console has told us anything.
  Resolution r = Resolve(in, st);
  CHECK(!r.ok);
  CHECK(r.detail.find("waiting") != std::string::npos);

  // A.5 is patched to channel 3.
  PatchAll(st, {{"LCL", 1}, {"LCL", 2}, {"A", 5}});
  r = Resolve(in, st);
  CHECK(r.ok && r.strip == (Source{SrcType::Ch, 3}));

  // Re-patch on the desk: A.5 moves to channel 1. Following is by source, so we move with it.
  PatchAll(st, {{"A", 5}, {"LCL", 2}, {"LCL", 1}});
  r = Resolve(in, st);
  CHECK(r.ok && r.strip == (Source{SrcType::Ch, 1}));

  // Two channels use the same source: lowest channel is followed, the other is reported.
  PatchAll(st, {{"LCL", 1}, {"A", 5}, {"A", 5}});
  r = Resolve(in, st);
  CHECK(r.ok && r.strip == (Source{SrcType::Ch, 2}) && r.alsoUsedBy.size() == 1);

  // Alt source active on channel 4 -> its alt patch counts, not the main one.
  Feed(st, S("/ch/4/in/conn/altgrp", "B"));
  Feed(st, Sfi("/ch/4/in/conn/altin", "9", 0, 9));
  Feed(st, Sfi("/ch/4/in/set/altsrc", "1", 1, 1));
  FollowTarget b9;
  b9.kind = BindKind::Input;
  b9.io = {"B", 9};
  r = Resolve(b9, st);
  CHECK(r.ok && r.strip == (Source{SrcType::Ch, 4}));

  // Stereo source USB 1/2 patched to channel 5 as USB.1: USB.2 also resolves to channel 5.
  PatchAll(st, {{"OFF", 1}, {"OFF", 1}, {"OFF", 1}, {"OFF", 1}, {"USB", 1}});
  FollowTarget usb2;
  usb2.kind = BindKind::Input;
  usb2.io = {"USB", 2};
  r = Resolve(usb2, st);
  CHECK(!r.ok);  // mode unknown -> treated as mono
  Feed(st, S("/io/in/USB/1/mode", "ST"));
  r = Resolve(usb2, st);
  CHECK(r.ok && r.strip == (Source{SrcType::Ch, 5}));

  // Unpatched source.
  FollowTarget c1;
  c1.kind = BindKind::Input;
  c1.io = {"C", 1};
  r = Resolve(c1, st);
  CHECK(!r.ok && r.detail.find("not patched") != std::string::npos);
}

static void TestResolveOutput() {
  WingState st;
  PatchAll(st, {{"LCL", 1}, {"A", 7}});
  FollowTarget out;
  out.kind = BindKind::Output;
  out.io = {"CRD", 12};

  CHECK(!Resolve(out, st).ok);
  // Card out 12 carries source A.7, which is on channel 2.
  Feed(st, S("/io/out/CRD/12/grp", "A"));
  Feed(st, Sfi("/io/out/CRD/12/in", "7", 0, 7));
  Resolution r = Resolve(out, st);
  CHECK(r.ok && r.strip == (Source{SrcType::Ch, 2}));

  // Card out fed from a bus follows the bus strip.
  Feed(st, S("/io/out/CRD/12/grp", "BUS"));
  Feed(st, Sfi("/io/out/CRD/12/in", "3", 0, 3));
  r = Resolve(out, st);
  CHECK(r.ok && r.strip == (Source{SrcType::Bus, 3}));

  // Card out fed from a user signal that taps channel 9 (direct-out style).
  Feed(st, S("/io/out/CRD/12/grp", "USR"));
  Feed(st, Sfi("/io/out/CRD/12/in", "2", 0, 2));
  r = Resolve(out, st);
  CHECK(!r.ok);  // user routing not known yet
  Feed(st, S("/io/user/2/grp", "CH"));
  Feed(st, Sfi("/io/user/2/in", "9", 0, 9));
  r = Resolve(out, st);
  CHECK(r.ok && r.strip == (Source{SrcType::Ch, 9}));

  // Aux *input* group in /io/out is a source, not the aux strip.
  PatchAll(st, {{"LCL", 1}, {"AUX", 4}});
  Feed(st, S("/io/out/CRD/12/grp", "AUX"));
  Feed(st, Sfi("/io/out/CRD/12/in", "4", 0, 4));
  r = Resolve(out, st);
  CHECK(r.ok && r.strip == (Source{SrcType::Ch, 2}));

  Feed(st, S("/io/out/CRD/12/grp", "OFF"));
  CHECK(!Resolve(out, st).ok);

  // Interest: an output binding asks for its routing, then for the followed strip.
  std::set<std::string> fast, slow;
  Feed(st, S("/io/out/CRD/12/grp", "A"));
  Feed(st, Sfi("/io/out/CRD/12/in", "7", 0, 7));
  PatchAll(st, {{"LCL", 1}, {"A", 7}});
  CollectInterest(out, st, &fast, &slow);
  CHECK(slow.count("/io/out/CRD/12/grp") && slow.count("/ch/40/in/conn/grp") &&
        slow.count("/aux/8/in/set/altsrc") && slow.count("/io/in/A/6/mode"));
  CHECK(fast.count("/ch/2/fdr") && fast.count("/ch/2/mute") && fast.count("/ch/2/name"));
}

static void TestSerialize() {
  Binding b;
  b.trackGuid = "{01234567-89AB-CDEF-0123-456789ABCDEF}";
  b.target.kind = BindKind::Output;
  b.target.io = {"CRD", 12};
  b.enabled = true;
  b.followMute = false;
  b.followFader = true;
  b.offsetCentiDb = -350;
  std::string s = SerializeBinding(b);
  CHECK(s == "2|{01234567-89AB-CDEF-0123-456789ABCDEF}|out|CRD|12|1|0|1|-350");
  Binding p;
  CHECK(ParseBinding(s, &p));
  CHECK(p.trackGuid == b.trackGuid && p.target == b.target && p.enabled && !p.followMute &&
        p.followFader && p.offsetCentiDb == -350);

  b.target.kind = BindKind::Strip;
  b.target.strip = {SrcType::Dca, 3};
  CHECK(ParseBinding(SerializeBinding(b), &p) && p.target == b.target);

  CHECK(!ParseBinding("2|{short}|strip|ch|1|1|1|1|0", &p));
  CHECK(!ParseBinding("2|{01234567-89AB-CDEF-0123-456789ABCDEF}|strip|ch|99|1|1|1|0", &p));
  CHECK(!ParseBinding("9|{01234567-89AB-CDEF-0123-456789ABCDEF}|strip|ch|1|1|1|1|0", &p));
  b.fxGuid = "{AAAAAAAA-89AB-CDEF-0123-456789ABCDEF}";
  CHECK(ParseBinding(SerializeBinding(b), &p) && p.fxGuid == b.fxGuid && !p.fxOwned);
  b.fxOwned = true;
  CHECK(ParseBinding(SerializeBinding(b), &p) && p.fxGuid == b.fxGuid && p.fxOwned);
  // Extra trailing fields (future versions) are tolerated.
  CHECK(ParseBinding("2|{01234567-89AB-CDEF-0123-456789ABCDEF}|strip|ch|1|1|1|1|0|x", &p));
}

static void TestCombine() {
  WingState st;
  Fader(st, "/ch/1", -10);
  Mute(st, "/ch/1", 0);
  Fader(st, "/dca/2", -5);
  Mute(st, "/dca/2", 0);
  Mute(st, "/mgrp/1", 0);

  Binding ch;
  ch.target.strip = {SrcType::Ch, 1};
  ch.offsetCentiDb = 200;
  Binding dca;
  dca.target.strip = {SrcType::Dca, 2};
  Binding mg;
  mg.target.strip = {SrcType::MuteGroup, 1};
  std::vector<const Binding*> all = {&ch, &dca, &mg};

  FollowSwitches sw;
  sw.master = false;
  TrackTarget t = ComputeTrackTarget(all, st, sw);
  CHECK(!t.setVolume && !t.setMute);  // master off: leave REAPER alone

  sw.master = true;
  t = ComputeTrackTarget(all, st, sw);
  CHECK(t.setVolume && t.setMute && !t.mute);
  CHECK_NEAR(t.sumDb, -10 + 2 + -5, 1e-6);  // channel + offset + DCA

  Mute(st, "/dca/2", 1);
  t = ComputeTrackTarget(all, st, sw);
  CHECK(t.mute);  // DCA mute mutes the track
  Mute(st, "/dca/2", 0);
  Mute(st, "/mgrp/1", 1);
  CHECK(ComputeTrackTarget(all, st, sw).mute);  // mute group too
  Mute(st, "/mgrp/1", 0);

  Fader(st, "/dca/2", -144);
  t = ComputeTrackTarget(all, st, sw);
  CHECK(t.volume == 0.0);

  // Global and per-binding switches.
  sw.fader = false;
  t = ComputeTrackTarget(all, st, sw);
  CHECK(!t.setVolume && t.setMute);
  sw.fader = true;
  sw.mute = false;
  t = ComputeTrackTarget(all, st, sw);
  CHECK(t.setVolume && !t.setMute);
  sw.mute = true;
  ch.enabled = false;
  dca.followFader = false;
  t = ComputeTrackTarget(all, st, sw);
  CHECK(!t.setVolume);  // no participating fader source left
  CHECK(t.setMute);     // DCA + mute group still follow mute

  // Values not yet received: binding does not participate.
  Binding unknown;
  unknown.target.strip = {SrcType::Ch, 20};
  t = ComputeTrackTarget({&unknown}, st, sw);
  CHECK(!t.setVolume && !t.setMute);
}

static void TestConsoleInfo() {
  DiscoveredConsole c;
  CHECK(ParseConsoleInfo("WING,192.168.1.62,FOH,ngc-full,S123,3.0.6", &c));
  CHECK(c.ip == "192.168.1.62" && c.name == "FOH" && c.model == "ngc-full" && c.firmware == "3.0.6");
  CHECK(!ParseConsoleInfo("X32,1.2.3.4", &c));
}

int main() {
  TestOsc();
  TestDecode();
  TestAddresses();
  TestResolve();
  TestResolveOutput();
  TestSerialize();
  TestCombine();
  TestConsoleInfo();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
