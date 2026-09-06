# core/cpp —— C++ 内核占位（target：go_core_cpp）。
#
# 定位：未来“并行与高效”C++ 后端的槽位。当前只有两个头文件纯函数
# （flip.hpp / udim.hpp）+ 一个自测可执行，作用是证明这个槽位存在、能独立
# configure/build/test；不要往这里引入 Qt 或任何第三方依赖——core 不认识 UI。
#
# 注意：本文件刻意不调用 project()。项目名与语言（CXX）由仓库根的
# CMakeLists.txt 统一声明，子目录保持被动。从根工程 add_subdirectory 进来
# 时没有任何额外动作；若单独 configure 本目录（cmake -S core/cpp），
# CMake 会隐式补一个默认工程并完成编译器检测，因此独立自测同样可用——
# 仅会有一条关于“缺少 project()”的 dev 警告，这是“根统一管 project()”
# 这条约定的已知代价，可忽略（已在 CMake 4.3 实测通过）。

# 独立 configure 本目录时保证策略基线明确；从根工程进来时根已有不低于此的
# 基线，这里再声明一次不产生冲突（只影响本子目录作用域）。
cmake_minimum_required(VERSION 3.16)

# ---- 内核 ----
# 占位期选 INTERFACE 而不是 STATIC：内核目前只有 inline 头文件函数，没有任何
# .cpp 可编译，STATIC 会得到一个空归档（部分工具链会直接报错或产生无意义的
# 产物）。将来引入真正的 .cpp 实现时，把 INTERFACE 改成 STATIC、并把源文件
# 挂到同一个 target 上即可——target 名 go_core_cpp 保持不变，依赖方（根工程、
# 各前端）的链接写法一行不用改。这是本目录的既定演进路径。
add_library(go_core_cpp INTERFACE)
target_include_directories(go_core_cpp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---- 自测 ----
add_executable(go_core_cpp_tests tests/go_core_cpp_tests.cpp)
target_link_libraries(go_core_cpp_tests PRIVATE go_core_cpp)
target_compile_features(go_core_cpp_tests PRIVATE cxx_std_17)

# 测试可执行放进默认构建（不设 EXCLUDE_FROM_ALL，也不挂在 BUILD_TESTING 门后）：
# 这个 exe 就是“cpp 后端槽位存在且可用”的活证明。让它随每次 preset 构建一起
# 编译，头文件一坏立刻暴露，而不是等某天某个前端接入时才发现槽位早已腐烂。
# 代价只是一次百余行代码的编译，值得。
enable_testing()
add_test(NAME go_core_cpp COMMAND go_core_cpp_tests)
