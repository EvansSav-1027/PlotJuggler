// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/MapTileProvider.h"

#include <QCoreApplication>
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>

namespace PJ {
namespace {

constexpr auto kDefaultTileUrl =
    "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}";
constexpr auto kDefaultAttribution = "Imagery © Esri, Maxar, Earthstar Geographics";
constexpr int kMaxInflight = 6;
constexpr std::size_t kMaxQueued = 256;
constexpr int kMemoryCacheKiB = 96 * 1024;
constexpr qint64 kDiskCacheBytes = 512LL * 1024 * 1024;
constexpr int kRetryAfterSec = 60;

}  // namespace

MapTileProvider& MapTileProvider::instance() {
  // Parented to the application so it is torn down with the event loop rather
  // than during static destruction (after QNetworkAccessManager's backend is gone).
  static MapTileProvider* provider = [] {
    auto* created = new MapTileProvider();
    created->setParent(QCoreApplication::instance());
    return created;
  }();
  return *provider;
}

MapTileProvider::MapTileProvider() : memory_(kMemoryCacheKiB) {
  QSettings settings;
  url_template_ = settings.value(QStringLiteral("Plot/map_tile_url"), QString::fromLatin1(kDefaultTileUrl)).toString();
  attribution_ =
      settings.value(QStringLiteral("Plot/map_tile_attribution"), QString::fromUtf8(kDefaultAttribution)).toString();

  network_ = new QNetworkAccessManager(this);
  const QString cache_root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  if (!cache_root.isEmpty()) {
    auto* disk = new QNetworkDiskCache(network_);
    disk->setCacheDirectory(QDir(cache_root).filePath(QStringLiteral("map_tiles")));
    disk->setMaximumCacheSize(kDiskCacheBytes);
    network_->setCache(disk);
  }
}

QImage MapTileProvider::cached(const MapTileKey& key) const {
  const QImage* image = memory_.object(key.packed());
  return image != nullptr ? *image : QImage{};
}

void MapTileProvider::request(const MapTileKey& key) {
  const quint64 packed = key.packed();
  if (memory_.contains(packed) || queued_or_inflight_.contains(packed)) {
    return;
  }
  if (const auto failed = failed_until_.constFind(packed);
      failed != failed_until_.cend() && QDateTime::currentDateTimeUtc() < *failed) {
    return;
  }
  queue_.push_front(key);
  queued_or_inflight_.insert(packed, true);
  while (queue_.size() > kMaxQueued) {
    queued_or_inflight_.remove(queue_.back().packed());
    queue_.pop_back();
  }
  pump();
}

void MapTileProvider::pump() {
  while (inflight_ < kMaxInflight && !queue_.empty()) {
    const MapTileKey key = queue_.front();
    queue_.pop_front();

    QString url = url_template_;
    url.replace(QLatin1String("{z}"), QString::number(key.z))
        .replace(QLatin1String("{x}"), QString::number(key.x))
        .replace(QLatin1String("{y}"), QString::number(key.y));

    QNetworkRequest request{QUrl(url)};
    // Tile servers' usage policies require an identifying User-Agent.
    request.setHeader(
        QNetworkRequest::UserAgentHeader,
        QStringLiteral("PlotJuggler/%1").arg(QCoreApplication::applicationVersion()));
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    ++inflight_;
    QNetworkReply* reply = network_->get(request);
    const quint64 packed = key.packed();
    connect(reply, &QNetworkReply::finished, this, [this, reply, packed]() { onFinished(reply, packed); });
  }
}

void MapTileProvider::onFinished(QNetworkReply* reply, quint64 packed) {
  reply->deleteLater();
  --inflight_;
  queued_or_inflight_.remove(packed);

  QImage image;
  if (reply->error() == QNetworkReply::NoError) {
    image.loadFromData(reply->readAll());
  }
  if (image.isNull()) {
    failed_until_.insert(packed, QDateTime::currentDateTimeUtc().addSecs(kRetryAfterSec));
  } else {
    failed_until_.remove(packed);
    image = image.convertToFormat(QImage::Format_RGB32);
    const int cost_kib = std::max(1, static_cast<int>(image.sizeInBytes() / 1024));
    memory_.insert(packed, new QImage(std::move(image)), cost_kib);
    emit tileReady();
  }
  pump();
}

}  // namespace PJ
