// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/command_buffer/service/shared_image/ozone_image_backing_factory.h"

#include "base/files/file_util.h"
#include "cc/test/pixel_comparator.h"
#include "cc/test/pixel_test_utils.h"
#include "components/viz/common/resources/shared_image_format.h"
#include "components/viz/common/resources/shared_image_format_utils.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/command_buffer/service/shared_image/gl_ozone_image_representation.h"
#include "gpu/command_buffer/service/shared_image/ozone_image_backing.h"
#include "gpu/command_buffer/service/shared_image/ozone_image_gl_textures_holder.h"
#include "gpu/command_buffer/service/shared_image/shared_image_manager.h"
#include "gpu/command_buffer/service/shared_image/shared_image_test_base.h"
#include "gpu/config/gpu_finch_features.h"
#include "gpu/vulkan/buildflags.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gl/gl_context.h"
#include "ui/gl/gl_surface_egl.h"
#include "ui/gl/gl_utils.h"
#include "ui/gl/init/gl_factory.h"
#include "ui/ozone/public/ozone_platform.h"

namespace gpu {

namespace {

class FakeOnScreenSurface : public gl::SurfacelessEGL {
 public:
  FakeOnScreenSurface(gl::GLDisplayEGL* display, const gfx::Size& size)
      : gl::SurfacelessEGL(display, size) {}

  // gl::GLSurface:
  bool IsOffscreen() override { return false; }
  bool IsSurfaceless() const override { return false; }

 protected:
  ~FakeOnScreenSurface() override { InvalidateWeakPtrs(); }
};

#if BUILDFLAG(ENABLE_VULKAN)
gfx::GpuFenceHandle CreateFenceHandleForStateTest() {
  base::ScopedFD read_fd;
  base::ScopedFD write_fd;
  CHECK(base::CreatePipe(&read_fd, &write_fd,
                         /*non_blocking=*/false));
  gfx::GpuFenceHandle fence;
  fence.Adopt(std::move(read_fd));
  return fence;
}
#endif

}  // namespace

class OzoneImageBackingFactoryTest : public SharedImageTestBase {
 public:
  OzoneImageBackingFactoryTest() = default;
  ~OzoneImageBackingFactoryTest() override = default;

  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(InitializeContext(GrContextType::kGL));

    backing_factory_ = std::make_unique<OzoneImageBackingFactory>(
        context_state_.get(), gpu_workarounds_);

    shared_image_representation_factory_ =
        std::make_unique<SharedImageRepresentationFactory>(
            &shared_image_manager_, nullptr);
  }

 protected:
  bool IsEglImageSupported() const {
    bool result =
        context_state_->MakeCurrent(gl_surface_.get(), /*needs_gl=*/true);
    DCHECK(result);

    // Check the required extensions to support egl images.
    auto* egl_display = gl::GetDefaultDisplayEGL();
    if (egl_display && egl_display->ext->b_EGL_KHR_image_base &&
        egl_display->ext->b_EGL_KHR_gl_texture_2D_image &&
        gl::g_current_gl_driver->ext.b_GL_OES_EGL_image) {
      return true;
    }
    return false;
  }

  MemoryTypeTracker memory_type_tracker_{nullptr};
  SharedImageManager shared_image_manager_;

  std::unique_ptr<SharedImageRepresentationFactory>
      shared_image_representation_factory_;
  std::unique_ptr<OzoneImageBackingFactory> backing_factory_;
};

#if BUILDFLAG(ENABLE_VULKAN)
TEST_F(OzoneImageBackingFactoryTest, ScopedVulkanStateRoundTrip) {
  const Mailbox mailbox = Mailbox::Generate();
  auto backing =
      backing_factory_->CreateSharedImage(mailbox,
                                          {viz::SinglePlaneFormat::kRGBA_8888,
                                           {100, 100},
                                           gfx::ColorSpace::CreateSRGB(),
                                           kTopLeft_GrSurfaceOrigin,
                                           kPremul_SkAlphaType,
                                           SHARED_IMAGE_USAGE_WEBGPU_WRITE,
                                           "ExternalVulkanImageStateTest"},
                                          gpu::kNullSurfaceHandle, false);
  ASSERT_TRUE(backing);

  auto* ozone_backing = static_cast<OzoneImageBacking*>(backing.get());
  auto first_write = ozone_backing->BeginAccess(
      /*readonly=*/false, OzoneImageBacking::AccessStream::kWebGPU);
  ASSERT_TRUE(first_write);
  std::optional<ExternalVulkanImageState> initial_state;
  EXPECT_TRUE(first_write->TakeExternalVulkanImageState(&initial_state));
  EXPECT_FALSE(initial_state);
  first_write->AbortBeforeAcquire();

  const ExternalVulkanImageState producer_state{
      .old_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .new_layout = VK_IMAGE_LAYOUT_GENERAL,
      .external_queue_family = VK_QUEUE_FAMILY_EXTERNAL_KHR,
  };
  ozone_backing->external_vulkan_state_ = producer_state;
  EXPECT_EQ(ozone_backing->external_vulkan_state_, producer_state);

  const ExternalVulkanImageState consumer_state{
      .old_layout = VK_IMAGE_LAYOUT_GENERAL,
      .new_layout = VK_IMAGE_LAYOUT_GENERAL,
      .external_queue_family = VK_QUEUE_FAMILY_EXTERNAL_KHR,
  };
  auto first_read = ozone_backing->BeginAccess(
      /*readonly=*/true, OzoneImageBacking::AccessStream::kWebGPU);
  ASSERT_TRUE(first_read);
  EXPECT_TRUE(first_read->TakeBeginFences().empty());
  std::optional<ExternalVulkanImageState> acquired_state;
  EXPECT_TRUE(first_read->TakeExternalVulkanImageState(&acquired_state));
  EXPECT_EQ(acquired_state, producer_state);
  EXPECT_TRUE(first_read->needs_end_fence());
  first_read->CommitAcquire();
  EXPECT_TRUE(first_read->EndVulkan(CreateFenceHandleForStateTest(),
                                   consumer_state));
  EXPECT_EQ(ozone_backing->external_vulkan_state_, consumer_state);
  EXPECT_EQ(ozone_backing->active_accesses_, 0u);
  EXPECT_FALSE(ozone_backing->vulkan_ownership_in_progress_);
}

TEST_F(OzoneImageBackingFactoryTest,
       ScopedVulkanUnusedAccessRestoresState) {
  const Mailbox mailbox = Mailbox::Generate();
  auto backing = backing_factory_->CreateSharedImage(
      mailbox,
      {viz::SinglePlaneFormat::kRGBA_8888,
       {100, 100},
       gfx::ColorSpace::CreateSRGB(),
       kTopLeft_GrSurfaceOrigin,
       kPremul_SkAlphaType,
       SHARED_IMAGE_USAGE_DISPLAY_READ | SHARED_IMAGE_USAGE_SCANOUT |
           SHARED_IMAGE_USAGE_WEBGPU_READ,
       "ExternalVulkanUnusedAccessTest"},
      gpu::kNullSurfaceHandle, false);
  ASSERT_TRUE(backing);

  auto* ozone_backing = static_cast<OzoneImageBacking*>(backing.get());
  const ExternalVulkanImageState state{
      .old_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .new_layout = VK_IMAGE_LAYOUT_GENERAL,
      .external_queue_family = VK_QUEUE_FAMILY_EXTERNAL_KHR,
  };
  ozone_backing->external_vulkan_state_ = state;

  auto access = ozone_backing->BeginAccess(
      /*readonly=*/true, OzoneImageBacking::AccessStream::kWebGPU);
  ASSERT_TRUE(access);
  EXPECT_TRUE(access->TakeBeginFences().empty());
  std::optional<ExternalVulkanImageState> acquired_state;
  EXPECT_TRUE(access->TakeExternalVulkanImageState(&acquired_state));
  EXPECT_EQ(acquired_state, state);
  access->CommitAcquire();

  EXPECT_TRUE(access->EndVulkanWithoutGpuUse(gfx::GpuFenceHandle()));
  EXPECT_EQ(ozone_backing->external_vulkan_state_, state);
  EXPECT_EQ(ozone_backing->active_accesses_, 0u);
  EXPECT_FALSE(ozone_backing->vulkan_ownership_in_progress_);
  EXPECT_FALSE(context_state_->context_lost());
}

TEST_F(OzoneImageBackingFactoryTest, ScopedAccessAbortRestoresState) {
  const Mailbox mailbox = Mailbox::Generate();
  auto backing = backing_factory_->CreateSharedImage(
      mailbox,
      {viz::SinglePlaneFormat::kRGBA_8888,
       {100, 100},
       gfx::ColorSpace::CreateSRGB(),
       kTopLeft_GrSurfaceOrigin,
       kPremul_SkAlphaType,
       SHARED_IMAGE_USAGE_DISPLAY_WRITE | SHARED_IMAGE_USAGE_SCANOUT |
           SHARED_IMAGE_USAGE_WEBGPU_WRITE,
       "ExternalVulkanAbortTest"},
      gpu::kNullSurfaceHandle, false);
  ASSERT_TRUE(backing);

  auto* ozone_backing = static_cast<OzoneImageBacking*>(backing.get());
  const ExternalVulkanImageState state{
      .old_layout = VK_IMAGE_LAYOUT_GENERAL,
      .new_layout = VK_IMAGE_LAYOUT_GENERAL,
      .external_queue_family = VK_QUEUE_FAMILY_EXTERNAL_KHR,
  };
  ozone_backing->external_vulkan_state_ = state;

  auto access = ozone_backing->BeginAccess(
      /*readonly=*/false, OzoneImageBacking::AccessStream::kWebGPU);
  ASSERT_TRUE(access);
  std::optional<ExternalVulkanImageState> acquired_state;
  EXPECT_TRUE(access->TakeExternalVulkanImageState(&acquired_state));
  EXPECT_EQ(acquired_state, state);
  EXPECT_FALSE(ozone_backing->external_vulkan_state_);

  access->AbortBeforeAcquire();
  EXPECT_EQ(ozone_backing->external_vulkan_state_, state);
  EXPECT_EQ(ozone_backing->active_accesses_, 0u);
  EXPECT_FALSE(ozone_backing->is_write_in_progress_);
  EXPECT_FALSE(ozone_backing->vulkan_ownership_in_progress_);
}

TEST_F(OzoneImageBackingFactoryTest, ImplicitAccessPreservesVulkanState) {
  const Mailbox mailbox = Mailbox::Generate();
  auto backing = backing_factory_->CreateSharedImage(
      mailbox,
      {viz::SinglePlaneFormat::kRGBA_8888,
       {100, 100},
       gfx::ColorSpace::CreateSRGB(),
       kTopLeft_GrSurfaceOrigin,
       kPremul_SkAlphaType,
       SHARED_IMAGE_USAGE_DISPLAY_READ | SHARED_IMAGE_USAGE_SCANOUT,
       "ImplicitVulkanStateTest"},
      gpu::kNullSurfaceHandle, false);
  ASSERT_TRUE(backing);

  auto* ozone_backing = static_cast<OzoneImageBacking*>(backing.get());
  const ExternalVulkanImageState state{
      .old_layout = VK_IMAGE_LAYOUT_GENERAL,
      .new_layout = VK_IMAGE_LAYOUT_GENERAL,
      .external_queue_family = VK_QUEUE_FAMILY_EXTERNAL_KHR,
  };
  ozone_backing->external_vulkan_state_ = state;

  auto access = ozone_backing->BeginAccess(
      /*readonly=*/true, OzoneImageBacking::AccessStream::kOverlay);
  ASSERT_TRUE(access);
  EXPECT_TRUE(access->TakeBeginFences().empty());
  access->CommitAcquire();
  access->End(gfx::GpuFenceHandle());

  EXPECT_EQ(ozone_backing->external_vulkan_state_, state);
  EXPECT_EQ(ozone_backing->active_accesses_, 0u);
  EXPECT_FALSE(ozone_backing->vulkan_ownership_in_progress_);
}

TEST_F(OzoneImageBackingFactoryTest, PostAcquireFailureInvalidatesBacking) {
  const Mailbox mailbox = Mailbox::Generate();
  auto backing = backing_factory_->CreateSharedImage(
      mailbox,
      {viz::SinglePlaneFormat::kRGBA_8888,
       {100, 100},
       gfx::ColorSpace::CreateSRGB(),
       kTopLeft_GrSurfaceOrigin,
       kPremul_SkAlphaType,
       SHARED_IMAGE_USAGE_DISPLAY_WRITE | SHARED_IMAGE_USAGE_SCANOUT |
           SHARED_IMAGE_USAGE_WEBGPU_WRITE,
       "ExternalVulkanFailureTest"},
      gpu::kNullSurfaceHandle, false);
  ASSERT_TRUE(backing);

  auto* ozone_backing = static_cast<OzoneImageBacking*>(backing.get());
  auto access = ozone_backing->BeginAccess(
      /*readonly=*/false, OzoneImageBacking::AccessStream::kWebGPU);
  ASSERT_TRUE(access);
  EXPECT_TRUE(access->TakeBeginFences().empty());
  std::optional<ExternalVulkanImageState> state;
  EXPECT_TRUE(access->TakeExternalVulkanImageState(&state));
  access->CommitAcquire();
  access->InvalidateAfterAcquire();

  EXPECT_TRUE(context_state_->context_lost());
  EXPECT_EQ(ozone_backing->active_accesses_, 0u);
  EXPECT_FALSE(ozone_backing->vulkan_ownership_in_progress_);
  EXPECT_FALSE(ozone_backing->external_vulkan_state_);
}

TEST_F(OzoneImageBackingFactoryTest,
       BackingDestructionAbandonsExternalVulkanOwnership) {
  const Mailbox mailbox = Mailbox::Generate();
  auto backing = backing_factory_->CreateSharedImage(
      mailbox,
      {viz::SinglePlaneFormat::kRGBA_8888,
       {100, 100},
       gfx::ColorSpace::CreateSRGB(),
       kTopLeft_GrSurfaceOrigin,
       kPremul_SkAlphaType,
       SHARED_IMAGE_USAGE_DISPLAY_WRITE | SHARED_IMAGE_USAGE_SCANOUT |
           SHARED_IMAGE_USAGE_WEBGPU_WRITE,
       "ExternalVulkanBackingDestructionTest"},
      gpu::kNullSurfaceHandle, false);
  ASSERT_TRUE(backing);

  auto* ozone_backing = static_cast<OzoneImageBacking*>(backing.get());
  auto access = ozone_backing->BeginAccess(
      /*readonly=*/false, OzoneImageBacking::AccessStream::kWebGPU);
  ASSERT_TRUE(access);
  EXPECT_TRUE(access->TakeBeginFences().empty());
  std::optional<ExternalVulkanImageState> state;
  EXPECT_TRUE(access->TakeExternalVulkanImageState(&state));
  access->CommitAcquire();
  access->AbandonForBackingDestruction();

  EXPECT_FALSE(context_state_->context_lost());
  EXPECT_EQ(ozone_backing->active_accesses_, 0u);
  EXPECT_FALSE(ozone_backing->vulkan_ownership_in_progress_);
  EXPECT_FALSE(ozone_backing->external_vulkan_state_);
}

TEST_F(OzoneImageBackingFactoryTest, RejectsConcurrentVulkanOwner) {
  const Mailbox mailbox = Mailbox::Generate();
  auto backing = backing_factory_->CreateSharedImage(
      mailbox,
      {viz::SinglePlaneFormat::kRGBA_8888,
       {100, 100},
       gfx::ColorSpace::CreateSRGB(),
       kTopLeft_GrSurfaceOrigin,
       kPremul_SkAlphaType,
       SHARED_IMAGE_USAGE_DISPLAY_READ | SHARED_IMAGE_USAGE_SCANOUT |
           SHARED_IMAGE_USAGE_WEBGPU_READ,
       "ExternalVulkanConcurrentReadTest"},
      gpu::kNullSurfaceHandle, false);
  ASSERT_TRUE(backing);

  auto* ozone_backing = static_cast<OzoneImageBacking*>(backing.get());
  ozone_backing->external_vulkan_state_ = ExternalVulkanImageState{
      .old_layout = VK_IMAGE_LAYOUT_GENERAL,
      .new_layout = VK_IMAGE_LAYOUT_GENERAL,
      .external_queue_family = VK_QUEUE_FAMILY_EXTERNAL_KHR,
  };

  auto native_read = ozone_backing->BeginAccess(
      /*readonly=*/true, OzoneImageBacking::AccessStream::kOverlay);
  auto vulkan_read = ozone_backing->BeginAccess(
      /*readonly=*/true, OzoneImageBacking::AccessStream::kWebGPU);
  ASSERT_TRUE(native_read);
  ASSERT_TRUE(vulkan_read);

  std::optional<ExternalVulkanImageState> state;
  EXPECT_FALSE(vulkan_read->TakeExternalVulkanImageState(&state));
  EXPECT_FALSE(state);
  vulkan_read->AbortBeforeAcquire();
  native_read->AbortBeforeAcquire();
  EXPECT_EQ(ozone_backing->active_accesses_, 0u);
  EXPECT_TRUE(ozone_backing->external_vulkan_state_);

  auto vulkan_owner = ozone_backing->BeginAccess(
      /*readonly=*/true, OzoneImageBacking::AccessStream::kWebGPU);
  ASSERT_TRUE(vulkan_owner);
  EXPECT_TRUE(vulkan_owner->TakeExternalVulkanImageState(&state));
  EXPECT_TRUE(state);
  EXPECT_FALSE(ozone_backing->external_vulkan_state_);
  EXPECT_FALSE(ozone_backing->BeginAccess(
      /*readonly=*/true, OzoneImageBacking::AccessStream::kOverlay));
  vulkan_owner->AbortBeforeAcquire();
  EXPECT_EQ(ozone_backing->active_accesses_, 0u);
  EXPECT_TRUE(ozone_backing->external_vulkan_state_);
}
#endif  // BUILDFLAG(ENABLE_VULKAN)

TEST_F(OzoneImageBackingFactoryTest, UsesCacheForTextureHolders) {
  if (!IsEglImageSupported()) {
    GTEST_SKIP();
  }

  EXPECT_TRUE(context_state_->MakeCurrent(context_state_->surface(),
                                          true /* needs_gl*/));

  const Mailbox mailbox = Mailbox::Generate();
  auto backing =
      backing_factory_->CreateSharedImage(mailbox,
                                          {viz::SinglePlaneFormat::kRGBA_8888,
                                           {100, 100},
                                           gfx::ColorSpace::CreateSRGB(),
                                           kTopLeft_GrSurfaceOrigin,
                                           kPremul_SkAlphaType,
                                           SHARED_IMAGE_USAGE_GLES2_READ,
                                           "TestLabel"},
                                          gpu::kNullSurfaceHandle, false);
  EXPECT_TRUE(backing);

  auto* backing_ptr = static_cast<OzoneImageBacking*>(backing.get());

  auto shared_image =
      shared_image_manager_.Register(std::move(backing), &memory_type_tracker_);
  EXPECT_TRUE(shared_image);

  // Create and validate GLTexture representation.
  auto gl_representation =
      shared_image_representation_factory_->ProduceGLTexturePassthrough(
          mailbox);
  EXPECT_TRUE(gl_representation);
  EXPECT_TRUE(gl_representation->GetTexturePassthrough()->service_id());

  // Verify there is only one per-context textures holder now.
  EXPECT_EQ(1u, backing_ptr->per_context_cached_textures_holders_.size());
  // Verify the context key is the current context.
  auto& cached_textures_holdes =
      *backing_ptr->per_context_cached_textures_holders_.begin();
  EXPECT_EQ(context_state_->context(), cached_textures_holdes.first);

  auto gl_representation2 =
      shared_image_representation_factory_->ProduceGLTexturePassthrough(
          mailbox);
  EXPECT_TRUE(gl_representation2);
  EXPECT_TRUE(gl_representation2->GetTexturePassthrough()->service_id());

  // Verify there is still only one per-context textures holder now.
  EXPECT_EQ(1u, backing_ptr->per_context_cached_textures_holders_.size());

  // Both representations must have the same texture id as OzoneImageBacking
  // holds a cache for GLTexture(Passthrough)OzoneImageRepresentations'
  // TextureHolder (though, if the context is different, the service_id will
  // repeat).
  EXPECT_EQ(gl_representation->GetTexturePassthrough()->service_id(),
            gl_representation2->GetTexturePassthrough()->service_id());

  auto gl_context = gl::init::CreateGLContext(nullptr, gl_surface_.get(),
                                              gl::GLContextAttribs());
  ASSERT_TRUE(gl_context);
  bool make_current_result = gl_context->MakeCurrent(gl_surface_.get());
  ASSERT_TRUE(make_current_result);

  auto gl_representation3 =
      shared_image_representation_factory_->ProduceGLTexturePassthrough(
          mailbox);
  EXPECT_TRUE(gl_representation3);

  // Verify there is now two per-context textures holders.
  EXPECT_EQ(2u, backing_ptr->per_context_cached_textures_holders_.size());

  auto gl_representation4 =
      shared_image_representation_factory_->ProduceGLTexturePassthrough(
          mailbox);
  EXPECT_TRUE(gl_representation4);
  EXPECT_TRUE(gl_representation4->GetTexturePassthrough()->service_id());

  // Both representations must share the same texture id.
  EXPECT_EQ(gl_representation3->GetTexturePassthrough()->service_id(),
            gl_representation4->GetTexturePassthrough()->service_id());

  // Cannot compare service_ids of the |gl_representation3/4| with
  // |gl_representation1/2| as they will be the same as the first ones because
  // these representations' texture was created for a different context.
}

// Verifies that the cache is not used for onscreen surfaces.
TEST_F(OzoneImageBackingFactoryTest, UsesCacheForTextureHolders2) {
  if (!IsEglImageSupported()) {
    GTEST_SKIP();
  }

  EXPECT_TRUE(context_state_->MakeCurrent(context_state_->surface(),
                                          true /* needs_gl*/));

  const Mailbox mailbox = Mailbox::Generate();
  auto backing =
      backing_factory_->CreateSharedImage(mailbox,
                                          {viz::SinglePlaneFormat::kRGBA_8888,
                                           {100, 100},
                                           gfx::ColorSpace::CreateSRGB(),
                                           kTopLeft_GrSurfaceOrigin,
                                           kPremul_SkAlphaType,
                                           SHARED_IMAGE_USAGE_GLES2_READ,
                                           "TestLabel"},
                                          gpu::kNullSurfaceHandle, false);
  EXPECT_TRUE(backing);

  auto* backing_ptr = static_cast<OzoneImageBacking*>(backing.get());

  auto shared_image =
      shared_image_manager_.Register(std::move(backing), &memory_type_tracker_);
  EXPECT_TRUE(shared_image);

  // Create and validate GLTexture representation.
  auto gl_representation =
      shared_image_representation_factory_->ProduceGLTexturePassthrough(
          mailbox);
  EXPECT_TRUE(gl_representation);
  EXPECT_TRUE(gl_representation->GetTexturePassthrough()->service_id());

  // Verify there is only one per-context textures holder now.
  EXPECT_EQ(1u, backing_ptr->per_context_cached_textures_holders_.size());
  // Verify the context key is the current context.
  auto& cached_textures_holdes =
      *backing_ptr->per_context_cached_textures_holders_.begin();
  EXPECT_EQ(context_state_->context(), cached_textures_holdes.first);

  scoped_refptr<gl::GLSurface> fake_onscreen_gl_surface(
      new FakeOnScreenSurface(gl::GetDefaultDisplayEGL(), {100, 100}));
  ASSERT_FALSE(fake_onscreen_gl_surface->IsOffscreen());

  scoped_refptr<gl::GLContext> gl_context = gl::init::CreateGLContext(
      nullptr, fake_onscreen_gl_surface.get(), gl::GLContextAttribs());
  ASSERT_TRUE(gl_context);
  ASSERT_FALSE(gl_context->default_surface());
  bool make_current_result =
      gl_context->MakeCurrent(fake_onscreen_gl_surface.get());
  ASSERT_TRUE(make_current_result);

  auto gl_representation2 =
      shared_image_representation_factory_->ProduceGLTexturePassthrough(
          mailbox);
  EXPECT_TRUE(gl_representation2);

  // Verify there is still one per-context textures holders as the last current
  // context was created for an onscreen surface.
  EXPECT_EQ(1u, backing_ptr->per_context_cached_textures_holders_.size());
}

TEST_F(OzoneImageBackingFactoryTest, MarksContextLostOnContextLost) {
  if (!IsEglImageSupported()) {
    GTEST_SKIP();
  }

  EXPECT_TRUE(context_state_->MakeCurrent(context_state_->surface(),
                                          true /* needs_gl*/));

  const Mailbox mailbox = Mailbox::Generate();
  auto backing =
      backing_factory_->CreateSharedImage(mailbox,
                                          {viz::SinglePlaneFormat::kRGBA_8888,
                                           {100, 100},
                                           gfx::ColorSpace::CreateSRGB(),
                                           kTopLeft_GrSurfaceOrigin,
                                           kPremul_SkAlphaType,
                                           SHARED_IMAGE_USAGE_GLES2_READ,
                                           "TestLabel"},
                                          gpu::kNullSurfaceHandle, false);
  EXPECT_TRUE(backing);

  auto* backing_ptr = static_cast<OzoneImageBacking*>(backing.get());

  auto shared_image =
      shared_image_manager_.Register(std::move(backing), &memory_type_tracker_);
  EXPECT_TRUE(shared_image);

  // Create another context and produce a glTexture. if the context is marked as
  // lost, the image must notify the texture holders as well.
  auto gl_context = gl::init::CreateGLContext(nullptr, gl_surface_.get(),
                                              gl::GLContextAttribs());
  ASSERT_TRUE(gl_context);
  bool make_current_result = gl_context->MakeCurrent(gl_surface_.get());
  ASSERT_TRUE(make_current_result);

  auto gl_representation =
      shared_image_representation_factory_->ProduceGLTexturePassthrough(
          mailbox);
  EXPECT_TRUE(gl_representation);

  // Make own reference of the texture holders as the SI will remove its
  // reference.
  auto it =
      backing_ptr->per_context_cached_textures_holders_.find(gl_context.get());
  ASSERT_TRUE(it != backing_ptr->per_context_cached_textures_holders_.end());
  auto textures_holder_ref = it->second;

  backing_ptr->OnGLContextLost(gl_context.get());

  ASSERT_TRUE(backing_ptr->per_context_cached_textures_holders_.empty());

  EXPECT_TRUE(textures_holder_ref->WasContextLost());

  // The holder must have already been marked as a context lost. However, the
  // representation should be marked as a context lost as well so that it can
  // exercise the DCHECK that verifies the texture holders have already been
  // marked as context lost.
  gl_representation->OnContextLost();
  gl_representation.reset();

  // Manually destroy the glTexture to avoid leaking it.
  EXPECT_EQ(1u, textures_holder_ref->GetNumberOfTextures());
  const GLuint service_id =
      textures_holder_ref->texture(/*plane_index=*/0)->service_id();
  glDeleteTextures(1, &service_id);
}

// Same as above, but with an onscreen surface.
TEST_F(OzoneImageBackingFactoryTest, MarksContextLostOnContextLost2) {
  if (!IsEglImageSupported()) {
    GTEST_SKIP();
  }

  EXPECT_TRUE(context_state_->MakeCurrent(context_state_->surface(),
                                          true /* needs_gl*/));

  const Mailbox mailbox = Mailbox::Generate();
  auto backing =
      backing_factory_->CreateSharedImage(mailbox,
                                          {viz::SinglePlaneFormat::kRGBA_8888,
                                           {100, 100},
                                           gfx::ColorSpace::CreateSRGB(),
                                           kTopLeft_GrSurfaceOrigin,
                                           kPremul_SkAlphaType,
                                           SHARED_IMAGE_USAGE_GLES2_READ,
                                           "TestLabel"},
                                          gpu::kNullSurfaceHandle, false);
  EXPECT_TRUE(backing);

  auto* backing_ptr = static_cast<OzoneImageBacking*>(backing.get());

  auto shared_image =
      shared_image_manager_.Register(std::move(backing), &memory_type_tracker_);
  EXPECT_TRUE(shared_image);

  scoped_refptr<gl::GLSurface> fake_onscreen_gl_surface(
      new FakeOnScreenSurface(gl::GetDefaultDisplayEGL(), {100, 100}));
  ASSERT_FALSE(fake_onscreen_gl_surface->IsOffscreen());

  scoped_refptr<gl::GLContext> gl_context = gl::init::CreateGLContext(
      nullptr, fake_onscreen_gl_surface.get(), gl::GLContextAttribs());
  ASSERT_TRUE(gl_context);
  ASSERT_FALSE(gl_context->default_surface());
  bool make_current_result =
      gl_context->MakeCurrent(fake_onscreen_gl_surface.get());
  ASSERT_TRUE(make_current_result);

  {
    // gles2::TexturePassthrough
    auto gl_representation =
        shared_image_representation_factory_->ProduceGLTexturePassthrough(
            mailbox);
    EXPECT_TRUE(gl_representation);
    EXPECT_EQ(0u, backing_ptr->per_context_cached_textures_holders_.size());

    auto* ozone_reprensentation =
        static_cast<GLTexturePassthroughOzoneImageRepresentation*>(
            gl_representation.get());
    auto textures_holder_ref = ozone_reprensentation->textures_holder_;

    gl_representation->OnContextLost();
    gl_representation.reset();

    EXPECT_TRUE(textures_holder_ref->WasContextLost());

    // Manually destroy the glTexture to avoid leaking it.
    EXPECT_EQ(1u, textures_holder_ref->GetNumberOfTextures());
    const GLuint service_id =
        textures_holder_ref->texture(/*plane_index=*/0)->service_id();
    glDeleteTextures(1, &service_id);
  }
}

TEST_F(OzoneImageBackingFactoryTest, RemovesTextureHoldersOnContextDestroy) {
  if (!IsEglImageSupported()) {
    GTEST_SKIP();
  }

  EXPECT_TRUE(context_state_->MakeCurrent(context_state_->surface(),
                                          true /* needs_gl*/));

  const Mailbox mailbox = Mailbox::Generate();
  auto backing =
      backing_factory_->CreateSharedImage(mailbox,
                                          {viz::SinglePlaneFormat::kRGBA_8888,
                                           {100, 100},
                                           gfx::ColorSpace::CreateSRGB(),
                                           kTopLeft_GrSurfaceOrigin,
                                           kPremul_SkAlphaType,
                                           SHARED_IMAGE_USAGE_GLES2_READ,
                                           "TestLabel"},
                                          gpu::kNullSurfaceHandle, false);
  EXPECT_TRUE(backing);

  auto* backing_ptr = static_cast<OzoneImageBacking*>(backing.get());

  auto shared_image =
      shared_image_manager_.Register(std::move(backing), &memory_type_tracker_);
  EXPECT_TRUE(shared_image);

  auto gl_context = gl::init::CreateGLContext(nullptr, gl_surface_.get(),
                                              gl::GLContextAttribs());
  ASSERT_TRUE(gl_context);
  bool make_current_result = gl_context->MakeCurrent(gl_surface_.get());
  ASSERT_TRUE(make_current_result);

  auto gl_representation =
      shared_image_representation_factory_->ProduceGLTexturePassthrough(
          mailbox);
  EXPECT_TRUE(gl_representation);

  // Remove the reference here so that when SI removes its own, the texture is
  // actually safely destroyed.
  gl_representation.reset();

  gl_context.reset();

  ASSERT_TRUE(backing_ptr->per_context_cached_textures_holders_.empty());
}

// If textures are created for different contexts, the SI must restore a
// previous current context upon destruction a texture from a different context.
TEST_F(OzoneImageBackingFactoryTest, RestoresContextOnAnotherContextDestroy) {
  if (!IsEglImageSupported()) {
    GTEST_SKIP();
  }

  EXPECT_TRUE(context_state_->MakeCurrent(context_state_->surface(),
                                          true /* needs_gl*/));

  const Mailbox mailbox = Mailbox::Generate();
  auto backing =
      backing_factory_->CreateSharedImage(mailbox,
                                          {viz::SinglePlaneFormat::kRGBA_8888,
                                           {100, 100},
                                           gfx::ColorSpace::CreateSRGB(),
                                           kTopLeft_GrSurfaceOrigin,
                                           kPremul_SkAlphaType,
                                           SHARED_IMAGE_USAGE_GLES2_READ,
                                           "TestLabel"},
                                          gpu::kNullSurfaceHandle, false);
  EXPECT_TRUE(backing);

  auto shared_image =
      shared_image_manager_.Register(std::move(backing), &memory_type_tracker_);
  EXPECT_TRUE(shared_image);

  auto gl_representation =
      shared_image_representation_factory_->ProduceGLTexturePassthrough(
          mailbox);
  EXPECT_TRUE(gl_representation);

  auto gl_context = gl::init::CreateGLContext(nullptr, gl_surface_.get(),
                                              gl::GLContextAttribs());
  ASSERT_TRUE(gl_context);
  EXPECT_TRUE(gl_context->MakeCurrent(gl_surface_.get()));

  auto gl_representation2 =
      shared_image_representation_factory_->ProduceGLTexturePassthrough(
          mailbox);
  EXPECT_TRUE(gl_representation2);

  EXPECT_TRUE(context_state_->MakeCurrent(context_state_->surface(),
                                          true /* needs_gl*/));

  // Remove the reference here so that when SI removes its own, the texture is
  // actually safely destroyed.
  gl_representation2.reset();
  gl_context.reset();

  EXPECT_TRUE(context_state_->context()->IsCurrent(gl_surface_.get()));
}

// Verifies that if there is a compatible context, the texture is reused. Eg,
// there was a request to create a texture for one context, then the context was
// changed and another request came. If the contexts are compatible, the texture
// holder is reused. Otherwise, a new texture is created.
TEST_F(OzoneImageBackingFactoryTest, FindsCompatibleContextAndReusesTexture) {
  if (!IsEglImageSupported()) {
    GTEST_SKIP();
  }

  EXPECT_TRUE(context_state_->MakeCurrent(context_state_->surface(),
                                          true /* needs_gl*/));

  const Mailbox mailbox = Mailbox::Generate();
  auto backing =
      backing_factory_->CreateSharedImage(mailbox,
                                          {viz::SinglePlaneFormat::kRGBA_8888,
                                           {100, 100},
                                           gfx::ColorSpace::CreateSRGB(),
                                           kTopLeft_GrSurfaceOrigin,
                                           kPremul_SkAlphaType,
                                           SHARED_IMAGE_USAGE_GLES2_READ,
                                           "TestLabel"},
                                          gpu::kNullSurfaceHandle, false);
  EXPECT_TRUE(backing);

  auto* backing_ptr = static_cast<OzoneImageBacking*>(backing.get());

  auto shared_image =
      shared_image_manager_.Register(std::move(backing), &memory_type_tracker_);
  EXPECT_TRUE(shared_image);

  // Create and validate GLTexture representation.
  auto gl_representation =
      shared_image_representation_factory_->ProduceGLTexturePassthrough(
          mailbox);
  EXPECT_TRUE(gl_representation);
  EXPECT_TRUE(gl_representation->GetTexturePassthrough()->service_id());

  // Verify there is only one per-context textures holder now.
  EXPECT_EQ(1u, backing_ptr->per_context_cached_textures_holders_.size());

  gl::GLContextAttribs attribs;
  attribs.global_texture_share_group = true;
  attribs.angle_context_virtualization_group_number =
      gl::AngleContextVirtualizationGroup::kGLImageProcessor;
  auto gl_context =
      gl::init::CreateGLContext(nullptr, gl_surface_.get(), attribs);
  ASSERT_TRUE(gl_context);
  bool make_current_result = gl_context->MakeCurrent(gl_surface_.get());
  ASSERT_TRUE(make_current_result);

  auto gl_representation2 =
      shared_image_representation_factory_->ProduceGLTexturePassthrough(
          mailbox);
  EXPECT_TRUE(gl_representation2);

  // Verify there is now two per-context textures holders.
  EXPECT_EQ(2u, backing_ptr->per_context_cached_textures_holders_.size());
  // And they are different of course.
  EXPECT_NE(gl_representation->GetTexturePassthrough(),
            gl_representation2->GetTexturePassthrough());

  const std::vector<std::pair<bool, gl::AngleContextVirtualizationGroup>>
      kTestVariations = {
          {true, gl::AngleContextVirtualizationGroup::kDefault},
          {false, gl::AngleContextVirtualizationGroup::kDefault},
          {true, gl::AngleContextVirtualizationGroup::kDrDc},
          {false, gl::AngleContextVirtualizationGroup::kDrDc},
          {true, gl::AngleContextVirtualizationGroup::kGLImageProcessor},
          {false, gl::AngleContextVirtualizationGroup::kGLImageProcessor},
          {true, gl::AngleContextVirtualizationGroup::kWebViewRenderThread},
          {false, gl::AngleContextVirtualizationGroup::kWebViewRenderThread},
      };
  for (const auto& variation : kTestVariations) {
    gl::GLContextAttribs attributes;
    // Create one more context. It'll have similar attributes to the previous
    // context. And, thus, the texture must be reused.
    attributes.global_texture_share_group = variation.first;
    attributes.angle_context_virtualization_group_number = variation.second;
    auto new_context =
        gl::init::CreateGLContext(nullptr, gl_surface_.get(), attributes);
    ASSERT_TRUE(new_context);
    make_current_result = new_context->MakeCurrent(gl_surface_.get());
    ASSERT_TRUE(make_current_result);

    auto representation =
        shared_image_representation_factory_->ProduceGLTexturePassthrough(
            mailbox);
    EXPECT_TRUE(representation);

    // Verify there is three per-context textures holders stored..
    EXPECT_EQ(3u, backing_ptr->per_context_cached_textures_holders_.size());
    // .. but those two last are the same holders as OzoneImageBacking will
    // reuse the textures if contexts are compatible.
    if (gl_context->CanShareTexturesWithContext(new_context.get())) {
      EXPECT_EQ(representation->GetTexturePassthrough(),
                gl_representation2->GetTexturePassthrough());
    } else {
      EXPECT_NE(representation->GetTexturePassthrough(),
                gl_representation2->GetTexturePassthrough());
    }
    // And they are always different from the first context, of course.
    EXPECT_NE(gl_representation->GetTexturePassthrough(),
              representation->GetTexturePassthrough());
  }
}

// If the cached textures holder is shared between two contexts, which are
// compatible for such usage, destruction of one context or making it loose
// context shouldn't make a texture holder destroy textures or mark texture as
// context lost. Once all contexts have context lost or are destroy, only then
// the holder must destroy textures and/or mark them as context lost.
TEST_F(OzoneImageBackingFactoryTest, CorrectlyDestroysAndMarksContextLost) {
  if (!IsEglImageSupported()) {
    GTEST_SKIP();
  }

  EXPECT_TRUE(context_state_->MakeCurrent(context_state_->surface(),
                                          true /* needs_gl*/));

  const Mailbox mailbox = Mailbox::Generate();
  auto backing =
      backing_factory_->CreateSharedImage(mailbox,
                                          {viz::SinglePlaneFormat::kRGBA_8888,
                                           {100, 100},
                                           gfx::ColorSpace::CreateSRGB(),
                                           kTopLeft_GrSurfaceOrigin,
                                           kPremul_SkAlphaType,
                                           SHARED_IMAGE_USAGE_GLES2_READ,
                                           "TestLabel"},
                                          gpu::kNullSurfaceHandle, false);
  EXPECT_TRUE(backing);

  auto* backing_ptr = static_cast<OzoneImageBacking*>(backing.get());

  auto shared_image =
      shared_image_manager_.Register(std::move(backing), &memory_type_tracker_);
  EXPECT_TRUE(shared_image);

  gl::GLContextAttribs attribs;
  attribs.global_texture_share_group = true;
  attribs.angle_context_virtualization_group_number =
      gl::AngleContextVirtualizationGroup::kGLImageProcessor;
  auto gl_context =
      gl::init::CreateGLContext(nullptr, gl_surface_.get(), attribs);
  ASSERT_TRUE(gl_context);
  bool make_current_result = gl_context->MakeCurrent(gl_surface_.get());
  ASSERT_TRUE(make_current_result);

  auto gl_representation =
      shared_image_representation_factory_->ProduceGLTexturePassthrough(
          mailbox);
  EXPECT_TRUE(gl_representation);
  EXPECT_EQ(1u, backing_ptr->per_context_cached_textures_holders_.size());

  auto new_context =
      gl::init::CreateGLContext(nullptr, gl_surface_.get(), attribs);
  ASSERT_TRUE(new_context);
  make_current_result = new_context->MakeCurrent(gl_surface_.get());
  ASSERT_TRUE(make_current_result);

  auto gl_representation2 =
      shared_image_representation_factory_->ProduceGLTexturePassthrough(
          mailbox);
  EXPECT_TRUE(gl_representation2);
  EXPECT_EQ(gl_representation->GetTexturePassthrough(),
            gl_representation2->GetTexturePassthrough());

  EXPECT_EQ(2u, backing_ptr->per_context_cached_textures_holders_.size());

  auto holder_ref1 =
      backing_ptr->per_context_cached_textures_holders_.begin()->second;
  auto holder_ref2 =
      backing_ptr->per_context_cached_textures_holders_.rbegin()->second;
  EXPECT_EQ(holder_ref1, holder_ref2);
  backing_ptr->OnGLContextLost(new_context.get());
  EXPECT_FALSE(holder_ref1->WasContextLost());
  EXPECT_EQ(1u, backing_ptr->per_context_cached_textures_holders_.size());
  EXPECT_EQ(1u, holder_ref1->GetNumberOfTextures());

  new_context.reset();
  EXPECT_EQ(1u, backing_ptr->per_context_cached_textures_holders_.size());
  EXPECT_EQ(1u, holder_ref1->GetNumberOfTextures());

  gl_context.reset();
  EXPECT_EQ(0u, backing_ptr->per_context_cached_textures_holders_.size());
  EXPECT_EQ(0u, holder_ref1->GetNumberOfTextures());
}

TEST_F(OzoneImageBackingFactoryTest, CreateGpuMemoryBufferHandle) {
  for (auto format : viz::GetMappableSharedImageFormatForTesting()) {
    gfx::BufferUsage usages[] = {
        gfx::BufferUsage::GPU_READ,
        gfx::BufferUsage::SCANOUT,
        gfx::BufferUsage::SCANOUT_CAMERA_READ_WRITE,
        gfx::BufferUsage::CAMERA_AND_CPU_READ_WRITE,
        gfx::BufferUsage::SCANOUT_CPU_READ_WRITE,
        gfx::BufferUsage::SCANOUT_VDA_WRITE,
        gfx::BufferUsage::PROTECTED_SCANOUT,
        gfx::BufferUsage::PROTECTED_SCANOUT_VDA_WRITE,
        gfx::BufferUsage::GPU_READ_CPU_READ_WRITE,
        gfx::BufferUsage::SCANOUT_VEA_CPU_READ,
        gfx::BufferUsage::VEA_READ_CAMERA_AND_CPU_READ_WRITE,
        gfx::BufferUsage::SCANOUT_FRONT_RENDERING,
    };
    for (auto usage : usages) {
      if (!ui::OzonePlatform::GetInstance()->IsNativePixmapConfigSupported(
              format, usage)) {
        continue;
      }

      gfx::GpuMemoryBufferHandle handle =
          OzoneImageBackingFactory::CreateGpuMemoryBufferHandle(
              /*vulkan_context_provider=*/nullptr, gfx::Size(2, 2), format,
              usage);
      EXPECT_EQ(handle.type, gfx::NATIVE_PIXMAP);
    }
  }
}
TEST_F(OzoneImageBackingFactoryTest, UploadAndReadback) {
  if (!IsEglImageSupported()) {
    GTEST_SKIP();
  }

  EXPECT_TRUE(context_state_->MakeCurrent(context_state_->surface(),
                                          true /* needs_gl*/));

  const Mailbox mailbox = Mailbox::Generate();
  const auto format = viz::SinglePlaneFormat::kRGBA_8888;
  const gfx::Size size(100, 100);
  const auto color_space = gfx::ColorSpace::CreateSRGB();
  const auto surface_origin = kTopLeft_GrSurfaceOrigin;
  const auto alpha_type = kPremul_SkAlphaType;
  auto usage = SHARED_IMAGE_USAGE_GLES2_READ | SHARED_IMAGE_USAGE_GLES2_WRITE;

  auto backing = backing_factory_->CreateSharedImage(
      mailbox,
      {format, size, color_space, surface_origin, alpha_type, usage,
       "TestLabel"},
      gpu::kNullSurfaceHandle, false);
  ASSERT_TRUE(backing);

  std::vector<SkBitmap> upload_bitmaps = AllocateRedBitmaps(format, size);
  std::vector<SkPixmap> upload_pixmaps = GetSkPixmaps(upload_bitmaps);

  bool upload_result = backing->UploadFromMemory(upload_pixmaps);
  EXPECT_TRUE(upload_result);

  std::vector<SkBitmap> readback_bitmaps(1);
  readback_bitmaps[0].allocPixels(
      SkImageInfo::Make(size.width(), size.height(),
                        viz::ToClosestSkColorType(format, 0), alpha_type));
  std::vector<SkPixmap> readback_pixmaps = GetSkPixmaps(readback_bitmaps);

  bool readback_result = backing->ReadbackToMemory(readback_pixmaps);
  EXPECT_TRUE(readback_result);

  EXPECT_TRUE(cc::MatchesBitmap(readback_bitmaps[0], upload_bitmaps[0],
                                cc::ExactPixelComparator()));
}

}  // namespace gpu
