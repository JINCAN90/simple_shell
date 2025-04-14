# 设定CMake的最低版本要求
cmake_minimum_required(VERSION 3.10)

# 设定项目名称
project(SimpleLinuxShell)

# 设定C++标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 添加可执行文件
add_executable(simple_shell main.cpp)

# 查找并链接必要的库
find_library(LIBC c)
target_link_libraries(simple_shell PRIVATE ${LIBC})