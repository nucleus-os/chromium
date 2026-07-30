// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_X11_OZONE_PLATFORM_X11_H_
#define UI_OZONE_PLATFORM_X11_OZONE_PLATFORM_X11_H_

#include "base/component_export.h"

namespace ui {

class OzonePlatform;

// Constructor hook for use in ozone_platform_list.cc
OzonePlatform* CreateOzonePlatformX11();

// Indicate that CEF is using multi-threaded-message-loop.
COMPONENT_EXPORT(OZONE) void SetMultiThreadedMessageLoopX11();

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_X11_OZONE_PLATFORM_X11_H_
