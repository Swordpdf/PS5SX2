// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): Eden's renderer_vulkan/present/lsfg_chain.cpp on lsfg_vk.h (lsfg_common.h says what changed:
// the namespace, no MemoryAllocator, the wrapper's handles).

#include <algorithm>

#include "lsfg_chain.h"
#include "lsfg_shaders.h"

namespace lsfg {

namespace {

constexpr u32 FIXED_DESCRIPTOR_SETS = 64;
constexpr u32 DESCRIPTOR_SETS_PER_SLOT = 112;
constexpr size_t FIRST_DELTA_LEVEL = 4;

} // Anonymous namespace

LsfgChain::LsfgChain(const Device& device,
                     const LsfgShaders& shaders, VkExtent2D extent, VkFormat format,
                     f32 flow_scale)
    : resources{device, flow_scale},
      descriptor_pool{CreateLsfgDescriptorPool(
          device, FIXED_DESCRIPTOR_SETS +
                      DESCRIPTOR_SETS_PER_SLOT * static_cast<u32>(LSFG_GENERATION_SLOTS))} {
    // PS5SX2: the input frames are drawn into by PCSX2's present (colour attachments in `format`, the swapchain's) and
    // only sampled by the passes; the black 1x1 image stands for a missing one (lsfg_common.h).
    for (auto& image : frames) {
        image = LsfgImage(device, extent, format,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }
    null_image = LsfgImage(device, VkExtent2D{1, 1}, LSFG_DEFAULT_FORMAT,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    LsfgDescriptorWriter::SetNullView(null_image.View());

    mipmaps = LsfgMipmaps(device, shaders, resources, descriptor_pool, frames,
                          flow_scale);

    alpha_passes = LsfgAlphaPasses(device, shaders);
    for (size_t i = 0; i < LSFG_MIP_LEVELS; ++i) {
        alpha[i] = LsfgAlpha(device, alpha_passes, resources, descriptor_pool,
                             mipmaps.Output(i));
    }

    beta = LsfgBeta(device, shaders, resources, descriptor_pool,
                    alpha[0].Outputs());

    for (size_t i = 0; i < LSFG_MIP_LEVELS; ++i) {
        const size_t level = LSFG_MIP_LEVELS - 1 - i;
        gamma[i] = LsfgGamma(device, shaders, resources, descriptor_pool,
                             alpha[level].Outputs(),
                             beta.Output(std::min(level, LSFG_BETA_OUTPUTS - 1)),
                             i == 0 ? nullptr : &gamma[i - 1].Output());

        if (i < FIRST_DELTA_LEVEL) {
            continue;
        }

        const size_t index = i - FIRST_DELTA_LEVEL;
        delta[index] = LsfgDelta(
            device, shaders, resources, descriptor_pool, alpha[level].Outputs(),
            beta.Output(level), i == FIRST_DELTA_LEVEL ? nullptr : &gamma[i - 1].Output(),
            i == FIRST_DELTA_LEVEL ? nullptr : &delta[index - 1].Output1(),
            i == FIRST_DELTA_LEVEL ? nullptr : &delta[index - 1].Output2());
    }

    generate = LsfgGenerate(device, shaders, resources, descriptor_pool, frames,
                            gamma[LSFG_MIP_LEVELS - 1].Output(),
                            delta[LSFG_DELTA_INSTANCES - 1].Output1(),
                            delta[LSFG_DELTA_INSTANCES - 1].Output2());
    LsfgDescriptorWriter::SetNullView(VK_NULL_HANDLE);
}

void LsfgChain::Prepare(CommandBuffer cmdbuf) {
    if (prepared || null_image.Handle() == VK_NULL_HANDLE) {
        return;
    }
    prepared = true;
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = null_image.Handle();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {},
                           std::span<const VkImageMemoryBarrier>(&barrier, 1));
    const VkClearColorValue black = {};
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(*cmdbuf, null_image.Handle(), VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {},
                           std::span<const VkImageMemoryBarrier>(&barrier, 1));
    null_image.SetLayout(VK_IMAGE_LAYOUT_GENERAL);
}

void LsfgChain::DispatchShared(CommandBuffer cmdbuf, u64 frame_count) {
    mipmaps.Dispatch(cmdbuf, frame_count);

    for (size_t stage = 0; stage < LSFG_ALPHA_STAGES; ++stage) {
        LsfgBarriers barriers(cmdbuf);
        for (auto& level : alpha) {
            level.PushBarriers(barriers, frame_count, stage);
        }
        barriers.Build();

        alpha_passes.Get(stage).BindPipeline(cmdbuf);
        for (auto& level : alpha) {
            level.DispatchStage(cmdbuf, frame_count, stage);
        }
    }

    beta.Dispatch(cmdbuf, frame_count);
}

void LsfgChain::DispatchGeneration(CommandBuffer cmdbuf, u64 frame_count,
                                   size_t generation_count, size_t generation, u32 target,
                                   VkImage image, VkExtent2D extent) {
    const size_t slot = LsfgGenerationSlot(generation_count, generation);
    for (size_t i = 0; i < LSFG_MIP_LEVELS; ++i) {
        gamma[i].Dispatch(cmdbuf, frame_count, slot);
        if (i >= FIRST_DELTA_LEVEL) {
            delta[i - FIRST_DELTA_LEVEL].Dispatch(cmdbuf, frame_count, slot);
        }
    }
    generate.Dispatch(cmdbuf, frame_count, slot, target, image, extent);
}

} // namespace lsfg
