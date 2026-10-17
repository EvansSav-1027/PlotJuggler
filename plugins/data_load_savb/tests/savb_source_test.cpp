// SPDX-License-Identifier: MPL-2.0
//
// Loads the built plugin through the SDK host, imports synthetic SAVB logs
// written with the logger's own framing, and checks that what reaches the host
// is what sav_log_to_csv.py's CSV gives PlotJuggler's CSV loader: one topic,
// one field per CSV column, one record per row of non-blank cells.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "pj_base/plugin_data_api.h"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_plugins/host/data_source_library.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "savb_log.hpp"

namespace {

// ---------------------------------------------------------------------------
// Log writer — same framing as sensor_data_logger.cpp
// ---------------------------------------------------------------------------

class LogBuilder {
 public:
  LogBuilder(const std::string& columns, uint16_t version = 2) {
    bytes_ += "SAVB";
    put(version);
    put(static_cast<uint16_t>(columns.size()));
    bytes_ += columns;
  }

  void fmt(uint8_t id, const std::string& name, const std::string& types, const std::string& decimals,
           const std::string& labels, uint8_t flags = 0) {
    size_t len = 0;
    for (char t : types) {
      len += savb::typeSize(t);
    }
    header(128);
    bytes_ += static_cast<char>(id);
    bytes_ += static_cast<char>(len);
    bytes_ += static_cast<char>(flags);
    bytes_ += name;
    bytes_ += static_cast<char>(types.size());
    bytes_ += types;
    bytes_ += decimals;
    put(static_cast<uint16_t>(labels.size()));
    bytes_ += labels;
  }

  void header(uint8_t id) {
    bytes_ += static_cast<char>(0xA3);
    bytes_ += static_cast<char>(0x95);
    bytes_ += static_cast<char>(id);
  }

  template <typename T>
  void put(T v) {
    char raw[sizeof(T)];
    std::memcpy(raw, &v, sizeof(T));
    bytes_.append(raw, sizeof(T));
  }

  void z(const std::string& s) {
    std::string padded = s;
    padded.resize(64, '\0');
    bytes_ += padded;
  }

  void raw(const std::string& s) { bytes_ += s; }

  const std::string& bytes() const { return bytes_; }

 private:
  std::string bytes_;
};

constexpr uint8_t kTick = 1, kImu = 2, kCtrl = 3, kMode = 4, kCal = 5, kSonar = 6;

// The CSV column order the logger stores — a subset of the real one, with
// derived (TIME/host_time, IMU/accel_cal/x_mps2, SONAR/age_s) and
// never-logged (BAT/bat1/voltage) columns. TICK/mono_ns is logged but is not a
// CSV column.
const std::vector<std::string> kColumns = {
    "seq", "TIME/host_time", "TIME/host_epoch_ns", "IMU/gyro/x_degs", "IMU/accel/x_mps2",
    "IMU/accel_cal/x_mps2", "IMU/temp_c", "EKF/status/position_quality", "PWM/left_us", "RCIN/estop",
    "STATUS/operator_mode", "STATUS/active_behavior", "SONAR/last_update_ns", "SONAR/age_s", "BAT/bat1/voltage"};

// kColumns minus its text columns, which are not plotted.
const std::vector<std::string> kPlottedColumns = {
    "seq", "TIME/host_epoch_ns", "IMU/gyro/x_degs", "IMU/accel/x_mps2", "IMU/accel_cal/x_mps2", "IMU/temp_c",
    "EKF/status/position_quality", "PWM/left_us", "RCIN/estop", "STATUS/operator_mode", "SONAR/last_update_ns",
    "SONAR/age_s", "BAT/bat1/voltage"};

// `legacy_utc` names the readable time column the way older logs (true-UTC
// epochs) do: TIME/host_time_utc.
std::string joined(bool legacy_utc) {
  std::string out;
  for (const auto& c : kColumns) {
    out += (out.empty() ? "" : ",") + ((legacy_utc && c == "TIME/host_time") ? std::string("TIME/host_time_utc") : c);
  }
  return out;
}

// Header + FMTs + CAL, as SensorDataLogger::open() writes them.
LogBuilder makePreamble(bool legacy_utc = false) {
  LogBuilder b(joined(legacy_utc));
  b.fmt(kTick, "TICK", "Iqq", "000", "seq,TIME/host_epoch_ns,TICK/mono_ns");
  b.fmt(kImu, "IMU_", "iib", "440", "IMU/gyro/x_degs,IMU/accel/x_mps2,IMU/temp_c");
  b.fmt(kCtrl, "CTRL", "Hx", "00", "PWM/left_us,RCIN/estop:1|EKF/status/position_quality:2");
  b.fmt(kMode, "MODE", "BZ", "00", "STATUS/operator_mode,STATUS/active_behavior", 0x01);
  b.fmt(kCal, "CAL_", "dd", "00", "CAL/imu_accel_offset_x,CAL/imu_accel_scale_x");
  b.fmt(kSonar, "SONR", "q", "0", "SONAR/last_update_ns");
  b.header(kCal);
  b.put(0.5);
  b.put(2.0);
  return b;
}

void tick(LogBuilder& b, uint32_t seq, int64_t host_ns, int64_t mono_ns) {
  b.header(kTick);
  b.put(seq);
  b.put(host_ns);
  b.put(mono_ns);
}

void imu(LogBuilder& b, int32_t gyro_x, int32_t accel_x) {
  b.header(kImu);
  b.put(gyro_x);
  b.put(accel_x);
  b.put(int8_t{41});
}

// ---------------------------------------------------------------------------
// Recording host
// ---------------------------------------------------------------------------

struct Record {
  int64_t ts;
  std::map<std::string, PJ_scalar_value_t> values;  // only the cells the record carries
};

struct WriteState {
  std::vector<std::string> topics;                             // by handle id - 1
  std::vector<std::string> field_order;                        // registration order
  std::map<std::pair<uint32_t, uint32_t>, std::string> fields;  // (topic, field id) -> name
  std::map<std::string, PJ_primitive_type_t> types;
  std::vector<Record> records;
  std::deque<std::string> strings;  // owned copies of string cells (deque: stable addresses)
  uint32_t next_field = 0;
};

struct RuntimeState {
  std::vector<std::string> messages;
};

bool ensureTopic(void* ctx, PJ_string_view_t name, PJ_topic_handle_t* out, PJ_error_t*) noexcept {
  auto* s = static_cast<WriteState*>(ctx);
  s->topics.emplace_back(name.data, name.size);
  *out = PJ_topic_handle_t{static_cast<uint32_t>(s->topics.size())};
  return true;
}

bool ensureField(void* ctx, PJ_topic_handle_t topic, PJ_string_view_t name, PJ_primitive_type_t type,
                 PJ_field_handle_t* out, PJ_error_t*) noexcept {
  auto* s = static_cast<WriteState*>(ctx);
  const uint32_t id = ++s->next_field;
  const std::string n(name.data, name.size);
  s->fields[{topic.id, id}] = n;
  s->field_order.push_back(n);
  s->types[n] = type;
  *out = PJ_field_handle_t{topic, id};
  return true;
}

bool appendRecord(void*, PJ_topic_handle_t, int64_t, const PJ_named_field_value_t*, uint64_t, PJ_error_t*) noexcept {
  return true;
}

bool appendBound(void* ctx, PJ_topic_handle_t, int64_t ts, const PJ_bound_field_value_t* fields, uint64_t n,
                 PJ_error_t*) noexcept {
  auto* s = static_cast<WriteState*>(ctx);
  Record rec{ts, {}};
  for (uint64_t i = 0; i < n; ++i) {
    PJ_scalar_value_t v = fields[i].value;
    if (v.type == PJ_PRIMITIVE_TYPE_STRING) {
      // The view only lives for this call; keep a copy and point at it.
      s->strings.emplace_back(v.data.as_string.data, v.data.as_string.size);
      v.data.as_string.data = s->strings.back().c_str();
    }
    rec.values[s->fields[{fields[i].field.topic.id, fields[i].field.id}]] = v;
  }
  s->records.push_back(rec);
  return true;
}

bool appendArrow(void*, PJ_topic_handle_t, struct ArrowArrayStream* stream, PJ_string_view_t, PJ_error_t*) noexcept {
  if (stream != nullptr && stream->release != nullptr) {
    stream->release(stream);
  }
  return true;
}

void reportMessage(void* ctx, PJ_data_source_message_level_t, PJ_string_view_t msg) noexcept {
  static_cast<RuntimeState*>(ctx)->messages.emplace_back(msg.data, msg.size);
}
bool progressStart(void*, PJ_string_view_t, uint64_t, bool, PJ_error_t*) noexcept { return true; }
bool progressUpdate(void*, uint64_t) noexcept { return true; }
void progressFinish(void*) noexcept {}
bool isStopRequested(void*) noexcept { return false; }
void notifyState(void*, PJ_data_source_state_t) noexcept {}
void requestStop(void*, PJ_data_source_state_t, PJ_string_view_t) noexcept {}
bool ensureParserBinding(void*, const PJ_parser_binding_request_t*, PJ_parser_binding_handle_t* out,
                         PJ_error_t*) noexcept {
  *out = PJ_parser_binding_handle_t{1};
  return true;
}
int showMessageBox(void*, PJ_message_box_type_t, PJ_string_view_t, PJ_string_view_t, int) noexcept {
  return PJ_MSG_BTN_OK;
}

class SavbSourceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto lib = PJ::DataSourceLibrary::load(SAVB_PLUGIN_PATH);
    ASSERT_TRUE(lib) << lib.error();
    lib_ = std::move(*lib);

    static const PJ_source_write_host_vtable_t write_vtable = {
        .abi_version = PJ_PLUGIN_DATA_API_VERSION,
        .struct_size = sizeof(PJ_source_write_host_vtable_t),
        .ensure_topic = ensureTopic,
        .ensure_field = ensureField,
        .append_record = appendRecord,
        .append_bound_record = appendBound,
        .append_arrow_stream = appendArrow,
    };
    static const PJ_data_source_runtime_host_vtable_t runtime_vtable = {
        .protocol_version = 1,
        .struct_size = sizeof(PJ_data_source_runtime_host_vtable_t),
        .report_message = reportMessage,
        .progress_start = progressStart,
        .progress_update = progressUpdate,
        .progress_finish = progressFinish,
        .is_stop_requested = isStopRequested,
        .notify_state = notifyState,
        .request_stop = requestStop,
        .ensure_parser_binding = ensureParserBinding,
        .show_message_box = showMessageBox,
        .list_available_encodings = nullptr,
        .push_message = nullptr,
        .notify_available_topics = nullptr,
        .attach_source_record = nullptr,
        .complete_ingest = nullptr,
        .set_dataset_metadata = nullptr,
    };
    registry_.registerService<PJ::sdk::SourceWriteHostService>(
        PJ_source_write_host_t{.ctx = &write_, .vtable = &write_vtable});
    registry_.registerService<PJ::sdk::DataSourceRuntimeHostService>(
        PJ_data_source_runtime_host_t{.ctx = &runtime_, .vtable = &runtime_vtable});
  }

  /// Writes `bytes` to <tmp>/log_test.bin and imports it; returns whether start() succeeded.
  bool import(const std::string& bytes) {
    const auto path = std::filesystem::temp_directory_path() / "log_test.bin";
    std::ofstream(path, std::ios::binary) << bytes;
    auto handle = lib_.createHandle();
    EXPECT_TRUE(handle.loadConfig(R"({"filepath":")" + path.string() + R"("})"));
    EXPECT_TRUE(handle.bind(registry_.view()));
    const bool ok = static_cast<bool>(handle.start());
    std::filesystem::remove(path);
    return ok;
  }

  double num(size_t row, const std::string& col) { return write_.records.at(row).values.at(col).data.as_float64; }
  bool has(size_t row, const std::string& col) { return write_.records.at(row).values.count(col) != 0; }

  PJ::DataSourceLibrary lib_;
  WriteState write_;
  RuntimeState runtime_;
  PJ::ServiceRegistryBuilder registry_;
};

// 2025-10-16 12:30:12.250 UTC.
constexpr int64_t kEpoch = 1'760'617'812'250'000'000;

TEST_F(SavbSourceTest, OneTopicWithExactlyTheCsvColumns) {
  auto b = makePreamble();
  tick(b, 0, kEpoch, 5'000'000'000);
  imu(b, 12345, 98100);
  ASSERT_TRUE(import(b.bytes()));

  // Named like the CSV loader names a CSV: the file's base name.
  ASSERT_EQ(write_.topics, std::vector<std::string>{"log_test"});
  // Every plottable CSV column, in CSV order — no text columns
  // (TIME/host_time, STATUS/active_behavior), no TICK/mono_ns, no CAL/*,
  // nothing renamed — all float64, as the CSV loader registers numbers.
  EXPECT_EQ(write_.field_order, kPlottedColumns);
  for (const auto& [name, type] : write_.types) {
    EXPECT_EQ(type, PJ_PRIMITIVE_TYPE_FLOAT64) << name;
  }
}

TEST_F(SavbSourceTest, OneRecordPerTickWithTheRowsCells) {
  auto b = makePreamble();
  tick(b, 0, kEpoch, 5'000'000'000);
  imu(b, 12345, 98100);  // 1.2345 deg/s, 9.81 m/s^2
  tick(b, 1, kEpoch + 20'000'000, 5'020'000'000);  // nothing fresh this tick
  ASSERT_TRUE(import(b.bytes()));

  ASSERT_EQ(write_.records.size(), 2u);
  EXPECT_EQ(write_.records[0].ts, kEpoch);
  EXPECT_EQ(write_.records[1].ts, kEpoch + 20'000'000);
  EXPECT_DOUBLE_EQ(num(0, "seq"), 0.0);
  EXPECT_DOUBLE_EQ(num(0, "IMU/gyro/x_degs"), 1.2345);
  EXPECT_DOUBLE_EQ(num(0, "IMU/temp_c"), 41.0);
  EXPECT_DOUBLE_EQ(num(0, "TIME/host_epoch_ns"), static_cast<double>(kEpoch));
  EXPECT_DOUBLE_EQ(num(0, "IMU/accel_cal/x_mps2"), (9.81 - 0.5) / 2.0);  // from CAL
  // A blank CSV cell is left out of the record, as the CSV loader does.
  EXPECT_FALSE(has(1, "IMU/gyro/x_degs"));
  EXPECT_FALSE(has(1, "IMU/accel_cal/x_mps2"));
  EXPECT_FALSE(has(0, "BAT/bat1/voltage"));
  EXPECT_DOUBLE_EQ(num(1, "seq"), 1.0);
}

TEST_F(SavbSourceTest, StickyValuesAreCarriedOntoEveryLaterRow) {
  auto b = makePreamble();
  tick(b, 0, kEpoch, 0);
  b.header(kMode);
  b.put(uint8_t{2});
  b.z("waypoint_follower");
  tick(b, 1, kEpoch + 1, 1);
  tick(b, 2, kEpoch + 2, 2);
  ASSERT_TRUE(import(b.bytes()));
  ASSERT_EQ(write_.records.size(), 3u);
  for (size_t r = 0; r < 3; ++r) {
    EXPECT_DOUBLE_EQ(num(r, "STATUS/operator_mode"), 2.0) << r;
    EXPECT_FALSE(has(r, "STATUS/active_behavior")) << r;  // text: not plotted
  }
}

TEST_F(SavbSourceTest, SentinelsAreNaNAndBlankTheDerivedColumn) {
  auto b = makePreamble();
  tick(b, 0, kEpoch, 0);
  imu(b, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max());
  ASSERT_TRUE(import(b.bytes()));
  EXPECT_TRUE(std::isnan(num(0, "IMU/gyro/x_degs")));   // CSV "nan"
  EXPECT_TRUE(std::isnan(num(0, "IMU/accel/x_mps2")));  // CSV "ovf"
  EXPECT_FALSE(has(0, "IMU/accel_cal/x_mps2"));         // decoder leaves it blank
}

TEST_F(SavbSourceTest, BitfieldSlotsAndSonarAge) {
  auto b = makePreamble();
  tick(b, 0, kEpoch, 7'500'000'000);
  b.header(kCtrl);
  b.put(uint16_t{1500});
  b.put(uint8_t{0b101});  // estop=1, position_quality=2
  b.header(kSonar);
  b.put(int64_t{7'250'000'000});
  ASSERT_TRUE(import(b.bytes()));
  EXPECT_DOUBLE_EQ(num(0, "PWM/left_us"), 1500.0);
  EXPECT_DOUBLE_EQ(num(0, "RCIN/estop"), 1.0);
  EXPECT_DOUBLE_EQ(num(0, "EKF/status/position_quality"), 2.0);
  EXPECT_DOUBLE_EQ(num(0, "SONAR/age_s"), 0.25);  // TICK/mono_ns - SONAR/last_update_ns
}

#ifndef _WIN32
// Holds the process timezone at UTC+5:30 (a POSIX TZ string, so no tzdata is
// needed) for one test.
class ScopedIst {
 public:
  ScopedIst() {
    const char* old = std::getenv("TZ");
    had_ = old != nullptr;
    saved_ = had_ ? old : "";
    setenv("TZ", "IST-5:30", 1);
    tzset();
  }
  ~ScopedIst() {
    if (had_) {
      setenv("TZ", saved_.c_str(), 1);
    } else {
      unsetenv("TZ");
    }
    tzset();
  }

 private:
  bool had_ = false;
  std::string saved_;
};
constexpr int64_t kIstOffsetNs = (5 * 3600 + 30 * 60) * 1'000'000'000LL;

// The epoch counts from 1970-01-01 00:00 UTC. An older log holds it as true
// UTC, so the time axis gets this machine's offset to read as local time —
// the same shift sav_log_to_csv.py applies — fixed once for the whole log.
TEST_F(SavbSourceTest, OlderUtcLogsAreShownInLocalTime) {
  const ScopedIst ist;
  auto b = makePreamble(/*legacy_utc=*/true);
  tick(b, 0, kEpoch, 0);
  tick(b, 1, kEpoch + 20'000'000, 20'000'000);
  ASSERT_TRUE(import(b.bytes()));
  ASSERT_EQ(write_.records.size(), 2u);
  EXPECT_EQ(write_.records[0].ts, kEpoch + kIstOffsetNs);
  EXPECT_EQ(write_.records[1].ts, kEpoch + 20'000'000 + kIstOffsetNs);
  EXPECT_DOUBLE_EQ(num(0, "TIME/host_epoch_ns"), static_cast<double>(kEpoch + kIstOffsetNs));
}

// Newer logs already hold local wall-clock time: not shifted a second time.
TEST_F(SavbSourceTest, NewerLogsAreNotShiftedAgain) {
  const ScopedIst ist;
  auto b = makePreamble();
  tick(b, 0, kEpoch, 0);
  ASSERT_TRUE(import(b.bytes()));
  ASSERT_EQ(write_.records.size(), 1u);
  EXPECT_EQ(write_.records[0].ts, kEpoch);
  EXPECT_DOUBLE_EQ(num(0, "TIME/host_epoch_ns"), static_cast<double>(kEpoch));
}
#endif

TEST_F(SavbSourceTest, ResyncsOverGarbageAndKeepsDataBeforeATruncatedTail) {
  auto b = makePreamble();
  tick(b, 0, kEpoch, 0);
  b.raw(std::string("\x01\x02\xA3\x07junk", 8));
  imu(b, 10000, 0);
  tick(b, 1, kEpoch + 1, 1);
  b.header(kImu);
  b.raw("cut");  // power loss mid-message
  ASSERT_TRUE(import(b.bytes()));
  ASSERT_EQ(write_.records.size(), 2u);
  EXPECT_DOUBLE_EQ(num(0, "IMU/gyro/x_degs"), 1.0);
  bool warned = false;
  for (const auto& m : runtime_.messages) {
    warned |= m.find("skipped") != std::string::npos;
  }
  EXPECT_TRUE(warned);
}

TEST_F(SavbSourceTest, RejectsNonSavbFiles) {
  EXPECT_FALSE(import("PK\x03\x04 not a log at all"));
  LogBuilder future("seq", 99);
  EXPECT_FALSE(import(future.bytes()));
}

}  // namespace
