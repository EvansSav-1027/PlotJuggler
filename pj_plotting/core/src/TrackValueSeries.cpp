// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/TrackValueSeries.h"

#include <algorithm>
#include <cmath>

#include "pj_datastore/chunk.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_runtime/SessionManager.h"

namespace PJ {

TrackValueSeries::TrackValueSeries(SessionManager* session, CurveDescriptor source)
    : session_(session), source_(std::move(source)) {}

void TrackValueSeries::ensureBuilt() const {
  if (!dirty_) {
    return;
  }
  dirty_ = false;
  samples_.clear();
  range_.reset();
  if (session_ == nullptr) {
    return;
  }

  const auto lock = session_->dataEngine().lockEngine();
  const TopicStorage* storage = session_->dataEngine().getTopicStorage(source_.topic_id);
  if (storage == nullptr) {
    return;
  }
  const Timestamp retention_floor = storage->retentionFloor();
  for (const TopicChunk& chunk : storage->sealedChunks()) {
    if (source_.column_index >= chunk.columns.size()) {
      continue;
    }
    for (std::size_t row = 0; row < chunk.stats.row_count; ++row) {
      const Timestamp time = chunk.readTimestamp(row);
      if (time < retention_floor || chunk.isNull(source_.column_index, row)) {
        continue;
      }
      const double value = chunk.readNumericAsDouble(source_.column_index, row);
      if (!std::isfinite(value)) {
        continue;
      }
      samples_.emplace_back(time, value);
      if (range_.has_value()) {
        range_->min = std::min(range_->min, value);
        range_->max = std::max(range_->max, value);
      } else {
        range_ = Range<double>{.min = value, .max = value};
      }
    }
  }
  if (range_.has_value() && !(range_->max > range_->min)) {
    range_->max = range_->min + 1.0;
  }
}

std::optional<double> TrackValueSeries::valueAtOrBefore(Timestamp raw_time) const {
  ensureBuilt();
  const auto after = std::upper_bound(
      samples_.begin(), samples_.end(), raw_time, [](Timestamp time, const auto& sample) { return time < sample.first; });
  if (after == samples_.begin()) {
    return std::nullopt;
  }
  return std::prev(after)->second;
}

std::optional<Range<double>> TrackValueSeries::valueRange() const {
  ensureBuilt();
  return range_;
}

}  // namespace PJ
