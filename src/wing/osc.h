// Minimal OSC 1.0 codec for talking to a Behringer WING.
//
// Deliberately asymmetric: the encoder can only build messages WITHOUT arguments. On the WING an
// argument-less message is a query (or a command such as /*S), so this plugin is physically unable
// to change anything on the console. Keep it that way.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wf {

struct OscArg {
  char type = 0;  // 'i', 'f', 's' (other types are skipped during decode)
  int32_t i = 0;
  float f = 0.0f;
  std::string s;
};

struct OscMessage {
  std::string address;
  std::vector<OscArg> args;
};

// Encodes "<address>" followed by an empty type tag string (",").
std::vector<uint8_t> OscEncodeQuery(const std::string& address);

// Decodes a packet. Bundles are flattened into their contained messages.
// Returns false if the packet is not valid OSC.
bool OscDecode(const uint8_t* data, size_t len, std::vector<OscMessage>& out);

// Human-readable dump for logging, e.g. "/ch/1/fdr ,sff "-3.0" 0.68 -3.0".
std::string OscToString(const OscMessage& msg);

}  // namespace wf
