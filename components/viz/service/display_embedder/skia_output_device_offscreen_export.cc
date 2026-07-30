// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/service/display_embedder/skia_output_device_offscreen_export.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "components/viz/service/display_embedder/offscreen_output_connection.h"
#include "gpu/command_buffer/common/shared_image_info.h"
#include "gpu/command_buffer/service/shared_image/shared_image_factory.h"
#include "gpu/command_buffer/service/shared_image/shared_image_representation.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "ui/gfx/native_pixmap.h"

namespace viz {

SkiaOutputDeviceOffscreenExport::SkiaOutputDeviceOffscreenExport(
    scoped_refptr<gpu::SharedContextState> context_state,
    scoped_refptr<gpu::MemoryTracker> memory_tracker,
    DidSwapBufferCompleteCallback did_swap_buffer_complete_callback,
    gpu::SharedImageFactory* shared_image_factory,
    gpu::SharedImageRepresentationFactory* representation_factory,
    std::unique_ptr<OffscreenOutputConnection> connection)
    : SkiaOutputDevice(context_state->gr_context(),
                       context_state->graphite_shared_context(),
                       std::move(memory_tracker),
                       std::move(did_swap_buffer_complete_callback)),
      context_state_(std::move(context_state)),
      shared_image_factory_(shared_image_factory),
      representation_factory_(representation_factory) {
  CHECK(connection);
  CHECK(connection->is_valid());
  client_.Bind(std::move(connection->client));
  receiver_.Bind(std::move(connection->output));
  receiver_.set_disconnect_handler(base::BindOnce(
      &SkiaOutputDeviceOffscreenExport::Shutdown, base::Unretained(this)));

  capabilities_.uses_default_gl_framebuffer = false;
  capabilities_.output_surface_origin = gfx::SurfaceOrigin::kTopLeft;
  capabilities_.backdrop_filters_replace_destination = true;
  capabilities_.number_of_buffers = kBufferCount;
  capabilities_.pending_swap_params.max_pending_swaps = kBufferCount - 1;
  capabilities_.sk_color_type_map[SinglePlaneFormat::kRGBA_8888] =
      kRGBA_8888_SkColorType;
  capabilities_.sk_color_type_map[SinglePlaneFormat::kBGRA_8888] =
      kBGRA_8888_SkColorType;
}

SkiaOutputDeviceOffscreenExport::~SkiaOutputDeviceOffscreenExport() {
  Shutdown();
}

bool SkiaOutputDeviceOffscreenExport::Reshape(const ReshapeParams& params) {
  if (failed_ || params.transform != gfx::OVERLAY_TRANSFORM_NONE) {
    return false;
  }
  size_ = params.GfxSize();
  if (size_.IsEmpty() || size_.width() > capabilities_.max_texture_size ||
      size_.height() > capabilities_.max_texture_size) {
    return false;
  }
  color_space_ = params.image_info.colorSpace()
                     ? gfx::ColorSpace(*params.image_info.colorSpace())
                     : gfx::ColorSpace::CreateSRGB();
  alpha_type_ = params.image_info.alphaType();
  sample_count_ = params.sample_count;
  format_ = params.image_info.colorType() == kRGBA_8888_SkColorType
                ? SharedImageFormat(SinglePlaneFormat::kRGBA_8888)
                : SharedImageFormat(SinglePlaneFormat::kBGRA_8888);
  queue_.Reshape();
  for (size_t i = 0; i < kBufferCount; ++i) {
    if (queue_.slot(i).state == OffscreenOutputQueue::State::kAvailable) {
      DestroySlot(i);
    }
  }
  return true;
}

SkSurface* SkiaOutputDeviceOffscreenExport::BeginPaint(
    std::vector<GrBackendSemaphore>* end_semaphores) {
  auto index = queue_.BeginFrame();
  if (!index) {
    // Viz's max-pending-swap limit reserves one slot for rendering. Reaching
    // this branch means the swap/release accounting contract was violated;
    // fail the endpoint instead of returning a null paint target and
    // potentially stranding the accepted BeginFrame.
    ReportError(mojom::OffscreenOutputError::kProtocolError);
    return nullptr;
  }
  rendering_slot_ = *index;
  Slot& slot = slots_[*index];
  if (slot.resource_generation != queue_.generation() &&
      !AllocateSlot(*index)) {
    queue_.CancelFrame(*index);
    rendering_slot_.reset();
    ReportError(mojom::OffscreenOutputError::kAllocationFailed);
    return nullptr;
  }

  std::vector<GrBackendSemaphore> begin_semaphores;
  SkSurfaceProps surface_props;
  slot.write = slot.skia->BeginScopedWriteAccess(
      sample_count_, surface_props, gfx::Rect(size_), &begin_semaphores,
      end_semaphores,
      gpu::SharedImageRepresentation::AllowUnclearedAccess::kYes);
  if (!slot.write) {
    queue_.CancelFrame(*index);
    rendering_slot_.reset();
    ReportError(mojom::OffscreenOutputError::kContextLost);
    return nullptr;
  }
  SkSurface* surface = slot.write->surface();
  if (!begin_semaphores.empty() &&
      !surface->wait(begin_semaphores.size(), begin_semaphores.data(),
                     /*deleteSemaphoresAfterWait=*/false)) {
    slot.write.reset();
    queue_.CancelFrame(*index);
    rendering_slot_.reset();
    ReportError(mojom::OffscreenOutputError::kContextLost);
    return nullptr;
  }
  return surface;
}

void SkiaOutputDeviceOffscreenExport::EndPaint() {
  if (!rendering_slot_) {
    return;
  }
  Slot& slot = slots_[*rendering_slot_];
  slot.write.reset();
  // Every exported buffer receives a full-root render, so the SharedImage is
  // completely initialized before it becomes visible to the consumer.
  slot.skia->SetCleared();
}

void SkiaOutputDeviceOffscreenExport::Present(
    const std::optional<gfx::Rect>& /*update_rect*/,
    BufferPresentedCallback feedback,
    OutputSurfaceFrame frame) {
  if (!rendering_slot_ || failed_) {
    ReportError(mojom::OffscreenOutputError::kProtocolError);
    return;
  }
  const size_t index = *rendering_slot_;
  rendering_slot_.reset();
  Slot& slot = slots_[index];
  auto token = queue_.Publish(index);
  if (!token) {
    ReportError(mojom::OffscreenOutputError::kProtocolError);
    return;
  }
  slot.read = slot.overlay->BeginScopedReadAccess();
  if (!slot.read) {
    ReportError(mojom::OffscreenOutputError::kContextLost);
    return;
  }
  std::optional<gpu::ExternalVulkanImageState> external_state;
  if (!slot.read->TakeExternalVulkanImageState(&external_state)) {
    ReportError(mojom::OffscreenOutputError::kSynchronizationStateLost);
    return;
  }
  if (!external_state || !external_state->IsValid()) {
    ReportError(mojom::OffscreenOutputError::kSynchronizationStateLost);
    return;
  }
  scoped_refptr<gfx::NativePixmap> pixmap = slot.read->GetNativePixmap();
  if (!pixmap) {
    ReportError(mojom::OffscreenOutputError::kUnsupported);
    return;
  }
  gfx::GpuFenceHandle acquire_fence = slot.read->TakeAcquireFence();
  if (acquire_fence.is_null()) {
    ReportError(mojom::OffscreenOutputError::kSynchronizationStateLost);
    return;
  }

  StartSwapBuffers(std::move(feedback));
  swap_order_.push_back(*token);
  slot.published_token = *token;
  slot.frame.emplace(std::move(frame));

  auto exported = mojom::OffscreenOutputFrame::New();
  exported->generation = queue_.generation();
  exported->slot_index = index;
  exported->content_serial = queue_.slot(index).content_serial;
  exported->frame_token = *token;
  exported->coded_size = size_;
  exported->visible_rect = gfx::Rect(size_);
  exported->damage_rect = gfx::Rect(size_);
  exported->format = format_;
  exported->color_space = color_space_;
  exported->alpha_type = alpha_type_;
  exported->origin = mojom::OffscreenOutputOrigin::kTopLeft;
  exported->queue_family_index = external_state->external_queue_family;
  exported->producer_old_layout = external_state->old_layout;
  exported->producer_new_layout = external_state->new_layout;
  exported->native_pixmap = pixmap->ExportHandle();
  exported->acquire_fence = std::move(acquire_fence);
  exported->frame_time = base::TimeTicks::Now();
  exported->swap_trace_id = slot.frame->data.swap_trace_id;
  client_->OnFrameAvailable(std::move(exported));
}

void SkiaOutputDeviceOffscreenExport::AcknowledgeFrame(
    uint64_t frame_token) {
  if (failed_) {
    return;
  }
  Slot* acknowledged = nullptr;
  for (Slot& slot : slots_) {
    if (slot.frame && slot.published_token == frame_token &&
        !slot.swap_acknowledged) {
      acknowledged = &slot;
      break;
    }
  }
  if (!acknowledged) {
    ReportError(mojom::OffscreenOutputError::kProtocolError);
    return;
  }
  acknowledged->swap_acknowledged = true;
  DrainAcknowledgedSwaps();
}

void SkiaOutputDeviceOffscreenExport::ReleaseFrame(
    uint64_t frame_token,
    gfx::GpuFenceHandle release_fence) {
  if (failed_ || release_fence.is_null()) {
    ReportError(mojom::OffscreenOutputError::kProtocolError);
    return;
  }
  Slot* released = nullptr;
  for (Slot& slot : slots_) {
    if (slot.published_token == frame_token && slot.read &&
        !slot.consumer_released) {
      released = &slot;
      break;
    }
  }
  if (!released || !released->read) {
    ReportError(mojom::OffscreenOutputError::kProtocolError);
    return;
  }
  if (!released->read->SetReleaseExternalVulkanImageState(
          {.old_layout = VK_IMAGE_LAYOUT_GENERAL,
           .new_layout = VK_IMAGE_LAYOUT_GENERAL,
           .external_queue_family = VK_QUEUE_FAMILY_EXTERNAL_KHR})) {
    ReportError(mojom::OffscreenOutputError::kSynchronizationStateLost);
    return;
  }
  released->read->SetReleaseFence(std::move(release_fence));
  released->read.reset();
  released->consumer_released = true;
  if (released->swap_completed) {
    RecycleReleasedSlot(released - slots_.data());
  }
}

bool SkiaOutputDeviceOffscreenExport::AllocateSlot(size_t index) {
  DestroySlot(index);
  Slot& slot = slots_[index];
  slot.mailbox = gpu::Mailbox::Generate();
  const gpu::SharedImageUsageSet usage = gpu::SHARED_IMAGE_USAGE_DISPLAY_READ |
                                         gpu::SHARED_IMAGE_USAGE_DISPLAY_WRITE |
                                         gpu::SHARED_IMAGE_USAGE_SCANOUT;
  if (!shared_image_factory_->CreateSharedImage(
          slot.mailbox,
          gpu::SharedImageInfo(format_, size_, color_space_,
                               kTopLeft_GrSurfaceOrigin, alpha_type_, usage,
                               "OffscreenOutputExport"),
          gpu::kNullSurfaceHandle, gfx::BufferUsage::SCANOUT)) {
    return false;
  }
  if (!context_state_->IsGraphiteDawnVulkan()) {
    DestroySlot(index);
    return false;
  }
  slot.skia =
      representation_factory_->ProduceSkia(slot.mailbox, context_state_, usage);
  slot.overlay = representation_factory_->ProduceOverlay(slot.mailbox);
  if (!slot.skia || !slot.overlay) {
    DestroySlot(index);
    return false;
  }
  slot.resource_generation = queue_.generation();
  return true;
}

void SkiaOutputDeviceOffscreenExport::DestroySlot(size_t index) {
  Slot& slot = slots_[index];
  slot.write.reset();
  if (slot.read) {
    CHECK(slot.read->AbandonExternalVulkanAccessForBackingDestruction());
  }
  slot.read.reset();
  slot.skia.reset();
  slot.overlay.reset();
  if (!slot.mailbox.IsZero()) {
    shared_image_factory_->DestroySharedImage(slot.mailbox);
  }
  slot = Slot();
}

void SkiaOutputDeviceOffscreenExport::DiscardBackbuffer() {
  // Visibility changes must not invalidate published frames or permanently
  // disable the endpoint. The fixed pool naturally stops producing while Viz
  // is not drawing.
}

void SkiaOutputDeviceOffscreenExport::Shutdown() {
  if (failed_) {
    return;
  }
  failed_ = true;
  receiver_.reset();
  client_.reset();
  queue_.Abandon();
  for (size_t i = 0; i < kBufferCount; ++i) {
    DestroySlot(i);
  }
}

void SkiaOutputDeviceOffscreenExport::ReportError(
    mojom::OffscreenOutputError error) {
  if (failed_) {
    return;
  }
  client_->OnOutputError(error);
  Shutdown();
}

void SkiaOutputDeviceOffscreenExport::RecycleReleasedSlot(size_t index) {
  Slot& slot = slots_[index];
  CHECK(slot.swap_completed);
  CHECK(slot.consumer_released);
  CHECK(slot.published_token);
  if (queue_.Release(slot.published_token) != index) {
    ReportError(mojom::OffscreenOutputError::kProtocolError);
    return;
  }
  slot.published_token = 0;
  slot.swap_acknowledged = false;
  slot.swap_completed = false;
  slot.consumer_released = false;
  if (slot.resource_generation != queue_.generation()) {
    DestroySlot(index);
  }
}

void SkiaOutputDeviceOffscreenExport::DrainAcknowledgedSwaps() {
  while (!swap_order_.empty()) {
    const uint64_t token = swap_order_.front();
    std::optional<size_t> found_index;
    for (size_t i = 0; i < slots_.size(); ++i) {
      Slot& slot = slots_[i];
      if (slot.frame && slot.published_token == token &&
          slot.swap_acknowledged) {
        found_index = i;
        break;
      }
    }
    if (!found_index) {
      break;
    }
    Slot& found = slots_[*found_index];
    OutputSurfaceFrame frame = std::move(*found.frame);
    const gfx::Size frame_size = frame.size;
    found.frame.reset();
    swap_order_.pop_front();
    found.swap_completed = true;
    FinishSwapBuffers(gfx::SwapCompletionResult(gfx::SwapResult::SWAP_ACK),
                      frame_size, std::move(frame), gfx::Rect(size_));
    if (found.consumer_released) {
      RecycleReleasedSlot(*found_index);
      if (failed_) {
        return;
      }
    }
  }
}

}  // namespace viz
