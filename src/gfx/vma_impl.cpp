// Single TU compiling the VMA implementation. Configuration macros
// (VMA_STATIC_VULKAN_FUNCTIONS=0, VMA_DYNAMIC_VULKAN_FUNCTIONS=1) come from
// the build so every includer agrees.

#include "gfx/vk_api.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4127 4189 4324 4505)
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
