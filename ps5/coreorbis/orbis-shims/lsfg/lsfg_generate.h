// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): Eden's renderer_vulkan/present/lsfg_generate.h on lsfg_vk.h (lsfg_common.h says what changed:
// the namespace, no MemoryAllocator, the wrapper's handles).

#pragma once

#include <array>

#include "lsfg_common.h"

namespace lsfg {

class LsfgShaders;

class LsfgGenerate {
public:
    LsfgGenerate() = default;
    LsfgGenerate(const Device& device, const LsfgShaders& shaders, LsfgResources& resources,
                 DescriptorPool& descriptor_pool, LsfgImagePair& frames, LsfgImage& motion,
                 LsfgImage& detail1, LsfgImage& detail2);

    void SetTarget(const Device& device, size_t slot, u32 target, VkImageView view);

    void Dispatch(CommandBuffer cmdbuf, u64 frame_count, size_t slot, u32 target,
                  VkImage image, VkExtent2D extent);

private:
    struct Target {
        std::array<VkDescriptorSet, 2> descriptor_sets{};
        VkImageView view{};
    };

    struct Generation {
        std::array<Target, LSFG_MAX_TARGETS> targets{};
        VkBuffer buffer{};
    };

    LsfgImagePair* frames{};
    LsfgImage* motion{};
    LsfgImage* detail1{};
    LsfgImage* detail2{};
    VkSampler sampler{};
    VkSampler edge_sampler{};

    LsfgPass pass;
    std::array<Generation, LSFG_GENERATION_SLOTS> generations{};
    DescriptorSets owned_sets;
};

} // namespace lsfg
