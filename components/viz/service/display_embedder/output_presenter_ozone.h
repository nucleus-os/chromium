// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_VIZ_SERVICE_DISPLAY_EMBEDDER_OUTPUT_PRESENTER_OZONE_H_
#define COMPONENTS_VIZ_SERVICE_DISPLAY_EMBEDDER_OUTPUT_PRESENTER_OZONE_H_

#include <memory>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "components/viz/service/display_embedder/output_presenter.h"
#include "components/viz/service/viz_service_export.h"

namespace ui {
class OzonePresenter;
}  // namespace ui

namespace viz {

// Adapts Ozone's rendering-API-independent native-pixmap presenter to Viz's
// output presenter contract. This path never creates or makes current a GL
// context.
class VIZ_SERVICE_EXPORT OutputPresenterOzone final : public OutputPresenter {
 public:
  explicit OutputPresenterOzone(std::unique_ptr<ui::OzonePresenter> presenter);
  ~OutputPresenterOzone() override;

  OutputPresenterOzone(const OutputPresenterOzone&) = delete;
  OutputPresenterOzone& operator=(const OutputPresenterOzone&) = delete;

  // OutputPresenter:
  void InitializeCapabilities(OutputSurface::Capabilities* capabilities) final;
  bool Reshape(const ReshapeParams& params) final;
  void Present(SwapCompletionCallback completion_callback,
               BufferPresentedCallback presentation_callback,
               gfx::FrameData data) final;
  void ScheduleOverlayPlane(
      const OverlayPlaneCandidate& overlay_plane_candidate,
      ScopedOverlayAccess* access) final;

 private:
  std::unique_ptr<ui::OzonePresenter> presenter_;
  // Plane scheduling is one frame transaction. The platform receives cloned
  // producer fences while planes are collected, but the SharedImage accesses
  // are committed only when every plane in the frame was accepted.
  bool frame_schedule_succeeded_ = true;
  std::vector<raw_ptr<ScopedOverlayAccess>> pending_accesses_;
};

}  // namespace viz

#endif  // COMPONENTS_VIZ_SERVICE_DISPLAY_EMBEDDER_OUTPUT_PRESENTER_OZONE_H_
