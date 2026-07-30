// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GPU_COMMAND_BUFFER_SERVICE_SHARED_IMAGE_OZONE_IMAGE_BACKING_H_
#define GPU_COMMAND_BUFFER_SERVICE_SHARED_IMAGE_OZONE_IMAGE_BACKING_H_

#include <dawn/webgpu_cpp.h>

#include <memory>
#include <optional>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/gtest_prod_util.h"
#include "base/memory/scoped_refptr.h"
#include "build/build_config.h"
#include "gpu/command_buffer/common/mailbox.h"
#include "gpu/command_buffer/common/shared_image_info.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/command_buffer/service/memory_tracking.h"
#include "gpu/command_buffer/service/shared_context_state.h"
#include "gpu/command_buffer/service/shared_image/shared_image_backing.h"
#include "gpu/command_buffer/service/shared_image/shared_image_representation.h"
#include "gpu/config/gpu_driver_bug_workarounds.h"
#include "gpu/gpu_gles2_export.h"
#include "ui/gfx/buffer_types.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/gpu_fence.h"
#include "ui/gfx/gpu_fence_handle.h"
#include "ui/gfx/native_pixmap.h"
#include "ui/gl/buildflags.h"
#include "ui/gl/gl_context.h"

namespace gpu {
class OzoneImageGLTexturesHolder;

// Implementation of SharedImageBacking that uses a NativePixmap created via
// an Ozone surface factory. The memory associated with the pixmap can be
// aliased by both GL and Vulkan for use in rendering or compositing.
class GPU_GLES2_EXPORT OzoneImageBacking final
    : public ClearTrackingSharedImageBacking,
      public gl::GLContext::GLContextObserver {
 public:
  OzoneImageBacking(
      const Mailbox& mailbox,
      const SharedImageInfo& si_info,
      scoped_refptr<SharedContextState> context_state,
      scoped_refptr<gfx::NativePixmap> pixmap,
      const GpuDriverBugWorkarounds& workarounds,
      std::optional<gfx::BufferUsage> buffer_usage = std::nullopt);

  OzoneImageBacking(const OzoneImageBacking&) = delete;
  OzoneImageBacking& operator=(const OzoneImageBacking&) = delete;

  ~OzoneImageBacking() override;

  // gpu::SharedImageBacking:
  SharedImageBackingType GetType() const override;
  void Update(std::unique_ptr<gfx::GpuFence> in_fence) override;
  bool UploadFromMemory(const std::vector<SkPixmap>& pixmaps) override;
  bool ReadbackToMemory(const std::vector<SkPixmap>& pixmaps) override;
  scoped_refptr<gfx::NativePixmap> GetNativePixmap() override;
  gfx::GpuMemoryBufferHandle GetGpuMemoryBufferHandle() override;
  bool IsImportedFromExo() override;

  enum class AccessStream { kGL, kVulkan, kWebGPU, kOverlay, kLast };

  // Owns one backing access and the synchronization objects removed from the
  // backing for that access. Vulkan ownership state can only be consumed and
  // replaced through this object, so it cannot outlive or become detached
  // from the access whose fence makes it valid.
  class GPU_GLES2_EXPORT ScopedAccess {
   public:
    ScopedAccess(const ScopedAccess&) = delete;
    ScopedAccess& operator=(const ScopedAccess&) = delete;
    ~ScopedAccess();

    std::vector<gfx::GpuFenceHandle> TakeBeginFences();
    bool needs_end_fence() const {
#if BUILDFLAG(ENABLE_VULKAN)
      // Returning ownership to an external Vulkan queue always needs a fence,
      // even when ordinary same-stream read tracking would not require one.
      return need_end_fence_ || took_vulkan_state_;
#else
      return need_end_fence_;
#endif
    }
    bool acquire_committed() const { return acquire_committed_; }

#if BUILDFLAG(ENABLE_VULKAN)
    bool TakeExternalVulkanImageState(
        std::optional<ExternalVulkanImageState>* state);
    bool has_external_vulkan_state() const;
#endif

    // Call after the consumer has accepted the acquire fences and, for a
    // Vulkan access, the ownership-transfer state.
    void CommitAcquire();

    // Completes an implicit external access. Any Vulkan transfer not consumed
    // by this access remains attached to the backing.
    void End(gfx::GpuFenceHandle fence);

#if BUILDFLAG(ENABLE_VULKAN)
    // Completes an explicit Vulkan ownership access. The release state is the
    // exact transfer that the next owner must match.
    bool EndVulkan(gfx::GpuFenceHandle fence,
                   ExternalVulkanImageState release_state);

    // Completes an explicit Vulkan access for which the backend submitted no
    // GPU work. Since no queue-family transfer occurred, the consumed state
    // remains authoritative and is restored unchanged.
    bool EndVulkanWithoutGpuUse(gfx::GpuFenceHandle fence);
#endif

    // Valid only before CommitAcquire(). Restores all synchronization and
    // ownership state removed by BeginAccess().
    void AbortBeforeAcquire();

    // Use after an accepted acquire can no longer be completed. The old state
    // cannot safely be restored once GPU ownership may have changed.
    void InvalidateAfterAcquire();

    // Ends access accounting without restoring consumed Vulkan ownership and
    // without losing the context. This is only valid when the backing is
    // destroyed immediately afterward.
    void AbandonForBackingDestruction();

   private:
    friend class OzoneImageBacking;

    ScopedAccess(
        OzoneImageBacking* backing,
        bool readonly,
        AccessStream access_stream,
        bool need_end_fence,
        std::vector<gfx::GpuFenceHandle> begin_fences,
        base::flat_map<AccessStream, gfx::GpuFenceHandle> consumed_read_fences,
        gfx::GpuFenceHandle consumed_external_write_fence,
        gfx::GpuFenceHandle consumed_write_fence);

    OzoneImageBacking* const backing_;
    const bool readonly_;
    const AccessStream access_stream_;
    const bool need_end_fence_;
    std::vector<gfx::GpuFenceHandle> begin_fences_;
    base::flat_map<AccessStream, gfx::GpuFenceHandle> consumed_read_fences_;
    gfx::GpuFenceHandle consumed_external_write_fence_;
    gfx::GpuFenceHandle consumed_write_fence_;
#if BUILDFLAG(ENABLE_VULKAN)
    std::optional<ExternalVulkanImageState> consumed_vulkan_state_;
    bool took_vulkan_state_ = false;
#endif
    bool acquire_committed_ = false;
    bool finished_ = false;
  };

 protected:
  std::unique_ptr<DawnImageRepresentation> ProduceDawn(
      SharedImageManager* manager,
      MemoryTypeTracker* tracker,
      const wgpu::Device& device,
      wgpu::BackendType backend_type,
      std::vector<wgpu::TextureFormat> view_formats,
      scoped_refptr<SharedContextState> context_state) override;
  std::unique_ptr<SkiaGraphiteImageRepresentation> ProduceSkiaGraphite(
      SharedImageManager* manager,
      MemoryTypeTracker* tracker,
      scoped_refptr<SharedContextState> context_state) override;
  std::unique_ptr<GLTexturePassthroughImageRepresentation>
  ProduceGLTexturePassthrough(SharedImageManager* manager,
                              MemoryTypeTracker* tracker) override;
  std::unique_ptr<SkiaGaneshImageRepresentation> ProduceSkiaGanesh(
      SharedImageManager* manager,
      MemoryTypeTracker* tracker,
      scoped_refptr<SharedContextState> context_state) override;
  std::unique_ptr<OverlayImageRepresentation> ProduceOverlay(
      SharedImageManager* manager,
      MemoryTypeTracker* tracker) override;

#if BUILDFLAG(ENABLE_VULKAN)
  std::unique_ptr<VulkanImageRepresentation> ProduceVulkan(
      SharedImageManager* manager,
      MemoryTypeTracker* tracker,
      gpu::VulkanDeviceQueue* vulkan_device_queue,
      gpu::VulkanImplementation& vulkan_impl,
      bool needs_detiling) override;
#endif

 private:
  friend class GLTexturePassthroughOzoneImageRepresentation;
  friend class DawnOzoneImageRepresentation;
  friend class SkiaVkOzoneImageRepresentation;
  friend class VulkanOzoneImageRepresentation;
  class OverlayOzoneImageRepresentation;

  FRIEND_TEST_ALL_PREFIXES(OzoneImageBackingFactoryTest,
                           UsesCacheForTextureHolders);
  FRIEND_TEST_ALL_PREFIXES(OzoneImageBackingFactoryTest,
                           UsesCacheForTextureHolders2);
  FRIEND_TEST_ALL_PREFIXES(OzoneImageBackingFactoryTest,
                           MarksContextLostOnContextLost);
  FRIEND_TEST_ALL_PREFIXES(OzoneImageBackingFactoryTest,
                           MarksContextLostOnContextLost2);
  FRIEND_TEST_ALL_PREFIXES(OzoneImageBackingFactoryTest,
                           RemovesTextureHoldersOnContextDestroy);
  FRIEND_TEST_ALL_PREFIXES(OzoneImageBackingFactoryTest,
                           FindsCompatibleContextAndReusesTexture);
  FRIEND_TEST_ALL_PREFIXES(OzoneImageBackingFactoryTest,
                           CorrectlyDestroysAndMarksContextLost);
#if BUILDFLAG(ENABLE_VULKAN)
  FRIEND_TEST_ALL_PREFIXES(OzoneImageBackingFactoryTest,
                           ScopedVulkanStateRoundTrip);
  FRIEND_TEST_ALL_PREFIXES(OzoneImageBackingFactoryTest,
                           ScopedVulkanUnusedAccessRestoresState);
  FRIEND_TEST_ALL_PREFIXES(OzoneImageBackingFactoryTest,
                           ScopedAccessAbortRestoresState);
  FRIEND_TEST_ALL_PREFIXES(OzoneImageBackingFactoryTest,
                           ImplicitAccessPreservesVulkanState);
  FRIEND_TEST_ALL_PREFIXES(OzoneImageBackingFactoryTest,
                           PostAcquireFailureInvalidatesBacking);
  FRIEND_TEST_ALL_PREFIXES(OzoneImageBackingFactoryTest,
                           BackingDestructionAbandonsExternalVulkanOwnership);
  FRIEND_TEST_ALL_PREFIXES(OzoneImageBackingFactoryTest,
                           RejectsConcurrentVulkanOwner);
#endif

  void FlushAndSubmitIfNecessary(
      std::vector<GrBackendSemaphore> signal_semaphores,
      SharedContextState* const shared_context_state);

  std::unique_ptr<ScopedAccess> BeginAccess(bool readonly,
                                            AccessStream access_stream);
  void EndAccessInternal(bool readonly,
                         AccessStream access_stream,
                         gfx::GpuFenceHandle fence);
  void AbortAccessInternal(ScopedAccess* access);
  void InvalidateAccessInternal(ScopedAccess* access);
  void AbandonAccessForBackingDestructionInternal(ScopedAccess* access);

  scoped_refptr<OzoneImageGLTexturesHolder> RetainGLTexturePerContextCache();

  // gl::GLContext::GLContextObserver:
  void OnGLContextLost(gl::GLContext* context) override;
  void OnGLContextWillDestroy(gl::GLContext* context) override;

  void OnGLContextLostOrDestroy(gl::GLContext* context, bool mark_context_lost);

  // Returns a GpuMemoryBufferHandle for a single plane of the backing pixmap.
  gfx::GpuMemoryBufferHandle GetSinglePlaneGpuMemoryBufferHandle(
      uint32_t index);

  void DestroyTexturesOnContext(OzoneImageGLTexturesHolder* holder,
                                gl::GLContext* context);

#if BUILDFLAG(USE_DAWN)
  bool UploadFromMemoryGraphite(const std::vector<SkPixmap>& pixmaps);
  bool ReadbackToMemoryGraphite(const std::vector<SkPixmap>& pixmaps);
#endif  // BUILDFLAG(USE_DAWN)

  uint32_t reads_in_progress_ = 0;
  bool is_write_in_progress_ = false;
  uint32_t active_accesses_ = 0;
  int write_streams_count_;

  scoped_refptr<gfx::NativePixmap> pixmap_;
  // Per-context texture holders that are cached to reduce the number of
  // allocations/deallocations of textures and their EGLImages. That's
  // especially handy for raster tasks as there can be tens of tasks resulting
  // in creation and destruction of EGLImages, which is costly.
  std::map<gl::GLContext*, scoped_refptr<OzoneImageGLTexturesHolder>>
      per_context_cached_textures_holders_;

  // Write fence that is external and does not do Begin/EndAccess (eg. exo)
  gfx::GpuFenceHandle external_write_fence_;
  gfx::GpuFenceHandle write_fence_;
  base::flat_map<AccessStream, gfx::GpuFenceHandle> read_fences_;
  AccessStream last_write_stream_;
#if BUILDFLAG(ENABLE_VULKAN)
  std::optional<ExternalVulkanImageState> external_vulkan_state_;
  bool has_vulkan_ownership_history_ = false;
  bool vulkan_ownership_in_progress_ = false;
#endif
  scoped_refptr<SharedContextState> context_state_;
  const GpuDriverBugWorkarounds workarounds_;
  const bool imported_from_exo_ = false;
};

}  // namespace gpu

#endif  // GPU_COMMAND_BUFFER_SERVICE_SHARED_IMAGE_OZONE_IMAGE_BACKING_H_
