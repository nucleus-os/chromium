// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/command_buffer/service/shared_image/skia_vk_ozone_image_representation.h"

#include <utility>

#include "components/viz/common/gpu/vulkan_context_provider.h"
#include "components/viz/common/resources/shared_image_format_utils.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/command_buffer/service/memory_tracking.h"
#include "gpu/command_buffer/service/shared_context_state.h"
#include "gpu/command_buffer/service/shared_image/shared_image_format_service_utils.h"
#include "gpu/command_buffer/service/shared_image/shared_image_representation.h"
#include "gpu/command_buffer/service/skia_utils.h"
#include "gpu/command_buffer/service/texture_manager.h"
#include "gpu/vulkan/vulkan_device_queue.h"
#include "gpu/vulkan/vulkan_fence_helper.h"
#include "gpu/vulkan/vulkan_function_pointers.h"
#include "gpu/vulkan/vulkan_image.h"
#include "gpu/vulkan/vulkan_implementation.h"
#include "gpu/vulkan/vulkan_util.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkColorType.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/gpu/MutableTextureState.h"
#include "third_party/skia/include/gpu/ganesh/GrBackendSemaphore.h"
#include "third_party/skia/include/gpu/ganesh/GrBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/SkSurfaceGanesh.h"
#include "third_party/skia/include/gpu/ganesh/vk/GrVkBackendSemaphore.h"
#include "third_party/skia/include/gpu/ganesh/vk/GrVkBackendSurface.h"
#include "third_party/skia/include/gpu/vk/VulkanMutableTextureState.h"
#include "third_party/skia/include/private/chromium/GrPromiseImageTexture.h"

namespace gpu {

// Vk backed Skia representation of OzoneImageBacking.
SkiaVkOzoneImageRepresentation::SkiaVkOzoneImageRepresentation(
    SharedImageManager* manager,
    OzoneImageBacking* backing,
    scoped_refptr<SharedContextState> context_state,
    std::vector<std::unique_ptr<VulkanImage>> vulkan_images,
    MemoryTypeTracker* tracker)
    : SkiaGaneshImageRepresentation(context_state->gr_context(),
                                    manager,
                                    backing,
                                    tracker),
      vulkan_images_(std::move(vulkan_images)),
      context_state_(std::move(context_state)) {
  DCHECK(backing);
  DCHECK(context_state_);
  DCHECK(context_state_->vk_context_provider());
  CHECK(!vulkan_images_.empty());

  for (const auto& vulkan_image : vulkan_images_) {
    CHECK(vulkan_image);
    auto promise_texture =
        GrPromiseImageTexture::Make(GrBackendTextures::MakeVk(
            vulkan_image->size().width(), vulkan_image->size().height(),
            CreateGrVkImageInfo(vulkan_image.get(), format(), color_space())));
    if (!promise_texture) {
      LOG(ERROR) << "Unable to create GrPromiseImageTexture";
      promise_textures_.clear();
      break;
    }
    promise_textures_.push_back(std::move(promise_texture));
  }
}

SkiaVkOzoneImageRepresentation::~SkiaVkOzoneImageRepresentation() {
  DCHECK_EQ(mode_, RepresentationAccessMode::kNone);
  surfaces_.clear();
  VulkanFenceHelper* fence_helper =
      context_state_->vk_context_provider()->GetDeviceQueue()->GetFenceHelper();
  for (auto& vulkan_image : vulkan_images_) {
    fence_helper->EnqueueVulkanObjectCleanupForSubmittedWork(
        std::move(vulkan_image));
  }
}

std::vector<sk_sp<SkSurface>> SkiaVkOzoneImageRepresentation::BeginWriteAccess(
    int final_msaa_count,
    const SkSurfaceProps& surface_props,
    const gfx::Rect& update_rect,
    std::vector<GrBackendSemaphore>* begin_semaphores,
    std::vector<GrBackendSemaphore>* end_semaphores,
    std::unique_ptr<skgpu::MutableTextureState>* end_state) {
  DCHECK_EQ(mode_, RepresentationAccessMode::kNone);

  if (!BeginAccess(/*readonly=*/false, begin_semaphores, end_semaphores)) {
    return {};
  }

  auto* gr_context = context_state_->gr_context();
  if (gr_context->abandoned()) {
    LOG(ERROR) << "GrContext is abandoned.";
    backing_access_.reset();
    ResetSemaphores();
    explicit_vulkan_access_ = false;
    external_queue_family_ = VK_QUEUE_FAMILY_IGNORED;
    mode_ = RepresentationAccessMode::kNone;
    return {};
  }

  if (surfaces_.empty() || final_msaa_count != surface_msaa_count_ ||
      surface_props != surfaces_.front()->props()) {
    surfaces_.clear();
    for (int plane = 0; plane < format().NumberOfPlanes(); plane++) {
      const auto& promise_texture = promise_textures_[plane];
      DCHECK(promise_texture);
      // External sampler is not supported with WriteAccess.
      SkColorType sk_color_type = viz::ToClosestSkColorType(format(), plane);
      auto surface = SkSurfaces::WrapBackendTexture(
          gr_context, promise_texture->backendTexture(), surface_origin(),
          final_msaa_count, sk_color_type, color_space().ToSkColorSpace(),
          &surface_props);
      if (!surface) {
        LOG(ERROR) << "MakeFromBackendTexture() failed.";
        backing_access_.reset();
        ResetSemaphores();
        explicit_vulkan_access_ = false;
        external_queue_family_ = VK_QUEUE_FAMILY_IGNORED;
        mode_ = RepresentationAccessMode::kNone;
        return {};
      }
      surfaces_.push_back(std::move(surface));
    }
    surface_msaa_count_ = final_msaa_count;
  }

  *end_state = GetEndAccessState();
  if (!backing_access_->acquire_committed()) {
    backing_access_->CommitAcquire();
  }

  return surfaces_;
}

std::vector<sk_sp<GrPromiseImageTexture>>
SkiaVkOzoneImageRepresentation::BeginWriteAccess(
    std::vector<GrBackendSemaphore>* begin_semaphores,
    std::vector<GrBackendSemaphore>* end_semaphores,
    std::unique_ptr<skgpu::MutableTextureState>* end_state) {
  DCHECK_EQ(mode_, RepresentationAccessMode::kNone);

  if (!BeginAccess(/*readonly=*/false, begin_semaphores, end_semaphores)) {
    return {};
  }

  *end_state = GetEndAccessState();
  if (!backing_access_->acquire_committed()) {
    backing_access_->CommitAcquire();
  }

  return promise_textures_;
}

void SkiaVkOzoneImageRepresentation::EndWriteAccess() {
  DCHECK_EQ(mode_, RepresentationAccessMode::kWrite);
  for (const auto& surface : surfaces_) {
    DCHECK(surface->unique());
  }
  EndAccess(/*readonly=*/false);
  surfaces_.clear();
}

std::vector<sk_sp<GrPromiseImageTexture>>
SkiaVkOzoneImageRepresentation::BeginReadAccess(
    std::vector<GrBackendSemaphore>* begin_semaphores,
    std::vector<GrBackendSemaphore>* end_semaphores,
    std::unique_ptr<skgpu::MutableTextureState>* end_state) {
  DCHECK_EQ(mode_, RepresentationAccessMode::kNone);
  DCHECK(surfaces_.empty());

  if (!BeginAccess(/*readonly=*/true, begin_semaphores, end_semaphores)) {
    return {};
  }

  *end_state = GetEndAccessState();
  if (!backing_access_->acquire_committed()) {
    backing_access_->CommitAcquire();
  }

  return promise_textures_;
}

void SkiaVkOzoneImageRepresentation::EndReadAccess() {
  DCHECK_EQ(mode_, RepresentationAccessMode::kRead);
  DCHECK(surfaces_.empty());

  EndAccess(/*readonly=*/true);
}

gpu::VulkanImplementation* SkiaVkOzoneImageRepresentation::vk_implementation() {
  return context_state_->vk_context_provider()->GetVulkanImplementation();
}

VkDevice SkiaVkOzoneImageRepresentation::vk_device() {
  return context_state_->vk_context_provider()
      ->GetDeviceQueue()
      ->GetVulkanDevice();
}

bool SkiaVkOzoneImageRepresentation::BeginAccess(
    bool readonly,
    std::vector<GrBackendSemaphore>* begin_semaphores,
    std::vector<GrBackendSemaphore>* end_semaphores) {
  DCHECK(begin_semaphores);
  DCHECK(end_access_semaphore_ == VK_NULL_HANDLE);

  backing_access_ = ozone_backing()->BeginAccess(
      readonly, OzoneImageBacking::AccessStream::kVulkan);
  if (!backing_access_) {
    return false;
  }

  VkDevice device = vk_device();
  auto* implementation = vk_implementation();

  std::vector<gfx::GpuFenceHandle> fences = backing_access_->TakeBeginFences();
  explicit_vulkan_access_ = NeedsExternalOwnershipTransfer();
  if (explicit_vulkan_access_ &&
      external_queue_family_ == VK_QUEUE_FAMILY_IGNORED) {
    LOG(ERROR) << "Cannot determine one external Vulkan queue family for all "
                  "Ganesh image planes";
    backing_access_.reset();
    explicit_vulkan_access_ = false;
    return false;
  }
  std::optional<ExternalVulkanImageState> external_state;
  if (explicit_vulkan_access_) {
    if (!backing_access_->TakeExternalVulkanImageState(&external_state)) {
      backing_access_.reset();
      explicit_vulkan_access_ = false;
      external_queue_family_ = VK_QUEUE_FAMILY_IGNORED;
      return false;
    }
    if (external_state &&
        (!external_state->IsValid() ||
         external_state->external_queue_family != external_queue_family_)) {
      LOG(ERROR) << "Invalid external Vulkan layout state for Ganesh";
      backing_access_.reset();
      explicit_vulkan_access_ = false;
      external_queue_family_ = VK_QUEUE_FAMILY_IGNORED;
      return false;
    }
    if (!external_state && IsCleared()) {
      if (ozone_backing()->has_vulkan_ownership_history_) {
        LOG(ERROR) << "Initialized Ozone image lost its Vulkan ownership state";
        backing_access_.reset();
        explicit_vulkan_access_ = false;
        external_queue_family_ = VK_QUEUE_FAMILY_IGNORED;
        return false;
      }
      // Preserve an initialized native pixmap on its first Vulkan acquire.
      // Once Vulkan has owned it, EndVulkan() records the exact state and this
      // bootstrap path is no longer permitted.
      external_state = ExternalVulkanImageState{
          .old_layout = VK_IMAGE_LAYOUT_GENERAL,
          .new_layout = VK_IMAGE_LAYOUT_GENERAL,
          .external_queue_family = external_queue_family_};
    }
  }

  std::vector<GrBackendSemaphore> imported_semaphores;
  for (auto& fence : fences) {
    VkSemaphore vk_semaphore = implementation->ImportSemaphoreHandle(
        device, SemaphoreHandle(std::move(fence)));
    if (vk_semaphore == VK_NULL_HANDLE) {
      backing_access_.reset();
      ResetSemaphores();
      explicit_vulkan_access_ = false;
      external_queue_family_ = VK_QUEUE_FAMILY_IGNORED;
      return false;
    }

    begin_access_semaphores_.emplace_back(vk_semaphore);
    imported_semaphores.emplace_back(GrBackendSemaphores::MakeVk(vk_semaphore));
  }

  if (explicit_vulkan_access_) {
    GrDirectContext* gr_context = context_state_->gr_context();
    if (!imported_semaphores.empty() &&
        !gr_context->wait(imported_semaphores.size(),
                          imported_semaphores.data(),
                          /*deleteSemaphoresAfterWait=*/false)) {
      backing_access_.reset();
      ResetSemaphores();
      explicit_vulkan_access_ = false;
      external_queue_family_ = VK_QUEUE_FAMILY_IGNORED;
      return false;
    }

    // Ganesh accepted the wait. A later failure cannot restore the producer's
    // transfer as though no acquire work had been recorded.
    backing_access_->CommitAcquire();
    if (external_state) {
      const uint32_t local_queue_family = context_state_->vk_context_provider()
                                              ->GetDeviceQueue()
                                              ->GetVulkanQueueIndex();
      const skgpu::MutableTextureState acquire_state =
          skgpu::MutableTextureStates::MakeVulkan(external_state->new_layout,
                                                  local_queue_family);
      for (const auto& promise_texture : promise_textures_) {
        GrBackendTexture backend_texture = promise_texture->backendTexture();
        backend_texture.setMutableState(skgpu::MutableTextureStates::MakeVulkan(
            external_state->old_layout, external_state->external_queue_family));
        if (!gr_context->setBackendTextureState(backend_texture,
                                                acquire_state)) {
          LOG(ERROR) << "Failed to record external Vulkan acquire for Ganesh";
          backing_access_->InvalidateAfterAcquire();
          backing_access_.reset();
          ResetSemaphores();
          explicit_vulkan_access_ = false;
          external_queue_family_ = VK_QUEUE_FAMILY_IGNORED;
          return false;
        }
      }
    }
  } else {
    begin_semaphores->insert(begin_semaphores->end(),
                             imported_semaphores.begin(),
                             imported_semaphores.end());
  }

  if (end_semaphores && backing_access_->needs_end_fence()) {
    end_access_semaphore_ =
        vk_implementation()->CreateExternalSemaphore(vk_device());

    if (end_access_semaphore_ == VK_NULL_HANDLE) {
      DLOG(ERROR) << "Failed to create the external semaphore.";
      backing_access_.reset();
      ResetSemaphores();
      explicit_vulkan_access_ = false;
      external_queue_family_ = VK_QUEUE_FAMILY_IGNORED;
      return false;
    }

    end_semaphores->emplace_back(
        GrBackendSemaphores::MakeVk(end_access_semaphore_));
  }

  mode_ = readonly ? RepresentationAccessMode::kRead
                   : RepresentationAccessMode::kWrite;
  return true;
}

void SkiaVkOzoneImageRepresentation::EndAccess(bool readonly) {
  gfx::GpuFenceHandle fence;
  if (end_access_semaphore_ != VK_NULL_HANDLE) {
    SemaphoreHandle semaphore_handle = vk_implementation()->GetSemaphoreHandle(
        vk_device(), end_access_semaphore_);
    fence = std::move(semaphore_handle).ToGpuFenceHandle();
    DLOG_IF(ERROR, fence.is_null())
        << "Failed to convert the external semaphore to fence.";
  }

  if (explicit_vulkan_access_) {
    std::optional<ExternalVulkanImageState> release_state =
        GetReleaseVulkanState();
    if (release_state) {
      backing_access_->EndVulkan(std::move(fence), *release_state);
    } else {
      LOG(ERROR) << "Ganesh did not return exact Vulkan ownership state";
      backing_access_->InvalidateAfterAcquire();
    }
  } else {
    backing_access_->End(std::move(fence));
  }
  backing_access_.reset();

  ResetSemaphores();
  explicit_vulkan_access_ = false;
  external_queue_family_ = VK_QUEUE_FAMILY_IGNORED;
  mode_ = RepresentationAccessMode::kNone;
}

void SkiaVkOzoneImageRepresentation::ResetSemaphores() {
  std::vector<VkSemaphore> semaphores = std::move(begin_access_semaphores_);
  begin_access_semaphores_.clear();
  if (end_access_semaphore_ != VK_NULL_HANDLE) {
    semaphores.emplace_back(end_access_semaphore_);
    end_access_semaphore_ = VK_NULL_HANDLE;
  }
  if (!semaphores.empty()) {
    VulkanFenceHelper* fence_helper = context_state_->vk_context_provider()
                                          ->GetDeviceQueue()
                                          ->GetFenceHelper();
    fence_helper->EnqueueSemaphoresCleanupForSubmittedWork(
        std::move(semaphores));
  }
}

std::unique_ptr<skgpu::MutableTextureState>
SkiaVkOzoneImageRepresentation::GetEndAccessState() {
  if (!explicit_vulkan_access_) {
    return nullptr;
  }

  return std::make_unique<skgpu::MutableTextureState>(
      skgpu::MutableTextureStates::MakeVulkan(VK_IMAGE_LAYOUT_UNDEFINED,
                                              external_queue_family_));
}

bool SkiaVkOzoneImageRepresentation::NeedsExternalOwnershipTransfer() {
  // `kSingleDeviceUsage` defines the set of usages for which only the Vulkan
  // device from SharedContextState is used. If the SI has any usages outside
  // this set (e.g., if it has any GLES2 usage), then it will be accessed
  // beyond the Vulkan device from SharedContextState and hence does not have
  // single-device usage.
  const SharedImageUsageSet kSingleDeviceUsage =
      SHARED_IMAGE_USAGE_DISPLAY_READ | SHARED_IMAGE_USAGE_DISPLAY_WRITE |
      SHARED_IMAGE_USAGE_RASTER_READ | SHARED_IMAGE_USAGE_RASTER_WRITE;

  // If SharedImage is used outside of current VkDeviceQueue we need to transfer
  // image back to it's original queue. Note, that for multithreading we use
  // same vkDevice, so technically we could transfer between queues instead of
  // jumping to external queue. But currently it's not possible because we
  // create new vkImage each time.
  if (!kSingleDeviceUsage.HasAll(ozone_backing()->usage()) ||
      ozone_backing()->is_thread_safe()) {
    uint32_t queue_family_index = vulkan_images_.front()->queue_family_index();
    // All VkImages must be allocated for the same queue family.
    for (const auto& vulkan_image : vulkan_images_) {
      if (vulkan_image->queue_family_index() != queue_family_index) {
        external_queue_family_ = VK_QUEUE_FAMILY_IGNORED;
        return true;
      }
    }
    if (queue_family_index == VK_QUEUE_FAMILY_IGNORED) {
      external_queue_family_ = VK_QUEUE_FAMILY_IGNORED;
      return true;
    }
    external_queue_family_ = queue_family_index;
    return true;
  }
  external_queue_family_ = VK_QUEUE_FAMILY_IGNORED;
  return false;
}

std::optional<ExternalVulkanImageState>
SkiaVkOzoneImageRepresentation::GetReleaseVulkanState() const {
  std::optional<ExternalVulkanImageState> release_state;
  for (const auto& promise_texture : promise_textures_) {
    GrVkImageInfo image_info;
    if (!GrBackendTextures::GetVkImageInfo(promise_texture->backendTexture(),
                                           &image_info)) {
      return std::nullopt;
    }
    ExternalVulkanImageState plane_state{
        .old_layout = image_info.fImageLayout,
        .new_layout = image_info.fImageLayout,
        .external_queue_family = image_info.fCurrentQueueFamily,
    };
    if (!plane_state.IsValid() ||
        plane_state.external_queue_family != external_queue_family_) {
      return std::nullopt;
    }
    if (release_state && *release_state != plane_state) {
      return std::nullopt;
    }
    release_state = plane_state;
  }
  return release_state;
}

}  // namespace gpu
