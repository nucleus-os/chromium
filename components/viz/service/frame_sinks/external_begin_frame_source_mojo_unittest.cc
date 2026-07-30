// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/service/frame_sinks/external_begin_frame_source_mojo.h"

#include <memory>

#include "base/functional/callback.h"
#include "components/viz/service/frame_sinks/frame_sink_manager_impl.h"
#include "components/viz/test/begin_frame_args_test.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace viz {
namespace {

constexpr FrameSinkId kFrameSinkId(1, 1);

BeginFrameArgs CreateBeginFrameArgsWithSourceId(uint64_t source_id) {
  return CreateBeginFrameArgsForTesting(BEGINFRAME_FROM_HERE, source_id,
                                        /*sequence_number=*/1);
}

class ExternalBeginFrameSourceMojoTest : public testing::Test {
 public:
  ExternalBeginFrameSourceMojoTest() = default;
  ~ExternalBeginFrameSourceMojoTest() override = default;

  std::unique_ptr<ExternalBeginFrameSourceMojo> CreateSource() {
    mojo::AssociatedRemote<mojom::ExternalBeginFrameController> controller;
    return std::make_unique<ExternalBeginFrameSourceMojo>(
        &frame_sink_manager_,
        controller.BindNewEndpointAndPassDedicatedReceiver(),
        mojo::NullAssociatedRemote(), BeginFrameSource::kNotRestartableId);
  }

  void DidBeginFrame(const BeginFrameArgs& args) {
    frame_sink_manager_.DidBeginFrame(kFrameSinkId, args);
  }

  void DidFinishFrame(const BeginFrameArgs& args) {
    frame_sink_manager_.DidFinishFrame(kFrameSinkId, args);
  }

  FrameSinkManagerImpl& frame_sink_manager_for_testing() {
    return frame_sink_manager_;
  }

 private:
  FrameSinkManagerImpl frame_sink_manager_{
      FrameSinkManagerImpl::InitParams(/*output_surface_provider=*/nullptr)};
};

TEST_F(ExternalBeginFrameSourceMojoTest,
       UnactivatedSourceIgnoresStartingSourceIdBeginFrame) {
  auto source = CreateSource();
  const BeginFrameArgs args =
      CreateBeginFrameArgsWithSourceId(BeginFrameArgs::kStartingSourceId);

  DidBeginFrame(args);

  EXPECT_TRUE(source->pending_frame_sinks_for_testing().empty());
}

TEST_F(ExternalBeginFrameSourceMojoTest,
       ActivatedSourceTracksOnlyItsOriginalSourceId) {
  auto source = CreateSource();
  const BeginFrameArgs external_args =
      CreateBeginFrameArgsWithSourceId(/*source_id=*/123);
  source->IssueExternalBeginFrame(external_args, base::DoNothing());

  DidBeginFrame(
      CreateBeginFrameArgsWithSourceId(BeginFrameArgs::kStartingSourceId));
  EXPECT_TRUE(source->pending_frame_sinks_for_testing().empty());

  DidBeginFrame(external_args);
  EXPECT_TRUE(source->pending_frame_sinks_for_testing().contains(kFrameSinkId));
}

TEST_F(ExternalBeginFrameSourceMojoTest,
       DestructionCompletesPendingFrameWithoutDamage) {
  auto source = CreateSource();
  const BeginFrameArgs args = CreateBeginFrameArgsWithSourceId(123);
  bool completed = false;
  source->IssueExternalBeginFrame(
      args, /*force=*/false,
      base::BindOnce(
          [](bool* completed, const BeginFrameAck& ack) {
            EXPECT_FALSE(ack.has_damage);
            *completed = true;
          },
          &completed));

  source.reset();

  EXPECT_TRUE(completed);
}

TEST_F(ExternalBeginFrameSourceMojoTest,
       ExplicitAbortCompletesPendingFrameWithoutDamage) {
  mojo::AssociatedRemote<mojom::ExternalBeginFrameController> controller;
  auto source = std::make_unique<ExternalBeginFrameSourceMojo>(
      &frame_sink_manager_for_testing(),
      controller.BindNewEndpointAndPassDedicatedReceiver(),
      mojo::NullAssociatedRemote(), BeginFrameSource::kNotRestartableId);
  const BeginFrameArgs args = CreateBeginFrameArgsWithSourceId(124);
  bool completed = false;
  source->IssueExternalBeginFrame(
      args, /*force=*/false,
      base::BindOnce(
          [](bool* completed, const BeginFrameAck& ack) {
            EXPECT_FALSE(ack.has_damage);
            *completed = true;
          },
          &completed));

  controller->AbortPendingFrame();
  controller.FlushForTesting();

  EXPECT_TRUE(completed);
}

TEST_F(ExternalBeginFrameSourceMojoTest,
       GpuBusyAbortReturnsOriginalFrameIdAcrossMojo) {
  mojo::AssociatedRemote<mojom::ExternalBeginFrameController> controller;
  auto source = std::make_unique<ExternalBeginFrameSourceMojo>(
      &frame_sink_manager_for_testing(),
      controller.BindNewEndpointAndPassDedicatedReceiver(),
      mojo::NullAssociatedRemote(), BeginFrameSource::kNotRestartableId);
  source->SetIsGpuBusy(true);

  // GPU-busy throttling allows the first frame through and defers the next.
  const BeginFrameArgs first_args =
      CreateBeginFrameArgsWithSourceId(/*source_id=*/125);
  std::optional<BeginFrameAck> first_ack;
  controller->IssueExternalBeginFrame(
      first_args, /*force=*/false,
      base::BindOnce(
          [](std::optional<BeginFrameAck>* result, const BeginFrameAck& ack) {
            *result = ack;
          },
          &first_ack));
  controller.FlushForTesting();
  controller->AbortPendingFrame();
  controller.FlushForTesting();
  ASSERT_TRUE(first_ack);
  EXPECT_EQ(first_ack->frame_id, first_args.frame_id);

  const BeginFrameArgs deferred_args = CreateBeginFrameArgsForTesting(
      BEGINFRAME_FROM_HERE, /*source_id=*/125, /*sequence_number=*/2);
  std::optional<BeginFrameAck> deferred_ack;
  controller->IssueExternalBeginFrame(
      deferred_args, /*force=*/false,
      base::BindOnce(
          [](std::optional<BeginFrameAck>* result, const BeginFrameAck& ack) {
            *result = ack;
          },
          &deferred_ack));
  controller.FlushForTesting();
  controller->AbortPendingFrame();
  controller.FlushForTesting();

  ASSERT_TRUE(deferred_ack);
  EXPECT_EQ(deferred_ack->frame_id, deferred_args.frame_id);
  EXPECT_FALSE(deferred_ack->has_damage);

  // Releasing GPU throttling must not deliver the aborted deferred frame.
  source->SetIsGpuBusy(false);
}

}  // namespace
}  // namespace viz
