// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/service/display_embedder/offscreen_output_queue.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace viz {
namespace {

TEST(OffscreenOutputQueueTest, AppliesBackpressureAndReusesReleasedSlot) {
  OffscreenOutputQueue queue(4);
  queue.Reshape();
  uint64_t tokens[4];
  for (size_t i = 0; i < 4; ++i) {
    auto slot = queue.BeginFrame();
    ASSERT_TRUE(slot);
    auto token = queue.Publish(*slot);
    ASSERT_TRUE(token);
    tokens[i] = *token;
  }
  EXPECT_FALSE(queue.BeginFrame());
  auto released = queue.Release(tokens[1]);
  ASSERT_TRUE(released);
  EXPECT_EQ(*released, 1u);
  EXPECT_EQ(queue.BeginFrame(), 1u);
}

TEST(OffscreenOutputQueueTest, RejectsDuplicateAndUnknownRelease) {
  OffscreenOutputQueue queue(2);
  queue.Reshape();
  auto slot = queue.BeginFrame();
  ASSERT_TRUE(slot);
  auto token = queue.Publish(*slot);
  ASSERT_TRUE(token);
  EXPECT_TRUE(queue.Release(*token));
  EXPECT_FALSE(queue.Release(*token));
  EXPECT_FALSE(queue.Release(*token + 100));
}

TEST(OffscreenOutputQueueTest, RetainsPublishedSlotsAcrossReshape) {
  OffscreenOutputQueue queue(2);
  const uint64_t first_generation = queue.Reshape();
  auto old_slot = queue.BeginFrame();
  ASSERT_TRUE(old_slot);
  auto old_token = queue.Publish(*old_slot);
  ASSERT_TRUE(old_token);

  const uint64_t second_generation = queue.Reshape();
  EXPECT_GT(second_generation, first_generation);
  EXPECT_EQ(queue.slot(*old_slot).state,
            OffscreenOutputQueue::State::kRetired);

  auto new_slot = queue.BeginFrame();
  ASSERT_TRUE(new_slot);
  EXPECT_NE(*new_slot, *old_slot);
  EXPECT_EQ(queue.slot(*new_slot).generation, second_generation);

  EXPECT_EQ(queue.Release(*old_token), *old_slot);
  EXPECT_EQ(queue.slot(*old_slot).generation, second_generation);
}

TEST(OffscreenOutputQueueTest, AbandonInvalidatesAllOperations) {
  OffscreenOutputQueue queue(2);
  queue.Reshape();
  auto slot = queue.BeginFrame();
  ASSERT_TRUE(slot);
  queue.Abandon();
  EXPECT_FALSE(queue.Publish(*slot));
  EXPECT_FALSE(queue.BeginFrame());
  for (size_t i = 0; i < queue.slot_count(); ++i) {
    EXPECT_EQ(queue.slot(i).state, OffscreenOutputQueue::State::kAbandoned);
  }
}

}  // namespace
}  // namespace viz
