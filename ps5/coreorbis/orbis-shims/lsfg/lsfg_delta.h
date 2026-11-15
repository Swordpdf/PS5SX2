// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): Eden's renderer_vulkan/present/lsfg_delta.h on lsfg_vk.h (lsfg_common.h says what changed:
// the namespace, no MemoryAllocator, the wrapper's handles).

#pragma once

#include <array>

#include "lsfg_common.h"

namespace lsfg {

class LsfgShaders;

constexpr size_t LSFG_DELTA_STAGES = 10;
constexpr size_t LSFG_DELTA_TEMPS = 3;

class LsfgDelta {
public:
    LsfgDelta() = default;
    LsfgDelta(const Device& device, const LsfgShaders& shaders,
              LsfgResources& resources, DescriptorPool& descriptor_pool,
              LsfgImageHistory& inputs, LsfgImage& flow_input, LsfgImage* previous_gamma,
              LsfgImage* previous1, LsfgImage* previous2);

    void Dispatch(CommandBuffer cmdbuf, u64 frame_count, size_t slot);

    [[nodiscard]] LsfgImage& Output1() {
        return out_image1;
    }

    [[nodiscard]] LsfgImage& Output2() {
        return out_image2;
    }

private:
    struct Generation {
        std::array<VkDescriptorSet, LSFG_HISTORY_SLOTS> first_descriptor_sets{};
        std::array<VkDescriptorSet, LSFG_HISTORY_SLOTS> sixth_descriptor_sets{};
        std::array<VkDescriptorSet, LSFG_DELTA_STAGES - 2> descriptor_sets{};
    };

    LsfgImageHistory* inputs{};
    LsfgImage* flow_input{};
    LsfgImage* previous_gamma{};
    LsfgImage* previous1{};
    LsfgImage* previous2{};

    std::array<LsfgPass, LSFG_DELTA_STAGES> passes;
    std::array<Generation, LSFG_GENERATION_SLOTS> generations{};
    DescriptorSets owned_sets;

    std::array<LsfgImage, LSFG_DELTA_TEMPS> temp1;
    LsfgImagePair temp2;
    LsfgImage out_image1;
    LsfgImage out_image2;
};

} // namespace lsfg
