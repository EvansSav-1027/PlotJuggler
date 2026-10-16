// SPDX-License-Identifier: MPL-2.0
#pragma once

// Reader for SAV Helm OS SensorDataLogger flight-recorder logs ("SAVB").
//
// Framing (ArduPilot DataFlash-style, little-endian), as written by
// sav-os/src/hal/plugins_real/sensor_data_logger.cpp and read by
// sav-os/tools/sav_log_to_csv.py — this reader follows that script:
//
//   file header:  "SAVB" | u16 version | u16 n | n bytes of CSV column order
//   message:      0xA3 0x95 | u8 id | fixed-length packed payload
//   FMT (id 128): u8 id | u8 len | u8 flags | char[4] name | u8 nfields |
//                 char[nfields] type | char[nfields] decimals ('0'..'9') |
//                 u16 n | n bytes of comma-separated labels
//
// Qt-free and SDK-free so it can be tested on its own.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace savb {

/// One sub-value of an x/X bitfield; slots are packed least significant bits first.
struct BitSlot {
  std::string label;
  unsigned shift = 0;
  uint32_t mask = 0;
};

/// One field of a message type, as its FMT record describes it.
struct Field {
  char type = 0;  ///< b/B h/H i/I q/Q int8..uint64, d float64, Z char[64], x/X uint8/uint16 bitfield
  /// N > 0: the field holds round(value * 10^N); the type's minimum stands for
  /// NaN/inf and its maximum for "out of range" — both read back as NaN.
  int decimals = 0;
  std::string label;          ///< the CSV column name; for a bitfield, the raw "a:1|b:2" slot text
  size_t offset = 0;          ///< byte offset within the payload
  std::vector<BitSlot> bits;  ///< non-empty only for x/X
};

/// A message type, from its FMT record.
struct Format {
  static constexpr uint8_t kFlagSticky = 0x01;  ///< logged on change only; values hold until the next one

  uint8_t id = 0;
  uint8_t length = 0;  ///< payload bytes, excluding the 3-byte message header
  uint8_t flags = 0;
  std::string name;  ///< 4 chars, e.g. "TICK", "IMU_", "EKF1"
  std::vector<Field> fields;

  /// Index of the (non-bitfield) field labelled `label`, or -1.
  [[nodiscard]] int find(const std::string& label) const;
};

struct ScanStats {
  std::vector<std::string> columns;  ///< the file header's CSV column order (sav_log_to_csv.py's header row)
  uint64_t messages = 0;             ///< data messages delivered (FMT records excluded)
  uint64_t skipped_bytes = 0;        ///< bytes dropped while resyncing, plus a truncated tail
  bool stopped = false;              ///< the callback asked to stop
};

/// Called once per data message, in file order. `payload` points at
/// `fmt.length` bytes inside the scanned buffer. Return false to stop.
using MessageCallback = std::function<bool(const Format& fmt, const uint8_t* payload)>;

/// Called once per FMT record as it is read. The logger writes every FMT
/// before the first data message, so all formats are known by the first call
/// of the message callback.
using FormatCallback = std::function<void(const Format& fmt)>;

/// Walks a whole log. A message whose id has no FMT yet, or a corrupt
/// stretch, is skipped by resyncing on the next header (counted in
/// `stats.skipped_bytes`), so a log cut off by a power loss still yields
/// everything before the cut. Returns an error text for a buffer that is not a
/// SAVB log of a supported version, an empty string otherwise.
[[nodiscard]] std::string scanLog(
    std::span<const uint8_t> buf, const MessageCallback& on_message, ScanStats& stats,
    const FormatCallback& on_format = {});

/// Size in bytes of field type `t`, 0 for an unknown type.
[[nodiscard]] size_t typeSize(char t);

/// Raw little-endian load; the payload has no alignment guarantee.
template <typename T>
[[nodiscard]] T load(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

/// The field's raw integer (any integer or bitfield type), sign-extended.
[[nodiscard]] int64_t rawInteger(const Field& f, const uint8_t* payload);

/// The field's physical value: a scaled field divided back by 10^N (NaN for
/// its sentinels), a 'd' as stored, an unscaled integer converted. NaN for Z.
[[nodiscard]] double numericValue(const Field& f, const uint8_t* payload);

}  // namespace savb
