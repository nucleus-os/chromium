// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PUBLIC_OZONE_PRESENTER_H_
#define UI_OZONE_PUBLIC_OZONE_PRESENTER_H_

#include <memory>

#include "base/component_export.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "ui/gfx/frame_data.h"
#include "ui/gfx/native_pixmap.h"
#include "ui/gfx/presentation_feedback.h"
#include "ui/gfx/swap_result.h"

namespace gfx {
class ColorSpace;
class GpuFence;
class Size;
struct OverlayPlaneData;
}  // namespace gfx

namespace ui {

// Accelerated, rendering-API-independent presentation for an Ozone platform
// window. Rendering backends schedule native pixmaps and explicit acquire
// fences; the platform owns submission and presentation feedback.
class COMPONENT_EXPORT(OZONE_BASE) OzonePresenter {
 public:
  using SwapCompletionCallback =
      base::OnceCallback<void(gfx::SwapCompletionResult)>;
  using PresentationCallback =
      base::OnceCallback<void(const gfx::PresentationFeedback&)>;

  OzonePresenter(const OzonePresenter&) = delete;
  OzonePresenter& operator=(const OzonePresenter&) = delete;

  virtual ~OzonePresenter();

  virtual bool Resize(const gfx::Size& pixel_size,
                      float scale_factor,
                      const gfx::ColorSpace& color_space,
                      bool has_alpha) = 0;

  virtual bool ScheduleOverlayPlane(
      scoped_refptr<gfx::NativePixmap> image,
      std::unique_ptr<gfx::GpuFence> acquire_fence,
      const gfx::OverlayPlaneData& plane) = 0;

  virtual void Present(SwapCompletionCallback completion,
                       PresentationCallback presentation,
                       gfx::FrameData frame_data) = 0;

  virtual bool SupportsViewporter() const = 0;
  virtual bool SupportsPlaneGpuFences() const = 0;

 protected:
  OzonePresenter();
};

}  // namespace ui

#endif  // UI_OZONE_PUBLIC_OZONE_PRESENTER_H_
