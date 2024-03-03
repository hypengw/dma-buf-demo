#include <iostream>
#include <string>
#include <format>
#include <VkBootstrap.h>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <csignal>

#define VK_CHECK(X)   assert(! X)
#define MSG(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)

struct DmaBuf {
    int      width, height;
    uint32_t fourcc;
    int      offset, pitch;
    int      fd;
};

VkBool32 DebugUtilsMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                     VkDebugUtilsMessageTypeFlagsEXT,
                                     const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                     void*) {
    std::cerr << "validation layer: " << pCallbackData->pMessage << std::endl;
    return 1;
}

auto send(const char* sockname, int dma_buf_fd, const DmaBuf& img) {
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);

    {
        struct sockaddr_un addr;
        addr.sun_family = AF_UNIX;
        if (strlen(sockname) >= sizeof(addr.sun_path)) {
            MSG("Socket filename '%s' is too long, max %d", sockname, (int)sizeof(addr.sun_path));
            return;
        }
        strcpy(addr.sun_path, sockname);
        if (-1 == bind(sockfd, (const struct sockaddr*)&addr, sizeof(addr))) {
            perror("Cannot bind unix socket");
            return;
        }

        if (-1 == listen(sockfd, 1)) {
            perror("Cannot listen on unix socket");
            return;
        }
    }

    for (;;) {
        int connfd = accept(sockfd, NULL, NULL);
        if (connfd < 0) {
            perror("Cannot accept unix socket");
            return;
        }

        MSG("accepted socket %d", connfd);

        struct msghdr msg = { 0 };

        struct iovec io = {
            .iov_base = (void*)&img,
            .iov_len  = sizeof(img),
        };
        msg.msg_iov    = &io;
        msg.msg_iovlen = 1;

        char cmsg_buf[CMSG_SPACE(sizeof(dma_buf_fd))];
        msg.msg_control      = cmsg_buf;
        msg.msg_controllen   = sizeof(cmsg_buf);
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level     = SOL_SOCKET;
        cmsg->cmsg_type      = SCM_RIGHTS;
        cmsg->cmsg_len       = CMSG_LEN(sizeof(dma_buf_fd));
        memcpy(CMSG_DATA(cmsg), &dma_buf_fd, sizeof(dma_buf_fd));

        ssize_t sent = sendmsg(connfd, &msg, MSG_CONFIRM);
        if (sent < 0) {
            perror("cannot sendmsg");
            return;
        }

        MSG("sent %d", (int)sent);

        close(connfd);
    }
}

auto recv(const char* sockname) {
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    {
        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        if (strlen(sockname) >= sizeof(addr.sun_path)) {
            MSG("Socket filename '%s' is too long, max %d", sockname, (int)sizeof(addr.sun_path));
            return;
        }

        strcpy(addr.sun_path, sockname);
        if (-1 == connect(sockfd, (const struct sockaddr*)&addr, sizeof(addr))) {
            perror("Cannot connect to unix socket");
            return;
        }

        MSG("connected");
    }

    DmaBuf img;

    {
        struct msghdr msg = { 0 };

        struct iovec io = {
            .iov_base = &img,
            .iov_len  = sizeof(img),
        };
        msg.msg_iov    = &io;
        msg.msg_iovlen = 1;

        char cmsg_buf[CMSG_SPACE(sizeof(img.fd))];
        msg.msg_control      = cmsg_buf;
        msg.msg_controllen   = sizeof(cmsg_buf);
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level     = SOL_SOCKET;
        cmsg->cmsg_type      = SCM_RIGHTS;
        cmsg->cmsg_len       = CMSG_LEN(sizeof(img.fd));

        MSG("recvmsg");
        ssize_t recvd = recvmsg(sockfd, &msg, 0);
        if (recvd <= 0) {
            perror("cannot recvmsg");
            return;
        }

        MSG("Received %d", (int)recvd);

        if (io.iov_len == sizeof(img) - sizeof(img.fd)) {
            MSG("Received metadata size mismatch: %d received, %d expected",
                (int)io.iov_len,
                (int)sizeof(img) - (int)sizeof(img.fd));
            return;
        }

        if (cmsg->cmsg_len != CMSG_LEN(sizeof(img.fd))) {
            MSG("Received fd size mismatch: %d received, %d expected",
                (int)cmsg->cmsg_len,
                (int)CMSG_LEN(sizeof(img.fd)));
            return;
        }

        memcpy(&img.fd, CMSG_DATA(cmsg), sizeof(img.fd));
    }
}

auto vk(bool sender) {
    vkb::InstanceBuilder builder;
    auto                 inst = builder.require_api_version(VK_API_VERSION_1_2)
                    .enable_validation_layers()
                    .set_debug_callback(DebugUtilsMessengerCallback)
                    .build()
                    .value();
    vkb::PhysicalDeviceSelector phy_selector(inst);
    phy_selector.require_present(false).add_required_extensions(
        { "VK_KHR_external_memory",
          "VK_KHR_external_memory_fd",
          "VK_EXT_external_memory_dma_buf" });

    vkb::DeviceBuilder device_builder(phy_selector.select().value());
    auto               device = device_builder.build().value();

    if (sender) {
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
            VkBufferMemoryRequirementsInfo2 buffer_req {
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2
            };
            buffer_req.buffer = buffer;
            vkGetBufferMemoryRequirements2(device, &buffer_req, &mem_req);

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

        int fd { 0 };
        {
            VkMemoryGetFdInfoKHR info {};
            info.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
            info.memory     = memory;
            info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            auto vkGetMemoryHandle =
                PFN_vkGetMemoryFdKHR(vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
            VK_CHECK(vkGetMemoryHandle(device, &info, &fd));
        }

        VkQueue         queue       = device.get_queue(vkb::QueueType::compute).value();
        auto            queue_index = device.get_queue_index(vkb::QueueType::compute).value();
        VkCommandPool   pool;
        VkCommandBuffer cmd;

        {
            VkCommandPoolCreateInfo ci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            ci.queueFamilyIndex = queue_index;
            VK_CHECK(vkCreateCommandPool(device, &ci, nullptr, &pool));
            VkCommandBufferAllocateInfo cmd_ci { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            cmd_ci.commandPool        = pool;
            cmd_ci.commandBufferCount = 1;
            cmd_ci.level              = VkCommandBufferLevel::VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            VK_CHECK(vkAllocateCommandBuffers(device, &cmd_ci, &cmd));
        }
        {
            VkCommandBufferBeginInfo begin_ci { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            VK_CHECK(vkBeginCommandBuffer(cmd, &begin_ci));

            vkCmdFillBuffer(cmd, buffer, 0, VK_WHOLE_SIZE, 100);
            VK_CHECK(vkEndCommandBuffer(cmd));
            VkSubmitInfo si { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.commandBufferCount = 1;
            si.pCommandBuffers    = &cmd;
            VK_CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
            VK_CHECK(vkQueueWaitIdle(queue));
        }
        DmaBuf img;
        send("/tmp/dma", fd, img);
    } else {
        recv("/tmp/dma");
    }
}

auto main() -> int {
    // cpu dma
    auto pid = fork();
    switch (pid) {
    case -1: perror("fork"); exit(EXIT_FAILURE);
    case 0:
        sleep(2);
        vk(false);
        puts("Child exiting.");
        exit(EXIT_SUCCESS);
    default:
        printf("Child is PID %jd\n", (intmax_t)pid);
        vk(true);
        puts("Parent exiting.");
        kill(pid, SIGKILL);
        exit(EXIT_SUCCESS);
    }
    return 0;
}
