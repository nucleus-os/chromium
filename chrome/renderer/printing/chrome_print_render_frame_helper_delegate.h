// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_RENDERER_PRINTING_CHROME_PRINT_RENDER_FRAME_HELPER_DELEGATE_H_
#define CHROME_RENDERER_PRINTING_CHROME_PRINT_RENDER_FRAME_HELPER_DELEGATE_H_

#include <optional>

#include "components/printing/renderer/print_render_frame_helper.h"

class ChromePrintRenderFrameHelperDelegate
    : public printing::PrintRenderFrameHelper::Delegate {
 public:
  explicit ChromePrintRenderFrameHelperDelegate(
      std::optional<bool> print_preview_enabled = std::nullopt);

  ChromePrintRenderFrameHelperDelegate(
      const ChromePrintRenderFrameHelperDelegate&) = delete;
  ChromePrintRenderFrameHelperDelegate& operator=(
      const ChromePrintRenderFrameHelperDelegate&) = delete;

  ~ChromePrintRenderFrameHelperDelegate() override;

  // Set the value for the next instance of this object that is created.
  static void SetNextPrintPreviewEnabled(std::optional<bool> enabled);

 private:
  // printing::PrintRenderFrameHelper::Delegate:
  blink::WebElement GetPdfElement(blink::WebLocalFrame* frame) override;
  bool IsPrintPreviewEnabled() override;
  bool OverridePrint(blink::WebLocalFrame* frame) override;
  bool ShouldGenerateTaggedPDF() override;

  const std::optional<bool> print_preview_enabled_;
};

#endif  // CHROME_RENDERER_PRINTING_CHROME_PRINT_RENDER_FRAME_HELPER_DELEGATE_H_
