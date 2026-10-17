// SPDX-License-Identifier: MPL-2.0
//
// PlotJuggler data loader for SAV Helm OS flight-recorder logs (.bin, "SAVB").
//
// Produces what loading the CSV from sav-os/tools/sav_log_to_csv.py with
// PlotJuggler's CSV loader produces, minus the text columns (nothing to plot):
// one topic named after the file, one float64 field per numeric CSV column
// (the column order stored in the log's header, names verbatim), one record
// per CSV row (one per logged TICK) holding that row's non-blank cells. The
// row is built the way the decoder builds it — sticky messages carried
// forward, CAL kept out of the rows, derived columns recomputed — so a layout
// made for the CSV works on the .bin unchanged.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "savb_log.hpp"
#include "savb_manifest.hpp"

namespace {

// One logged value as the decoder holds it before formatting: which field of
// which message, where its payload is (inside the file buffer, which outlives
// the import), and for a bitfield which slot.
struct RawCell {
  const savb::Field* field = nullptr;
  const uint8_t* payload = nullptr;
  int bit = -1;
};

// The decoder's as_float(): numeric value, or nullopt for a text field and for
// a scaled field's NaN / out-of-range sentinel (a blank derived cell).
std::optional<double> asNumber(const RawCell& c) {
  if (c.bit >= 0) {
    const auto& slot = c.field->bits[static_cast<size_t>(c.bit)];
    return static_cast<double>((static_cast<uint64_t>(savb::rawInteger(*c.field, c.payload)) >> slot.shift) & slot.mask);
  }
  if (c.field->type == 'Z') {
    return std::nullopt;
  }
  const double v = savb::numericValue(*c.field, c.payload);
  if (std::isnan(v) && c.field->type != 'd') {
    return std::nullopt;
  }
  return v;
}

// Calibrated columns the decoder rebuilds from a logged vector plus CAL
// (sav_log_to_csv.py, _cal_columns).
struct CalRule {
  const char* src_prefix;
  const char* dst_prefix;
  const char* suffix;
  const char* offset_key;
  const char* scale_key;
  bool mag;  // (a/1.5 - offset) * scale * 1.5 (1.5 mgauss/LSB); else (a - offset) / scale
};
constexpr CalRule kCalRules[] = {
    {"IMU/accel/", "IMU/accel_cal/", "_mps2", "imu_accel_offset", "imu_accel_scale", false},
    {"MAG/accel/", "MAG/accel_cal/", "_mps2", "mag_accel_offset", "mag_accel_scale", false},
    {"MAG/raw/", "MAG/cal/", "_mgauss", "mag_hard_iron", "mag_soft_iron_scale", true},
};
constexpr double kMagLsbMgauss = 1.5;

// Logs written before the logger shifted its wall-clock epochs hold true UTC
// (1970-01-01 00:00 UTC based); they name the readable column TIME/host_time_utc.
// Newer logs already hold local wall-clock time. Same rule as sav_log_to_csv.py.
constexpr uint16_t kLocalEpochVersion = 3;
constexpr const char* kUtcEpochMarker = "TIME/host_time_utc";
constexpr const char* kHostEpoch = "TIME/host_epoch_ns";
constexpr const char* kGpsEpoch = "GPS/epoch_ns";
constexpr int64_t kNsPerS = 1'000'000'000;

// Seconds since 1970 of a broken-down time read as if it were UTC.
int64_t civilSeconds(const std::tm& t) {
  const std::chrono::year_month_day day{
      std::chrono::year{t.tm_year + 1900}, std::chrono::month{static_cast<unsigned>(t.tm_mon + 1)},
      std::chrono::day{static_cast<unsigned>(t.tm_mday)}};
  const int64_t days = static_cast<int64_t>(std::chrono::sys_days{day}.time_since_epoch().count());
  return (days * 86400) + (int64_t{t.tm_hour} * 3600) + (int64_t{t.tm_min} * 60) + int64_t{t.tm_sec};
}

// This machine's offset from UTC at `epoch_ns`, in ns (+19800 s in IST).
int64_t localUtcOffsetNs(int64_t epoch_ns) {
  const std::time_t secs = static_cast<std::time_t>(epoch_ns / kNsPerS);
  std::tm utc{};
  std::tm local{};
#ifdef _WIN32
  gmtime_s(&utc, &secs);
  localtime_s(&local, &secs);
#else
  gmtime_r(&secs, &utc);
  localtime_r(&secs, &local);
#endif
  return (civilSeconds(local) - civilSeconds(utc)) * kNsPerS;
}

// How a derived column is computed.
struct Derived {
  enum class Kind { kSonarAge, kCal } kind;
  std::string src;  // kCal: source column
  std::string offset_key, scale_key;
  bool mag = false;
};

std::map<std::string, Derived> derivedColumns() {
  std::map<std::string, Derived> out;
  out["SONAR/age_s"] = {Derived::Kind::kSonarAge, {}, {}, {}, false};
  for (const auto& r : kCalRules) {
    for (const char axis : {'x', 'y', 'z'}) {
      out[std::string(r.dst_prefix) + axis + r.suffix] = {
          Derived::Kind::kCal, std::string(r.src_prefix) + axis + r.suffix,
          std::string("CAL/") + r.offset_key + '_' + axis, std::string("CAL/") + r.scale_key + '_' + axis, r.mag};
    }
  }
  return out;
}

class SavbSource : public PJ::FileSourceBase {
 public:
  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest;
  }

  std::string saveConfig() const override {
    return nlohmann::json{{"filepath", filepath_}}.dump();
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded()) {
      return PJ::unexpected(std::string("invalid config JSON"));
    }
    filepath_ = cfg.value("filepath", std::string{});
    return PJ::okStatus();
  }

  PJ::Status importData() override {
    if (filepath_.empty()) {
      return PJ::unexpected(std::string("no filepath configured"));
    }
    std::ifstream file(filepath_, std::ios::binary);
    if (!file.is_open()) {
      return PJ::unexpected(std::string("cannot open file: ") + filepath_);
    }
    const std::vector<uint8_t> buf((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    (void)runtimeHost().progressStart("Importing SAV log", buf.size(), true);

    columns_.clear();
    formats_.clear();
    derived_ = derivedColumns();
    // Older logs carry the *_utc names.
    text_labels_ = {"TIME/host_time", "GPS/time", "TIME/host_time_utc", "GPS/time_utc"};
    cal_.clear();
    sticky_.clear();
    row_.reset();
    fields_.clear();
    rows_ = 0;
    epoch_is_utc_ = false;
    local_offset_ns_.reset();
    PJ::Status status = PJ::okStatus();
    uint64_t since_progress = 0;

    savb::ScanStats stats;
    const std::string err = savb::scanLog(
        buf,
        [&](const savb::Format& fmt, const uint8_t* payload) {
          if (columns_.empty()) {
            epoch_is_utc_ = stats.version < kLocalEpochVersion &&
                            std::find(stats.columns.begin(), stats.columns.end(), kUtcEpochMarker) != stats.columns.end();
            // The header and every FMT are read before the first message, so
            // the text columns are known here: leave them out.
            for (const auto& col : stats.columns) {
              if (text_labels_.count(col) == 0) {
                columns_.push_back(col);
              }
            }
          }
          if (fmt.name == "TICK") {
            if (row_ && !(status = emitRow())) {
              return false;
            }
            row_ = sticky_;
          }
          if (fmt.name == "CAL_") {
            for (const auto& f : fmt.fields) {
              cal_[f.label] = savb::numericValue(f, payload);
            }
          } else {
            // Cells outlive this callback (the last row is written after the
            // scan), so they point at our own copy of the format.
            putCells(formats_.at(fmt.id), payload);
          }
          if (++since_progress == 4096) {
            since_progress = 0;
            (void)runtimeHost().progressUpdate(static_cast<uint64_t>(payload - buf.data()));
            if (runtimeHost().isStopRequested()) {
              return false;  // keep what is written so far ("Stop and Keep")
            }
          }
          return true;
        },
        stats,
        [&](const savb::Format& fmt) {
          formats_.insert_or_assign(fmt.id, fmt);
          for (const auto& f : fmt.fields) {
            if (f.type == 'Z') {
              text_labels_.insert(f.label);
            }
          }
        });
    if (!err.empty()) {
      return PJ::unexpected(err + ": " + filepath_);
    }
    if (!status) {
      return status;
    }
    if (columns_.empty()) {  // no message at all: still show the columns
      for (const auto& col : stats.columns) {
        if (text_labels_.count(col) == 0) {
          columns_.push_back(col);
        }
      }
    }
    if (row_ && !(status = emitRow())) {
      return status;
    }
    if (fields_.empty() && !(status = registerFields())) {
      return status;  // a log with no TICK still shows its columns
    }

    if (stats.skipped_bytes > 0) {
      runtimeHost().reportMessage(
          PJ::DataSourceMessageLevel::kWarning,
          "SAV log: skipped " + std::to_string(stats.skipped_bytes) +
              " unreadable byte(s) (corrupt stretch or truncated tail); data may be incomplete");
    }
    runtimeHost().reportMessage(
        PJ::DataSourceMessageLevel::kInfo,
        "Imported " + std::to_string(rows_) + " rows, " + std::to_string(columns_.size()) + " columns");
    return PJ::okStatus();
  }

 private:
  // Unpacks one message into label -> cell, the decoder's Format.cells():
  // a bitfield becomes one cell per slot. Sticky messages also update the
  // carry-forward set. Before the first TICK only sticky values are kept.
  void putCells(const savb::Format& fmt, const uint8_t* payload) {
    const bool sticky = (fmt.flags & savb::Format::kFlagSticky) != 0;
    for (const auto& f : fmt.fields) {
      auto put = [&](const std::string& label, int bit) {
        const RawCell cell{&f, payload, bit};
        if (sticky) {
          sticky_[label] = cell;
        }
        if (row_) {
          (*row_)[label] = cell;
        }
      };
      if (f.bits.empty()) {
        put(f.label, -1);
      } else {
        for (size_t b = 0; b < f.bits.size(); ++b) {
          put(f.bits[b].label, static_cast<int>(b));
        }
      }
    }
  }

  // A wall-clock epoch as local time. A true-UTC (older) log gets this machine's
  // offset, taken once from the first stamp and kept for the whole log, as the
  // logger does, so a DST change cannot make the axis jump; non-positive stamps
  // (no value) and newer logs pass through.
  int64_t localWallClock(int64_t epoch_ns) {
    if (!epoch_is_utc_ || epoch_ns <= 0) {
      return epoch_ns;
    }
    if (!local_offset_ns_) {
      local_offset_ns_ = localUtcOffsetNs(epoch_ns);
    }
    return epoch_ns + *local_offset_ns_;
  }

  // One topic named like the CSV loader's (the file's base name), and every
  // plotted CSV column registered in CSV order as float64, as the CSV loader
  // registers numeric columns.
  PJ::Status registerFields() {
    std::string topic_name = std::filesystem::path(filepath_).stem().string();
    if (topic_name.empty()) {
      topic_name = "data";
    }
    auto topic = writeHost().ensureTopic(topic_name);
    if (!topic) {
      return PJ::unexpected(topic.error());
    }
    topic_ = *topic;
    fields_.clear();
    for (const auto& col : columns_) {
      auto field = writeHost().ensureField(topic_, col, PJ::PrimitiveType::kFloat64);
      if (!field) {
        return PJ::unexpected(field.error());
      }
      fields_.push_back(*field);
    }
    return PJ::okStatus();
  }

  // Writes the current row as one record: each CSV column that has a value
  // (logged, carried, or derived), stamped with the row's host clock.
  PJ::Status emitRow() {
    if (fields_.empty()) {
      if (auto s = registerFields(); !s) {
        return s;
      }
    }
    const auto& row = *row_;
    auto cell = [&row](const std::string& label) -> const RawCell* {
      auto it = row.find(label);
      return it == row.end() ? nullptr : &it->second;
    };
    auto raw_int = [&](const std::string& label) -> std::optional<int64_t> {
      const RawCell* c = cell(label);
      if (c == nullptr || c->bit >= 0) {
        return std::nullopt;
      }
      return savb::rawInteger(*c->field, c->payload);
    };

    int64_t ts = 0;
    if (auto host = raw_int(kHostEpoch)) {
      ts = localWallClock(*host);
    }

    record_.clear();
    for (size_t i = 0; i < columns_.size(); ++i) {
      const std::string& col = columns_[i];
      if (const RawCell* c = cell(col)) {
        if (c->bit >= 0) {
          record_.push_back({.field = fields_[i], .value = *asNumber(*c)});
        } else if (col == kHostEpoch || col == kGpsEpoch) {
          record_.push_back(
              {.field = fields_[i],
               .value = static_cast<double>(localWallClock(savb::rawInteger(*c->field, c->payload)))});
        } else {
          record_.push_back({.field = fields_[i], .value = savb::numericValue(*c->field, c->payload)});
        }
        continue;
      }
      auto d = derived_.find(col);
      if (d == derived_.end()) {
        continue;
      }
      std::optional<double> num;
      switch (d->second.kind) {
        case Derived::Kind::kSonarAge: {
          auto sonar = raw_int("SONAR/last_update_ns");
          auto mono = raw_int("TICK/mono_ns");
          if (sonar && mono) {
            num = static_cast<double>(*mono - *sonar) / 1e9;
          }
          break;
        }
        case Derived::Kind::kCal: {
          const RawCell* src = cell(d->second.src);
          const auto off = cal_.find(d->second.offset_key);
          const auto scale = cal_.find(d->second.scale_key);
          if (src == nullptr || off == cal_.end() || scale == cal_.end()) {
            break;
          }
          if (auto a = asNumber(*src)) {
            num = d->second.mag ? (*a / kMagLsbMgauss - off->second) * scale->second * kMagLsbMgauss
                                : (*a - off->second) / scale->second;
          }
          break;
        }
      }
      if (num) {
        record_.push_back({.field = fields_[i], .value = *num});
      }
    }
    ++rows_;
    return writeHost().appendBoundRecord(
        topic_, PJ::Timestamp{ts}, PJ::Span<const PJ::sdk::BoundFieldValue>(record_.data(), record_.size()));
  }

  std::string filepath_;
  std::vector<std::string> columns_;  // plotted CSV columns, in the file header's order
  std::map<uint8_t, savb::Format> formats_;  // by message id; RawCell::field points in here
  std::set<std::string> text_labels_;  // text columns, which are not plotted
  std::map<std::string, Derived> derived_;
  std::map<std::string, double> cal_;  // CAL/<key> -> value
  std::unordered_map<std::string, RawCell> sticky_;
  std::optional<std::unordered_map<std::string, RawCell>> row_;  // the row being built; none before the first TICK
  PJ::sdk::TopicHandle topic_{};
  std::vector<PJ::sdk::FieldHandle> fields_;  // parallel to columns_
  std::vector<PJ::sdk::BoundFieldValue> record_;
  uint64_t rows_ = 0;
  bool epoch_is_utc_ = false;                // see localWallClock()
  std::optional<int64_t> local_offset_ns_;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(SavbSource, kSavbManifest)
