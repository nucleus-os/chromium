// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_INPUT_FLING_SCHEDULER_BASE_H_
#define COMPONENTS_INPUT_FLING_SCHEDULER_BASE_H_

#include "base/memory/raw_ptr.h"
#include "components/input/fling_controller.h"
#include "components/viz/common/frame_sinks/begin_frame_source.h"

namespace ui {
class Compositor;
}

namespace input {

class FlingSchedulerBase : public FlingControllerSchedulerClient {
 public:
  virtual void ProgressFlingOnBeginFrameIfneeded(
      base::TimeTicks current_time) = 0;
  // Sets |begin_frame_source| to drive fling events generation.
  // TODO(410801272): This is only used by FlingSchedulerAndroid implementation
  // in Viz currently, but in future we would want to migrate browser's
  // implementations to use this to progress flings.
  virtual void SetBeginFrameSource(viz::BeginFrameSource* begin_frame_source) {}
  
  void SetCompositor(ui::Compositor* compositor) {
    compositor_ = compositor;
  }

 protected:
  raw_ptr<ui::Compositor> compositor_ = nullptr;
};

}  // namespace input

#endif  // COMPONENTS_INPUT_FLING_SCHEDULER_BASE_H_
