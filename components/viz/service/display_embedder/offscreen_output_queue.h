// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_VIZ_SERVICE_DISPLAY_EMBEDDER_OFFSCREEN_OUTPUT_QUEUE_H_
#define COMPONENTS_VIZ_SERVICE_DISPLAY_EMBEDDER_OFFSCREEN_OUTPUT_QUEUE_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "components/viz/service/viz_service_export.h"

namespace viz {

// Tracks the externally-owned lifetime of a fixed-size offscreen output pool.
// GPU resources remain the responsibility of SkiaOutputDeviceOffscreenExport.
class VIZ_SERVICE_EXPORT OffscreenOutputQueue {
 public:
  enum class State {
    kAvailable,
    kRendering,
    kPublished,
    kRetired,
    kAbandoned,
  };

  struct Slot {
    State state = State::kAvailable;
    uint64_t generation = 0;
    uint64_t frame_token = 0;
    uint64_t content_serial = 0;
  };

  explicit OffscreenOutputQueue(size_t slot_count);
  ~OffscreenOutputQueue();

  OffscreenOutputQueue(const OffscreenOutputQueue&) = delete;
  OffscreenOutputQueue& operator=(const OffscreenOutputQueue&) = delete;

  // Starts a new buffer generation. Published slots remain alive as retired
  // slots until their matching release arrives.
  uint64_t Reshape();
  std::optional<size_t> BeginFrame();
  std::optional<uint64_t> Publish(size_t slot_index);
  std::optional<size_t> Release(uint64_t frame_token);
  void CancelFrame(size_t slot_index);
  void Abandon();
  std::optional<size_t> NextAvailableSlot() const;

  const Slot& slot(size_t index) const { return slots_[index]; }
  size_t slot_count() const { return slots_.size(); }
  uint64_t generation() const { return generation_; }

 private:
  std::vector<Slot> slots_;
  uint64_t generation_ = 0;
  uint64_t next_frame_token_ = 1;
  uint64_t next_content_serial_ = 1;
  bool abandoned_ = false;
};

}  // namespace viz

#endif  // COMPONENTS_VIZ_SERVICE_DISPLAY_EMBEDDER_OFFSCREEN_OUTPUT_QUEUE_H_
