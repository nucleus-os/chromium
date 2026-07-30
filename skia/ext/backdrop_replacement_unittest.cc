// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkImageFilter.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkPicture.h"
#include "third_party/skia/include/core/SkPictureRecorder.h"
#include "third_party/skia/include/core/SkRect.h"
#include "third_party/skia/include/effects/SkImageFilters.h"

namespace skia {
namespace {

TEST(BackdropReplacement, IsBoundedAndRecordable) {
  constexpr int kWidth = 20;
  constexpr int kHeight = 10;
  const SkPath replacement = SkPath::Rect(SkRect::MakeWH(kWidth / 2, kHeight));
  auto identity = SkImageFilters::Offset(0, 0, nullptr);

  auto draw = [&](SkCanvas* canvas) {
    canvas->clear(SK_ColorTRANSPARENT);

    SkPaint red;
    red.setColor(SK_ColorRED);
    canvas->drawRect(SkRect::MakeWH(kWidth, kHeight), red);

    SkPaint restore;
    restore.setAlphaf(0.5f);
    SkCanvas::SaveLayerRec rec(nullptr, &restore, identity.get(), 0);
    rec.fBackdropReplacement = &replacement;
    canvas->saveLayer(rec);
    canvas->restore();
  };

  auto verify = [](const SkBitmap& bitmap) {
    const SkColor replaced = bitmap.getColor(kWidth / 4, kHeight / 2);
    const SkColor untouched = bitmap.getColor(3 * kWidth / 4, kHeight / 2);
    const int replaced_alpha = SkColorGetA(replaced);
    EXPECT_GE(replaced_alpha, 127);
    EXPECT_LE(replaced_alpha, 128);
    EXPECT_EQ(static_cast<int>(SkColorGetR(replaced)), 255);
    EXPECT_EQ(static_cast<int>(SkColorGetA(untouched)), 255);
    EXPECT_EQ(static_cast<int>(SkColorGetR(untouched)), 255);
  };

  SkBitmap direct;
  direct.allocN32Pixels(kWidth, kHeight);
  SkCanvas direct_canvas(direct);
  draw(&direct_canvas);
  verify(direct);

  SkPictureRecorder recorder;
  draw(recorder.beginRecording(SkRect::MakeWH(kWidth, kHeight)));
  sk_sp<SkPicture> picture = recorder.finishRecordingAsPicture();

  SkBitmap recorded;
  recorded.allocN32Pixels(kWidth, kHeight);
  SkCanvas recorded_canvas(recorded);
  recorded_canvas.drawPicture(picture);
  verify(recorded);

  sk_sp<SkData> serialized = picture->serialize();
  ASSERT_TRUE(serialized);
  sk_sp<SkPicture> deserialized =
      SkPicture::MakeFromData(serialized->data(), serialized->size());
  ASSERT_TRUE(deserialized);

  SkBitmap round_tripped;
  round_tripped.allocN32Pixels(kWidth, kHeight);
  SkCanvas round_tripped_canvas(round_tripped);
  round_tripped_canvas.drawPicture(deserialized);
  verify(round_tripped);
}

}  // namespace
}  // namespace skia
