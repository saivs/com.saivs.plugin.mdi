// Minimal Vulkan type definitions for MDI plugin.
// Only the types needed by IUnityGraphicsVulkan.h and our backend.
// Avoids requiring the full Vulkan SDK.
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#define VKAPI_ATTR
#define VKAPI_CALL __stdcall
#define VKAPI_PTR  __stdcall
#else
#define VKAPI_ATTR
#define VKAPI_CALL
#define VKAPI_PTR
#endif

#define VK_DEFINE_HANDLE(object) typedef struct object##_T* object;
#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(object) typedef struct object##_T* object;
#define VK_NULL_HANDLE 0

// Constants
#define VK_TRUE  1
#define VK_FALSE 0

// Dispatchable handles
VK_DEFINE_HANDLE(VkInstance)
VK_DEFINE_HANDLE(VkPhysicalDevice)
VK_DEFINE_HANDLE(VkDevice)
VK_DEFINE_HANDLE(VkQueue)
VK_DEFINE_HANDLE(VkCommandBuffer)

// Non-dispatchable handles
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkPipelineCache)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDeviceMemory)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkBuffer)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkImage)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkRenderPass)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkFramebuffer)

// Basic types
typedef uint64_t VkDeviceSize;
typedef uint32_t VkFlags;
typedef uint32_t VkBool32;

// Flag typedefs
typedef VkFlags VkMemoryPropertyFlags;
typedef VkFlags VkImageAspectFlags;
typedef VkFlags VkImageUsageFlags;
typedef VkFlags VkBufferUsageFlags;
typedef VkFlags VkPipelineStageFlags;
typedef VkFlags VkAccessFlags;

// Pipeline stage bits (VkPipelineStageFlagBits)
#define VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT 0x00000002

// Access flags (VkAccessFlagBits)
#define VK_ACCESS_INDIRECT_COMMAND_READ_BIT 0x00000001

// Enums (as integers — we don't use specific values)
typedef enum VkImageLayout { VK_IMAGE_LAYOUT_MAX_ENUM = 0x7FFFFFFF } VkImageLayout;
typedef enum VkFormat { VK_FORMAT_MAX_ENUM = 0x7FFFFFFF } VkFormat;
typedef enum VkImageTiling { VK_IMAGE_TILING_MAX_ENUM = 0x7FFFFFFF } VkImageTiling;
typedef enum VkImageType { VK_IMAGE_TYPE_MAX_ENUM = 0x7FFFFFFF } VkImageType;
typedef enum VkSampleCountFlagBits { VK_SAMPLE_COUNT_FLAG_BITS_MAX_ENUM = 0x7FFFFFFF } VkSampleCountFlagBits;
typedef enum VkCommandBufferLevel {
    VK_COMMAND_BUFFER_LEVEL_PRIMARY = 0,
    VK_COMMAND_BUFFER_LEVEL_SECONDARY = 1,
    VK_COMMAND_BUFFER_LEVEL_MAX_ENUM = 0x7FFFFFFF
} VkCommandBufferLevel;

// Structs
typedef struct VkExtent3D {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
} VkExtent3D;

typedef struct VkImageSubresource {
    VkImageAspectFlags aspectMask;
    uint32_t mipLevel;
    uint32_t arrayLayer;
} VkImageSubresource;

// VkPhysicalDeviceFeatures — full struct (Vulkan 1.0 core).
// We only read multiDrawIndirect, but the struct must be complete for correct layout.
typedef struct VkPhysicalDeviceFeatures {
    VkBool32 robustBufferAccess;
    VkBool32 fullDrawIndexUint32;
    VkBool32 imageCubeArray;
    VkBool32 independentBlend;
    VkBool32 geometryShader;
    VkBool32 tessellationShader;
    VkBool32 sampleRateShading;
    VkBool32 dualSrcBlend;
    VkBool32 logicOp;
    VkBool32 multiDrawIndirect;
    VkBool32 drawIndirectFirstInstance;
    VkBool32 depthClamp;
    VkBool32 depthBiasClamp;
    VkBool32 fillModeNonSolid;
    VkBool32 depthBounds;
    VkBool32 wideLines;
    VkBool32 largePoints;
    VkBool32 alphaToOne;
    VkBool32 multiViewport;
    VkBool32 samplerAnisotropy;
    VkBool32 textureCompressionETC2;
    VkBool32 textureCompressionASTC_LDR;
    VkBool32 textureCompressionBC;
    VkBool32 occlusionQueryPrecise;
    VkBool32 pipelineStatisticsQuery;
    VkBool32 vertexPipelineStoresAndAtomics;
    VkBool32 fragmentStoresAndAtomics;
    VkBool32 shaderTessellationAndGeometryPointSize;
    VkBool32 shaderImageGatherExtended;
    VkBool32 shaderStorageImageExtendedFormats;
    VkBool32 shaderStorageImageMultisample;
    VkBool32 shaderStorageImageReadWithoutFormat;
    VkBool32 shaderStorageImageWriteWithoutFormat;
    VkBool32 shaderUniformBufferArrayDynamicIndexing;
    VkBool32 shaderSampledImageArrayDynamicIndexing;
    VkBool32 shaderStorageBufferArrayDynamicIndexing;
    VkBool32 shaderStorageImageArrayDynamicIndexing;
    VkBool32 shaderClipDistance;
    VkBool32 shaderCullDistance;
    VkBool32 shaderFloat64;
    VkBool32 shaderInt64;
    VkBool32 shaderInt16;
    VkBool32 shaderResourceResidency;
    VkBool32 shaderResourceMinLod;
    VkBool32 sparseBinding;
    VkBool32 sparseResidencyBuffer;
    VkBool32 sparseResidencyImage2D;
    VkBool32 sparseResidencyImage3D;
    VkBool32 sparseResidency2Samples;
    VkBool32 sparseResidency4Samples;
    VkBool32 sparseResidency8Samples;
    VkBool32 sparseResidency16Samples;
    VkBool32 sparseResidencyAliased;
    VkBool32 variableMultisampleRate;
    VkBool32 inheritedQueries;
} VkPhysicalDeviceFeatures;

// Function pointer types
typedef void     (VKAPI_PTR *PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_vkGetInstanceProcAddr)(VkInstance instance, const char* pName);
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_vkGetDeviceProcAddr)(VkDevice device, const char* pName);

typedef void (VKAPI_PTR *PFN_vkGetPhysicalDeviceFeatures)(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures);

typedef void (VKAPI_PTR *PFN_vkCmdDrawIndexedIndirect)(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    uint32_t drawCount,
    uint32_t stride);

typedef void (VKAPI_PTR *PFN_vkCmdDrawIndexedIndirectCount)(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride);
