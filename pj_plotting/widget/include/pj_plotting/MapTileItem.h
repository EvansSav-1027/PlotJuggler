#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <qwt_plot_item.h>

#include <QStringView>

class QImage;
class QPainter;
class QwtScaleMap;

namespace PJ {

/// Which geographic coordinate a series holds, judged from its field name.
enum class GeoAxisRole { kNone, kLatitude, kLongitude };

/// Classifies a field name/path by its last segment ("GPS/Lat", "gps_lat",
/// "latitude_deg" -> latitude; "lon", "lng", "long", "longitude" -> longitude),
/// case-insensitively and only on whole words ("flat", "along" are kNone). A
/// name-based guess: local metric frames ("pos_x", "north") stay kNone, so they
/// never get satellite imagery behind them.
[[nodiscard]] GeoAxisRole geoAxisRoleOf(QStringView field_name);

/**
 * Satellite imagery underlay for XY position plots.
 *
 * Interprets the plot's axes as WGS84 degrees — X = longitude, Y = latitude —
 * and paints the Web Mercator tiles of
 * MapTileProvider covering the visible area at a zoom level matched to the
 * canvas resolution. Each tile is placed by mapping its geographic corners
 * through the plot's scale maps, so imagery stays registered to the data under
 * any zoom or aspect ratio (at low zoom a tile is drawn in
 * horizontal strips to follow the Mercator latitude stretch). A missing tile is
 * requested and meanwhile filled from the nearest cached ancestor. Anything
 * outside lon ±180 / lat ±85.05 is left blank, so non-geographic data simply
 * shows no imagery.
 *
 * Draws below the grid and curves. The owner must replot on
 * MapTileProvider::tileReady() for downloaded tiles to appear.
 */
class MapTileItem : public QwtPlotItem {
 public:
  MapTileItem();

  [[nodiscard]] int rtti() const override {
    return QwtPlotItem::Rtti_PlotUserItem + 1;
  }

  void draw(
      QPainter* painter, const QwtScaleMap& xMap, const QwtScaleMap& yMap, const QRectF& canvasRect) const override;

 private:
  /// Paint `source` (a sub-rect of `image`) over the geographic extent of tile (z, x, y).
  void drawTile(
      QPainter* painter, const QwtScaleMap& xMap, const QwtScaleMap& yMap, const QImage& image, const QRectF& source,
      int z, int x, int y) const;

  /// Paint the provider's attribution in the canvas' bottom-right corner (CPU-rasterized; see RasterTextEngine).
  static void drawAttribution(QPainter* painter, const QRectF& canvasRect);
};

}  // namespace PJ
