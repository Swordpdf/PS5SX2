// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): Eden's renderer_vulkan/present/lsfg_beta.h on lsfg_vk.h (lsfg_common.h says what changed:
// the namespace, no MemoryAllocator, the wrapper's handles).

#pragma once

#include <array>

#include "lsfg_common.h"

namespace lsfg {

class LsfgShaders;

constexpr size_t LSFG_BETA_STAGES = 5;
constexpr size_t LSFG_BETA_OUTPUTS = 6;

class LsfgBeta {
public:
    LsfgBeta() = default;
    LsfgBeta(const Device& device, const LsfgShaders& shaders,
             LsfgResources& resources, DescriptorPool& descriptor_pool,
             LsfgImageHistory& inputs);

    void Dispatch(CommandBuffer cmdbuf, u64 frame_count);

    [[nodiscard]] LsfgImage& Output(size_t level) {
        return out_images[level];
    }

private:
    LsfgImageHistory* inputs{};

    std::array<LsfgPass, LSFG_BETA_STAGES> passes;
    std::array<VkDescriptorSet, LSFG_HISTORY_SLOTS> first_descriptor_sets{};
    std::array<VkDescriptorSet, LSFG_BETA_STAGES - 1> descriptor_sets{};
    DescriptorSets owned_sets;

    LsfgImagePair temp1;
    LsfgImagePair temp2;
    std::array<LsfgImage, LSFG_BETA_OUTPUTS> out_images;
};

} // namespace lsfg
