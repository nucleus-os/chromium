// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/service/display_embedder/output_presenter_ozone.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/check.h"
#include "base/logging.h"
#include "components/viz/common/resources/shared_image_format.h"
#include "gpu/command_buffer/service/shared_image/shared_image_representation.h"
#include "third_party/skia/include/core/SkColorType.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/gpu_fence.h"
#include "ui/gfx/native_pixmap.h"
#include "ui/gfx/overlay_plane_data.h"
#include "ui/ozone/public/ozone_presenter.h"

namespace viz {

namespace {

std::unique_ptr<gfx::GpuFence> TakeGpuFence(gfx::GpuFenceHandle fence) {
  return fence.is_null() ? nullptr
                         : std::make_unique<gfx::GpuFence>(std::move(fence));
}

}  // namespace

OutputPresenterOzone::OutputPresenterOzone(
    std::unique_ptr<ui::OzonePresenter> presenter)
    : presenter_(std::move(presenter)) {
  CHECK(presenter_);
  CHECK(presenter_->SupportsPlaneGpuFences())
      << "The native Ozone presenter requires explicit GPU fence support.";
}

OutputPresenterOzone::~OutputPresenterOzone() = default;

void OutputPresenterOzone::InitializeCapabilities(
    OutputSurface::Capabilities* capabilities) {
  capabilities->supports_post_sub_buffer = true;
  capabilities->supports_viewporter = presenter_->SupportsViewporter();
  capabilities->supports_surfaceless = true;
  capabilities->supports_target_damage = true;
  capabilities->output_surface_origin = gfx::SurfaceOrigin::kTopLeft;
  capabilities->resize_based_on_root_surface = true;
  capabilities->present_requires_make_current = false;

  capabilities->sk_color_type_map[SinglePlaneFormat::kBGR_565] =
      kRGB_565_SkColorType;
  capabilities->sk_color_type_map[SinglePlaneFormat::kRGBA_4444] =
      kARGB_4444_SkColorType;
  capabilities->sk_color_type_map[SinglePlaneFormat::kRGBX_8888] =
      kRGB_888x_SkColorType;
  capabilities->sk_color_type_map[SinglePlaneFormat::kRGBA_8888] =
      kRGBA_8888_SkColorType;
  capabilities->sk_color_type_map[SinglePlaneFormat::kBGRX_8888] =
      kBGRA_8888_SkColorType;
  capabilities->sk_color_type_map[SinglePlaneFormat::kBGRA_8888] =
      kBGRA_8888_SkColorType;
  capabilities->sk_color_type_map[SinglePlaneFormat::kBGRA_1010102] =
      kBGRA_1010102_SkColorType;
  capabilities->sk_color_type_map[SinglePlaneFormat::kRGBA_1010102] =
      kRGBA_1010102_SkColorType;
  capabilities->sk_color_type_map[SinglePlaneFormat::kRGBA_F16] =
      kRGBA_F16_SkColorType;
}

bool OutputPresenterOzone::Reshape(const ReshapeParams& params) {
  return presenter_->Resize(params.GfxSize(), params.device_scale_factor,
                            params.color_space,
                            /*has_alpha=*/!params.image_info.isOpaque());
}

void OutputPresenterOzone::Present(
    SwapCompletionCallback completion_callback,
    BufferPresentedCallback presentation_callback,
    gfx::FrameData data) {
  const bool commit_frame = std::exchange(frame_schedule_succeeded_, true);
  std::vector<raw_ptr<ScopedOverlayAccess>> accesses =
      std::move(pending_accesses_);
  pending_accesses_.clear();
  if (commit_frame) {
    for (ScopedOverlayAccess* access : accesses) {
      access->CommitReadAccess();
    }
  }

  presenter_->Present(std::move(completion_callback),
                      std::move(presentation_callback), std::move(data));
}

void OutputPresenterOzone::ScheduleOverlayPlane(
    const OverlayPlaneCandidate& overlay_plane_candidate,
    ScopedOverlayAccess* access) {
  if (!frame_schedule_succeeded_) {
    return;
  }

  scoped_refptr<gfx::NativePixmap> native_pixmap =
      access ? access->GetNativePixmap() : nullptr;
  if (!native_pixmap && !overlay_plane_candidate.is_solid_color &&
      !overlay_plane_candidate.is_root_render_pass) {
    // Overlay candidates are scheduled speculatively. A video overlay may
    // disappear or lose its backing between candidate selection and access;
    // skip that plane without poisoning the root frame. A missing root pixmap
    // remains fatal and is forwarded so the platform presenter rejects the
    // incomplete frame atomically.
    return;
  }

  gfx::OverlayPlaneData plane(
      overlay_plane_candidate.plane_z_order, overlay_plane_candidate.transform,
      overlay_plane_candidate.display_rect, overlay_plane_candidate.uv_rect,
      !overlay_plane_candidate.is_opaque,
      ToEnclosingRect(overlay_plane_candidate.damage_rect),
      overlay_plane_candidate.opacity, overlay_plane_candidate.priority_hint,
      overlay_plane_candidate.rounded_corners,
      overlay_plane_candidate.color_space, overlay_plane_candidate.hdr_metadata,
      overlay_plane_candidate.color, overlay_plane_candidate.is_solid_color,
      overlay_plane_candidate.is_root_render_pass,
      overlay_plane_candidate.clip_rect, overlay_plane_candidate.overlay_type);
  if (!native_pixmap && !overlay_plane_candidate.is_solid_color) {
    LOG_IF(WARNING, overlay_plane_candidate.is_root_render_pass)
        << "Root render pass is missing its native pixmap.";
  }

#if DCHECK_IS_ON()
  if (overlay_plane_candidate.is_solid_color) {
    CHECK(overlay_plane_candidate.color.has_value());
  }
#endif

  std::unique_ptr<gfx::GpuFence> acquire_fence;
  if (access) {
    acquire_fence = TakeGpuFence(access->CloneAcquireFence());
  }

  frame_schedule_succeeded_ = presenter_->ScheduleOverlayPlane(
      std::move(native_pixmap), std::move(acquire_fence), plane);
  if (frame_schedule_succeeded_ && access &&
      std::ranges::find(pending_accesses_, access) == pending_accesses_.end()) {
    pending_accesses_.push_back(access);
  }
}

}  // namespace viz
