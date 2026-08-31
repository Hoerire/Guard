cmake_minimum_required(VERSION 3.22.1)
project("guard")

# 编译为独立可执行程序（而非 JNI 库），由 App 通过 root 部署到
# /data/local/tmp/Guard/Guard 运行。
add_executable(guard guard.c)

target_compile_options(guard PRIVATE -O2 -fPIE -Wextra -Wno-unused-parameter)
target_link_options(guard PRIVATE -pie)