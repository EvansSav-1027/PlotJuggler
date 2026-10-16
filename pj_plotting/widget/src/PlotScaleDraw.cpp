// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotScaleDraw.h"

#include <qwt_text.h>

#include <QDateTime>
#include <QLocale>
#include <QTimeZone>
#include <cmath>

using namespace Qt::StringLiterals;

namespace PJ {

QwtText PlotScaleDraw::label(double value) const {
  const QLocale locale;
  QString str = locale.toString(value, 'f', 6);
  // Strip trailing fractional zeros, then a bare decimal separator — using the
  // locale's own digits/separator so decimal-comma locales trim correctly. The
  // 'f' format guarantees a separator, which shields the integer digits.
  const QString zero = locale.zeroDigit();
  const QString point = locale.decimalPoint();
  while (str.endsWith(zero)) {
    str.chop(zero.size());
  }
  if (str.endsWith(point)) {
    str.chop(point.size());
  }
  return str;
}

QwtText TimeScaleDraw::label(double value) const {
  return QDateTime::fromMSecsSinceEpoch(std::llround(value * 1000.0), QTimeZone::UTC).toString(u"hh:mm:ss.z"_s);
}

}  // namespace PJ
