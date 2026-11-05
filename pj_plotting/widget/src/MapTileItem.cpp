// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/MapTileItem.h"

#include <qwt_scale_map.h>

#include <QImage>
#include <QPainter>
#include <QRegularExpression>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <numbers>

#include "pj_plotting/MapTileProvider.h"
#include "pj_plotting/RasterLabel.h"

namespace PJ {
namespace {

constexpr double kMaxMercatorLat = 85.05112878;
constexpr double kTileSizePx = 256.0;
/// Upper bound on tiles drawn per paint; the zoom level is lowered until it fits.
constexpr int kMaxTilesPerPaint = 160;
/// How many levels up to look for a cached stand-in while a tile downloads.
constexpr int kMaxAncestorFallback = 6;

double lonToTileX(double lon, int z) {
  return (lon + 180.0) / 360.0 * std::ldexp(1.0, z);
}

double latToTileY(double lat, int z) {
  const double rad = lat * std::numbers::pi / 180.0;
  return (1.0 - std::asinh(std::tan(rad)) / std::numbers::pi) / 2.0 * std::ldexp(1.0, z);
}

double tileXToLon(double x, int z) {
  return x / std::ldexp(1.0, z) * 360.0 - 180.0;
}

double tileYToLat(double y, int z) {
  const double n = std::numbers::pi * (1.0 - 2.0 * y / std::ldexp(1.0, z));
  return std::atan(std::sinh(n)) * 180.0 / std::numbers::pi;
}

}  // namespace

GeoAxisRole geoAxisRoleOf(QStringView field_name) {
  static const QRegularExpression kLatitude(
      QStringLiteral("(?:^|[^a-z])(?:lat|latitude)(?:[^a-z]|$)"), QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression kLongitude(
      QStringLiteral("(?:^|[^a-z])(?:lon|lng|long|longitude)(?:[^a-z]|$)"), QRegularExpression::CaseInsensitiveOption);
  const qsizetype slash = std::max(field_name.lastIndexOf(u'/'), field_name.lastIndexOf(u'.'));
  const QStringView leaf = field_name.mid(slash + 1);
  const bool latitude = kLatitude.matchView(leaf).hasMatch();
  const bool longitude = kLongitude.matchView(leaf).hasMatch();
  if (latitude == longitude) {
    return GeoAxisRole::kNone;  // neither, or ambiguous ("latlon")
  }
  return latitude ? GeoAxisRole::kLatitude : GeoAxisRole::kLongitude;
}

MapTileItem::MapTileItem() {
  setZ(1.0);  // below the grid (Qwt default z 10) and curves (~20)
  setItemAttribute(QwtPlotItem::AutoScale, false);
  setItemAttribute(QwtPlotItem::Legend, false);
}

void MapTileItem::draw(
    QPainter* painter, const QwtScaleMap& xMap, const QwtScaleMap& yMap, const QRectF& canvasRect) const {
  const QwtScaleMap& lon_map = xMap;
  const QwtScaleMap& lat_map = yMap;

  const double lon_min = std::max(-180.0, std::min(lon_map.s1(), lon_map.s2()));
  const double lon_max = std::min(180.0, std::max(lon_map.s1(), lon_map.s2()));
  const double lat_min = std::max(-kMaxMercatorLat, std::min(lat_map.s1(), lat_map.s2()));
  const double lat_max = std::min(kMaxMercatorLat, std::max(lat_map.s1(), lat_map.s2()));
  const double lon_span_deg = std::abs(lon_map.s2() - lon_map.s1());
  const double lon_span_px = std::abs(lon_map.p2() - lon_map.p1());
  if (!(lon_min < lon_max) || !(lat_min < lat_max) || lon_span_deg <= 0.0 || lon_span_px <= 0.0) {
    return;
  }

  // Pick the zoom whose tiles render at >= native resolution along longitude.
  const double px_per_deg = lon_span_px / lon_span_deg;
  int z = static_cast<int>(std::ceil(std::log2(px_per_deg * 360.0 / kTileSizePx)));
  z = std::clamp(z, 0, MapTileProvider::kMaxZoom);

  int x0 = 0;
  int x1 = 0;
  int y0 = 0;
  int y1 = 0;
  for (;; --z) {
    const int last = (1 << z) - 1;
    x0 = std::clamp(static_cast<int>(std::floor(lonToTileX(lon_min, z))), 0, last);
    x1 = std::clamp(static_cast<int>(std::floor(lonToTileX(lon_max, z))), 0, last);
    y0 = std::clamp(static_cast<int>(std::floor(latToTileY(lat_max, z))), 0, last);
    y1 = std::clamp(static_cast<int>(std::floor(latToTileY(lat_min, z))), 0, last);
    if (z == 0 || (x1 - x0 + 1) * (y1 - y0 + 1) <= kMaxTilesPerPaint) {
      break;
    }
  }

  MapTileProvider& provider = MapTileProvider::instance();
  painter->save();
  painter->setClipRect(canvasRect, Qt::IntersectClip);
  painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

  for (int ty = y0; ty <= y1; ++ty) {
    for (int tx = x0; tx <= x1; ++tx) {
      const MapTileKey key{z, tx, ty};
      QImage image = provider.cached(key);
      if (!image.isNull()) {
        drawTile(painter, xMap, yMap, image, QRectF(image.rect()), z, tx, ty);
        continue;
      }
      provider.request(key);
      for (int up = 1; up <= std::min(z, kMaxAncestorFallback); ++up) {
        image = provider.cached(MapTileKey{z - up, tx >> up, ty >> up});
        if (image.isNull()) {
          continue;
        }
        const double cell = static_cast<double>(image.width()) / static_cast<double>(1 << up);
        const int mask = (1 << up) - 1;
        const QRectF source((tx & mask) * cell, (ty & mask) * cell, cell, cell);
        drawTile(painter, xMap, yMap, image, source, z, tx, ty);
        break;
      }
    }
  }

  painter->restore();
  drawAttribution(painter, canvasRect);
}

void MapTileItem::drawTile(
    QPainter* painter, const QwtScaleMap& xMap, const QwtScaleMap& yMap, const QImage& image, const QRectF& source,
    int z, int x, int y) const {
  const auto to_canvas = [&](double lon, double lat) { return QPointF(xMap.transform(lon), yMap.transform(lat)); };

  // Within a tile, longitude is linear in pixels but latitude is not. Low zooms
  // span enough latitude that a single affine quad would visibly misregister,
  // so those tiles are split into horizontal strips.
  const int strips = z < 4 ? 32 : (z < 8 ? 8 : 1);
  const double lon0 = tileXToLon(x, z);
  const double lon1 = tileXToLon(x + 1, z);
  const QTransform base = painter->worldTransform();
  const double strip_h = source.height() / strips;

  for (int i = 0; i < strips; ++i) {
    const double lat_top = tileYToLat(y + static_cast<double>(i) / strips, z);
    const double lat_bottom = tileYToLat(y + static_cast<double>(i + 1) / strips, z);
    const QPointF a = to_canvas(lon0, lat_top);
    const QPointF b = to_canvas(lon1, lat_top);
    const QPointF c = to_canvas(lon0, lat_bottom);
    const double w = source.width();
    // Affine map of the (0,0)-(w,strip_h) source strip onto the strip's canvas
    // rectangle; also covers inverted scales.
    const QTransform to_quad(
        (b.x() - a.x()) / w, (b.y() - a.y()) / w, (c.x() - a.x()) / strip_h, (c.y() - a.y()) / strip_h, a.x(), a.y());
    painter->setWorldTransform(to_quad * base);
    painter->drawImage(
        QRectF(0.0, 0.0, w, strip_h), image, QRectF(source.x(), source.y() + i * strip_h, w, strip_h));
  }
  painter->setWorldTransform(base);
}

void MapTileItem::drawAttribution(QPainter* painter, const QRectF& canvasRect) {
  const QString text = MapTileProvider::instance().attribution();
  const QSizeF size = rasterLabelSize(painter, text);
  drawRasterLabel(painter, QPointF(canvasRect.right() - size.width(), canvasRect.bottom() - size.height()), text);
}

}  // namespace PJ
