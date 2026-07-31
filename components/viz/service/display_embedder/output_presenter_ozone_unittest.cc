// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/service/display_embedder/output_presenter_ozone.h"

#include <memory>
#include <utility>

#include "base/check.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "components/viz/service/display/overlay_candidate.h"
#include "gpu/command_buffer/common/shared_image_info.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/command_buffer/service/memory_tracking.h"
#include "gpu/command_buffer/service/shared_image/shared_image_manager.h"
#include "gpu/command_buffer/service/shared_image/test_image_backing.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/gpu_fence.h"
#include "ui/gfx/native_pixmap.h"
#include "ui/gfx/overlay_plane_data.h"
#include "ui/ozone/public/ozone_presenter.h"

namespace viz {
namespace {

class FakeOzonePresenter final : public ui::OzonePresenter {
 public:
  FakeOzonePresenter() = default;
  ~FakeOzonePresenter() override = default;

  bool Resize(const gfx::Size& pixel_size,
              float scale_factor,
              const gfx::ColorSpace& color_space,
              bool has_alpha) override {
    resize_size = pixel_size;
    resize_scale = scale_factor;
    resize_color_space = color_space;
    resize_has_alpha = has_alpha;
    return resize_result;
  }

  bool ScheduleOverlayPlane(scoped_refptr<gfx::NativePixmap> image,
                            std::unique_ptr<gfx::GpuFence> acquire_fence,
                            const gfx::OverlayPlaneData& plane) override {
    ++schedule_count;
    scheduled_image = std::move(image);
    scheduled_fence = std::move(acquire_fence);
    scheduled_plane = plane;
    return schedule_result;
  }

  void Present(SwapCompletionCallback completion,
               PresentationCallback presentation,
               gfx::FrameData frame_data) override {
    presented_frame_data = frame_data;
    std::move(completion)
        .Run(gfx::SwapCompletionResult(gfx::SwapResult::SWAP_ACK));
    std::move(presentation).Run(gfx::PresentationFeedback::Failure());
  }

  bool SupportsViewporter() const override { return true; }
  bool SupportsPlaneGpuFences() const override { return true; }

  bool resize_result = true;
  bool schedule_result = true;
  gfx::Size resize_size;
  float resize_scale = 0.f;
  gfx::ColorSpace resize_color_space;
  bool resize_has_alpha = false;
  scoped_refptr<gfx::NativePixmap> scheduled_image;
  std::unique_ptr<gfx::GpuFence> scheduled_fence;
  gfx::OverlayPlaneData scheduled_plane;
  gfx::FrameData presented_frame_data;
  int schedule_count = 0;
};

class TestOverlayAccess {
 public:
  TestOverlayAccess() : tracker_(nullptr), mailbox_(gpu::Mailbox::Generate()) {
    gpu::SharedImageInfo info(
        SinglePlaneFormat::kBGRA_8888, gfx::Size(64, 64),
        gfx::ColorSpace::CreateSRGB(), kTopLeft_GrSurfaceOrigin,
        kPremul_SkAlphaType,
        gpu::SHARED_IMAGE_USAGE_DISPLAY_READ | gpu::SHARED_IMAGE_USAGE_SCANOUT,
        "OutputPresenterOzoneTest");
    auto backing = std::make_unique<gpu::TestImageBacking>(
        mailbox_, info, /*estimated_size=*/0);
    backing_ = backing.get();
    factory_ref_ = manager_.Register(std::move(backing), &tracker_);
    representation_ = manager_.ProduceOverlay(mailbox_, &tracker_);
    CHECK(representation_);
    representation_->SetCleared();
    access_ = representation_->BeginScopedReadAccess();
    CHECK(access_);
  }

  gpu::OverlayImageRepresentation::ScopedReadAccess* access() {
    return access_.get();
  }
  int commit_count() const { return backing_->overlay_access_commit_count(); }

 private:
  gpu::SharedImageManager manager_;
  gpu::MemoryTypeTracker tracker_;
  gpu::Mailbox mailbox_;
  raw_ptr<gpu::TestImageBacking> backing_;
  std::unique_ptr<gpu::SharedImageRepresentationFactoryRef> factory_ref_;
  std::unique_ptr<gpu::OverlayImageRepresentation> representation_;
  std::unique_ptr<gpu::OverlayImageRepresentation::ScopedReadAccess> access_;
};

TEST(OutputPresenterOzoneTest, AdvertisesNativePixmapCapabilities) {
  auto platform_presenter = std::make_unique<FakeOzonePresenter>();
  OutputPresenterOzone presenter(std::move(platform_presenter));
  OutputSurface::Capabilities capabilities;

  presenter.InitializeCapabilities(&capabilities);

  EXPECT_TRUE(capabilities.supports_post_sub_buffer);
  EXPECT_TRUE(capabilities.supports_target_damage);
  EXPECT_TRUE(capabilities.supports_viewporter);
  EXPECT_TRUE(capabilities.supports_surfaceless);
  EXPECT_TRUE(capabilities.resize_based_on_root_surface);
  EXPECT_FALSE(capabilities.present_requires_make_current);
  EXPECT_EQ(capabilities.output_surface_origin, gfx::SurfaceOrigin::kTopLeft);
  EXPECT_EQ(capabilities.sk_color_type_map.at(SinglePlaneFormat::kBGR_565),
            kRGB_565_SkColorType);
  EXPECT_EQ(capabilities.sk_color_type_map.at(SinglePlaneFormat::kRGBA_4444),
            kARGB_4444_SkColorType);
  EXPECT_EQ(capabilities.sk_color_type_map.at(SinglePlaneFormat::kRGBX_8888),
            kRGB_888x_SkColorType);
  EXPECT_EQ(capabilities.sk_color_type_map.at(SinglePlaneFormat::kBGRA_8888),
            kBGRA_8888_SkColorType);
  EXPECT_EQ(capabilities.sk_color_type_map.at(SinglePlaneFormat::kRGBA_8888),
            kRGBA_8888_SkColorType);
  EXPECT_EQ(capabilities.sk_color_type_map.at(SinglePlaneFormat::kBGRX_8888),
            kBGRA_8888_SkColorType);
  EXPECT_EQ(capabilities.sk_color_type_map.at(SinglePlaneFormat::kBGRA_1010102),
            kBGRA_1010102_SkColorType);
  EXPECT_EQ(capabilities.sk_color_type_map.at(SinglePlaneFormat::kRGBA_1010102),
            kRGBA_1010102_SkColorType);
  EXPECT_EQ(capabilities.sk_color_type_map.at(SinglePlaneFormat::kRGBA_F16),
            kRGBA_F16_SkColorType);
}

TEST(OutputPresenterOzoneTest, ForwardsReshapeWithoutGL) {
  auto platform_presenter = std::make_unique<FakeOzonePresenter>();
  auto* platform_presenter_ptr = platform_presenter.get();
  OutputPresenterOzone presenter(std::move(platform_presenter));
  SkiaOutputDevice::ReshapeParams params{
      .image_info = SkImageInfo::MakeN32Premul(640, 480),
      .color_space = gfx::ColorSpace::CreateSRGB(),
      .device_scale_factor = 1.5f,
  };

  EXPECT_TRUE(presenter.Reshape(params));
  EXPECT_EQ(platform_presenter_ptr->resize_size, gfx::Size(640, 480));
  EXPECT_EQ(platform_presenter_ptr->resize_scale, 1.5f);
  EXPECT_EQ(platform_presenter_ptr->resize_color_space,
            gfx::ColorSpace::CreateSRGB());
  EXPECT_TRUE(platform_presenter_ptr->resize_has_alpha);
}

TEST(OutputPresenterOzoneTest, ForwardsSolidColorPlaneAndCallbacks) {
  auto platform_presenter = std::make_unique<FakeOzonePresenter>();
  auto* platform_presenter_ptr = platform_presenter.get();
  OutputPresenterOzone presenter(std::move(platform_presenter));
  OverlayCandidate candidate;
  candidate.is_solid_color = true;
  candidate.is_root_render_pass = false;
  candidate.color = SkColors::kRed;
  candidate.opacity = 0.75f;
  candidate.display_rect = gfx::RectF(10.f, 20.f, 30.f, 40.f);
  candidate.damage_rect = gfx::RectF(11.f, 21.f, 4.f, 5.f);

  presenter.ScheduleOverlayPlane(candidate, nullptr);

  EXPECT_FALSE(platform_presenter_ptr->scheduled_image);
  EXPECT_FALSE(platform_presenter_ptr->scheduled_fence);
  EXPECT_TRUE(platform_presenter_ptr->scheduled_plane.is_solid_color);
  EXPECT_EQ(platform_presenter_ptr->scheduled_plane.color, SkColors::kRed);
  EXPECT_EQ(platform_presenter_ptr->scheduled_plane.opacity, 0.75f);
  EXPECT_EQ(platform_presenter_ptr->scheduled_plane.display_bounds,
            candidate.display_rect);

  bool completion_called = false;
  bool presentation_called = false;
  presenter.Present(
      base::BindOnce(
          [](bool* called, gfx::SwapCompletionResult result) {
            *called = true;
            EXPECT_EQ(result.swap_result, gfx::SwapResult::SWAP_ACK);
          },
          &completion_called),
      base::BindOnce(
          [](bool* called, const gfx::PresentationFeedback& feedback) {
            *called = true;
            EXPECT_TRUE(feedback.failed());
          },
          &presentation_called),
      gfx::FrameData());
  EXPECT_TRUE(completion_called);
  EXPECT_TRUE(presentation_called);
}

TEST(OutputPresenterOzoneTest, MissingRootPixmapFailsAtPlatformFrameBoundary) {
  auto platform_presenter = std::make_unique<FakeOzonePresenter>();
  auto* platform_presenter_ptr = platform_presenter.get();
  OutputPresenterOzone presenter(std::move(platform_presenter));
  OverlayCandidate candidate;
  candidate.is_root_render_pass = true;
  candidate.is_solid_color = false;

  presenter.ScheduleOverlayPlane(candidate, nullptr);

  EXPECT_EQ(platform_presenter_ptr->schedule_count, 1);
  EXPECT_FALSE(platform_presenter_ptr->scheduled_image);
  EXPECT_TRUE(platform_presenter_ptr->scheduled_plane.is_root_overlay);
  EXPECT_FALSE(platform_presenter_ptr->scheduled_plane.is_solid_color);
}

TEST(OutputPresenterOzoneTest, MissingTransientOverlayPixmapIsSkipped) {
  auto platform_presenter = std::make_unique<FakeOzonePresenter>();
  auto* platform_presenter_ptr = platform_presenter.get();
  OutputPresenterOzone presenter(std::move(platform_presenter));
  OverlayCandidate candidate;
  candidate.is_root_render_pass = false;
  candidate.is_solid_color = false;

  presenter.ScheduleOverlayPlane(candidate, nullptr);

  EXPECT_EQ(platform_presenter_ptr->schedule_count, 0);
}

TEST(OutputPresenterOzoneTest, CommitsAccessOnlyAtAcceptedFrameBoundary) {
  auto platform_presenter = std::make_unique<FakeOzonePresenter>();
  OutputPresenterOzone presenter(std::move(platform_presenter));
  TestOverlayAccess overlay_access;
  OverlayCandidate root;
  root.is_root_render_pass = true;

  presenter.ScheduleOverlayPlane(root, overlay_access.access());
  EXPECT_EQ(overlay_access.commit_count(), 0);
  presenter.Present(base::BindOnce([](gfx::SwapCompletionResult) {}),
                    base::BindOnce([](const gfx::PresentationFeedback&) {}),
                    gfx::FrameData());

  EXPECT_EQ(overlay_access.commit_count(), 1);
}

TEST(OutputPresenterOzoneTest, ReusedAccessIsCommittedExactlyOnce) {
  auto platform_presenter = std::make_unique<FakeOzonePresenter>();
  OutputPresenterOzone presenter(std::move(platform_presenter));
  TestOverlayAccess overlay_access;
  OverlayCandidate root;
  root.is_root_render_pass = true;

  for (int frame = 0; frame < 2; ++frame) {
    presenter.ScheduleOverlayPlane(root, overlay_access.access());
    presenter.Present(base::BindOnce([](gfx::SwapCompletionResult) {}),
                      base::BindOnce([](const gfx::PresentationFeedback&) {}),
                      gfx::FrameData());
  }

  EXPECT_EQ(overlay_access.commit_count(), 1);
}

TEST(OutputPresenterOzoneTest, RejectedFrameRollsBackEveryAccess) {
  auto platform_presenter = std::make_unique<FakeOzonePresenter>();
  auto* platform_presenter_ptr = platform_presenter.get();
  OutputPresenterOzone presenter(std::move(platform_presenter));
  TestOverlayAccess overlay_access;
  OverlayCandidate root;
  root.is_root_render_pass = true;
  presenter.ScheduleOverlayPlane(root, overlay_access.access());

  platform_presenter_ptr->schedule_result = false;
  OverlayCandidate rejected_solid_color;
  rejected_solid_color.is_solid_color = true;
  rejected_solid_color.color = SkColors::kBlack;
  presenter.ScheduleOverlayPlane(rejected_solid_color, nullptr);
  presenter.Present(base::BindOnce([](gfx::SwapCompletionResult) {}),
                    base::BindOnce([](const gfx::PresentationFeedback&) {}),
                    gfx::FrameData());

  EXPECT_EQ(overlay_access.commit_count(), 0);
}

}  // namespace
}  // namespace viz
