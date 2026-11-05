#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <QPointF>
#include <QSizeF>
#include <QString>

class QPainter;

namespace PJ {

/// Size a label drawn by drawRasterLabel() occupies, in logical pixels.
[[nodiscard]] QSizeF rasterLabelSize(const QPainter* painter, const QString& text);

/// Draws `text` on a translucent dark plate with its top-left at `top_left`.
/// Rasterized on the CPU per call and never cached: GL glyph atlases and resident
/// textures do not survive a GPU reset (see RasterTextEngine.h), so canvas text
/// must not go through QPainter::drawText on the GL paint engine.
void drawRasterLabel(QPainter* painter, const QPointF& top_left, const QString& text);

}  // namespace PJ
