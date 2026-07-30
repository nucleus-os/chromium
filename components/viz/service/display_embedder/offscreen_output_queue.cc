// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/viz/service/display_embedder/offscreen_output_queue.h"

#include "base/check.h"

namespace viz {

OffscreenOutputQueue::OffscreenOutputQueue(size_t slot_count)
    : slots_(slot_count) {
  CHECK(slot_count > 0u);
}

OffscreenOutputQueue::~OffscreenOutputQueue() = default;

uint64_t OffscreenOutputQueue::Reshape() {
  CHECK(!abandoned_);
  ++generation_;
  for (Slot& slot : slots_) {
    switch (slot.state) {
      case State::kAvailable:
        slot.generation = generation_;
        slot.frame_token = 0;
        slot.content_serial = 0;
        break;
      case State::kPublished:
        slot.state = State::kRetired;
        break;
      case State::kRendering:
        slot.state = State::kAvailable;
        slot.generation = generation_;
        slot.frame_token = 0;
        slot.content_serial = 0;
        break;
      case State::kRetired:
        break;
      case State::kAbandoned:
        CHECK(false);
    }
  }
  return generation_;
}

std::optional<size_t> OffscreenOutputQueue::BeginFrame() {
  if (abandoned_) {
    return std::nullopt;
  }
  auto index = NextAvailableSlot();
  if (index) {
    slots_[*index].state = State::kRendering;
  }
  return index;
}

std::optional<uint64_t> OffscreenOutputQueue::Publish(size_t slot_index) {
  if (abandoned_ || slot_index >= slots_.size()) {
    return std::nullopt;
  }
  Slot& slot = slots_[slot_index];
  if (slot.state != State::kRendering ||
      slot.generation != generation_) {
    return std::nullopt;
  }
  const uint64_t content_serial = next_content_serial_++;
  slot.content_serial = content_serial;
  slot.state = State::kPublished;
  slot.frame_token = next_frame_token_++;
  return slot.frame_token;
}

std::optional<size_t> OffscreenOutputQueue::Release(uint64_t frame_token) {
  if (!frame_token) {
    return std::nullopt;
  }
  for (size_t i = 0; i < slots_.size(); ++i) {
    Slot& slot = slots_[i];
    if ((slot.state != State::kPublished &&
         slot.state != State::kRetired) ||
        slot.frame_token != frame_token) {
      continue;
    }
    const bool crossed_generation =
        slot.state == State::kRetired || slot.generation != generation_;
    slot.state = State::kAvailable;
    slot.generation = generation_;
    slot.frame_token = 0;
    if (crossed_generation) {
      slot.content_serial = 0;
    }
    return i;
  }
  return std::nullopt;
}

void OffscreenOutputQueue::CancelFrame(size_t slot_index) {
  if (abandoned_ || slot_index >= slots_.size()) {
    return;
  }
  Slot& slot = slots_[slot_index];
  if (slot.state == State::kRendering) {
    slot.state = State::kAvailable;
    slot.frame_token = 0;
  }
}

std::optional<size_t> OffscreenOutputQueue::NextAvailableSlot() const {
  if (abandoned_) {
    return std::nullopt;
  }
  for (size_t i = 0; i < slots_.size(); ++i) {
    const Slot& slot = slots_[i];
    if (slot.state == State::kAvailable &&
        slot.generation == generation_) {
      return i;
    }
  }
  return std::nullopt;
}

void OffscreenOutputQueue::Abandon() {
  abandoned_ = true;
  for (Slot& slot : slots_) {
    slot.state = State::kAbandoned;
    slot.frame_token = 0;
  }
}

}  // namespace viz
