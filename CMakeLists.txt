# =============================================================================
# GeneralOperations —— 脚本启动器 monorepo 的超级构建（super-build）根
#
# 为什么超级构建放根：monorepo 的边界不靠仓库物理隔离，靠三件事——
#   1. 目录（ui/ 只管前端，core/ 只管后端）；
#   2. 依赖方向（ui → core 单向，由 add_subdirectory 的顺序和变量流向表达）；
#   3. 本文件这个单点：前端选择、输出命名、发行 zip 命名全在这里定义，
#      子目录只消费 GO_* 变量，不允许自己再发明一套命名。
# 分层与契约的完整说明见 docs/ARCHITECTURE.md。
# =============================================================================
cmake_minimum_required(VERSION 3.21)

# 为什么版本号只此一处：发行 zip 文件名、RELEASE 流程、AGENTS.md 的 bump 约定
# 全部指向这里的 project(... VERSION)。任何子项目都不得再声明自己的版本号，
# 否则"改一处版本号"就变成"改 N 处"，必然漏改。
# 0.2.000 标记 monorepo 重构起点（旧 Qt5 单仓仓库止于 0.1.00x）。
project(GeneralOperations
    VERSION 0.2.004
    DESCRIPTION "统一的脚本启动器 monorepo（多前端 x 多后端）"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC 专用：Qt5 官方 msvc2019_64 是动态 CRT 构建，必须一致否则链接期/运行期都炸。
# 对 MinGW 是空操作。放在根是因为该约束对任何 C++ 前端/后端都成立，不该由
# 各子目录重复声明。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# ---- 前端选择（monorepo 的总开关）----
# 默认 qt5 是唯一完整可用的前端；其余取值是占位，见各分支的说明。
# 后端不再是可选项：core/ 下两个内核无条件一起构建（见下），产物与发行
# 命名只区分前端、不区分后端，没有后端开关这回事。
set(GO_FRONTEND "qt5" CACHE STRING "启动器前端：qt5 | qt6.8lts(占位) | tauri2(占位)")
set_property(CACHE GO_FRONTEND PROPERTY STRINGS qt5 qt6.8lts tauri2)

# ---- 内核：脚本资产 ----
# 无条件 add_subdirectory：无论前端选什么都得拿到脚本目录路径；而且"脚本在
# 哪里"是 core/python 的自述，由它导出（GO_PYTHON_SCRIPTS_DIR），根不写死
# 路径——将来脚本换目录只动 core/python 一处。
add_subdirectory(core/python)

# 根再把它规范化成内核无关的名字：前端（ui/qt5 等）只认识 GO_CORE_SCRIPTS_DIR。
# 后端不再是可选项——core/python 与 core/cpp 都无条件 add_subdirectory 一起
# 构建、一起进发行，GO_CORE_SCRIPTS_DIR 只是指向脚本资产的指针（当前由
# core/python 提供），指向始终由根这单点维护。
set(GO_CORE_SCRIPTS_DIR "${GO_PYTHON_SCRIPTS_DIR}")

# ---- 输出命名规则（单点定义，见 docs/ARCHITECTURE.md「关键契约」）----
# bundle 目录带 前端 段：不同前端的产物落到各自子目录，互不覆盖；
# 带 x64 段：为将来可能的非 Windows/非 64 位产物留位。
set(GO_BUNDLE_DIR "${CMAKE_SOURCE_DIR}/output/x64-${GO_FRONTEND}")

# GO_ZIP_FILE 只在单配置（CMAKE_BUILD_TYPE 非空）下定义：
# release_zip 目标要按配置算 zip 名，多配置生成器（VS/Ninja Multi-Config）一次
# configure 出多个配置，这里按哪个算都不对。本项目全部 preset 都是 Ninja 单配置，
# 所以守卫只对"有人手改生成器"的场景生效——此时明确警告而不是悄悄产出错名的 zip。
if(CMAKE_BUILD_TYPE)
    string(TOLOWER "${CMAKE_BUILD_TYPE}" _go_conf_lower)
    set(GO_ZIP_FILE
        "${CMAKE_SOURCE_DIR}/output/GeneralOperations-${PROJECT_VERSION}_x64-${GO_FRONTEND}-${_go_conf_lower}.zip")
else()
    message(WARNING
        "CMAKE_BUILD_TYPE 为空（多配置生成器暂不支持），GO_ZIP_FILE 未定义，"
        "release_zip 目标不可用。请使用 CMakePresets.json 里的 Ninja preset。")
endif()

# ---- 内核：C++（占位）----
# 无条件 add_subdirectory：它的测试目标 go_core_cpp_tests 进默认构建，
# 保证占位代码一旦开始实现就有测试挂载点，不需要改根。
add_subdirectory(core/cpp)

# ---- 前端分支 ----
# 为什么前端走分支而内核无条件：前端决定整个 GUI 目标树（Qt 版本、QML 模块、
# windeployqt 差异），一次只能构建一个；内核只是"脚本资产 + 纯函数"，与前端
# 正交、无条件一起构建。未来接入新前端只需在这里加一个分支——这就是
# monorepo 多前端的扩展点，详见 docs/ARCHITECTURE.md「新增一个前端」。
if(GO_FRONTEND STREQUAL "qt5")
    add_subdirectory(ui/qt5)
elseif(GO_FRONTEND STREQUAL "qt6.8lts")
    # 占位目录：其 CMakeLists 目前直接 FATAL_ERROR（保留目录结构与迁移要点预记）。
    add_subdirectory(ui/qt6.8lts)
elseif(GO_FRONTEND STREQUAL "tauri2")
    # tauri2 不走 CMake 构建（cargo/tauri cli），所以没有 ui/tauri2/CMakeLists.txt
    # 可加进来，根在这里直接拦下。规划见 ui/tauri2/README.md。
    message(FATAL_ERROR
        "GO_FRONTEND=${GO_FRONTEND} 仅保留位置，尚未实现。"
        " 当前可用前端：qt5（-DGO_FRONTEND=qt5，默认）。")
else()
    message(FATAL_ERROR
        "未知的 GO_FRONTEND='${GO_FRONTEND}'。可选：qt5 | qt6.8lts | tauri2。")
endif()

# ---- 配置摘要：configure 结束时一眼核对关键值 ----
message(STATUS "----------------------------------------------------------")
message(STATUS "[GO] 前端 GO_FRONTEND      = ${GO_FRONTEND}")
message(STATUS "[GO] 脚本目录 GO_CORE_SCRIPTS_DIR = ${GO_CORE_SCRIPTS_DIR}")
message(STATUS "[GO] 产物目录 GO_BUNDLE_DIR = ${GO_BUNDLE_DIR}")
if(DEFINED GO_ZIP_FILE)
    message(STATUS "[GO] 发行 zip GO_ZIP_FILE  = ${GO_ZIP_FILE}")
endif()
message(STATUS "----------------------------------------------------------")
