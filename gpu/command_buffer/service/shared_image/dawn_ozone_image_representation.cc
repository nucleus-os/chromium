// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/command_buffer/service/shared_image/dawn_ozone_image_representation.h"

#include <optional>

#include <dawn/native/VulkanBackend.h>
#include <sync/sync.h>
#include <vulkan/vulkan.h>
// X11 Xlib.h defines Status as int and X.h defines Success as 0, both
// conflicting with wgpu::Status::Success.
#undef Status
#undef Success

#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/posix/eintr_wrapper.h"
#include "gpu/command_buffer/service/memory_tracking.h"
#include "gpu/command_buffer/service/shared_image/ozone_image_backing.h"
#include "gpu/command_buffer/service/shared_image/shared_image_backing.h"
#include "gpu/command_buffer/service/shared_image/shared_image_manager.h"
#include "gpu/command_buffer/service/shared_image/shared_image_representation.h"
#include "gpu/config/gpu_finch_features.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/gpu_fence_handle.h"
#include "ui/gfx/linux/drm_util_linux.h"  // nogncheck
#include "ui/gfx/native_pixmap.h"

namespace gpu {

DawnOzoneImageRepresentation::DawnOzoneImageRepresentation(
    SharedImageManager* manager,
    SharedImageBacking* backing,
    MemoryTypeTracker* tracker,
    wgpu::Device device,
    wgpu::TextureFormat format,
    std::vector<wgpu::TextureFormat> view_formats,
    scoped_refptr<gfx::NativePixmap> pixmap)
    : DawnImageRepresentation(manager, backing, tracker),
      device_(std::move(device)),
      format_(format),
      view_formats_(std::move(view_formats)),
      pixmap_(pixmap) {
  DCHECK(device_);
}

DawnOzoneImageRepresentation::~DawnOzoneImageRepresentation() {
  EndAccess();
}

wgpu::Texture DawnOzoneImageRepresentation::BeginAccess(
    wgpu::TextureUsage usage,
    wgpu::TextureUsage internal_usage) {
  // It doesn't make sense to have two overlapping BeginAccess calls on the same
  // representation.
  // TODO(blundell):Switch to using the return value of
  // OzoneImageBacking::BeginAccess().
  if (texture_) {
    LOG(ERROR)
        << "Attempting to begin access with before ending previous access.";
    return nullptr;
  }

  is_readonly_ =
      (usage & kWriteUsage) == 0 && (internal_usage & kWriteUsage) == 0;
  if (is_readonly_ && !IsCleared()) {
    // Read-only access of an uncleared texture is not allowed: clients
    // relying on Dawn's lazy clearing of uninitialized textures must make
    // this reliance explicit by passing a write usage.
    return nullptr;
  }

  backing_access_ = ozone_backing()->BeginAccess(
      is_readonly_, OzoneImageBacking::AccessStream::kWebGPU);
  if (!backing_access_) {
    return nullptr;
  }
  DCHECK(backing_access_->needs_end_fence() || is_readonly_);
  std::vector<gfx::GpuFenceHandle> fences = backing_access_->TakeBeginFences();

  wgpu::SharedTextureMemoryBeginAccessDescriptor begin_access_desc = {};
  begin_access_desc.initialized = IsCleared();

  wgpu::SharedTextureMemoryVkImageLayoutBeginState begin_layout{};
  std::optional<ExternalVulkanImageState> external_state;
  if (!backing_access_->TakeExternalVulkanImageState(&external_state)) {
    backing_access_.reset();
    return nullptr;
  }
  if (external_state) {
    if (!external_state->IsValid() ||
        external_state->external_queue_family != VK_QUEUE_FAMILY_EXTERNAL_KHR) {
      LOG(ERROR) << "Invalid external Vulkan layout state";
      backing_access_.reset();
      return nullptr;
    }
    begin_layout.oldLayout = external_state->old_layout;
    begin_layout.newLayout = external_state->new_layout;
  } else {
    if (begin_access_desc.initialized) {
      if (ozone_backing()->has_vulkan_ownership_history_) {
        LOG(ERROR) << "Initialized Ozone image lost its Vulkan ownership state";
        backing_access_.reset();
        return nullptr;
      }
      // A DMA-BUF can enter the backing with valid pixels before it has ever
      // had a Vulkan owner (for example, a decoded video frame). Preserve that
      // first external payload in GENERAL; every subsequent Vulkan handoff
      // must use the exact state returned by the previous owner.
      begin_layout.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      begin_layout.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    } else {
      // An uncleared first write may discard the previous contents.
      begin_layout.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      begin_layout.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
  }
  begin_access_desc.nextInChain = &begin_layout;

  // If the semaphore from BeginWrite is valid then pass it to
  // SharedTextureMemory::BeginAccess() below.
  std::vector<wgpu::SharedFence> shared_fences;
  std::vector<uint64_t> shared_fence_signals;

  begin_access_desc.fenceCount = fences.size();
  begin_access_desc.signaledValueCount = fences.size();
  if (fences.size()) {
    shared_fences.resize(fences.size());
    shared_fence_signals.resize(fences.size());
    for (size_t i = 0; i < fences.size(); i++) {
      wgpu::SharedFenceSyncFDDescriptor sync_fd_desc;
      // NOTE: There is no ownership transfer here, as Dawn internally dup()s
      // the passed-in handle.
      sync_fd_desc.handle = fences[i].Peek();
      wgpu::SharedFenceDescriptor fence_desc;
      fence_desc.nextInChain = &sync_fd_desc;
      shared_fences[i] = device_.ImportSharedFence(&fence_desc);
      if (!shared_fences[i]) {
        LOG(ERROR) << "Failed to import a shared-image acquire fence";
        backing_access_.reset();
        return nullptr;
      }
      // Pass 1 as the signaled value for the binary semaphore
      // (Dawn's SharedTextureMemoryVk verifies that this is the value passed).
      const uint64_t kSignaledValue = 1;
      shared_fence_signals[i] = kSignaledValue;
    }

    begin_access_desc.fences = shared_fences.data();
    begin_access_desc.signaledValues = shared_fence_signals.data();
  }
  gfx::Size pixmap_size = pixmap_->GetBufferSize();

  wgpu::DawnTextureInternalUsageDescriptor internalDesc;
  internalDesc.internalUsage = internal_usage;

  wgpu::TextureDescriptor texture_descriptor;
  texture_descriptor.format = format_;
  texture_descriptor.usage = static_cast<wgpu::TextureUsage>(usage);
  texture_descriptor.dimension = wgpu::TextureDimension::e2D;
  texture_descriptor.size = {static_cast<uint32_t>(size().width()),
                             static_cast<uint32_t>(size().height()),
                             /*depthOrArrayLayers=*/1};
  texture_descriptor.mipLevelCount = 1;
  texture_descriptor.sampleCount = 1;
  texture_descriptor.viewFormatCount = view_formats_.size();
  texture_descriptor.viewFormats = view_formats_.data();
  texture_descriptor.nextInChain = &internalDesc;

  wgpu::SharedTextureMemoryDmaBufDescriptor dmaBufDesc;
  dmaBufDesc.size = {static_cast<uint32_t>(pixmap_size.width()),
                     static_cast<uint32_t>(pixmap_size.height())};

  dmaBufDesc.drmFormat =
      ui::GetFourCCFormatFromSharedImageFormat(pixmap_->GetSharedImageFormat());
  dmaBufDesc.drmModifier = pixmap_->GetFormatModifier();

  std::vector<wgpu::SharedTextureMemoryDmaBufPlane> planes(
      pixmap_->GetNumberOfPlanes());
  dmaBufDesc.planeCount = pixmap_->GetNumberOfPlanes();
  for (uint32_t plane_idx = 0; plane_idx < dmaBufDesc.planeCount; ++plane_idx) {
    // Dawn is not an ownership transfer. Dawn will internally duplicate fds as
    // necessary.
    planes[plane_idx].fd = pixmap_->GetDmaBufFd(plane_idx);
    planes[plane_idx].stride = pixmap_->GetDmaBufPitch(plane_idx);
    planes[plane_idx].offset = pixmap_->GetDmaBufOffset(plane_idx);
  }

  dmaBufDesc.planes = planes.data();

  wgpu::SharedTextureMemoryDescriptor desc;
  desc.label = "DawnOzoneImageRepresentation";
  desc.nextInChain = &dmaBufDesc;

  if (!shared_texture_memory_) {
    shared_texture_memory_ = device_.ImportSharedTextureMemory(&desc);
    if (!shared_texture_memory_) {
      LOG(ERROR) << "Failed to import shared-image DMA-BUF memory";
      backing_access_.reset();
      return nullptr;
    }
  }

  texture_ = shared_texture_memory_.CreateTexture(&texture_descriptor);
  if (!texture_) {
    LOG(ERROR) << "Failed to create a texture from shared-image DMA-BUF memory";
    backing_access_.reset();
    return nullptr;
  }
  if (shared_texture_memory_.BeginAccess(texture_, &begin_access_desc) !=
      wgpu::Status::Success) {
    LOG(ERROR) << "Failed to begin access for shared image.";
    texture_.Destroy();
    texture_ = nullptr;
    backing_access_.reset();
    return nullptr;
  }

  backing_access_->CommitAcquire();
  return texture_;
}

void DawnOzoneImageRepresentation::EndAccess() {
  if (!texture_) {
    return;
  }
  auto invalidate_access = [this]() {
    backing_access_->InvalidateAfterAcquire();
    backing_access_.reset();
    texture_.Destroy();
    texture_ = nullptr;
  };

  wgpu::SharedTextureMemoryEndAccessState end_access_desc = {};
  wgpu::SharedTextureMemoryVkImageLayoutEndState end_layout{};
  end_access_desc.nextInChain = &end_layout;

  if (shared_texture_memory_.EndAccess(texture_, &end_access_desc) !=
      wgpu::Status::Success) {
    LOG(ERROR) << "Failed to end access for DawnOzoneImageRepresentation";
    invalidate_access();
    return;
  }

  if (end_access_desc.initialized) {
    SetCleared();
  }

  const ExternalVulkanImageState external_state{
      .old_layout = static_cast<VkImageLayout>(end_layout.oldLayout),
      .new_layout = static_cast<VkImageLayout>(end_layout.newLayout),
      .external_queue_family = VK_QUEUE_FAMILY_EXTERNAL_KHR};
  const bool access_was_unused =
      end_layout.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
      end_layout.newLayout == VK_IMAGE_LAYOUT_UNDEFINED;

  CHECK(end_access_desc.fenceCount == end_access_desc.signaledValueCount);

  // Note: Dawn may export zero fences if there were no begin fences,
  // AND the WGPUTexture was not used on the GPU queue within the
  // access scope. Otherwise, it should either export fences from Dawn
  // signaled after the WGPUTexture's last use, or it should re-export
  // the begin fences if the WGPUTexture was unused.
  gfx::GpuFenceHandle fence;
  if (end_access_desc.fenceCount) {
    wgpu::SharedFenceExportInfo export_info;
    wgpu::SharedFenceSyncFDExportInfo sync_fd_export_info;
    export_info.nextInChain = &sync_fd_export_info;
    end_access_desc.fences[0].ExportInfo(&export_info);
    // Dawn will close its FD when `end_access_desc` falls out of scope, and
    // so it is necessary to dup() it to give OzoneImageBacking an FD that it
    // can own.
    base::ScopedFD fd_handle_merged(
        HANDLE_EINTR(dup(sync_fd_export_info.handle)));
    if (!fd_handle_merged.is_valid()) {
      LOG(ERROR) << "Failed to duplicate Dawn's shared-image release fence";
      invalidate_access();
      return;
    }
    for (size_t i = 1; i < end_access_desc.fenceCount; i++) {
      auto& additional_fence = UNSAFE_TODO(end_access_desc.fences[i]);
      additional_fence.ExportInfo(&export_info);
      // The 'sync_merge' returns a new handle that is unowned. Wrap in scope
      // to ensure ownership.
      base::ScopedFD merged(HANDLE_EINTR(
          sync_merge("", fd_handle_merged.get(), sync_fd_export_info.handle)));
      if (!merged.is_valid()) {
        LOG(ERROR) << "Failed to merge Dawn's shared-image release fences";
        invalidate_access();
        return;
      }
      fd_handle_merged = std::move(merged);
    }
    // Avoid fence handle 'dup' by moving the scope.
    fence.Adopt(std::move(fd_handle_merged));
  }

  bool access_completed = false;
  if (access_was_unused) {
    // Dawn deliberately leaves the Vulkan layout state unset when the
    // WGPUTexture never reached its queue. No ownership transfer occurred,
    // so restore the exact state consumed at BeginAccess. Any fences here
    // are the acquire fences re-exported by Dawn, not new GPU work.
    access_completed =
        backing_access_->EndVulkanWithoutGpuUse(std::move(fence));
  } else {
    access_completed =
        backing_access_->EndVulkan(std::move(fence), external_state);
  }
  if (!access_completed) {
    LOG(ERROR) << "Dawn returned invalid external Vulkan ownership state";
  }
  backing_access_.reset();

  texture_.Destroy();
  texture_ = nullptr;
}

}  // namespace gpu
