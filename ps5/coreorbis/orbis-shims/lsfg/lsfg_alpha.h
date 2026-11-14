// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2025 lsfg-vk
// SPDX-License-Identifier: GPL-3.0-or-later

// PS5SX2 (2026-11-14, AI-assisted): Eden's renderer_vulkan/present/lsfg_alpha.h on lsfg_vk.h (lsfg_common.h says what changed:
// the namespace, no MemoryAllocator, the wrapper's handles).

#pragma once

#include <array>

#include "lsfg_common.h"

namespace lsfg {

class LsfgShaders;

constexpr size_t LSFG_ALPHA_STAGES = 4;

class LsfgAlphaPasses {
public:
    LsfgAlphaPasses() = default;
    LsfgAlphaPasses(const Device& device, const LsfgShaders& shaders);

    [[nodiscard]] const LsfgPass& Get(size_t stage) const {
        return passes[stage];
    }

private:
    std::array<LsfgPass, LSFG_ALPHA_STAGES> passes;
};

class LsfgAlpha {
public:
    LsfgAlpha() = default;
    LsfgAlpha(const Device& device,
              const LsfgAlphaPasses& passes_, LsfgResources& resources,
              DescriptorPool& descriptor_pool, LsfgImage& input);

    void PushBarriers(LsfgBarriers& barriers, u64 frame_count, size_t stage);
    void DispatchStage(CommandBuffer cmdbuf, u64 frame_count, size_t stage);

    [[nodiscard]] LsfgImageHistory& Outputs() {
        return out_images;
    }

private:
    const LsfgAlphaPasses* passes{};
    LsfgImage* input{};

    std::array<VkDescriptorSet, LSFG_ALPHA_STAGES - 1> descriptor_sets{};
    std::array<VkDescriptorSet, LSFG_HISTORY_SLOTS> last_descriptor_sets{};
    DescriptorSets owned_sets;

    LsfgImage temp1;
    LsfgImage temp2;
    LsfgImagePair temp3;
    LsfgImageHistory out_images;
};

} // namespace lsfg
