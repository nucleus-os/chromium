// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_VIZ_SERVICE_DISPLAY_EMBEDDER_OFFSCREEN_OUTPUT_CONNECTION_H_
#define COMPONENTS_VIZ_SERVICE_DISPLAY_EMBEDDER_OFFSCREEN_OUTPUT_CONNECTION_H_

#include <utility>

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/viz/privileged/mojom/compositing/offscreen_output.mojom.h"

namespace viz {

// Paired endpoints for one explicitly requested exportable offscreen root.
// Ownership follows the root OutputSurface and is transferred to the concrete
// output device when that device is initialized.
struct OffscreenOutputConnection {
  OffscreenOutputConnection(
      mojo::PendingRemote<mojom::OffscreenOutputClient> client,
      mojo::PendingReceiver<mojom::OffscreenOutput> output)
      : client(std::move(client)), output(std::move(output)) {}

  OffscreenOutputConnection(const OffscreenOutputConnection&) = delete;
  OffscreenOutputConnection& operator=(const OffscreenOutputConnection&) =
      delete;
  OffscreenOutputConnection(OffscreenOutputConnection&&) = default;
  OffscreenOutputConnection& operator=(OffscreenOutputConnection&&) = default;
  ~OffscreenOutputConnection() = default;

  bool is_valid() const { return client.is_valid() && output.is_valid(); }

  mojo::PendingRemote<mojom::OffscreenOutputClient> client;
  mojo::PendingReceiver<mojom::OffscreenOutput> output;
};

}  // namespace viz

#endif  // COMPONENTS_VIZ_SERVICE_DISPLAY_EMBEDDER_OFFSCREEN_OUTPUT_CONNECTION_H_
