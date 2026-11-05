#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <qwt_plot_item.h>

#include <functional>
#include <memory>
#include <vector>

#include "pj_plotting/TrackValueSeries.h"
#include "pj_widgets/Colormap.h"

class QPainter;
class QwtScaleMap;

namespace PJ {

class PointSeriesXY;

/**
 * Draws XY tracks coloured by a third scalar (e.g. sonar depth along a GPS path).
 *
 * Each track sample takes the value of the colour field at-or-before its
 * timestamp, normalized over the field's full range and mapped through the shared
 * Colormap. The path is drawn as one polyline per run of equal colour bucket
 * (per-segment QPainter calls are pathological on the GL paint engine) over the
 * plain curve, with a min/max colour bar in the canvas' top-right corner.
 * Samples closer than one pixel to the previous one are skipped.
 */
class TrackColorItem : public QwtPlotItem {
 public:
  TrackColorItem();

  /// Tracks to colour, re-queried on every paint. The pointers must stay valid for the call.
  void setTracksProvider(std::function<std::vector<const PointSeriesXY*>()> provider) {
    tracks_provider_ = std::move(provider);
  }

  /// Colour field; null clears it (nothing is drawn).
  void setValueSeries(std::unique_ptr<TrackValueSeries> values) {
    values_ = std::move(values);
  }
  [[nodiscard]] TrackValueSeries* valueSeries() const noexcept {
    return values_.get();
  }

  void setColormap(Colormap colormap) {
    colormap_ = colormap;
  }
  [[nodiscard]] Colormap colormap() const noexcept {
    return colormap_;
  }

  [[nodiscard]] int rtti() const override {
    return QwtPlotItem::Rtti_PlotUserItem + 2;
  }

  void draw(
      QPainter* painter, const QwtScaleMap& xMap, const QwtScaleMap& yMap, const QRectF& canvasRect) const override;

 private:
  void drawColorBar(QPainter* painter, const QRectF& canvasRect, double min, double max) const;

  std::function<std::vector<const PointSeriesXY*>()> tracks_provider_;
  std::unique_ptr<TrackValueSeries> values_;
  Colormap colormap_ = Colormap::kTurbo;
};

}  // namespace PJ
