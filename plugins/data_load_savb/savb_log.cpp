// SPDX-License-Identifier: MPL-2.0
#include "savb_log.hpp"

#include <cmath>
#include <limits>
#include <optional>

namespace savb {
namespace {

constexpr uint8_t kSync0 = 0xA3;
constexpr uint8_t kSync1 = 0x95;
constexpr uint8_t kFmtId = 128;
constexpr size_t kMsgHeaderLen = 3;
constexpr size_t kZLen = 64;

// 1: original framing. 2: adds the x/X bitfield types (v1 logs read unchanged).
constexpr uint16_t kMinVersion = 1;
constexpr uint16_t kMaxVersion = 3;  // 3: epoch columns are local wall-clock, not UTC

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    const size_t end = s.find(sep, start);
    out.push_back(s.substr(start, end - start));
    if (end == std::string::npos) {
      return out;
    }
    start = end + 1;
  }
}

// "a:1|b:2" -> slots, or nullopt when malformed or wider than the container.
std::optional<std::vector<BitSlot>> parseBits(const std::string& label, unsigned container_bits) {
  std::vector<BitSlot> slots;
  unsigned shift = 0;
  for (const auto& entry : split(label, '|')) {
    const size_t colon = entry.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == entry.size()) {
      return std::nullopt;
    }
    unsigned width = 0;
    for (size_t i = colon + 1; i < entry.size(); ++i) {
      if (entry[i] < '0' || entry[i] > '9') {
        return std::nullopt;
      }
      width = width * 10 + static_cast<unsigned>(entry[i] - '0');
    }
    if (width == 0 || shift + width > container_bits) {
      return std::nullopt;
    }
    slots.push_back({entry.substr(0, colon), shift, (1U << width) - 1U});
    shift += width;
  }
  return slots;
}

// Parses the FMT body at `p` (just past the message header). On success sets
// `consumed` to its byte length. nullopt for a malformed or incomplete record.
std::optional<Format> parseFmt(const uint8_t* p, size_t avail, size_t& consumed) {
  if (avail < 8) {
    return std::nullopt;
  }
  Format fmt;
  fmt.id = p[0];
  fmt.length = p[1];
  fmt.flags = p[2];
  fmt.name.assign(reinterpret_cast<const char*>(p + 3), 4);
  const size_t nfields = p[7];
  size_t pos = 8;
  if (avail < pos + 2 * nfields + 2) {
    return std::nullopt;
  }
  const std::string types(reinterpret_cast<const char*>(p + pos), nfields);
  const std::string decimals(reinterpret_cast<const char*>(p + pos + nfields), nfields);
  pos += 2 * nfields;
  const size_t nlabels = load<uint16_t>(p + pos);
  pos += 2;
  if (avail < pos + nlabels) {
    return std::nullopt;
  }
  const auto labels = split(std::string(reinterpret_cast<const char*>(p + pos), nlabels), ',');
  pos += nlabels;
  if (labels.size() != nfields || fmt.id == kFmtId) {
    return std::nullopt;
  }

  size_t offset = 0;
  for (size_t i = 0; i < nfields; ++i) {
    Field f;
    f.type = types[i];
    f.label = labels[i];
    f.offset = offset;
    const size_t size = typeSize(f.type);
    if (size == 0 || decimals[i] < '0' || decimals[i] > '9') {
      return std::nullopt;
    }
    f.decimals = decimals[i] - '0';
    if (f.type == 'x' || f.type == 'X') {
      auto bits = parseBits(f.label, f.type == 'x' ? 8U : 16U);
      if (!bits) {
        return std::nullopt;
      }
      f.bits = std::move(*bits);
    }
    offset += size;
    fmt.fields.push_back(std::move(f));
  }
  if (offset != fmt.length) {
    return std::nullopt;
  }
  consumed = pos;
  return fmt;
}

}  // namespace

int Format::find(const std::string& label) const {
  for (size_t i = 0; i < fields.size(); ++i) {
    if (fields[i].bits.empty() && fields[i].label == label) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

size_t typeSize(char t) {
  switch (t) {
    case 'b': case 'B': case 'x': return 1;
    case 'h': case 'H': case 'X': return 2;
    case 'i': case 'I': return 4;
    case 'q': case 'Q': case 'd': return 8;
    case 'Z': return kZLen;
    default: return 0;
  }
}

int64_t rawInteger(const Field& f, const uint8_t* payload) {
  const uint8_t* p = payload + f.offset;
  switch (f.type) {
    case 'b': return load<int8_t>(p);
    case 'B': case 'x': return load<uint8_t>(p);
    case 'h': return load<int16_t>(p);
    case 'H': case 'X': return load<uint16_t>(p);
    case 'i': return load<int32_t>(p);
    case 'I': return load<uint32_t>(p);
    case 'q': return load<int64_t>(p);
    case 'Q': return static_cast<int64_t>(load<uint64_t>(p));
    default: return 0;
  }
}

double numericValue(const Field& f, const uint8_t* payload) {
  constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
  if (f.type == 'Z') {
    return kNan;
  }
  if (f.type == 'd') {
    return load<double>(payload + f.offset);
  }
  if (f.type == 'Q') {
    return static_cast<double>(load<uint64_t>(payload + f.offset));
  }
  const int64_t raw = rawInteger(f, payload);
  if (f.decimals == 0) {
    return static_cast<double>(raw);
  }
  // Scaled fields are signed; the type's extremes are the NaN / out-of-range sentinels.
  const unsigned bits = static_cast<unsigned>(typeSize(f.type)) * 8U;
  const int64_t hi = bits == 64 ? std::numeric_limits<int64_t>::max() : (int64_t{1} << (bits - 1)) - 1;
  const int64_t lo = -hi - 1;
  if (raw == lo || raw == hi) {
    return kNan;
  }
  return static_cast<double>(raw) / std::pow(10.0, f.decimals);
}

std::string scanLog(
    std::span<const uint8_t> buf, const MessageCallback& on_message, ScanStats& stats,
    const FormatCallback& on_format) {
  if (buf.size() < 8 || std::memcmp(buf.data(), "SAVB", 4) != 0) {
    return "not a SAVB log (bad magic)";
  }
  const uint16_t version = load<uint16_t>(buf.data() + 4);
  if (version < kMinVersion || version > kMaxVersion) {
    return "SAVB format version " + std::to_string(version) + " is not supported (this reader handles " +
           std::to_string(kMinVersion) + ".." + std::to_string(kMaxVersion) + ")";
  }
  const size_t ncols = load<uint16_t>(buf.data() + 6);
  if (8 + ncols > buf.size()) {
    return "truncated SAVB file header";
  }
  stats.version = version;
  stats.columns = split(std::string(reinterpret_cast<const char*>(buf.data() + 8), ncols), ',');
  size_t pos = 8 + ncols;

  std::vector<std::optional<Format>> formats(256);
  const size_t end = buf.size();
  while (pos + kMsgHeaderLen <= end) {
    if (buf[pos] != kSync0 || buf[pos + 1] != kSync1) {
      ++pos;
      ++stats.skipped_bytes;
      continue;
    }
    const uint8_t id = buf[pos + 2];
    const size_t body = pos + kMsgHeaderLen;

    if (id == kFmtId) {
      size_t consumed = 0;
      auto fmt = parseFmt(buf.data() + body, end - body, consumed);
      if (!fmt) {
        ++pos;
        ++stats.skipped_bytes;
        continue;
      }
      const uint8_t fmt_id = fmt->id;
      formats[fmt_id] = std::move(*fmt);
      if (on_format) {
        on_format(*formats[fmt_id]);
      }
      pos = body + consumed;
      continue;
    }

    const auto& fmt = formats[id];
    if (!fmt) {
      ++pos;
      ++stats.skipped_bytes;
      continue;
    }
    if (body + fmt->length > end) {
      stats.skipped_bytes += end - pos;  // truncated final message
      break;
    }
    ++stats.messages;
    if (!on_message(*fmt, buf.data() + body)) {
      stats.stopped = true;
      break;
    }
    pos = body + fmt->length;
  }
  return {};
}

}  // namespace savb
