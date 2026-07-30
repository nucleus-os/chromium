// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_WAYLAND_GPU_GBM_SURFACELESS_WAYLAND_H_
#define UI_OZONE_PLATFORM_WAYLAND_GPU_GBM_SURFACELESS_WAYLAND_H_

#include <memory>

#include "base/gtest_prod_util.h"
#include "base/memory/raw_ptr.h"
#include "ui/gl/presenter.h"

namespace gl {
class GLDisplayEGL;
}

namespace ui {

class WaylandBufferManagerGpu;
class WaylandBufferQueuePresenter;

// Legacy GL adapter for Wayland surfaceless presentation. The rendering-API
// independent frame queue and Wayland transaction state live in
// WaylandBufferQueuePresenter.
class GbmSurfacelessWayland : public gl::Presenter {
 public:
  GbmSurfacelessWayland(gl::GLDisplayEGL* display,
                        WaylandBufferManagerGpu* buffer_manager,
                        gfx::AcceleratedWidget widget);

  GbmSurfacelessWayland(const GbmSurfacelessWayland&) = delete;
  GbmSurfacelessWayland& operator=(const GbmSurfacelessWayland&) = delete;

  // gl::Presenter:
  bool ScheduleOverlayPlane(
      gl::OverlayImage image,
      std::unique_ptr<gfx::GpuFence> gpu_fence,
      const gfx::OverlayPlaneData& overlay_plane_data) override;
  void Present(SwapCompletionCallback completion_callback,
               PresentationCallback presentation_callback,
               gfx::FrameData data) override;
  void SetRelyOnImplicitSync() override;
  bool SupportsPlaneGpuFences() const override;
  bool SupportsOverridePlatformSize() const override;
  bool SupportsViewporter() const override;
  bool Resize(const gfx::Size& size,
              float scale_factor,
              const gfx::ColorSpace& color_space,
              bool has_alpha) override;

 private:
  FRIEND_TEST_ALL_PREFIXES(WaylandSurfaceFactoryTest,
                           GbmSurfacelessWaylandCheckOrderOfCallbacksTest);
  FRIEND_TEST_ALL_PREFIXES(WaylandSurfaceFactoryTest,
                           GbmSurfacelessWaylandCommitOverlaysCallbacksTest);
  FRIEND_TEST_ALL_PREFIXES(WaylandSurfaceFactoryTest,
                           GbmSurfacelessWaylandGroupOnSubmissionCallbacksTest);
  FRIEND_TEST_ALL_PREFIXES(WaylandSurfaceFactoryCompositorV3,
                           SurfaceDamageTest);

  ~GbmSurfacelessWayland() override;

  // Prevents a real GL flush in tests that do not create a GL context.
  void SetNoGLFlushForTests();

  const raw_ptr<WaylandBufferManagerGpu> buffer_manager_;
  std::unique_ptr<WaylandBufferQueuePresenter> presenter_;
  bool no_gl_flush_for_tests_ = false;
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_WAYLAND_GPU_GBM_SURFACELESS_WAYLAND_H_
