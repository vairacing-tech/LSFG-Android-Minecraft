#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

namespace lsfg_mc {

/// Initializes the passive Vulkan diagnostic observation layer.
/// This layer performs strictly read-only pass-through tracing of the first ~10 frames
/// and swapchain creation parameters, without altering any Vulkan state or presentation flow.
void init_passive_vulkan_diagnostics();

/// Returns whether any Vulkan activity (GIPA/GDPA/Swapchain) has been observed by our hook.
bool is_vulkan_observed();

} // namespace lsfg_mc
