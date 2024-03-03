#include <iostream>
#include <string>
#include <format>
#include <linux/dma-buf.h>
#include <VkBootstrap.h>

#define VK_CHECK(X) assert(! X)

auto main() -> int {
    vkb::InstanceBuilder builder;
    auto                 inst =
        builder.require_api_version(VK_API_VERSION_1_2).request_validation_layers().build().value();
    vkb::PhysicalDeviceSelector phy_selector(inst);
    phy_selector.require_present(false).add_required_extensions(
        { "VK_KHR_external_memory",
          "VK_KHR_external_memory_fd",
          "VK_EXT_external_memory_dma_buf" });
    vkb::DeviceBuilder device_builder(phy_selector.select().value());
    auto               device = device_builder.build().value();

    VkBuffer buffer;
    {
        VkBufferCreateInfo ci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        ci.size        = 256;
        ci.sharingMode = VkSharingMode::VK_SHARING_MODE_EXCLUSIVE;
        ci.usage       = VkBufferUsageFlagBits::VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VkBufferUsageFlagBits::VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VkExternalMemoryBufferCreateInfo ex_ci {};
        ex_ci.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        ex_ci.handleTypes =
            VkExternalMemoryHandleTypeFlagBits::VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        ci.pNext = &ex_ci;
        VK_CHECK(vkCreateBuffer(device, &ci, nullptr, &buffer));
    }
    VkMemoryDedicatedRequirements mem_dedicated_req {
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS
    };
    VkMemoryRequirements2 mem_req { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    mem_req.pNext = &mem_dedicated_req;

    VkDeviceMemory memory;
    {
        VkBufferMemoryRequirementsInfo2 bufferReqs {
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2
        };
        bufferReqs.buffer = buffer;
        vkGetBufferMemoryRequirements2(device, &bufferReqs, &mem_req);

        VkMemoryAllocateInfo alloc_info {};
        alloc_info.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = mem_req.memoryRequirements.size;
        // TODO
        alloc_info.memoryTypeIndex = 0;

        // enable export memory
        VkExportMemoryAllocateInfo ex_alloc_info {};
        ex_alloc_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        ex_alloc_info.handleTypes =
            VkExternalMemoryHandleTypeFlagBits::VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        alloc_info.pNext = &ex_alloc_info;

        if (mem_dedicated_req.requiresDedicatedAllocation) {
            VkMemoryDedicatedAllocateInfo delicated {};
            delicated.sType     = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
            delicated.buffer    = buffer;
            ex_alloc_info.pNext = &delicated;
        }

        VK_CHECK(vkAllocateMemory(device, &alloc_info, nullptr, &memory));
    }
    VK_CHECK(vkBindBufferMemory(device, buffer, memory, 0));

    int fd {0};
    {
        VkMemoryGetFdInfoKHR info {};
        info.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        info.memory     = memory;
        info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        auto vkGetMemoryHandle =
            PFN_vkGetMemoryFdKHR(vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
        VK_CHECK(vkGetMemoryHandle(device, &info, &fd));
    }

    std::cout << fd << std::endl;
    return 0;
}
