#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: Apache-2.0

// Generated from the plotjuggler_sdk project version. Do not edit.

#include <string_view>

#define PJ_SDK_VERSION_MAJOR 0
#define PJ_SDK_VERSION_MINOR 34
#define PJ_SDK_VERSION_PATCH 1

#define PJ_SDK_VERSION_AT_LEAST(major, minor, patch)                                                          \
  ((PJ_SDK_VERSION_MAJOR > (major)) || (PJ_SDK_VERSION_MAJOR == (major) && PJ_SDK_VERSION_MINOR > (minor)) || \
   (PJ_SDK_VERSION_MAJOR == (major) && PJ_SDK_VERSION_MINOR == (minor) && PJ_SDK_VERSION_PATCH >= (patch)))

namespace PJ {

/// Return the plotjuggler_sdk version used to configure this header.
[[nodiscard]] inline constexpr std::string_view sdkVersion() noexcept {
  return "0.34.1";
}

}  // namespace PJ
