# =============================================================================
# ui/qt5 —— Qt5 Quick (QML) 前端
#
# 为什么本文件不声明工程：project() 与版本号全仓库只此一处（根 CMakeLists.txt），
# 本目录只是根工程挂载的子目录，只消费根定义的 GO_* 变量（GO_CORE_SCRIPTS_DIR /
# GO_BUNDLE_DIR / GO_ZIP_FILE），不允许自己再发明一套命名。
# 目标名沿用第二代仓库的 GeneralOperationsLauncher —— 发行包入口 exe 同名。
# =============================================================================

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)   # QML 文件通过 resources.qrc 编进二进制

# Quick 传递带入 QmlModels/Qml/Gui/Core（Qml 再带 Network）。
#
# QmlWorkerScript 必须显式列出：它不在上面任何一条依赖链里，
# 但 qtquick2plugin.dll 运行时导入它。不写这行的话
# Qt5::QmlWorkerScript target 不会被定义，下面 copy-runtime 处直接 configure 失败。
# 我们不链接它，只是为了拿到 target 好在 POST_BUILD 里拷 dll。
#
# 刻意不要 Widgets（与 QML 渲染栈无关，白占体积）、
# 不要 QuickControls2（当前 QML 只用 QtQuick 基础元素手写；真需要 Controls 再加，
# 但记得那会引入新的插件依赖树，见 copy-runtime 处的说明）。
# QtGraphicalEffects 是纯 QML 模块，无 C++ 库可链，靠 qt.conf/windeployqt 解析。
find_package(Qt5 5.15 REQUIRED COMPONENTS Quick QmlWorkerScript)

add_executable(GeneralOperationsLauncher)

target_sources(GeneralOperationsLauncher PRIVATE
    src/main.cpp
    src/resources.qrc

    # core: 纯 C++，不认识 UI
    src/core/ScriptManifest.h
    src/core/ScriptManifest.cpp
    src/core/ScriptRegistry.h
    src/core/ScriptRegistry.cpp

    # model: core 与 QML 之间唯一的桥
    src/model/ScriptListModel.h
    src/model/ScriptListModel.cpp

    # model: 运行时布局持久化（exe 旁 layout.json）
    src/model/LayoutStore.h
    src/model/LayoutStore.cpp

    # runner: QProcess + Windows Job Object 托管完整脚本进程树
    src/model/ProcessTreeController.h
    src/model/ProcessTreeController.cpp
    src/model/ScriptRunner.h
    src/model/ScriptRunner.cpp

    # debug: 拖放诊断（--dropdebug 才启用日志文件）
    src/debug/DropDebugLogger.h
    src/debug/DropDebugLogger.cpp
)

# exe 图标（Windows 资源：windres 编译，见 src/app.rc 与 tools/gen_icon.py）
if(WIN32)
    target_sources(GeneralOperationsLauncher PRIVATE src/app.rc)
endif()

target_include_directories(GeneralOperationsLauncher PRIVATE src)

target_link_libraries(GeneralOperationsLauncher PRIVATE Qt5::Quick)

# ScriptRunner 的确定性自测：覆盖增量 UTF-8/容量截断，并在 Windows 上用
# detached 后代的哨兵文件验证对外可见的整棵进程树取消语义。
add_executable(script_runner_tests
    tests/ScriptRunnerTests.cpp
    src/core/ScriptManifest.h
    src/core/ScriptManifest.cpp
    src/core/ScriptRegistry.h
    src/core/ScriptRegistry.cpp
    src/model/ProcessTreeController.h
    src/model/ProcessTreeController.cpp
    src/model/ScriptRunner.h
    src/model/ScriptRunner.cpp
)
target_include_directories(script_runner_tests PRIVATE src)
target_link_libraries(script_runner_tests PRIVATE Qt5::Core)
target_compile_definitions(script_runner_tests PRIVATE SCRIPT_RUNNER_ENABLE_TEST_HOOKS)
enable_testing()
add_test(NAME script_runner COMMAND script_runner_tests)
set_tests_properties(script_runner PROPERTIES TIMEOUT 60)

# 圆角窗口（main.cpp 的 DwmSetWindowAttribute）。MinGW 8.1 自带
# libdwmapi.a，MSVC 的 SDK 也有，两边都不用额外配置。
if(WIN32)
    target_link_libraries(GeneralOperationsLauncher PRIVATE dwmapi)
endif()

# ---- 开发期脚本目录注入（monorepo 契约）----
# 开发期前端从内核 core/python/scripts 读脚本——前端不持有脚本副本，发行期由
# bundle 管线把该目录拷到 exe 旁（见下方 bundle 段与 src/main.cpp 的解析链）。
# 宏名是跨层契约（main.cpp 里 #ifndef 兜底为空串，单独编译不炸），一字不能改。
target_compile_definitions(GeneralOperationsLauncher PRIVATE
    GO_DEV_SCRIPTS_DIR="${GO_CORE_SCRIPTS_DIR}"
)

set_target_properties(GeneralOperationsLauncher PROPERTIES
    # 只有 Release 才隐藏控制台。
    # Debug 必须留在控制台子系统 —— QML 的错误（类型找不到、属性不存在、
    # 绑定循环）全部走 stderr，GUI 子系统下这些输出被直接丢弃，
    # 出问题的表现就是"窗口不出来，也没有任何提示"，完全无从下手。
    WIN32_EXECUTABLE $<$<CONFIG:Release>:ON>
)

# ---- 编译/链接选项：体积优先 ----
if(MSVC)
    target_compile_options(GeneralOperationsLauncher PRIVATE
        /utf-8              # 源码/执行字符集按 UTF-8，否则中文字面量按 GBK 解读会乱码
        /permissive-
        /W4
        /wd4127             # Qt 宏内部大量 constant conditional
        $<$<CONFIG:Release>:/O1>
        $<$<CONFIG:Release>:/GL>
        $<$<CONFIG:Release>:/Gy>
    )
    target_compile_options(script_runner_tests PRIVATE /utf-8 /permissive- /W4 /wd4127)
    target_link_options(GeneralOperationsLauncher PRIVATE
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
else()
    # GCC (MinGW)。源码字符集 GCC 默认就是 UTF-8，不需要 /utf-8 的对应物。
    target_compile_options(GeneralOperationsLauncher PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Release>:-Os>
        $<$<CONFIG:Release>:-ffunction-sections>
        $<$<CONFIG:Release>:-fdata-sections>
    )
    target_link_options(GeneralOperationsLauncher PRIVATE
        $<$<CONFIG:Release>:-Wl,--gc-sections>
        $<$<CONFIG:Release>:-s>                     # strip 符号表
    )
    # 刻意不加 -flto：GCC 8.1 的 LTO 配合 moc 生成代码偶有问题，
    # 省下的体积不值得拿"编译不过"来换。
    #
    # 也刻意不做 -static-libstdc++/-static-libgcc：Qt5 的 dll 自己就动态导入
    # libstdc++-6 / libgcc_s_seh-1 / libwinpthread-1（已用 PE 导入表核实），
    # 这三个无论如何都要分发，静态链进 exe 省不掉它们，只会让 exe 变大。
endif()

# ---- Qt 安装位置：copy-runtime / windeployqt / bundle 都要用 ----
if(WIN32)
    get_target_property(_qt_qmake Qt5::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin "${_qt_qmake}" DIRECTORY)
    get_filename_component(_qt_prefix "${_qt_bin}" DIRECTORY)
    # bundle 的 POST_BUILD 在下方展开 ${WINDEPLOYQT_EXE}，
    # 所以这个 find_program 必须在任何 POST_BUILD 注册之前完成。
    find_program(WINDEPLOYQT_EXE windeployqt HINTS "${_qt_bin}")
    if(NOT WINDEPLOYQT_EXE)
        message(WARNING "找不到 windeployqt，output/ 发行装配将缺 Qt 运行时")
    endif()
endif()

# ---- copy-runtime: 每次构建后把运行时拷到 exe 旁边（开发期用）----
#
# 只拷 dll。插件和 QML 模块靠 qt.conf 从 Qt 安装目录解析，不拷 ——
# 因为 Windows 加载器只认 exe 同级/PATH 里的 dll，而 Qt 自己的资源
# （plugins/ qml/ translations/）是 QLibraryInfo 按 prefix 找的，
# 这两件事的机制不同，能分开处理。
if(WIN32)
    # 用生成器表达式而不是硬编码文件名，让 CMake 按当前 config 解析。
    #
    # 【这个列表怎么来的，以及它为什么脆】
    # 不能只算 exe 链接库的传递闭包 —— QML 插件是运行时 dlopen 的，
    # 它们各自有独立的依赖树，从 exe 的导入表里根本看不见。
    # 实测：qtquick2plugin.dll 导入 Qt5QmlWorkerScript.dll，
    # 而 Qt5Quick.dll 并不导入它，所以只从 Qt5Quick 出发算必然漏。
    #
    # 漏了的表现极具误导性：Windows 报"找不到指定的模块"，指的是
    # 插件的某个依赖缺失，不是插件本身不存在 —— 很容易往错的方向查。
    #
    # 所以以后每加一个 QML import（QtQuick.Controls 之类），
    # 都要重新核一遍对应插件的导入表，或者干脆改用 windeployqt。
    set(_qt_runtime_targets
        Qt5::Core
        Qt5::Gui
        Qt5::Qml
        Qt5::QmlModels
        Qt5::QmlWorkerScript   # qtquick2plugin 的依赖，非 exe 的
        Qt5::Quick
        Qt5::Network           # Qt5Qml 运行时导入
    )

    set(_copy_cmds)
    foreach(_t IN LISTS _qt_runtime_targets)
        list(APPEND _copy_cmds
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:${_t}>" "$<TARGET_FILE_DIR:GeneralOperationsLauncher>"
        )
    endforeach()

    # MinGW 运行时没有 CMake target，只能按路径取。
    # 取 Qt 自己 bin 里那份 —— 它和这套 Qt 是配套构建的，版本必然对。
    if(MINGW)
        foreach(_rt libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
            list(APPEND _copy_cmds
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${_qt_bin}/${_rt}" "$<TARGET_FILE_DIR:GeneralOperationsLauncher>"
            )
        endforeach()
    endif()

    # copy_if_different 会比时间戳/内容，所以第一次之后几乎零成本，
    # 挂 POST_BUILD 上不会拖慢日常构建。
    add_custom_command(TARGET GeneralOperationsLauncher POST_BUILD
        ${_copy_cmds}
        COMMENT "拷贝 Qt5 运行时 dll 到输出目录"
        VERBATIM
    )

    # qt.conf：把 prefix 指回 Qt 安装目录。
    # 没有它，Qt5Core 会以为 prefix 就是 exe 所在目录（它不含编译期硬编码路径，
    # 完全相对定位），于是去 build/<preset>/qml/ 找模块，找不到 → 白窗口，且不报错。
    #
    # 【仅限开发期】这里写的是绝对路径，绑死本机 Qt 安装位置。
    # 分发包不能带这个文件，release 走 windeployqt（它会拷真正的 qml/ 和 plugins/）。
    file(GENERATE
        OUTPUT "$<TARGET_FILE_DIR:GeneralOperationsLauncher>/qt.conf"
        CONTENT "[Paths]\nPrefix=${_qt_prefix}\n"
    )
endif()

# ---- bundle: 把产物装配成自包含的 output/x64-<前端>/<配置>/（每次构建后增量执行）----
#
# 与上面 copy-runtime 的区别：copy-runtime 服务开发期（qt.conf 指回本机
# Qt 安装目录，绝对路径），bundle 服务发行 —— windeployqt 装配 Qt 运行时、
# qt.conf 相对化、scripts/ 拷入、内嵌 Python（Windows）/run.sh（Linux）。
# 目录带 前端-x64 段（GO_BUNDLE_DIR，根 CMakeLists 单点定义）：不同前端的产物
# 落到各自子目录，互不覆盖。
# 成品目录整体拷走或压 zip 后即可分发，细节与布局见 docs/RELEASE.md。
if(WIN32)
    set(_bundle_win 1)
else()
    set(_bundle_win 0)
endif()
add_custom_command(TARGET GeneralOperationsLauncher POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
        -DBUNDLE_EXE=$<TARGET_FILE:GeneralOperationsLauncher>
        -DBUNDLE_OUT=${GO_BUNDLE_DIR}/$<CONFIG>
        -DBUNDLE_CONF=$<CONFIG>
        -DBUNDLE_WIN=${_bundle_win}
        -DBUNDLE_MINGW=$<BOOL:${MINGW}>
        -DBUNDLE_QT_BIN=${_qt_bin}
        -DBUNDLE_WDT=${WINDEPLOYQT_EXE}
        # 脚本源 = 内核 core/python/scripts（monorepo 契约：前端不持有脚本副本），
        # 替代旧仓库 BUNDLE_SRC 推出的 <src>/scripts。
        -DBUNDLE_SCRIPTS=${GO_CORE_SCRIPTS_DIR}
        # QML 源目录（bundle 里 windeployqt 的 --qmldir 用），
        # 替代旧仓库 BUNDLE_SRC 推出的 <src>/src/qml。
        -DBUNDLE_QMLDIR=${CMAKE_CURRENT_SOURCE_DIR}/src/qml
        # Python embeddable 下载缓存，挪到仓库根 .cache/（只下一次），
        # 替代旧仓库 BUNDLE_SRC 推出的 <src>/.cache。
        -DBUNDLE_CACHE=${CMAKE_SOURCE_DIR}/.cache
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bundle.cmake
    VERBATIM
)

# ---- release_zip: 把 output/x64-<前端>/<配置> 压成发行 zip ----
# cmake -E tar 走 libarchive，写 zip 不需要外部工具。
# zip 路径由根 CMakeLists 单点定义（GO_ZIP_FILE），命名规则：
#   GeneralOperations-<版本>_x64-<前端>-<release|debug>.zip
# （末段是 CMAKE_BUILD_TYPE 的小写）。GO_ZIP_FILE 仅单配置（CMAKE_BUILD_TYPE
# 非空）下定义，多配置生成器下 release_zip 不可用——见根 CMakeLists 的守卫说明。
# 用法（仓库根执行）：cmake --build --preset qt5-mingw-release --target release_zip
if(GO_ZIP_FILE)
    add_custom_target(release_zip
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${GO_ZIP_FILE}"
        COMMAND "${CMAKE_COMMAND}" -E chdir "${GO_BUNDLE_DIR}/${CMAKE_BUILD_TYPE}"
                "${CMAKE_COMMAND}" -E tar "cf" "${GO_ZIP_FILE}" --format=zip "."
        DEPENDS GeneralOperationsLauncher
        COMMENT "打包 ${GO_ZIP_FILE}"
        VERBATIM
    )
endif()

# ---- windeployqt: 分发用 ----
if(WIN32)
    if(WINDEPLOYQT_EXE)
        set(_deploy_args
            --release
            --no-translations
            --no-svg
            # 让 windeployqt 扫 import 语句据此部署 QML 模块。
            # QML 项目漏了这个，分发包能启动但窗口全白。
            --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/src/qml"
        )
        # --no-compiler-runtime 只对 MSVC 成立（跳过 VC redist）。
        # MinGW 下 compiler runtime 就是 libstdc++/libgcc/libwinpthread 这三个，
        # 而 Qt5 的 dll 硬依赖它们 —— 跳过就是分发一个跑不起来的包。
        if(MSVC)
            list(APPEND _deploy_args --no-compiler-runtime)
        endif()

        # 刻意不加 --no-opengl-sw：Quick 的场景图要 OpenGL，
        # 那 3MB 软件光栅是驱动不给力时唯一的兜底。
        # （Widgets 时期加它是对的，QML 路线下加它是错的。）

        add_custom_target(deploy
            # 先删掉开发期的 qt.conf，否则它会覆盖 windeployqt 的路径解析，
            # 分发包到了别人机器上就去找一个不存在的 D:/Lib/... 
            COMMAND "${CMAKE_COMMAND}" -E rm -f "$<TARGET_FILE_DIR:GeneralOperationsLauncher>/qt.conf"
            COMMAND "${WINDEPLOYQT_EXE}" ${_deploy_args} "$<TARGET_FILE:GeneralOperationsLauncher>"
            COMMENT "windeployqt: 收集 Qt5 运行时 + QML 模块"
            VERBATIM
        )
        add_dependencies(deploy GeneralOperationsLauncher)
    else()
        message(WARNING "找不到 windeployqt，'deploy' 目标不可用")
    endif()
endif()
