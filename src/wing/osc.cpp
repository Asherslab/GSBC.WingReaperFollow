#include "osc.h"

#include <cstdio>
#include <cstring>

namespace wf {

namespace {

void AppendPaddedString(std::vector<uint8_t>& buf, const std::string& s) {
  buf.insert(buf.end(), s.begin(), s.end());
  buf.push_back(0);
  while (buf.size() % 4) buf.push_back(0);
}

// Reads a NUL-terminated, 4-byte padded string starting at *pos.
bool ReadPaddedString(const uint8_t* data, size_t len, size_t* pos, std::string* out) {
  size_t start = *pos;
  size_t end = start;
  while (end < len && data[end] != 0) end++;
  if (end >= len) return false;
  out->assign(reinterpret_cast<const char*>(data + start), end - start);
  size_t next = (end + 4) & ~static_cast<size_t>(3);
  if (next > len) return false;
  *pos = next;
  return true;
}

uint32_t ReadBE32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

bool DecodeMessage(const uint8_t* data, size_t len, OscMessage& msg) {
  size_t pos = 0;
  if (!ReadPaddedString(data, len, &pos, &msg.address)) return false;
  if (msg.address.empty() || msg.address[0] != '/') return false;
  msg.args.clear();
  if (pos >= len) return true;  // no type tag string at all: legal (old OSC), no args

  std::string tags;
  if (!ReadPaddedString(data, len, &pos, &tags)) return false;
  if (tags.empty() || tags[0] != ',') return false;

  for (size_t t = 1; t < tags.size(); t++) {
    OscArg arg;
    arg.type = tags[t];
    switch (tags[t]) {
      case 'i':
        if (pos + 4 > len) return false;
        arg.i = static_cast<int32_t>(ReadBE32(data + pos));
        pos += 4;
        break;
      case 'f': {
        if (pos + 4 > len) return false;
        uint32_t bits = ReadBE32(data + pos);
        std::memcpy(&arg.f, &bits, 4);
        pos += 4;
        break;
      }
      case 's':
      case 'S':
        arg.type = 's';
        if (!ReadPaddedString(data, len, &pos, &arg.s)) return false;
        break;
      case 'b': {  // blob: skip
        if (pos + 4 > len) return false;
        size_t n = ReadBE32(data + pos);
        pos += 4;
        size_t padded = (n + 3) & ~static_cast<size_t>(3);
        if (pos + padded > len) return false;
        pos += padded;
        continue;
      }
      case 'h':
      case 'd':
      case 't':
        if (pos + 8 > len) return false;
        pos += 8;
        continue;
      case 'T':
      case 'F':
      case 'N':
      case 'I':
        arg.type = 'i';
        arg.i = tags[t] == 'T' ? 1 : 0;
        break;
      default:
        return false;  // unknown type: can't know its size
    }
    msg.args.push_back(arg);
  }
  return true;
}

bool DecodeAny(const uint8_t* data, size_t len, std::vector<OscMessage>& out, int depth) {
  if (len < 4 || depth > 8) return false;
  if (len >= 16 && std::memcmp(data, "#bundle", 8) == 0) {
    size_t pos = 16;  // "#bundle\0" + 8-byte timetag
    bool any = false;
    while (pos + 4 <= len) {
      size_t n = ReadBE32(data + pos);
      pos += 4;
      if (n > len - pos) return any;
      any |= DecodeAny(data + pos, n, out, depth + 1);
      pos += n;
    }
    return any;
  }
  OscMessage msg;
  if (!DecodeMessage(data, len, msg)) return false;
  out.push_back(std::move(msg));
  return true;
}

}  // namespace

std::vector<uint8_t> OscEncodeQuery(const std::string& address) {
  std::vector<uint8_t> buf;
  buf.reserve(address.size() + 8);
  AppendPaddedString(buf, address);
  AppendPaddedString(buf, ",");
  return buf;
}

bool OscDecode(const uint8_t* data, size_t len, std::vector<OscMessage>& out) {
  return DecodeAny(data, len, out, 0);
}

std::string OscToString(const OscMessage& msg) {
  std::string s = msg.address + " ,";
  for (const OscArg& a : msg.args) s += a.type;
  char tmp[64];
  for (const OscArg& a : msg.args) {
    switch (a.type) {
      case 'i':
        snprintf(tmp, sizeof(tmp), " %d", a.i);
        s += tmp;
        break;
      case 'f':
        snprintf(tmp, sizeof(tmp), " %g", a.f);
        s += tmp;
        break;
      case 's':
        s += " \"" + a.s + "\"";
        break;
    }
  }
  return s;
}

}  // namespace wf
