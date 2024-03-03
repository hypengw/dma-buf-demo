cmake_minimum_required(VERSION 3.14)

project(
    cpu
    VERSION 0.1.0
    LANGUAGES CXX
)
find_package(Vulkan REQUIRED)

add_subdirectory(vk-bootstrap)

add_executable(main src/main.cpp)
target_compile_features(main PRIVATE cxx_std_20)
target_link_libraries(main PRIVATE vk-bootstrap::vk-bootstrap Vulkan::Vulkan)
