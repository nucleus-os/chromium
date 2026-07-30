// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_OZONE_PLATFORM_WAYLAND_GPU_WAYLAND_BUFFER_QUEUE_PRESENTER_H_
#define UI_OZONE_PLATFORM_WAYLAND_GPU_WAYLAND_BUFFER_QUEUE_PRESENTER_H_

#include <memory>
#include <vector>

#include "base/containers/circular_deque.h"
#include "base/gtest_prod_util.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/ozone/platform/wayland/common/wayland_overlay_config.h"
#include "ui/ozone/platform/wayland/gpu/wayland_surface_gpu.h"
#include "ui/ozone/public/ozone_presenter.h"

namespace ui {

class WaylandBufferManagerGpu;

using BufferId = uint32_t;

// Rendering-API-independent Wayland presentation of native pixmap planes. This
// class owns the frame queue and browser-process Wayland transaction protocol;
// rendering backends provide only native pixmaps and explicit acquire fences.
class WaylandBufferQueuePresenter : public OzonePresenter,
                                    public WaylandSurfaceGpu {
 public:
  WaylandBufferQueuePresenter(WaylandBufferManagerGpu* buffer_manager,
                              gfx::AcceleratedWidget widget);

  WaylandBufferQueuePresenter(const WaylandBufferQueuePresenter&) = delete;
  WaylandBufferQueuePresenter& operator=(const WaylandBufferQueuePresenter&) =
      delete;

  ~WaylandBufferQueuePresenter() override;

  float surface_scale_factor() const { return surface_scale_factor_; }

  void QueueWaylandOverlayConfig(wl::WaylandOverlayConfig config);

  // OzonePresenter:
  bool ScheduleOverlayPlane(
      scoped_refptr<gfx::NativePixmap> image,
      std::unique_ptr<gfx::GpuFence> acquire_fence,
      const gfx::OverlayPlaneData& overlay_plane_data) override;
  void Present(SwapCompletionCallback completion_callback,
               PresentationCallback presentation_callback,
               gfx::FrameData data) override;
  bool SupportsPlaneGpuFences() const override;
  bool SupportsViewporter() const override;
  bool Resize(const gfx::Size& size,
              float scale_factor,
              const gfx::ColorSpace& color_space,
              bool has_alpha) override;

  // Legacy GL configurations may explicitly select implicit synchronization
  // when the compositor cannot consume acquire fences. The native Ozone path
  // never calls this method.
  void SetRelyOnImplicitSync();

 private:
  FRIEND_TEST_ALL_PREFIXES(WaylandSurfaceFactoryTest,
                           OzonePresenterCallbacksMayDestroyPresenter);
  FRIEND_TEST_ALL_PREFIXES(WaylandSurfaceFactoryTest,
                           OzonePresenterRejectsStaleSurfaceResponse);

  // Holds solid color buffers.
  class SolidColorBufferHolder {
   public:
    SolidColorBufferHolder();
    ~SolidColorBufferHolder();

    BufferId GetOrCreateSolidColorBuffer(
        SkColor4f color,
        WaylandBufferManagerGpu* buffer_manager);

    void OnSubmission(BufferId buffer_id,
                      WaylandBufferManagerGpu* buffer_manager);
    void EraseAvailableBuffers(WaylandBufferManagerGpu* buffer_manager);
    void DestroyAllBuffers(WaylandBufferManagerGpu* buffer_manager);

   private:
    // Gpu-size holder for the solid color buffers. These are not backed by
    // anything and stored on the gpu side for convenience so that WBHM doesn't
    // become more complex.
    struct SolidColorBuffer {
      SolidColorBuffer(const SkColor4f& color, BufferId buffer_id)
          : color(color), buffer_id(buffer_id) {}
      SolidColorBuffer(SolidColorBuffer&& buffer) = default;
      SolidColorBuffer& operator=(SolidColorBuffer&& buffer) = default;
      ~SolidColorBuffer() = default;

      // Color of the buffer.
      SkColor4f color = SkColors::kWhite;
      // The buffer id that is mapped with the buffer id created on the browser
      // side.
      BufferId buffer_id = 0;
    };

    std::vector<SolidColorBuffer> inflight_solid_color_buffers_;
    std::vector<SolidColorBuffer> available_solid_color_buffers_;
  };

  // WaylandSurfaceGpu overrides:
  void OnSubmission(uint32_t frame_id,
                    const gfx::SwapResult& swap_result,
                    gfx::GpuFenceHandle release_fence) override;
  void OnPresentation(uint32_t frame_id,
                      const gfx::PresentationFeedback& feedback) override;
  void OnBufferManagerDisconnected() override;

  // PendingFrame here is a post-SkiaRenderer struct that contains overlays +
  // primary plane information. It is a compositor frame at the
  // AcceleratedWidget level. The browser process translates its configs into
  // Wayland surface attachments.
  struct PendingFrame {
    explicit PendingFrame(uint32_t frame_id);
    ~PendingFrame();

    // Unique identifier of the frame within this AcceleratedWidget.
    uint32_t frame_id;

    bool ready = false;

    SwapCompletionCallback completion_callback;
    PresentationCallback presentation_callback;
    gfx::FrameData data;

    // Says if scheduling succeeded.
    bool schedule_planes_succeeded = true;

    std::vector<BufferId> in_flight_color_buffers;
    // Contains the buffer IDs and plane state committed atomically for this
    // frame.
    std::vector<wl::WaylandOverlayConfig> configs;
  };

  void MaybeSubmitFrames();
  void RecycleSolidColorBuffers(PendingFrame* frame);

  void FenceRetired(uint32_t frame_id);
  void EnterFailedState();
  void FailPendingFrames(bool install_sentinel);

  const raw_ptr<WaylandBufferManagerGpu> buffer_manager_;

  // The platform window presented by this queue.
  gfx::AcceleratedWidget widget_;

  // PendingFrames that are waiting to be submitted. They can be either ready,
  // waiting for gpu fences, or still scheduling overlays.
  base::circular_deque<std::unique_ptr<PendingFrame>> unsubmitted_frames_;

  // PendingFrames that are submitted, pending OnSubmission() calls.
  base::circular_deque<std::unique_ptr<PendingFrame>> submitted_frames_;

  // PendingFrames that have received OnSubmission(), pending OnPresentation()
  // calls.
  base::circular_deque<std::unique_ptr<PendingFrame>>
      pending_presentation_frames_;
  bool last_swap_buffers_result_ = true;
  bool wait_for_unsupported_acquire_fences_ = true;
  // Scale factor of the current surface.
  float surface_scale_factor_ = 1.f;

  // Holds gpu side reference (buffer_ids) for solid color wl_buffers.
  std::unique_ptr<SolidColorBufferHolder> solid_color_buffers_holder_;

  base::WeakPtrFactory<WaylandBufferQueuePresenter> weak_factory_;
};

}  // namespace ui

#endif  // UI_OZONE_PLATFORM_WAYLAND_GPU_WAYLAND_BUFFER_QUEUE_PRESENTER_H_
