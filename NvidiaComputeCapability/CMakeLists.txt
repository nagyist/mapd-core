cmake_minimum_required(VERSION 3.25)
project(NvidiaComputeCapability)

include_directories(..)

enable_language(CUDA)

add_executable(get_nvidia_compute_capability get_nvidia_compute_capability.cpp)
set_source_files_properties(get_nvidia_compute_capability.cpp PROPERTIES LANGUAGE CUDA)
target_link_libraries(get_nvidia_compute_capability ${CUDA_LIBRARIES})
add_custom_command(
  COMMENT
    "Detecting NVIDIA CUDA compute capability for debug build. (NvidiaComputeCapability.txt)"
  OUTPUT
    ${CMAKE_BINARY_DIR}/../NvidiaComputeCapability.txt
  DEPENDS
    get_nvidia_compute_capability
  COMMAND
    get_nvidia_compute_capability > ${CMAKE_BINARY_DIR}/../NvidiaComputeCapability.txt
  COMMAND
    eval echo "NVIDIA CUDA compute capability: $(cat ${CMAKE_BINARY_DIR}/../NvidiaComputeCapability.txt)"
  VERBATIM
)
add_custom_target(NvidiaComputeCapability ALL DEPENDS ${CMAKE_BINARY_DIR}/../NvidiaComputeCapability.txt)
