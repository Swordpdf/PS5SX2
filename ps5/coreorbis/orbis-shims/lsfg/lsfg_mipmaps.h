// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): Eden's renderer_vulkan/present/lsfg_mipmaps.h on lsfg_vk.h (lsfg_common.h says what changed:
// the namespace, no MemoryAllocator, the wrapper's handles).

#pragma once

#include <array>

#include "lsfg_common.h"

namespace lsfg {

class LsfgShaders;

constexpr size_t LSFG_MIP_LEVELS = 7;

class LsfgMipmaps {
public:
    LsfgMipmaps() = default;
    LsfgMipmaps(const Device& device, const LsfgShaders& shaders,
                LsfgResources& resources, DescriptorPool& descriptor_pool,
                LsfgImagePair& frames, f32 flow_scale);

    void Dispatch(CommandBuffer cmdbuf, u64 frame_count);

    [[nodiscard]] LsfgImage& Output(size_t level) {
        return out_images[level];
    }

private:
    LsfgImagePair* frames{};

    LsfgPass pass;
    std::array<VkDescriptorSet, 2> descriptor_sets{};
    DescriptorSets owned_sets;

    VkExtent2D flow_extent{};
    std::array<LsfgImage, LSFG_MIP_LEVELS> out_images;
};

} // namespace lsfg
