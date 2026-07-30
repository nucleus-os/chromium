// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/command_buffer/service/shared_image/shared_image_format_service_utils.h"

#include "components/viz/common/resources/shared_image_format.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/gpu/graphite/dawn/DawnGraphiteTypes.h"

namespace gpu {

TEST(GraphiteDawnMultiplanarTest, SamplesExternalNv12PerPlaneOnLinux) {
  auto format = viz::SharedImageFormat(viz::MultiPlaneFormat::kNV12);
  format.SetPrefersExternalSampler();

  EXPECT_FALSE(GraphiteDawnUsesExternalSampler(format));

  skgpu::graphite::DawnTextureInfo plane0 = DawnBackendTextureInfo(
      format, /*readonly=*/true, /*is_yuv_plane=*/true, /*plane_index=*/0,
      /*array_slice=*/0, /*mipmapped=*/false,
      /*scanout_dcomp_surface=*/false,
      /*supports_multiplanar_rendering=*/false,
      /*supports_multiplanar_copy=*/false);
  skgpu::graphite::DawnTextureInfo plane1 = DawnBackendTextureInfo(
      format, /*readonly=*/true, /*is_yuv_plane=*/true, /*plane_index=*/1,
      /*array_slice=*/0, /*mipmapped=*/false,
      /*scanout_dcomp_surface=*/false,
      /*supports_multiplanar_rendering=*/false,
      /*supports_multiplanar_copy=*/false);

  EXPECT_EQ(plane0.fFormat, wgpu::TextureFormat::R8BG8Biplanar420Unorm);
  EXPECT_EQ(plane0.fViewFormat, wgpu::TextureFormat::R8Unorm);
  EXPECT_EQ(plane0.fAspect, wgpu::TextureAspect::Plane0Only);
  EXPECT_EQ(plane1.fFormat, wgpu::TextureFormat::R8BG8Biplanar420Unorm);
  EXPECT_EQ(plane1.fViewFormat, wgpu::TextureFormat::RG8Unorm);
  EXPECT_EQ(plane1.fAspect, wgpu::TextureAspect::Plane1Only);
}

TEST(GraphiteDawnMultiplanarTest, SamplesExternalP010PerPlaneOnLinux) {
  auto format = viz::SharedImageFormat(viz::MultiPlaneFormat::kP010);
  format.SetPrefersExternalSampler();

  EXPECT_FALSE(GraphiteDawnUsesExternalSampler(format));

  skgpu::graphite::DawnTextureInfo plane0 = DawnBackendTextureInfo(
      format, /*readonly=*/true, /*is_yuv_plane=*/true, /*plane_index=*/0,
      /*array_slice=*/0, /*mipmapped=*/false,
      /*scanout_dcomp_surface=*/false,
      /*supports_multiplanar_rendering=*/false,
      /*supports_multiplanar_copy=*/false);
  skgpu::graphite::DawnTextureInfo plane1 = DawnBackendTextureInfo(
      format, /*readonly=*/true, /*is_yuv_plane=*/true, /*plane_index=*/1,
      /*array_slice=*/0, /*mipmapped=*/false,
      /*scanout_dcomp_surface=*/false,
      /*supports_multiplanar_rendering=*/false,
      /*supports_multiplanar_copy=*/false);

  EXPECT_EQ(plane0.fFormat,
            wgpu::TextureFormat::R10X6BG10X6Biplanar420Unorm);
  EXPECT_EQ(plane0.fViewFormat, wgpu::TextureFormat::R16Unorm);
  EXPECT_EQ(plane0.fAspect, wgpu::TextureAspect::Plane0Only);
  EXPECT_EQ(plane1.fFormat,
            wgpu::TextureFormat::R10X6BG10X6Biplanar420Unorm);
  EXPECT_EQ(plane1.fViewFormat, wgpu::TextureFormat::RG16Unorm);
  EXPECT_EQ(plane1.fAspect, wgpu::TextureAspect::Plane1Only);
}

}  // namespace gpu
