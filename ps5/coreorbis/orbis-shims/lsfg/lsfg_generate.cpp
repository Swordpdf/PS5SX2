// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): Eden's renderer_vulkan/present/lsfg_generate.cpp on lsfg_vk.h (lsfg_common.h says what changed:
// the namespace, no MemoryAllocator, the wrapper's handles).

#include <vector>

#include "lossless_dll.h"
#include "lsfg_generate.h"
#include "lsfg_shaders.h"

namespace lsfg {

namespace {

constexpr u32 DISPATCH_TILE_SHIFT = 4;

[[nodiscard]] u32 GroupCount(u32 size) {
    return (size + (1u << DISPATCH_TILE_SHIFT) - 1) >> DISPATCH_TILE_SHIFT;
}

} // Anonymous namespace

LsfgGenerate::LsfgGenerate(const Device& device, const LsfgShaders& shaders,
                           LsfgResources& resources, DescriptorPool& descriptor_pool,
                           LsfgImagePair& frames_, LsfgImage& motion_, LsfgImage& detail1_,
                           LsfgImage& detail2_)
    : frames{&frames_}, motion{&motion_}, detail1{&detail1_}, detail2{&detail2_} {
    using namespace PerformanceShader;

    pass = LsfgPass(device, shaders, GENERATE,
                    {{1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
                     {2, VK_DESCRIPTOR_TYPE_SAMPLER},
                     {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
                     {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});

    sampler = resources.GetSampler();
    edge_sampler =
        resources.GetSampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_COMPARE_OP_ALWAYS, false);

    const std::vector<VkDescriptorSetLayout> layouts(
        LSFG_GENERATION_SLOTS * LSFG_MAX_TARGETS * 2, pass.SetLayout());
    owned_sets = AllocateDescriptorSets(device, *descriptor_pool, layouts);

    size_t next = 0;
    for (size_t slot = 0; slot < LSFG_GENERATION_SLOTS; ++slot) {
        Generation& target = generations[slot];
        target.buffer = resources.GetBuffer(LsfgSlotTimestamp(slot));

        for (auto& entry : target.targets) {
            for (auto& set : entry.descriptor_sets) {
                set = owned_sets[next++];
            }
        }
    }
}

void LsfgGenerate::SetTarget(const Device& device, size_t slot, u32 target, VkImageView view) {
    Target& entry = generations[slot].targets[target];
    if (entry.view == view) {
        return;
    }
    entry.view = view;

    for (size_t i = 0; i < entry.descriptor_sets.size(); ++i) {
        LsfgDescriptorWriter(entry.descriptor_sets[i])
            .AddUniformBuffer(generations[slot].buffer, LsfgResources::BufferSize())
            .AddSampler(sampler)
            .AddSampler(edge_sampler)
            .AddSampledImage((*frames)[1 - i])
            .AddSampledImage((*frames)[i])
            .AddSampledImage(*motion)
            .AddSampledImage(*detail1)
            .AddSampledImage(*detail2)
            .AddStorageView(view)
            .Build(device);
    }
}

void LsfgGenerate::Dispatch(CommandBuffer cmdbuf, u64 frame_count, size_t slot, u32 target,
                            VkImage image, VkExtent2D extent) {
    const Target& entry = generations[slot].targets[target];

    LsfgBarriers(cmdbuf)
        .WriteToReadAll(*frames)
        .WriteToRead(*motion)
        .WriteToRead(*detail1)
        .WriteToRead(*detail2)
        .DiscardToWrite(image)
        .Build();

    pass.Bind(cmdbuf, entry.descriptor_sets[frame_count % entry.descriptor_sets.size()]);
    cmdbuf.Dispatch(GroupCount(extent.width), GroupCount(extent.height), 1);
    // PS5SX2: Eden's barrier for its swapchain image (to colour attachment / transfer) is left to the caller: PS5SX2
    // samples the generated frame in its present pass (GSDeviceVK.cpp moves it to ShaderReadOnly).
}

} // namespace lsfg
