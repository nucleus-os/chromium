// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/wayland/gpu/wayland_buffer_queue_presenter.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/thread_pool.h"
#include "base/trace_event/trace_event.h"
#include "ui/gfx/gpu_fence.h"
#include "ui/gfx/gpu_fence_handle.h"
#include "ui/gfx/swap_result.h"
#include "ui/ozone/platform/wayland/gpu/wayland_buffer_manager_gpu.h"
#include "ui/ozone/platform/wayland/mojom/wayland_overlay_config.mojom.h"

namespace ui {

namespace {

// A test run showed only 9 inflight solid color buffers at the same time. Thus,
// allow to store max 12 buffers (including some margin) of solid color buffers
// and remove the rest.
static constexpr size_t kMaxSolidColorBuffers = 12;

struct FailedFrameCallbacks {
  OzonePresenter::SwapCompletionCallback completion;
  OzonePresenter::PresentationCallback presentation;
};

void WaitForGpuFences(std::vector<std::unique_ptr<gfx::GpuFence>> fences) {
  for (auto& fence : fences) {
    fence->Wait();
  }
}

}  // namespace

WaylandBufferQueuePresenter::SolidColorBufferHolder::SolidColorBufferHolder() =
    default;
WaylandBufferQueuePresenter::SolidColorBufferHolder::~SolidColorBufferHolder() =
    default;

BufferId WaylandBufferQueuePresenter::SolidColorBufferHolder::
    GetOrCreateSolidColorBuffer(SkColor4f color,
                                WaylandBufferManagerGpu* buffer_manager) {
  BufferId next_buffer_id = 0;

  // First try for an existing buffer.
  auto it = std::ranges::find(available_solid_color_buffers_, color,
                              &SolidColorBuffer::color);
  if (it != available_solid_color_buffers_.end()) {
    // This is a prefect color match so use this directly.
    next_buffer_id = it->buffer_id;
    inflight_solid_color_buffers_.emplace_back(std::move(*it));
    available_solid_color_buffers_.erase(it);
  } else {
    // Worst case allocate a new buffer. This definitely will occur on
    // startup.
    next_buffer_id = buffer_manager->AllocateBufferID();
    // Create wl_buffer on the browser side.
    CHECK(buffer_manager->supports_single_pixel_buffer());
    buffer_manager->CreateSinglePixelBuffer(color, next_buffer_id);

    // Allocate a backing structure that will be used to figure out if such
    // buffer has already existed.
    inflight_solid_color_buffers_.emplace_back(
        SolidColorBuffer(color, next_buffer_id));
  }
  DCHECK_GT(next_buffer_id, 0u);
  return next_buffer_id;
}

void WaylandBufferQueuePresenter::SolidColorBufferHolder::OnSubmission(
    BufferId buffer_id,
    WaylandBufferManagerGpu* buffer_manager) {
  // Solid color buffers do not require on submission as Skia doesn't track
  // them. Instead, they are tracked by WaylandBufferQueuePresenter. In the
  // future, when SharedImageFactory allows non-backed shared images, this
  // should be removed from here.
  auto it = std::ranges::find(inflight_solid_color_buffers_, buffer_id,
                              &SolidColorBuffer::buffer_id);
  if (it != inflight_solid_color_buffers_.end()) {
    available_solid_color_buffers_.emplace_back(std::move(*it));
    inflight_solid_color_buffers_.erase(it);
    // Keep track of the number of created buffers and erase the least used
    // ones until the maximum number of available solid color buffer.
    while (available_solid_color_buffers_.size() > kMaxSolidColorBuffers) {
      buffer_manager->DestroyBuffer(
          available_solid_color_buffers_.begin()->buffer_id);
      available_solid_color_buffers_.erase(
          available_solid_color_buffers_.begin());
    }
  }
}

void WaylandBufferQueuePresenter::SolidColorBufferHolder::EraseAvailableBuffers(
    WaylandBufferManagerGpu* buffer_manager) {
  for (const auto& buffer : available_solid_color_buffers_) {
    buffer_manager->DestroyBuffer(buffer.buffer_id);
  }
  available_solid_color_buffers_.clear();
}

void WaylandBufferQueuePresenter::SolidColorBufferHolder::DestroyAllBuffers(
    WaylandBufferManagerGpu* buffer_manager) {
  EraseAvailableBuffers(buffer_manager);
  for (const auto& buffer : inflight_solid_color_buffers_) {
    buffer_manager->DestroyBuffer(buffer.buffer_id);
  }
  inflight_solid_color_buffers_.clear();
}

WaylandBufferQueuePresenter::WaylandBufferQueuePresenter(
    WaylandBufferManagerGpu* buffer_manager,
    gfx::AcceleratedWidget widget)
    : buffer_manager_(buffer_manager),
      widget_(widget),
      solid_color_buffers_holder_(std::make_unique<SolidColorBufferHolder>()),
      weak_factory_(this) {
  buffer_manager_->RegisterSurface(widget_, this);
  unsubmitted_frames_.push_back(
      std::make_unique<PendingFrame>(buffer_manager_->AllocateFrameID()));
}

void WaylandBufferQueuePresenter::QueueWaylandOverlayConfig(
    wl::WaylandOverlayConfig config) {
  auto* frame = unsubmitted_frames_.back().get();
  DCHECK(frame);
  TRACE_EVENT("wayland",
              "WaylandBufferQueuePresenter::QueueWaylandOverlayConfig",
              "frame_id", frame->frame_id, "buffer_id", config.buffer_id);
  frame->configs.emplace_back(std::move(config));
}

bool WaylandBufferQueuePresenter::ScheduleOverlayPlane(
    scoped_refptr<gfx::NativePixmap> image,
    std::unique_ptr<gfx::GpuFence> acquire_fence,
    const gfx::OverlayPlaneData& overlay_plane_data) {
  // A presenter failure is terminal. Refuse ownership of any additional
  // images or fences so callers can unwind their access immediately.
  if (!last_swap_buffers_result_) {
    return false;
  }

  auto* frame = unsubmitted_frames_.back().get();
  // There are multiple scheduling submissions for the same frame. If the
  // previous schedule failed, there is no reason to continue.
  if (!frame->schedule_planes_succeeded) {
    return false;
  }

  // Solid color overlays are non-backed. Thus, queue them directly.
  // TODO(msisov): reconsider this once Linux Wayland compositors also support
  // creation of non-backed solid color wl_buffers.
  if (!image) {
    // Only solid color overlays can be non-backed.
    if (!overlay_plane_data.is_solid_color) {
      LOG(ERROR) << "Missing buffer for overlay that is not solid color.";
      frame->schedule_planes_succeeded = false;
      return false;
    }
    DCHECK(!acquire_fence);

    BufferId buf_id = solid_color_buffers_holder_->GetOrCreateSolidColorBuffer(
        overlay_plane_data.color.value(), buffer_manager_);
    // Invalid buffer id.
    if (buf_id == 0) {
      frame->schedule_planes_succeeded = false;
      return false;
    }
    frame->in_flight_color_buffers.push_back(buf_id);
    QueueWaylandOverlayConfig(
        {overlay_plane_data, nullptr, buf_id, surface_scale_factor()});
  } else {
    std::vector<gfx::GpuFence> acquire_fences;
    if (acquire_fence && (buffer_manager_->supports_acquire_fence() ||
                          wait_for_unsupported_acquire_fences_)) {
      acquire_fences.push_back(std::move(*acquire_fence));
    }

    frame->schedule_planes_succeeded = image->ScheduleOverlayPlane(
        widget_, overlay_plane_data, std::move(acquire_fences), {});
  }
  return frame->schedule_planes_succeeded;
}

void WaylandBufferQueuePresenter::Present(
    SwapCompletionCallback completion_callback,
    PresentationCallback presentation_callback,
    gfx::FrameData data) {
  TRACE_EVENT0("wayland", "WaylandBufferQueuePresenter::Present");
  // If last swap failed, don't try to schedule new ones.
  if (!last_swap_buffers_result_) {
    PendingFrame* discarded_frame = unsubmitted_frames_.back().get();
    RecycleSolidColorBuffers(discarded_frame);
    discarded_frame->configs.clear();
    std::move(completion_callback)
        .Run(gfx::SwapCompletionResult(gfx::SwapResult::SWAP_FAILED));
    // Notify the caller, the buffer is never presented on a screen.
    std::move(presentation_callback).Run(gfx::PresentationFeedback::Failure());
    return;
  }

  PendingFrame* frame = unsubmitted_frames_.back().get();
  frame->completion_callback = std::move(completion_callback);
  frame->presentation_callback = std::move(presentation_callback);
  frame->data = data;

  unsubmitted_frames_.push_back(
      std::make_unique<PendingFrame>(buffer_manager_->AllocateFrameID()));
  unsubmitted_frames_.back()->configs.reserve(frame->configs.size());
  // If Wayland server supports acquire_fences, they should be shipped with
  // buffers. Otherwise, we will wait for fences.
  if (buffer_manager_->supports_acquire_fence() ||
      !wait_for_unsupported_acquire_fences_ ||
      !frame->schedule_planes_succeeded) {
    frame->ready = true;
    MaybeSubmitFrames();
    return;
  }

  base::OnceClosure fence_wait_task;
  std::vector<std::unique_ptr<gfx::GpuFence>> fences;
  for (auto& config : frame->configs) {
    if (!config.access_fence_handle.is_null()) {
      fences.push_back(std::make_unique<gfx::GpuFence>(
          std::move(config.access_fence_handle)));
      config.access_fence_handle = gfx::GpuFenceHandle();
    }
  }

  fence_wait_task = base::BindOnce(&WaitForGpuFences, std::move(fences));

  base::OnceClosure fence_retired_callback =
      base::BindOnce(&WaylandBufferQueuePresenter::FenceRetired,
                     weak_factory_.GetWeakPtr(), frame->frame_id);

  base::ThreadPool::PostTaskAndReply(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      std::move(fence_wait_task), std::move(fence_retired_callback));
}

void WaylandBufferQueuePresenter::SetRelyOnImplicitSync() {
  wait_for_unsupported_acquire_fences_ = false;
}

bool WaylandBufferQueuePresenter::SupportsPlaneGpuFences() const {
  return buffer_manager_->supports_acquire_fence();
}

bool WaylandBufferQueuePresenter::SupportsViewporter() const {
  return buffer_manager_->supports_viewporter();
}

bool WaylandBufferQueuePresenter::Resize(const gfx::Size& size,
                                         float scale_factor,
                                         const gfx::ColorSpace& color_space,
                                         bool has_alpha) {
  surface_scale_factor_ = scale_factor;

  // Remove all the buffers.
  solid_color_buffers_holder_->EraseAvailableBuffers(buffer_manager_);

  return true;
}

WaylandBufferQueuePresenter::~WaylandBufferQueuePresenter() {
  weak_factory_.InvalidateWeakPtrs();
  buffer_manager_->UnregisterSurface(widget_);
  FailPendingFrames(/*install_sentinel=*/false);
  solid_color_buffers_holder_->DestroyAllBuffers(buffer_manager_);
}

WaylandBufferQueuePresenter::PendingFrame::PendingFrame(uint32_t frame_id)
    : frame_id(frame_id) {}

WaylandBufferQueuePresenter::PendingFrame::~PendingFrame() = default;

void WaylandBufferQueuePresenter::MaybeSubmitFrames() {
  while (!unsubmitted_frames_.empty() && unsubmitted_frames_.front()->ready) {
    auto submitted_frame = std::move(unsubmitted_frames_.front());
    unsubmitted_frames_.pop_front();

    if (!submitted_frame->schedule_planes_succeeded) {
      // Put the failed frame back at the head so EnterFailedState() can detach
      // all callbacks and install its terminal sentinel before invoking any
      // client code. A completion callback is allowed to synchronously destroy
      // this presenter.
      unsubmitted_frames_.push_front(std::move(submitted_frame));
      EnterFailedState();
      return;
    }

    buffer_manager_->CommitOverlays(widget_, submitted_frame->frame_id,
                                    submitted_frame->data,
                                    std::move(submitted_frame->configs));
    submitted_frames_.push_back(std::move(submitted_frame));
  }
}

void WaylandBufferQueuePresenter::RecycleSolidColorBuffers(
    PendingFrame* frame) {
  for (BufferId buffer : frame->in_flight_color_buffers) {
    solid_color_buffers_holder_->OnSubmission(buffer, buffer_manager_);
  }
  frame->in_flight_color_buffers.clear();
}

void WaylandBufferQueuePresenter::FenceRetired(uint32_t frame_id) {
  auto frame_it =
      std::ranges::find(unsubmitted_frames_, frame_id,
                        [](const std::unique_ptr<PendingFrame>& frame) {
                          return frame->frame_id;
                        });
  if (frame_it == unsubmitted_frames_.end()) {
    return;
  }

  // If the frame doesn't have a completion callback yet, it is the sentinel
  // frame that is still collecting overlay planes.
  if (!(*frame_it)->completion_callback) {
    return;
  }
  (*frame_it)->ready = true;
  MaybeSubmitFrames();
}

void WaylandBufferQueuePresenter::OnSubmission(
    uint32_t frame_id,
    const gfx::SwapResult& swap_result,
    gfx::GpuFenceHandle release_fence) {
  // Ignore a response that does not belong to the oldest submitted frame.
  if (submitted_frames_.empty() ||
      submitted_frames_.front()->frame_id != frame_id) {
    return;
  }

  auto submitted_frame = std::move(submitted_frames_.front());

  TRACE_EVENT("wayland", "WaylandBufferQueuePresenter::OnSubmission",
              "frame_id", submitted_frame->frame_id);

  submitted_frames_.pop_front();
  RecycleSolidColorBuffers(submitted_frame.get());

  // Check if the fence has retired.
  if (!release_fence.is_null()) {
    base::TimeTicks ticks;
    auto status =
        gfx::GpuFence::GetStatusChangeTime(release_fence.Peek(), &ticks);
    if (status == gfx::GpuFence::kSignaled) {
      release_fence = {};
    }
  }

  pending_presentation_frames_.push_back(std::move(submitted_frame));

  auto& pending_frame = pending_presentation_frames_.back();
  SwapCompletionCallback completion_callback =
      std::move(pending_frame->completion_callback);
  base::WeakPtr<WaylandBufferQueuePresenter> weak_this =
      weak_factory_.GetWeakPtr();
  std::move(completion_callback)
      .Run(gfx::SwapCompletionResult(swap_result, std::move(release_fence)));
  if (!weak_this) {
    return;
  }

  if (swap_result != gfx::SwapResult::SWAP_ACK &&
      swap_result != gfx::SwapResult::SWAP_NAK_RECREATE_BUFFERS) {
    EnterFailedState();
    return;
  }

  MaybeSubmitFrames();
}

void WaylandBufferQueuePresenter::OnPresentation(
    uint32_t frame_id,
    const gfx::PresentationFeedback& feedback) {
  if (pending_presentation_frames_.empty() ||
      pending_presentation_frames_.front()->frame_id != frame_id) {
    return;
  }

  PresentationCallback presentation_callback =
      std::move(pending_presentation_frames_.front()->presentation_callback);
  pending_presentation_frames_.pop_front();
  std::move(presentation_callback).Run(feedback);
}

void WaylandBufferQueuePresenter::OnBufferManagerDisconnected() {
  EnterFailedState();
}

void WaylandBufferQueuePresenter::EnterFailedState() {
  last_swap_buffers_result_ = false;
  FailPendingFrames(/*install_sentinel=*/true);
}

void WaylandBufferQueuePresenter::FailPendingFrames(bool install_sentinel) {
  std::vector<FailedFrameCallbacks> failed_callbacks;

  auto detach_frame = [this, &failed_callbacks](
                          std::unique_ptr<PendingFrame> frame,
                          bool completion_pending) {
    RecycleSolidColorBuffers(frame.get());
    FailedFrameCallbacks callbacks;
    if (completion_pending) {
      callbacks.completion = std::move(frame->completion_callback);
    }
    callbacks.presentation = std::move(frame->presentation_callback);
    if (callbacks.completion || callbacks.presentation) {
      failed_callbacks.push_back(std::move(callbacks));
    }
  };

  // Preserve frame order across the three lifecycle queues: a frame awaiting
  // presentation is older than one awaiting submission, which is older than a
  // frame whose acquire fence has not retired yet.
  while (!pending_presentation_frames_.empty()) {
    auto frame = std::move(pending_presentation_frames_.front());
    pending_presentation_frames_.pop_front();
    detach_frame(std::move(frame), /*completion_pending=*/false);
  }
  while (!submitted_frames_.empty()) {
    auto frame = std::move(submitted_frames_.front());
    submitted_frames_.pop_front();
    detach_frame(std::move(frame), /*completion_pending=*/true);
  }
  while (!unsubmitted_frames_.empty()) {
    auto frame = std::move(unsubmitted_frames_.front());
    unsubmitted_frames_.pop_front();
    detach_frame(std::move(frame), /*completion_pending=*/true);
  }

  if (install_sentinel) {
    unsubmitted_frames_.push_back(
        std::make_unique<PendingFrame>(buffer_manager_->AllocateFrameID()));
  }

  // Do not access any presenter member after this point. Either callback may
  // synchronously destroy the presenter and its owner.
  for (auto& callbacks : failed_callbacks) {
    if (callbacks.completion) {
      std::move(callbacks.completion)
          .Run(gfx::SwapCompletionResult(gfx::SwapResult::SWAP_FAILED));
    }
    if (callbacks.presentation) {
      std::move(callbacks.presentation)
          .Run(gfx::PresentationFeedback::Failure());
    }
  }
}

}  // namespace ui
