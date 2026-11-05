#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <optional>
#include <utility>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_runtime/CurveDescriptor.h"

namespace PJ {

class SessionManager;

/// A scalar field materialized as time-ordered (raw timestamp, value) pairs, for
/// colouring an XY track by a third quantity (e.g. sonar depth along a GPS path).
///
/// Holds plain values only — no chunk pointers — so streaming retention can free
/// chunks without any lifetime coupling. Lazily rebuilt after onTopicCommitted() /
/// onDataCleared(); null and non-finite samples are skipped. GUI thread only.
class TrackValueSeries {
 public:
  TrackValueSeries(SessionManager* session, CurveDescriptor source);

  [[nodiscard]] const CurveDescriptor& source() const noexcept {
    return source_;
  }

  void onTopicCommitted() {
    dirty_ = true;
  }
  void onDataCleared() {
    dirty_ = true;
  }

  /// The most recent value at-or-before `raw_time` (a sample holds until the next
  /// one, no interpolation), or nullopt before the first sample. The timestamps are
  /// raw dataset time, the same domain as PointSeriesXY::timestamp().
  [[nodiscard]] std::optional<double> valueAtOrBefore(Timestamp raw_time) const;

  /// Min/max over every finite sample; nullopt when the series is empty. A flat
  /// series is widened so the range is never degenerate.
  [[nodiscard]] std::optional<Range<double>> valueRange() const;

 private:
  void ensureBuilt() const;

  SessionManager* session_ = nullptr;
  CurveDescriptor source_;
  mutable std::vector<std::pair<Timestamp, double>> samples_;
  mutable std::optional<Range<double>> range_;
  mutable bool dirty_ = true;
};

}  // namespace PJ
