// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/RasterLabel.h"

#include <QFontMetricsF>
#include <QImage>
#include <QPaintDevice>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace PJ {
namespace {

QFont labelFont(const QPainter* painter) {
  QFont font = painter->font();
  font.setPointSizeF(7.5);
  return font;
}

}  // namespace

QSizeF rasterLabelSize(const QPainter* painter, const QString& text) {
  const QFontMetricsF metrics(labelFont(painter));
  return {metrics.horizontalAdvance(text) + 8.0, metrics.height() + 2.0};
}

void drawRasterLabel(QPainter* painter, const QPointF& top_left, const QString& text) {
  if (text.isEmpty()) {
    return;
  }
  const QFont font = labelFont(painter);
  const QSizeF size = rasterLabelSize(painter, text);

  // Qwt's GL backing store paints on a DPR-1 device and carries the display
  // ratio as a painter scale, so the effective ratio is the product of both.
  const double device_ratio = painter->device() != nullptr ? painter->device()->devicePixelRatioF() : 1.0;
  const double dpr = std::max(1.0, device_ratio * std::abs(painter->worldTransform().m11()));
  QImage label(
      static_cast<int>(std::ceil(size.width() * dpr)), static_cast<int>(std::ceil(size.height() * dpr)),
      QImage::Format_ARGB32_Premultiplied);
  label.setDevicePixelRatio(dpr);
  label.fill(QColor(0, 0, 0, 140));
  {
    QPainter text_painter(&label);
    text_painter.setRenderHint(QPainter::TextAntialiasing, true);
    text_painter.setFont(font);
    text_painter.setPen(QColor(235, 235, 235));
    text_painter.drawText(QRectF(QPointF(0, 0), size), Qt::AlignCenter, text);
  }
  painter->drawImage(top_left, label);
}

}  // namespace PJ
