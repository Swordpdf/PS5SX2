// PS5 port frontend: the Vulkan entry points the frontend uses, loaded into a table of its own.
//
// The frontend never declares the vk* prototypes (VK_NO_PROTOTYPES): the PS5 build links the
// driver and PCSX2's own loader keeps global function pointers of those names, so the frontend's
// pointers live in fe::Vk instead and are loaded from whatever vkGetInstanceProcAddr the platform
// hands it (the statically linked driver's vk_icdGetInstanceProcAddr on the PS5, libvulkan's on a PC).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#define FE_VK_GLOBAL_FUNCS(X) \
	X(vkCreateInstance) \
	X(vkEnumerateInstanceExtensionProperties)

#define FE_VK_INSTANCE_FUNCS(X) \
	X(vkDestroyInstance) \
	X(vkEnumeratePhysicalDevices) \
	X(vkGetPhysicalDeviceProperties) \
	X(vkGetPhysicalDeviceMemoryProperties) \
	X(vkGetPhysicalDeviceQueueFamilyProperties) \
	X(vkGetPhysicalDeviceFormatProperties) \
	X(vkEnumerateDeviceExtensionProperties) \
	X(vkCreateDevice) \
	X(vkGetDeviceProcAddr)

// Display and surface functions: loaded when present (the PS5 platform needs them, a PC preview
// doesn't).
#define FE_VK_INSTANCE_WSI_FUNCS(X) \
	X(vkGetPhysicalDeviceDisplayPropertiesKHR) \
	X(vkGetDisplayModePropertiesKHR) \
	X(vkGetPhysicalDeviceDisplayPlanePropertiesKHR) \
	X(vkGetDisplayPlaneSupportedDisplaysKHR) \
	X(vkCreateDisplayPlaneSurfaceKHR) \
	X(vkDestroySurfaceKHR) \
	X(vkGetPhysicalDeviceSurfaceSupportKHR) \
	X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
	X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
	X(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define FE_VK_DEVICE_FUNCS(X) \
	X(vkDestroyDevice) \
	X(vkGetDeviceQueue) \
	X(vkDeviceWaitIdle) \
	X(vkQueueSubmit) \
	X(vkQueueWaitIdle) \
	X(vkCreateCommandPool) \
	X(vkDestroyCommandPool) \
	X(vkAllocateCommandBuffers) \
	X(vkFreeCommandBuffers) \
	X(vkResetCommandBuffer) \
	X(vkBeginCommandBuffer) \
	X(vkEndCommandBuffer) \
	X(vkCreateFence) \
	X(vkDestroyFence) \
	X(vkWaitForFences) \
	X(vkResetFences) \
	X(vkCreateSemaphore) \
	X(vkDestroySemaphore) \
	X(vkCreateBuffer) \
	X(vkDestroyBuffer) \
	X(vkGetBufferMemoryRequirements) \
	X(vkAllocateMemory) \
	X(vkFreeMemory) \
	X(vkBindBufferMemory) \
	X(vkMapMemory) \
	X(vkUnmapMemory) \
	X(vkCreateImage) \
	X(vkDestroyImage) \
	X(vkGetImageMemoryRequirements) \
	X(vkBindImageMemory) \
	X(vkCreateImageView) \
	X(vkDestroyImageView) \
	X(vkCreateSampler) \
	X(vkDestroySampler) \
	X(vkCreateRenderPass) \
	X(vkDestroyRenderPass) \
	X(vkCreateFramebuffer) \
	X(vkDestroyFramebuffer) \
	X(vkCreateShaderModule) \
	X(vkDestroyShaderModule) \
	X(vkCreatePipelineLayout) \
	X(vkDestroyPipelineLayout) \
	X(vkCreateGraphicsPipelines) \
	X(vkDestroyPipeline) \
	X(vkCreateDescriptorSetLayout) \
	X(vkDestroyDescriptorSetLayout) \
	X(vkCreateDescriptorPool) \
	X(vkDestroyDescriptorPool) \
	X(vkAllocateDescriptorSets) \
	X(vkFreeDescriptorSets) \
	X(vkUpdateDescriptorSets) \
	X(vkCmdBeginRenderPass) \
	X(vkCmdEndRenderPass) \
	X(vkCmdBindPipeline) \
	X(vkCmdBindDescriptorSets) \
	X(vkCmdBindVertexBuffers) \
	X(vkCmdBindIndexBuffer) \
	X(vkCmdDraw) \
	X(vkCmdDrawIndexed) \
	X(vkCmdPushConstants) \
	X(vkCmdSetViewport) \
	X(vkCmdSetScissor) \
	X(vkCmdPipelineBarrier) \
	X(vkCmdCopyBufferToImage) \
	X(vkCmdCopyImageToBuffer)

#define FE_VK_DEVICE_WSI_FUNCS(X) \
	X(vkCreateSwapchainKHR) \
	X(vkDestroySwapchainKHR) \
	X(vkGetSwapchainImagesKHR) \
	X(vkAcquireNextImageKHR) \
	X(vkQueuePresentKHR)

namespace fe
{
struct Vk
{
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define FE_VK_DECLARE(name) PFN_##name name = nullptr;
	FE_VK_GLOBAL_FUNCS(FE_VK_DECLARE)
	FE_VK_INSTANCE_FUNCS(FE_VK_DECLARE)
	FE_VK_INSTANCE_WSI_FUNCS(FE_VK_DECLARE)
	FE_VK_DEVICE_FUNCS(FE_VK_DECLARE)
	FE_VK_DEVICE_WSI_FUNCS(FE_VK_DECLARE)
#undef FE_VK_DECLARE

	// Each returns false when a required entry point is missing (the name goes to *missing).
	bool LoadGlobal(PFN_vkGetInstanceProcAddr gipa, const char** missing);
	bool LoadInstance(VkInstance instance, bool wsi, const char** missing);
	bool LoadDevice(VkDevice device, bool wsi, const char** missing);
};
} // namespace fe
