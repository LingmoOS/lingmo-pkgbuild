cmake_minimum_required(VERSION 3.5)
project(lingmo-pkgbuild)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(lingmo-pkgbuild 
    src/main.cpp
    src/lingmo_pkgbuild.cpp
)

target_include_directories(lingmo-pkgbuild PRIVATE include)

# 添加对 unistd.h 的检查
include(CheckIncludeFile)
check_include_file(unistd.h HAVE_UNISTD_H)
if(HAVE_UNISTD_H)
    add_definitions(-DHAVE_UNISTD_H)
endif()

# 安装目标
install(TARGETS lingmo-pkgbuild
    RUNTIME DESTINATION bin
) 