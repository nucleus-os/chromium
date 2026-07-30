// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/wayland/gpu/gbm_surfaceless_wayland.h"

#include <memory>
#include <utility>

#include "ui/gl/gl_bindings.h"
#include "ui/ozone/platform/wayland/gpu/wayland_buffer_manager_gpu.h"
#include "ui/ozone/platform/wayland/gpu/wayland_buffer_queue_presenter.h"

namespace ui {

GbmSurfacelessWayland::GbmSurfacelessWayland(
    gl::GLDisplayEGL* /*display*/,
    WaylandBufferManagerGpu* buffer_manager,
    gfx::AcceleratedWidget widget)
    : buffer_manager_(buffer_manager),
      presenter_(std::make_unique<WaylandBufferQueuePresenter>(buffer_manager,
                                                               widget)) {}

GbmSurfacelessWayland::~GbmSurfacelessWayland() = default;

bool GbmSurfacelessWayland::ScheduleOverlayPlane(
    gl::OverlayImage image,
    std::unique_ptr<gfx::GpuFence> gpu_fence,
    const gfx::OverlayPlaneData& overlay_plane_data) {
  return presenter_->ScheduleOverlayPlane(
      std::move(image), std::move(gpu_fence), overlay_plane_data);
}

void GbmSurfacelessWayland::Present(SwapCompletionCallback completion_callback,
                                    PresentationCallback presentation_callback,
                                    gfx::FrameData data) {
  if (!no_gl_flush_for_tests_ && !buffer_manager_->supports_acquire_fence()) {
    glFlush();
  }
  presenter_->Present(std::move(completion_callback),
                      std::move(presentation_callback), std::move(data));
}

void GbmSurfacelessWayland::SetRelyOnImplicitSync() {
  presenter_->SetRelyOnImplicitSync();
}

bool GbmSurfacelessWayland::SupportsPlaneGpuFences() const {
  // Preserve the legacy contract: when the compositor cannot consume an
  // acquire fence, the delegated presenter retires it on a worker before
  // committing the frame.
  return true;
}

bool GbmSurfacelessWayland::SupportsOverridePlatformSize() const {
  return true;
}

bool GbmSurfacelessWayland::SupportsViewporter() const {
  return presenter_->SupportsViewporter();
}

bool GbmSurfacelessWayland::Resize(const gfx::Size& size,
                                   float scale_factor,
                                   const gfx::ColorSpace& color_space,
                                   bool has_alpha) {
  return presenter_->Resize(size, scale_factor, color_space, has_alpha);
}

void GbmSurfacelessWayland::SetNoGLFlushForTests() {
  no_gl_flush_for_tests_ = true;
}

}  // namespace ui
