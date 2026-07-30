// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/service/frame_sinks/external_begin_frame_source_mojo.h"

#include <utility>

#include "base/check_is_test.h"
#include "base/functional/bind.h"
#include "base/notreached.h"
#include "components/viz/service/frame_sinks/frame_sink_manager_impl.h"
#include "mojo/public/cpp/bindings/message.h"

namespace viz {

ExternalBeginFrameSourceMojo::ExternalBeginFrameSourceMojo(
    FrameSinkManagerImpl* frame_sink_manager,
    mojo::PendingAssociatedReceiver<mojom::ExternalBeginFrameController>
        controller_receiver,
    mojo::PendingAssociatedRemote<mojom::ExternalBeginFrameControllerClient>
        controller_client_remote,
    uint32_t restart_id,
    bool wait_for_all_frame_sinks)
    : ExternalBeginFrameSource(this, restart_id),
      frame_sink_manager_(frame_sink_manager),
      receiver_(this, std::move(controller_receiver)),
      remote_client_(std::move(controller_client_remote)),
      wait_for_all_frame_sinks_(wait_for_all_frame_sinks) {
  receiver_.set_disconnect_handler(base::BindOnce(
      &ExternalBeginFrameSourceMojo::AbortPendingFrame,
      base::Unretained(this)));
  frame_sink_manager_->AddObserver(this);
}

ExternalBeginFrameSourceMojo::~ExternalBeginFrameSourceMojo() {
  AbortPendingFrame();
  frame_sink_manager_->RemoveObserver(this);
  CHECK(!display_);
}

void ExternalBeginFrameSourceMojo::IssueExternalBeginFrame(
    const BeginFrameArgs& args,
    base::OnceCallback<void(const BeginFrameAck&)> callback) {
  if (pending_frame_callback_ ||
      (wait_for_all_frame_sinks_ && !pending_frame_sinks_.empty())) {
    mojo::ReportBadMessage("Got overlapping IssueExternalBeginFrame");
    return;
  }
  original_source_id_ = args.frame_id.source_id;
  pending_frame_args_ = args;

  OnBeginFrame(args);

  pending_frame_callback_ = std::move(callback);

  if (!display_) {
    CHECK_IS_TEST();
    return;
  }

  // Ensure that Display will receive the BeginFrame (as a missed one), even if
  // it doesn't currently need it. This way, we ensure that
  // `OnDisplayDidFinishFrame()` will be called for this BeginFrame.
  display_->SetNeedsOneBeginFrame(args);
  MaybeProduceFrameCallback();
}

#if BUILDFLAG(IS_MAC)
void ExternalBeginFrameSourceMojo::IssueExternalVSync(
    const CADisplayLinkParams& params) {
  // For ExternalBeginFrameSourceMojoMac only.
  NOTREACHED();
}

void ExternalBeginFrameSourceMojo::SetSupportedDisplayLinkId(
    int64_t display_id,
    bool is_supported) {
  // For ExternalBeginFrameSourceMojoMac only.
  NOTREACHED();
}
#endif

void ExternalBeginFrameSourceMojo::OnDestroyedCompositorFrameSink(
    const FrameSinkId& sink_id) {
  pending_frame_sinks_.erase(sink_id);
  MaybeProduceFrameCallback();
}

void ExternalBeginFrameSourceMojo::OnFrameSinkDidBeginFrame(
    const FrameSinkId& sink_id,
    const BeginFrameArgs& args) {
  if (!original_source_id_ || !pending_frame_callback_ ||
      args.frame_id.source_id != *original_source_id_ ||
      !pending_frame_args_ ||
      args.frame_id != pending_frame_args_->frame_id) {
    return;
  }
  pending_frame_sinks_.insert(sink_id);
}

void ExternalBeginFrameSourceMojo::OnFrameSinkDidFinishFrame(
    const FrameSinkId& sink_id,
    const BeginFrameArgs& args) {
  if (!original_source_id_ || !pending_frame_callback_ ||
      args.frame_id.source_id != *original_source_id_ ||
      !pending_frame_args_ ||
      args.frame_id != pending_frame_args_->frame_id) {
    return;
  }
  pending_frame_sinks_.erase(sink_id);
  MaybeProduceFrameCallback();
}

void ExternalBeginFrameSourceMojo::MaybeProduceFrameCallback() {
  if (!pending_frame_sinks_.empty())
    return;
  if (!pending_frame_callback_)
    return;

  if (pending_ack_) {
    CompletePendingFrame(*pending_ack_);
    return;
  }
  // If there aren't pending surfaces and the root frame is not missing,
  // the display scheduler is likely to produce proper frame, so let it do
  // its work. Otherwise, fire the pending frame callback early.
  if (!display_->IsRootFrameMissing() &&
      !display_->HasPendingSurfaces(*pending_frame_args_)) {
    return;
  }

  // All frame sinks are done with frame, yet the root frame is still missing,
  // the display won't draw, so resolve callback now.
  BeginFrameAck nak(pending_frame_args_->frame_id.source_id,
                    pending_frame_args_->frame_id.sequence_number,
                    /*has_damage=*/false);
  CompletePendingFrame(nak);
}

void ExternalBeginFrameSourceMojo::CompletePendingFrame(
    const BeginFrameAck& ack) {
  CHECK(pending_frame_callback_);
  CHECK(pending_frame_args_);
  CHECK(ack.frame_id == pending_frame_args_->frame_id);

  // If there are pending copy output requests that have not been fulfilled,
  // cancel them, as they won't be served till the next frame. This prevents
  // the client for waiting for them indefinitely.
  // The source is registered with the frame-sink manager as part of attaching
  // it to a Display. A transaction may also be aborted before that attachment
  // (or in tests), in which case there cannot be copy requests rooted at this
  // source and the manager has no source-to-root mapping to traverse.
  if (display_) {
    frame_sink_manager_->DiscardPendingCopyOfOutputRequests(this);
  }
  // Prevent missing begin frames from being sent to sinks that came late,
  // as this may result in two overlapping frames being sent, which is not
  // supported with full pipeline mode.
  CancelPendingBeginFrame();
  last_begin_frame_args_ = BeginFrameArgs();
  pending_frame_sinks_.clear();
  pending_ack_.reset();
  pending_frame_args_.reset();
  original_source_id_.reset();
  auto callback = std::move(pending_frame_callback_);
  std::move(callback).Run(ack);
}

void ExternalBeginFrameSourceMojo::OnDisplayDidFinishFrame(
    const BeginFrameId& frame_id,
    DisplaySchedulerDrawResult result) {
  if (!pending_frame_callback_ || !pending_frame_args_ ||
      frame_id != pending_frame_args_->frame_id) {
    return;
  }

  if (result == DisplaySchedulerDrawResult::kDrawnLate ||
      result == DisplaySchedulerDrawResult::kMayDrawLate) {
    NOTREACHED();
  }

  bool has_damage = (result == DisplaySchedulerDrawResult::kDrawn);
  BeginFrameAck ack(frame_id.source_id, frame_id.sequence_number, has_damage);

  if (wait_for_all_frame_sinks_ && !pending_frame_sinks_.empty()) {
    CHECK(!pending_ack_);
    pending_ack_ = ack;
    return;
  }
  CompletePendingFrame(ack);
}

void ExternalBeginFrameSourceMojo::OnDisplayDestroyed() {
  AbortPendingFrame();
  // As part of destruction, we are automatically removed as a display
  // observer. No need to call RemoveObserver.
  display_ = nullptr;
}

void ExternalBeginFrameSourceMojo::AbortPendingFrame() {
  if (!pending_frame_callback_) {
    return;
  }
  CHECK(pending_frame_args_);
  BeginFrameAck nak(pending_frame_args_->frame_id.source_id,
                    pending_frame_args_->frame_id.sequence_number,
                    /*has_damage=*/false);
  CompletePendingFrame(nak);
}

void ExternalBeginFrameSourceMojo::SetDisplay(Display* display) {
  if (display_)
    display_->RemoveObserver(this);
  display_ = display;
  if (display_)
    display_->AddObserver(this);
}

void ExternalBeginFrameSourceMojo::OnNeedsBeginFrames(bool needs_begin_frames) {
  if (remote_client_) {
    remote_client_->SetNeedsBeginFrame(needs_begin_frames);
  }
}

void ExternalBeginFrameSourceMojo::SetPreferredInterval(
    base::TimeDelta interval) {
  if (remote_client_) {
    remote_client_->SetPreferredInterval(interval);
  }
}

}  // namespace viz
