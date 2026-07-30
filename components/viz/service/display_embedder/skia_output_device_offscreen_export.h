// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_VIZ_SERVICE_DISPLAY_EMBEDDER_SKIA_OUTPUT_DEVICE_OFFSCREEN_EXPORT_H_
#define COMPONENTS_VIZ_SERVICE_DISPLAY_EMBEDDER_SKIA_OUTPUT_DEVICE_OFFSCREEN_EXPORT_H_

#include <array>
#include <deque>
#include <memory>
#include <optional>

#include "components/viz/service/display_embedder/offscreen_output_queue.h"
#include "components/viz/service/display_embedder/skia_output_device.h"
#include "gpu/command_buffer/common/mailbox.h"
#include "gpu/command_buffer/service/memory_tracking.h"
#include "gpu/command_buffer/service/shared_context_state.h"
#include "gpu/command_buffer/service/shared_image/shared_image_representation.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/viz/privileged/mojom/compositing/offscreen_output.mojom.h"

namespace gpu {
class SharedImageFactory;
class SharedImageRepresentationFactory;
}  // namespace gpu

namespace viz {

struct OffscreenOutputConnection;

// Exportable Vulkan/Ozone offscreen output. Its SharedImages remain borrowed
// by the external consumer until ReleaseFrame supplies a completion fence.
class SkiaOutputDeviceOffscreenExport final : public SkiaOutputDevice,
                                              public mojom::OffscreenOutput {
 public:
  SkiaOutputDeviceOffscreenExport(
      scoped_refptr<gpu::SharedContextState> context_state,
      scoped_refptr<gpu::MemoryTracker> memory_tracker,
      DidSwapBufferCompleteCallback did_swap_buffer_complete_callback,
      gpu::SharedImageFactory* shared_image_factory,
      gpu::SharedImageRepresentationFactory* representation_factory,
      std::unique_ptr<OffscreenOutputConnection> connection);
  ~SkiaOutputDeviceOffscreenExport() override;

  bool Reshape(const ReshapeParams& params) override;
  void Present(const std::optional<gfx::Rect>& update_rect,
               BufferPresentedCallback feedback,
               OutputSurfaceFrame frame) override;
  void DiscardBackbuffer() override;

 private:
  struct Slot {
    gpu::Mailbox mailbox;
    uint64_t resource_generation = 0;
    std::unique_ptr<gpu::SkiaImageRepresentation> skia;
    std::unique_ptr<gpu::OverlayImageRepresentation> overlay;
    std::unique_ptr<gpu::SkiaImageRepresentation::ScopedWriteAccess> write;
    std::unique_ptr<gpu::OverlayImageRepresentation::ScopedReadAccess> read;
    std::optional<OutputSurfaceFrame> frame;
    uint64_t published_token = 0;
    bool swap_acknowledged = false;
    bool swap_completed = false;
    bool consumer_released = false;
  };

  SkSurface* BeginPaint(
      std::vector<GrBackendSemaphore>* end_semaphores) override;
  void EndPaint() override;
  void AcknowledgeFrame(uint64_t frame_token) override;
  void ReleaseFrame(uint64_t frame_token,
                    gfx::GpuFenceHandle release_fence) override;

  bool AllocateSlot(size_t index);
  void DestroySlot(size_t index);
  void RecycleReleasedSlot(size_t index);
  void Shutdown();
  void ReportError(mojom::OffscreenOutputError error);
  void DrainAcknowledgedSwaps();

  static constexpr size_t kBufferCount = 4;
  scoped_refptr<gpu::SharedContextState> context_state_;
  const raw_ptr<gpu::SharedImageFactory> shared_image_factory_;
  const raw_ptr<gpu::SharedImageRepresentationFactory> representation_factory_;
  mojo::Remote<mojom::OffscreenOutputClient> client_;
  mojo::Receiver<mojom::OffscreenOutput> receiver_{this};
  OffscreenOutputQueue queue_{kBufferCount};
  std::array<Slot, kBufferCount> slots_;
  std::deque<uint64_t> swap_order_;
  std::optional<size_t> rendering_slot_;
  gfx::Size size_;
  gfx::ColorSpace color_space_;
  SharedImageFormat format_ = SinglePlaneFormat::kBGRA_8888;
  SkAlphaType alpha_type_ = kPremul_SkAlphaType;
  int sample_count_ = 1;
  bool failed_ = false;
};

}  // namespace viz

#endif  // COMPONENTS_VIZ_SERVICE_DISPLAY_EMBEDDER_SKIA_OUTPUT_DEVICE_OFFSCREEN_EXPORT_H_
