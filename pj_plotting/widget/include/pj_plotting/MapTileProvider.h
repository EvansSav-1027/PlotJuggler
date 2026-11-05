#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <QCache>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>
#include <cstdint>
#include <deque>

class QNetworkAccessManager;
class QNetworkReply;

namespace PJ {

/// One Web Mercator ("slippy map") tile address.
struct MapTileKey {
  int z = 0;
  int x = 0;
  int y = 0;

  [[nodiscard]] quint64 packed() const noexcept {
    return (static_cast<quint64>(z) << 58) | (static_cast<quint64>(x) << 29) | static_cast<quint64>(y);
  }
};

/**
 * Process-wide fetcher + cache for satellite map tiles, shared by every plot.
 *
 * Tiles are fetched lazily over HTTP from a `{z}/{x}/{y}` URL template (default:
 * Esri World Imagery; override with the QSettings key `Plot/map_tile_url`), kept
 * decoded in a bounded memory cache, and persisted raw in a QNetworkDiskCache
 * under the app cache dir so revisiting an area works offline. GUI thread only.
 *
 * Request policy: at most a few requests are in flight; newer requests jump the
 * queue (the most recent view wins) and the queue is bounded, so panning fast
 * never builds an unbounded backlog. A failed tile is not retried for a while.
 */
class MapTileProvider : public QObject {
  Q_OBJECT
 public:
  static MapTileProvider& instance();

  /// Deepest zoom level ever requested.
  static constexpr int kMaxZoom = 19;

  /// Decoded tile from the memory cache, or a null image when not (yet) loaded.
  /// Never triggers network traffic — call request() for that.
  [[nodiscard]] QImage cached(const MapTileKey& key) const;

  /// Queue `key` for download unless it is cached, in flight, queued, or
  /// recently failed. tileReady() fires once it lands in the memory cache.
  void request(const MapTileKey& key);

  /// Attribution line the tile source's terms require to be visible on the map.
  [[nodiscard]] QString attribution() const {
    return attribution_;
  }

 signals:
  /// A newly-downloaded tile is now available through cached().
  void tileReady();

 private:
  MapTileProvider();

  void pump();
  void onFinished(QNetworkReply* reply, quint64 packed);

  QNetworkAccessManager* network_ = nullptr;
  QString url_template_;
  QString attribution_;
  mutable QCache<quint64, QImage> memory_;  // cost = KiB; QCache::object() is non-const
  std::deque<MapTileKey> queue_;
  QHash<quint64, bool> queued_or_inflight_;
  QHash<quint64, QDateTime> failed_until_;
  int inflight_ = 0;
};

}  // namespace PJ
