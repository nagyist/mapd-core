if(MSVC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS -Wall")
else()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS -Wall -Wno-attributes")
endif()

add_library(RuntimeLibManager RuntimeLibManager.cpp RuntimeLibManager.h)

if(ENABLE_TORCH_TFS)
  target_include_directories(RuntimeLibManager PRIVATE ${TORCH_INCLUDE_DIRS})
endif()

target_link_libraries(RuntimeLibManager ${llvm_libs})