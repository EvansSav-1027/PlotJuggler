// SPDX-License-Identifier: MPL-2.0

// The on-demand satellite underlay for XY position plots. It applies only to
// lat/lon plots (longitude field on X, latitude on Y, judged by field name) —
// never to local metric XY plots — and the request round-trips through the plot
// XML so layouts and undo/redo snapshots restore it. Rendering itself needs the
// network and is not covered.

#include <gtest/gtest.h>

#include <QDomDocument>
#include <string_view>
#include <vector>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/MapTileItem.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/TrackValueSeries.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
using namespace Qt::StringLiterals;

namespace {

QString addScalarTopic(
    PJ::SessionManager& session, PJ::CatalogModel& catalog, PJ::DatasetId dataset_id, std::string_view topic_name,
    const std::vector<double>& values = {1.0}) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle_or = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle_or.has_value()) << handle_or.error();
  if (!handle_or.has_value()) {
    return {};
  }
  for (std::size_t i = 0; i < values.size(); ++i) {
    writer.appendScalar(*handle_or, static_cast<PJ::Timestamp>(i + 1) * 1'000'000'000, values[i]);
  }
  EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
  for (const auto& curve : catalog.curves()) {
    if (const auto descriptor = catalog.curveDescriptor(curve.name);
        descriptor && descriptor->topic_id == handle_or->topic_id) {
      return curve.name;
    }
  }
  return {};
}

struct Fixture {
  PJ::SessionManager session;
  PJ::CatalogModel catalog{&session};
  PJ::DatasetId dataset = 0;

  Fixture() {
    auto created = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.log"});
    EXPECT_TRUE(created.has_value());
    dataset = created.has_value() ? *created : 0;
  }
  QString key(std::string_view topic, const std::vector<double>& values = {1.0}) {
    return addScalarTopic(session, catalog, dataset, topic, values);
  }
};

}  // namespace

TEST(GeoAxisRole, ClassifiesByWholeWordLeafName) {
  using PJ::GeoAxisRole;
  EXPECT_EQ(PJ::geoAxisRoleOf(u"GPS/Lat"), GeoAxisRole::kLatitude);
  EXPECT_EQ(PJ::geoAxisRoleOf(u"/gps/latitude_deg"), GeoAxisRole::kLatitude);
  EXPECT_EQ(PJ::geoAxisRoleOf(u"pos.gps_lat"), GeoAxisRole::kLatitude);
  EXPECT_EQ(PJ::geoAxisRoleOf(u"GPS/Lng"), GeoAxisRole::kLongitude);
  EXPECT_EQ(PJ::geoAxisRoleOf(u"/fix/longitude"), GeoAxisRole::kLongitude);
  EXPECT_EQ(PJ::geoAxisRoleOf(u"/pos/x"), GeoAxisRole::kNone);
  EXPECT_EQ(PJ::geoAxisRoleOf(u"/terrain/flat"), GeoAxisRole::kNone);
  EXPECT_EQ(PJ::geoAxisRoleOf(u"/latlon"), GeoAxisRole::kNone);
  // Only the leaf counts: a "lat" topic segment does not make "x" a latitude.
  EXPECT_EQ(PJ::geoAxisRoleOf(u"/lat/x"), GeoAxisRole::kNone);
}

TEST(PlotWidgetMapBackground, OnlyLonOnXLatOnYPlotsAreGeographic) {
  Fixture f;
  const QString lon = f.key("/gps/lon");
  const QString lat = f.key("/gps/lat");
  const QString x = f.key("/pos/x");
  const QString y = f.key("/pos/y");

  PJ::PlotWidget geo(&f.session, &f.catalog);
  geo.setModeXY(true);
  ASSERT_NE(geo.addCurveXY(lon, lat, u"track"_s), nullptr);
  EXPECT_TRUE(geo.isGeographicPlot());

  PJ::PlotWidget local(&f.session, &f.catalog);
  local.setModeXY(true);
  ASSERT_NE(local.addCurveXY(x, y, u"local"_s), nullptr);
  EXPECT_FALSE(local.isGeographicPlot());

  PJ::PlotWidget swapped(&f.session, &f.catalog);
  swapped.setModeXY(true);
  ASSERT_NE(swapped.addCurveXY(lat, lon, u"swapped"_s), nullptr);
  EXPECT_FALSE(swapped.isGeographicPlot());

  // One local curve disqualifies a plot that also holds a lat/lon track.
  ASSERT_NE(geo.addCurveXY(x, y, u"local"_s), nullptr);
  EXPECT_FALSE(geo.isGeographicPlot());
}

TEST(PlotWidgetMapBackground, OffByDefaultAndRoundTripsThroughPlotXml) {
  Fixture f;
  const QString lon = f.key("/gps/lon");
  const QString lat = f.key("/gps/lat");

  PJ::PlotWidget source(&f.session, &f.catalog);
  source.setModeXY(true);
  ASSERT_NE(source.addCurveXY(lon, lat, u"track"_s), nullptr);
  EXPECT_FALSE(source.mapBackgroundEnabled());
  QDomDocument off_doc;
  const QDomElement off = source.xmlSaveState(off_doc);
  EXPECT_FALSE(off.hasAttribute(u"map_background"_s));

  source.setMapBackgroundEnabled(true);
  QDomDocument doc;
  const QDomElement on = source.xmlSaveState(doc);
  EXPECT_EQ(on.attribute(u"map_background"_s), u"satellite"_s);

  PJ::PlotWidget restored(&f.session, &f.catalog);
  ASSERT_TRUE(restored.xmlLoadState(on, /*autozoom=*/false));
  EXPECT_TRUE(restored.mapBackgroundEnabled());

  // Loading a snapshot without the attribute (e.g. undo past the toggle) turns it off.
  ASSERT_TRUE(restored.xmlLoadState(off, /*autozoom=*/false));
  EXPECT_FALSE(restored.mapBackgroundEnabled());
}

// The colour field is read as a step function of raw time: a sample holds until the next.
TEST(TrackValueSeries, ValueHoldsUntilNextSampleAndReportsRange) {
  Fixture f;
  const QString depth = f.key("/sonar/depth", {5.0, 10.0, 20.0});
  const auto descriptor = f.catalog.curveDescriptor(depth);
  ASSERT_TRUE(descriptor.has_value());

  PJ::TrackValueSeries series(&f.session, *descriptor);
  EXPECT_FALSE(series.valueAtOrBefore(500'000'000).has_value());  // before the first sample
  EXPECT_EQ(series.valueAtOrBefore(1'000'000'000), 5.0);
  EXPECT_EQ(series.valueAtOrBefore(2'500'000'000), 10.0);
  EXPECT_EQ(series.valueAtOrBefore(9'000'000'000), 20.0);
  ASSERT_TRUE(series.valueRange().has_value());
  EXPECT_DOUBLE_EQ(series.valueRange()->min, 5.0);
  EXPECT_DOUBLE_EQ(series.valueRange()->max, 20.0);
}

TEST(PlotWidgetMapBackground, TrackColorFieldRoundTripsThroughPlotXml) {
  Fixture f;
  const QString lon = f.key("/gps/lon");
  const QString lat = f.key("/gps/lat");
  f.key("/sonar/depth", {5.0, 10.0, 20.0});

  PJ::PlotWidget source(&f.session, &f.catalog);
  source.setModeXY(true);
  ASSERT_NE(source.addCurveXY(lon, lat, u"track"_s), nullptr);
  EXPECT_FALSE(source.trackColorRequested());

  source.setTrackColorField(u"/sonar/depth"_s, u"value"_s);
  source.setTrackColormap(PJ::Colormap::kViridis);
  QDomDocument doc;
  const QDomElement on = source.xmlSaveState(doc);
  EXPECT_EQ(on.attribute(u"track_color_topic"_s), u"/sonar/depth"_s);
  EXPECT_EQ(on.attribute(u"track_color_field"_s), u"value"_s);
  EXPECT_EQ(on.attribute(u"track_color_map"_s).toInt(), static_cast<int>(PJ::Colormap::kViridis));

  PJ::PlotWidget restored(&f.session, &f.catalog);
  ASSERT_TRUE(restored.xmlLoadState(on, /*autozoom=*/false));
  EXPECT_TRUE(restored.trackColorRequested());

  source.setTrackColorField(QString(), QString());
  QDomDocument cleared_doc;
  EXPECT_FALSE(source.xmlSaveState(cleared_doc).hasAttribute(u"track_color_topic"_s));
  ASSERT_TRUE(restored.xmlLoadState(source.xmlSaveState(cleared_doc), /*autozoom=*/false));
  EXPECT_FALSE(restored.trackColorRequested());
}
