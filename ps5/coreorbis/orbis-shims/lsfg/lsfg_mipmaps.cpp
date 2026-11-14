// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): Eden's renderer_vulkan/present/lsfg_mipmaps.cpp on lsfg_vk.h (lsfg_common.h says what changed:
// the namespace, no MemoryAllocator, the wrapper's handles).

#include <algorithm>
#include <vector>

#include "lossless_dll.h"
#include "lsfg_mipmaps.h"
#include "lsfg_shaders.h"

namespace lsfg {

namespace {

constexpr u32 DISPATCH_TILE_SHIFT = 6;

[[nodiscard]] u32 GroupCount(u32 size) {
    return (size + (1u << DISPATCH_TILE_SHIFT) - 1) >> DISPATCH_TILE_SHIFT;
}

} // Anonymous namespace

LsfgMipmaps::LsfgMipmaps(const Device& device,
                         const LsfgShaders& shaders, LsfgResources& resources,
                         DescriptorPool& descriptor_pool, LsfgImagePair& frames_,
                         f32 flow_scale)
    : frames{&frames_} {
    using namespace PerformanceShader;

    pass = LsfgPass(device, shaders, MIPMAPS,
                    {{1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
                     {1, VK_DESCRIPTOR_TYPE_SAMPLER},
                     {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
                     {LSFG_MIP_LEVELS, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}});

    const VkExtent2D input_extent = (*frames)[0].Extent();
    flow_extent = VkExtent2D{
        .width = std::max(1u, static_cast<u32>(static_cast<f32>(input_extent.width) * flow_scale)),
        .height = std::max(1u, static_cast<u32>(static_cast<f32>(input_extent.height) * flow_scale)),
    };

    for (size_t i = 0; i < LSFG_MIP_LEVELS; ++i) {
        const VkExtent2D level_extent{
            .width = flow_extent.width >> i,
            .height = flow_extent.height >> i,
        };
        out_images[i] = LsfgImage(device, level_extent, LsfgFlowFormat());
    }

    const std::vector<VkDescriptorSetLayout> layouts(descriptor_sets.size(), pass.SetLayout());
    owned_sets = AllocateDescriptorSets(device, *descriptor_pool, layouts);

    const VkSampler sampler = resources.GetSampler();
    const VkBuffer buffer = resources.GetBuffer();

    for (size_t i = 0; i < descriptor_sets.size(); ++i) {
        descriptor_sets[i] = owned_sets[i];
        LsfgDescriptorWriter(descriptor_sets[i])
            .AddUniformBuffer(buffer, LsfgResources::BufferSize())
            .AddSampler(sampler)
            .AddSampledImage((*frames)[i])
            .AddStorageImages(out_images)
            .Build(device);
    }
}

void LsfgMipmaps::Dispatch(CommandBuffer cmdbuf, u64 frame_count) {
    const size_t slot = frame_count % descriptor_sets.size();

    LsfgBarriers(cmdbuf).WriteToRead((*frames)[slot]).ReadToWriteAll(out_images).Build();

    pass.Bind(cmdbuf, descriptor_sets[slot]);
    cmdbuf.Dispatch(GroupCount(flow_extent.width), GroupCount(flow_extent.height), 1);
}

} // namespace lsfg
