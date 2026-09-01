// Copyright 2011 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/win/app_icon.h"

#include "chrome/common/chrome_constants.h"
#include "chrome/install_static/install_details.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/image/image_family.h"
#include "ui/gfx/win/icon_util.h"

namespace {

// Returns the resource id of the application icon.
int GetAppIconResourceId() {
  return install_static::InstallDetails::Get().app_icon_resource_id();
}

int g_exe_app_icon_resource_id = 0;

}  // namespace

void SetExeAppIconResourceId(int icon_id) {
  g_exe_app_icon_resource_id = icon_id;
}

HICON GetAppIcon() {
  // TODO(mgiuca): Use GetAppIconImageFamily/CreateExact instead of LoadIcon, to
  // get correct scaling. (See http://crbug.com/40443246)
  // HICON returned from LoadIcon do not leak and do not have to be destroyed.
  if (g_exe_app_icon_resource_id > 0) {
    // Try to load the icon from the exe first.
    if (auto icon = LoadIcon(GetModuleHandle(NULL),
            MAKEINTRESOURCE(g_exe_app_icon_resource_id))) {
      return icon;
    }
  }
  const int icon_id = GetAppIconResourceId();
  return LoadIcon(GetModuleHandle(chrome::kBrowserResourcesDll),
                  MAKEINTRESOURCE(icon_id));
}

HICON GetSmallAppIcon() {
  // TODO(mgiuca): Use GetAppIconImageFamily/CreateExact instead of LoadIcon, to
  // get correct scaling. (See http://crbug.com/40443246)
  gfx::Size size = GetSmallAppIconSize();
  // HICON returned from LoadImage must be released using DestroyIcon.
  if (g_exe_app_icon_resource_id > 0) {
    // Try to load the icon from the exe first.
    if (auto icon = static_cast<HICON>(LoadImage(
            GetModuleHandle(NULL), MAKEINTRESOURCE(g_exe_app_icon_resource_id),
            IMAGE_ICON, size.width(), size.height(),
            LR_DEFAULTCOLOR | LR_SHARED))) {
      return icon;
    }
  }
  const int icon_id = GetAppIconResourceId();
  return static_cast<HICON>(LoadImage(
      GetModuleHandle(chrome::kBrowserResourcesDll), MAKEINTRESOURCE(icon_id),
      IMAGE_ICON, size.width(), size.height(), LR_DEFAULTCOLOR | LR_SHARED));
}

gfx::Size GetAppIconSize() {
  return gfx::Size(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
}

gfx::Size GetSmallAppIconSize() {
  return gfx::Size(GetSystemMetrics(SM_CXSMICON),
                   GetSystemMetrics(SM_CYSMICON));
}

std::unique_ptr<gfx::ImageFamily> GetAppIconImageFamily() {
  if (g_exe_app_icon_resource_id > 0) {
    // Try to load the icon from the exe first.
    if (auto image_family = IconUtil::CreateImageFamilyFromIconResource(
            GetModuleHandle(NULL), g_exe_app_icon_resource_id)) {
      return image_family;
    }
  }
  const int icon_id = GetAppIconResourceId();
  return IconUtil::CreateImageFamilyFromIconResource(
      GetModuleHandle(chrome::kBrowserResourcesDll), icon_id);
}
