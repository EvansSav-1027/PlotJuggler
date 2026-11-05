// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/TrackColorItem.h"

#include <qwt_scale_map.h>

#include <QImage>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <algorithm>
#include <cmath>

#include "pj_plotting/PointSeriesXY.h"
#include "pj_plotting/RasterLabel.h"

namespace PJ {
namespace {

constexpr int kColorBuckets = 48;
constexpr double kTrackWidthPx = 3.0;
constexpr double kMinStepPx = 1.0;
constexpr int kBarWidth = 10;
constexpr int kBarHeight = 110;

QColor bucketColor(Colormap colormap, int bucket) {
  const ColormapRgb rgb = colorFor(colormap, static_cast<float>(bucket) / (kColorBuckets - 1));
  return QColor::fromRgbF(rgb.r, rgb.g, rgb.b);
}

}  // namespace

TrackColorItem::TrackColorItem() {
  setZ(25.0);  // above the plain curves (~20), below the marker overlay (40)
  setItemAttribute(QwtPlotItem::AutoScale, false);
  setItemAttribute(QwtPlotItem::Legend, false);
}

void TrackColorItem::draw(
    QPainter* painter, const QwtScaleMap& xMap, const QwtScaleMap& yMap, const QRectF& canvasRect) const {
  if (!tracks_provider_ || values_ == nullptr) {
    return;
  }
  const auto range = values_->valueRange();
  if (!range.has_value()) {
    return;
  }
  const double span = range->max - range->min;

  painter->save();
  painter->setClipRect(canvasRect, Qt::IntersectClip);
  painter->setRenderHint(QPainter::Antialiasing, true);

  QPen pen;
  pen.setWidthF(kTrackWidthPx);
  pen.setCapStyle(Qt::RoundCap);
  pen.setJoinStyle(Qt::RoundJoin);

  for (const PointSeriesXY* track : tracks_provider_()) {
    QPolygonF run;
    int run_bucket = -1;
    QPointF last_px;
    bool have_last = false;
    const auto flush = [&]() {
      if (run.size() > 1) {
        pen.setColor(bucketColor(colormap_, run_bucket));
        painter->setPen(pen);
        painter->drawPolyline(run);
      }
      run.clear();
    };

    const std::size_t count = track->size();
    for (std::size_t i = 0; i < count; ++i) {
      const QPointF point = track->sample(i);
      if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
        flush();
        have_last = false;
        continue;
      }
      const QPointF px(xMap.transform(point.x()), yMap.transform(point.y()));
      if (have_last && std::hypot(px.x() - last_px.x(), px.y() - last_px.y()) < kMinStepPx) {
        continue;
      }
      const auto value = values_->valueAtOrBefore(track->timestamp(i));
      if (!value.has_value()) {
        flush();
        have_last = false;
        continue;
      }
      const int bucket =
          std::clamp(static_cast<int>(std::lround((*value - range->min) / span * (kColorBuckets - 1))), 0, kColorBuckets - 1);
      if (bucket != run_bucket) {
        // The segment into this sample takes the new colour, so the new run
        // starts at the previous sample and the colours meet without a gap.
        flush();
        if (have_last) {
          run.append(last_px);
        }
        run_bucket = bucket;
      }
      run.append(px);
      last_px = px;
      have_last = true;
    }
    flush();
  }

  painter->restore();
  drawColorBar(painter, canvasRect, range->min, range->max);
}

void TrackColorItem::drawColorBar(QPainter* painter, const QRectF& canvasRect, double min, double max) const {
  QImage bar(1, kColorBuckets * 4, QImage::Format_RGB32);
  for (int row = 0; row < bar.height(); ++row) {
    const int bucket = (bar.height() - 1 - row) * (kColorBuckets - 1) / (bar.height() - 1);  // max on top
    bar.setPixelColor(0, row, bucketColor(colormap_, bucket));
  }

  const QString top_text = QString::number(max, 'g', 5);
  const QString bottom_text = QString::number(min, 'g', 5);
  const double label_width =
      std::max(rasterLabelSize(painter, top_text).width(), rasterLabelSize(painter, bottom_text).width());
  const double x = canvasRect.right() - kBarWidth - label_width - 12.0;
  // Bottom-right, above the map attribution: the top-right corner is the legend's default home.
  const double y = canvasRect.bottom() - kBarHeight - 30.0;

  painter->save();
  painter->drawImage(QRectF(x + label_width + 4.0, y, kBarWidth, kBarHeight), bar);
  painter->setPen(QPen(QColor(0, 0, 0, 160), 1.0));
  painter->setBrush(Qt::NoBrush);
  painter->drawRect(QRectF(x + label_width + 4.0, y, kBarWidth, kBarHeight));
  painter->restore();

  const double top_w = rasterLabelSize(painter, top_text).width();
  const double bottom_w = rasterLabelSize(painter, bottom_text).width();
  drawRasterLabel(painter, QPointF(x + label_width - top_w, y), top_text);
  drawRasterLabel(
      painter, QPointF(x + label_width - bottom_w, y + kBarHeight - rasterLabelSize(painter, bottom_text).height()),
      bottom_text);
}

}  // namespace PJ
