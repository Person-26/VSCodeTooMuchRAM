# Builds vsc-pdf, the Okular-based page renderer for vsc's PDF tiles (vsc itself is built by the Makefile).
cmake_minimum_required(VERSION 3.20)
project(vsc-pdf CXX)
set(CMAKE_CXX_STANDARD 20)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE MinSizeRel)
endif()
find_package(Okular6 REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
add_executable(vsc-pdf vsc-pdf.cpp)
target_include_directories(vsc-pdf PRIVATE /usr/include/okular/core)  # settings_core.h includes its siblings unprefixed
target_link_libraries(vsc-pdf Okular::Core Qt6::Widgets)
