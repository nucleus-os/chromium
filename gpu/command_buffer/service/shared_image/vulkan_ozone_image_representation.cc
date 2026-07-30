// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/command_buffer/service/shared_image/vulkan_ozone_image_representation.h"

#if BUILDFLAG(ENABLE_VULKAN)

#include "base/logging.h"
#include "gpu/command_buffer/service/shared_image/ozone_image_backing.h"

namespace gpu {

VulkanOzoneImageRepresentation::VulkanOzoneImageRepresentation(
    SharedImageManager* manager,
    SharedImageBacking* backing,
    MemoryTypeTracker* tracker,
    std::unique_ptr<gpu::VulkanImage> vulkan_image,
    gpu::VulkanDeviceQueue* vulkan_device_queue,
    gpu::VulkanImplementation& vulkan_impl)
    : VulkanImageRepresentation(manager,
                                backing,
                                tracker,
                                std::move(vulkan_image),
                                vulkan_device_queue,
                                vulkan_impl) {}

VulkanOzoneImageRepresentation::~VulkanOzoneImageRepresentation() = default;

bool VulkanOzoneImageRepresentation::BeginAccess(
    AccessMode access_mode,
    std::vector<VkSemaphore>& begin_semaphores,
    std::vector<VkSemaphore>& end_semaphores) {
  backing_access_ =
      ozone_backing()->BeginAccess(access_mode == AccessMode::kRead,
                                   OzoneImageBacking::AccessStream::kVulkan);
  if (!backing_access_) {
    return false;
  }
  if (backing_access_->has_external_vulkan_state()) {
    // The legacy raw-Vulkan representation does not expose the layout pair
    // recorded by its caller. Fail closed rather than leaving a consumed
    // ownership transfer attached to the backing.
    LOG(ERROR) << "Raw Vulkan cannot acquire tracked external ownership";
    backing_access_.reset();
    return false;
  }

  if (backing_access_->needs_end_fence()) {
    VkSemaphore end_semaphore = vulkan_impl_->CreateExternalSemaphore(
        vulkan_device_queue_->GetVulkanDevice());
    if (end_semaphore == VK_NULL_HANDLE) {
      backing_access_.reset();
      return false;
    }
    end_semaphores.emplace_back(end_semaphore);
  }

  std::vector<gfx::GpuFenceHandle> fences = backing_access_->TakeBeginFences();
  for (auto& fence : fences) {
    VkSemaphore begin_semaphore = vulkan_impl_->ImportSemaphoreHandle(
        vulkan_device_queue_->GetVulkanDevice(),
        SemaphoreHandle(std::move(fence)));
    if (begin_semaphore == VK_NULL_HANDLE) {
      backing_access_.reset();
      return false;
    }
    begin_semaphores.emplace_back(begin_semaphore);
  }

  backing_access_->CommitAcquire();
  return true;
}

void VulkanOzoneImageRepresentation::EndAccess(bool is_read_only,
                                               VkSemaphore end_semaphore) {
  gfx::GpuFenceHandle fence;
  if (end_semaphore != VK_NULL_HANDLE) {
    fence =
        std::move(vulkan_impl_->GetSemaphoreHandle(
                      vulkan_device_queue_->GetVulkanDevice(), end_semaphore))
            .ToGpuFenceHandle();
  }
  backing_access_->End(std::move(fence));
  backing_access_.reset();
}

}  // namespace gpu

#endif
