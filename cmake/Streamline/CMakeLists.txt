cmake_minimum_required(VERSION 3.21)

add_library(Streamline INTERFACE)

target_include_directories(
  Streamline INTERFACE
  ${CMAKE_SOURCE_DIR}/extern/Streamline-DX12/include
  ${CMAKE_SOURCE_DIR}/extern/Streamline-DX12/external/ngx-sdk/include)

if(MSVC)
  target_compile_options(Streamline INTERFACE /wd5103)
endif()
