# bundle.cmake —— 前端共同调用的发行装配脚本（cmake -P），
# 把构建产物装配成自包含的 output/<配置>/：
#   exe + scripts/ + Qt 运行时（windeployqt）+ 相对路径 qt.conf
#   + Python 启动器/探针 + requirements.txt（Windows）或 run.sh 首次运行建 venv（Linux）
#   + 后端可执行文件（@host exe 的执行体，构建产物收集进 tools/<名>/）
#
# 全程相对路径：成品目录整体拷走/压 zip 后可启动；Windows 上执行
# Python 脚本时由同级启动器在目标机准备解释器及依赖（需要网络）。
# 布局与部署说明见 docs/RELEASE.md。
#
# 参数（-D 传入）：
#   BUNDLE_EXE      构建产物绝对路径
#   BUNDLE_OUT      发行装配目标目录（CMake 侧按 GO_BUNDLE_DIR/<配置> 算好传入）
#   BUNDLE_CONF     Debug / Release（windeployqt 旗标用）
#   BUNDLE_WIN      1/0
#   BUNDLE_MINGW    1/0（MinGW 时 windeployqt 需带 --compiler-runtime）
#   BUNDLE_WDT      windeployqt 绝对路径（找不到传 WINDEPLOYQT_EXE-NOTFOUND）
#   BUNDLE_SCRIPTS  内核脚本目录（GO_CORE_SCRIPTS_DIR = core/python/scripts）：
#                   原样拷进装配目录 scripts/
#   BUNDLE_CATALOG  go_script_catalog 可执行文件；解析 BUNDLE_SCRIPTS 并生成
#                   requirements.txt，声明读取统一经过 core/cpp catalog
#   BUNDLE_RUNTIME  共享运行时工具目录（GO_PYTHON_RUNTIME_DIR）：将
#                   python-launcher.ps1 与 runtime-check.py 原样拷到 exe 同级
#   BUNDLE_QMLDIR   前端 QML 源目录，windeployqt 的 --qmldir 用
#                   —— 替代旧仓库从 BUNDLE_SRC 推出的 <src>/src/qml
#   BUNDLE_BACKENDS 后端可执行文件路径（@host exe 的执行体，core/cpp 构建
#                   产物，可为空）。逐个收进 tools/<去扩展名>/
#   BUNDLE_TOOLS    供前端调用的共享 core 工具路径（可为空），同样收进
#                   tools/<去扩展名>/；不作为 @host exe 执行体。
#   BUNDLE_UI_KIND  qt5（默认）/ tauri2；只控制前端运行时部署

if(NOT BUNDLE_UI_KIND)
    set(BUNDLE_UI_KIND "qt5")
endif()
if(NOT BUNDLE_UI_KIND MATCHES "^(qt5|tauri2)$")
    message(FATAL_ERROR "[bundle] 未知 BUNDLE_UI_KIND=${BUNDLE_UI_KIND}")
endif()
if(NOT BUNDLE_CONF MATCHES "^(Debug|Release)$")
    message(FATAL_ERROR "[bundle] BUNDLE_CONF 必须是 Debug 或 Release，当前为 '${BUNDLE_CONF}'")
endif()
if(NOT BUNDLE_OUT)
    message(FATAL_ERROR "[bundle] 必须提供 BUNDLE_OUT")
endif()
if(NOT BUNDLE_SCRIPTS OR NOT IS_DIRECTORY "${BUNDLE_SCRIPTS}")
    message(FATAL_ERROR "[bundle] BUNDLE_SCRIPTS 不存在或不是目录：${BUNDLE_SCRIPTS}")
endif()
if(NOT BUNDLE_CATALOG OR NOT EXISTS "${BUNDLE_CATALOG}" OR IS_DIRECTORY "${BUNDLE_CATALOG}")
    message(FATAL_ERROR "[bundle] 必须提供 go_script_catalog 可执行文件 BUNDLE_CATALOG：${BUNDLE_CATALOG}")
endif()

# 依赖列表只能由 core/cpp 的单一声明解析器生成。先在清理输出目录前
# 完整运行并校验 catalog，避免 CLI 缺失或结果无效时留下半成品发行目录。
execute_process(
        COMMAND "${BUNDLE_CATALOG}" --root "${BUNDLE_SCRIPTS}"
        RESULT_VARIABLE _catalog_result
        OUTPUT_VARIABLE _catalog_json
        ERROR_VARIABLE _catalog_error)
if(NOT "${_catalog_result}" STREQUAL "0")
    message(FATAL_ERROR
            "[bundle] go_script_catalog 执行失败 (rv=${_catalog_result})：${_catalog_error}")
endif()
string(JSON _catalog_type ERROR_VARIABLE _catalog_json_error TYPE "${_catalog_json}")
if(NOT _catalog_json_error STREQUAL "NOTFOUND" OR NOT _catalog_type STREQUAL "ARRAY")
    message(FATAL_ERROR "[bundle] go_script_catalog 输出不是有效的 JSON 数组：${_catalog_json_error}")
endif()
string(JSON _catalog_count ERROR_VARIABLE _catalog_json_error LENGTH "${_catalog_json}")
if(NOT _catalog_json_error STREQUAL "NOTFOUND")
    message(FATAL_ERROR "[bundle] 无法读取 catalog JSON 长度：${_catalog_json_error}")
endif()

set(_reqs "")
set(_req_keys "")
set(_script_index 0)
while(_script_index LESS _catalog_count)
    string(JSON _manifest_type ERROR_VARIABLE _catalog_json_error TYPE "${_catalog_json}" ${_script_index})
    if(NOT _catalog_json_error STREQUAL "NOTFOUND" OR NOT _manifest_type STREQUAL "OBJECT")
        message(FATAL_ERROR "[bundle] catalog 项 ${_script_index} 不是有效的对象：${_catalog_json_error}")
    endif()
    string(JSON _requirements_type ERROR_VARIABLE _catalog_json_error TYPE
            "${_catalog_json}" ${_script_index} "requires")
    if(NOT _catalog_json_error STREQUAL "NOTFOUND" OR NOT _requirements_type STREQUAL "ARRAY")
        message(FATAL_ERROR "[bundle] catalog 项 ${_script_index} 的 requires 不是数组：${_catalog_json_error}")
    endif()
    string(JSON _requirement_count ERROR_VARIABLE _catalog_json_error LENGTH
            "${_catalog_json}" ${_script_index} "requires")
    if(NOT _catalog_json_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR "[bundle] 无法读取 catalog 项 ${_script_index} 的 requires：${_catalog_json_error}")
    endif()

    set(_requirement_index 0)
    while(_requirement_index LESS _requirement_count)
        string(JSON _requirement_type ERROR_VARIABLE _catalog_json_error TYPE
                "${_catalog_json}" ${_script_index} "requires" ${_requirement_index})
        if(NOT _catalog_json_error STREQUAL "NOTFOUND" OR NOT _requirement_type STREQUAL "STRING")
            message(FATAL_ERROR "[bundle] catalog 项 ${_script_index} 的 requires[${_requirement_index}] 不是字符串：${_catalog_json_error}")
        endif()
        string(JSON _raw_requirement ERROR_VARIABLE _catalog_json_error GET
                "${_catalog_json}" ${_script_index} "requires" ${_requirement_index})
        if(NOT _catalog_json_error STREQUAL "NOTFOUND")
            message(FATAL_ERROR "[bundle] 无法读取 catalog 项 ${_script_index} 的 requires[${_requirement_index}]：${_catalog_json_error}")
        endif()
        string(REGEX REPLACE "\\?$" "" _requirement "${_raw_requirement}")
        if(NOT _requirement MATCHES "^[A-Za-z0-9_.-]+(==[A-Za-z0-9.*+!-]+)?$")
            message(FATAL_ERROR "[bundle] catalog 项 ${_script_index} 含无效依赖声明：${_raw_requirement}")
        endif()
        string(TOLOWER "${_requirement}" _requirement_key)
        list(FIND _req_keys "${_requirement_key}" _requirement_seen)
        if(_requirement_seen EQUAL -1)
            list(APPEND _req_keys "${_requirement_key}")
            list(APPEND _reqs "${_requirement}")
        endif()
        math(EXPR _requirement_index "${_requirement_index} + 1")
    endwhile()
    math(EXPR _script_index "${_script_index} + 1")
endwhile()

# 递归清理只允许发生在本仓库约定的 output/x64-<前端>/<配置> 目录。
# 先校验词法路径，再创建目录并检查解析后的物理路径，拒绝越界参数和目录符号链接。
get_filename_component(_bundle_repo_root "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)
get_filename_component(_expected_out
        "${_bundle_repo_root}/output/x64-${BUNDLE_UI_KIND}/${BUNDLE_CONF}" ABSOLUTE)
get_filename_component(_provided_out "${BUNDLE_OUT}" ABSOLUTE)
if(WIN32)
    string(TOLOWER "${_expected_out}" _expected_out_cmp)
    string(TOLOWER "${_provided_out}" _provided_out_cmp)
else()
    set(_expected_out_cmp "${_expected_out}")
    set(_provided_out_cmp "${_provided_out}")
endif()
if(NOT _provided_out_cmp STREQUAL _expected_out_cmp)
    message(FATAL_ERROR
            "[bundle] BUNDLE_OUT 必须是仓库发行目录 '${_expected_out}'，当前为 '${_provided_out}'")
endif()
file(MAKE_DIRECTORY "${_expected_out}")
file(REAL_PATH "${_expected_out}" _resolved_out)
if(WIN32)
    string(TOLOWER "${_resolved_out}" _resolved_out_cmp)
else()
    set(_resolved_out_cmp "${_resolved_out}")
endif()
if(NOT _resolved_out_cmp STREQUAL _expected_out_cmp)
    message(FATAL_ERROR
            "[bundle] 解析后的 BUNDLE_OUT 越出仓库发行目录：${_resolved_out}")
endif()

set(_out "${_resolved_out}")

# 清掉旧版发行包遗留的大体积运行时与第三方下载工具。逐个确认实体
# 仍在已核验的装配目录里，拒绝旧目录被改成指向外部的链接。
foreach(_legacy_rel python tools/realesrgan)
    if(EXISTS "${_out}/${_legacy_rel}")
        file(REAL_PATH "${_out}/${_legacy_rel}" _legacy_real)
        if(NOT _legacy_real STREQUAL "${_out}/${_legacy_rel}")
            message(FATAL_ERROR "[bundle] 遗留目录是指向别处的链接：${_out}/${_legacy_rel}")
        endif()
        file(REMOVE_RECURSE "${_out}/${_legacy_rel}")
    endif()
endforeach()

# ---- 1. exe + scripts（运行期 scripts 解析优先 exe 同级，见 main.cpp）----
file(COPY "${BUNDLE_EXE}" DESTINATION "${_out}")
file(COPY "${BUNDLE_SCRIPTS}" DESTINATION "${_out}"
     FILES_MATCHING PATTERN "*.py")

# ---- 1b. 后端可执行文件（@host exe 的执行体，构建产物收集）----
# 这里收的是本仓 core/cpp 编出的后端 exe（BUNDLE_BACKENDS
# 由前端 CMakeLists 以 $<TARGET_FILE:go_pmx2glb> 传入，可为空 —— 空列表时
# 本段一个字节都不动）。启动器按脚本头 @exe <名> 到 tools/<名>/ 找执行体，
# 故目录名取 exe 去扩展名，文件保持原名（含扩展名）。先建后端再装配的时序
# 由前端 CMakeLists 的 add_dependencies 钉死；万一产物仍缺失就直接报错中断
# 构建 —— 宁可构建失败，也不静默出一个缺后端的发行包。
foreach(_backend ${BUNDLE_BACKENDS})
    if(_backend STREQUAL "")
        continue()
    endif()
    if(NOT EXISTS "${_backend}")
        message(FATAL_ERROR
                "[bundle] 后端可执行文件不存在：${_backend}"
                "（核对前端 CMakeLists 的 BUNDLE_BACKENDS 传参与 add_dependencies）")
    endif()
    get_filename_component(_bk_dir "${_backend}" NAME_WE)
    # 落位 ${BUNDLE_OUT}/tools/${_bk_dir}/<原名含扩展名>，
    # Windows 下即 tools/go_pmx2glb/go_pmx2glb.exe。
    file(COPY "${_backend}" DESTINATION "${_out}/tools/${_bk_dir}")
endforeach()

# 共享 core 命令行工具与 @host exe 后端落位相同，但用途不同。Tauri 用
# go_script_catalog 读取与 Qt 静态链接的同一套声明解析实现。
foreach(_tool ${BUNDLE_TOOLS})
    if(_tool STREQUAL "")
        continue()
    endif()
    if(NOT EXISTS "${_tool}")
        message(FATAL_ERROR "[bundle] 共享 core 工具不存在：${_tool}")
    endif()
    get_filename_component(_tool_dir "${_tool}" NAME_WE)
    file(COPY "${_tool}" DESTINATION "${_out}/tools/${_tool_dir}")
endforeach()

# ---- 2. requirements.txt：唯一来源是共享 catalog 的 requires 字段 ----
# 带 ? 的依赖也进入清单；Windows 启动器在运行前检查全部依赖。
#
# 【架构约束】后端（C++ core + Python 脚本）不依赖 Qt：Qt 只存在于前端。
# 这里顺手检查脚本源码，发现 Qt 绑定就直接报错拦下；依赖声明不再扫描源码。
file(GLOB_RECURSE _py_files "${BUNDLE_SCRIPTS}/*.py")
foreach(_f ${_py_files})
    file(READ "${_f}" _txt)
    string(REGEX MATCH "(PySide[0-9]?|PyQt[56]?|qtpy)" _qt_hit "${_txt}")
    if(_qt_hit)
        message(FATAL_ERROR "${_f} 引用了 Qt 绑定（${_qt_hit}）。脚本层是纯后端，禁止依赖 Qt —— 否则 Python 环境就要跟着 Qt 的版本和许可走了。")
    endif()
endforeach()
list(JOIN _reqs "\n" _req_text)
file(WRITE "${_out}/requirements.txt" "${_req_text}\n")

if(BUNDLE_WIN)
  if(NOT BUNDLE_UI_KIND STREQUAL "tauri2")
    # ---- 3. Qt 运行时：windeployqt 装配 ----
    # stamp 增量：exe 每次重链都会变新，所以每次构建都会重跑一遍
    # windeployqt（几秒，copy 代价近零），但 Qt 升级/被删后重建也能自愈。
    if(BUNDLE_WDT)
        set(_stamp "${_out}/.windeployqt.stamp")
        set(_exe "${_out}/GeneralOperationsLauncher.exe")
        file(TIMESTAMP "${_exe}" _exe_ts)
        file(TIMESTAMP "${_stamp}" _stamp_ts)
        if(NOT EXISTS "${_stamp}" OR _stamp_ts STRLESS "${_exe_ts}")
            set(_deploy_cmd "${BUNDLE_WDT}" --dir "${_out}"
                    --no-translations --no-svg
                    --qmldir "${BUNDLE_QMLDIR}")
            if(BUNDLE_MINGW)
                # MinGW 的三个运行时 dll（libgcc/libstdc++/libwinpthread）
                # 由 windeployqt 的 --compiler-runtime 负责，跳过 = 白包。
                list(APPEND _deploy_cmd --compiler-runtime)
            endif()
            # 【不要加 --release/--debug 旗标】5.15.2 的 windeployqt 在
            # 显式 --release 下会报 "Unable to find the platform plugin"
            # （kit 里平台插件的变体名和它要找的对不上）；不带旗标时它
            # 按产物自行判断，实测正常。MinGW 的 Qt 只有 release 二进制，
            # Debug/Release 配置部署的实际是同一套 Qt 运行时。
            # 【PATH 必须清洗】本机 PATH 里有别的 Qt 安装（如 Qt 6.10.1），
            # windeployqt 沿 Windows 加载器的搜索顺序解析传递依赖时会撞进去，
            # 对 5.15 的产物报找不到 Qt5Core.dll。用 cmake -E env 给子进程
            # 一个只含本 kit 的 PATH（CMake 4.x 的 execute_process 已没有
            # ENVIRONMENT 选项，-E env 是跨版本稳定的等价物）。
            execute_process(COMMAND
                            "${CMAKE_COMMAND}" -E env
                            "PATH=${BUNDLE_QT_BIN};C:\\Windows\\System32;C:\\Windows"
                            ${_deploy_cmd} "${BUNDLE_EXE}"
                            RESULT_VARIABLE _rv
                            OUTPUT_VARIABLE _deploy_out
                            ERROR_VARIABLE _deploy_err)
            if(_rv EQUAL 0)
                file(WRITE "${_stamp}" "")
                # windeployqt 会把 QtQuick 目录整棵拷下，其中 VirtualKeyboard
                # （qml + 插件，约 15MB）只有触屏输入法用到——本应用永远
                # import 不到它，纯行李，剪掉。
                if(EXISTS "${_out}/QtQuick/VirtualKeyboard")
                    file(REMOVE_RECURSE "${_out}/QtQuick/VirtualKeyboard")
                endif()
            else()
                message(WARNING "windeployqt 失败（rv=${_rv}）: ${_deploy_err} ${_deploy_out}")
            endif()
        endif()
        # qt.conf 指向自身目录 —— 成品与开发机的 Qt 安装位置彻底解耦
        #（开发期的 build 目录里那份 qt.conf 是绝对路径，勿混用）。
        file(WRITE "${_out}/qt.conf" "[Paths]\nPrefix=.\n")
    else()
        message(WARNING "找不到 windeployqt，output/${BUNDLE_CONF} 缺 Qt 运行时，无法独立运行")
    endif()

  endif()
    # ---- Windows Python 启动器与运行时探针（源码随包分发）----
    if(NOT BUNDLE_RUNTIME OR NOT IS_DIRECTORY "${BUNDLE_RUNTIME}")
        message(FATAL_ERROR "[bundle] BUNDLE_RUNTIME 不存在：${BUNDLE_RUNTIME}")
    endif()
    foreach(_runtime_file python-launcher.ps1 runtime-check.py)
        if(NOT EXISTS "${BUNDLE_RUNTIME}/${_runtime_file}")
            message(FATAL_ERROR "[bundle] 运行时文件不存在：${BUNDLE_RUNTIME}/${_runtime_file}")
        endif()
        file(COPY "${BUNDLE_RUNTIME}/${_runtime_file}" DESTINATION "${_out}")
    endforeach()

    # windeployqt 会把以下未使用插件带入发行目录，且只增不删；构建后统一剪除。
    # Tauri 不部署 Qt，因而只保留通用脚本缓存清理。
    if(NOT BUNDLE_UI_KIND STREQUAL "tauri2")
        foreach(_rel
                Qt5RemoteObjects.dll
                QtQml/RemoteObjects
                virtualkeyboard
                platforminputcontexts
                qmltooling
                bearer
                imageformats/qgif.dll
                imageformats/qicns.dll
                imageformats/qjpeg.dll
                imageformats/qtga.dll
                imageformats/qtiff.dll
                imageformats/qwbmp.dll
                imageformats/qwebp.dll)
            if(EXISTS "${_out}/${_rel}")
                file(REMOVE_RECURSE "${_out}/${_rel}")
            endif()
        endforeach()
    endif()
    file(REMOVE_RECURSE "${_out}/scripts/__pycache__")
else()
    # ---- Linux：前端运行时由系统提供；Python 依赖由 run.sh 首次运行建 venv ----
    get_filename_component(_launcher_name "${BUNDLE_EXE}" NAME)
    # venv 不能在构建期预装 —— 它绑定 glibc/解释器 ABI，换机器即废。
    file(WRITE "${_out}/run.sh"
"#!/bin/sh
# Linux 运行入口。Qt 走系统包（依赖清单见 docs/RELEASE.md）；
# Python 依赖在首次运行时建 venv 逐包安装（单包失败不影响其余），
# 之后直接启动。脚本层是纯后端，不依赖 Qt。
cd \"$(dirname \"$0\")\" || exit 1
if [ ! -x runtime/venv/bin/python ]; then
    echo \"[部署] 创建 venv 并安装 requirements.txt ...\"
    python3 -m venv runtime/venv || exit 1
    while read -r pkg; do
        runtime/venv/bin/pip install --disable-pip-version-check \"$pkg\" \\
            || echo \"[警告] $pkg 安装失败，相关脚本将不可用\"
    done < requirements.txt
fi
exec ./\"${_launcher_name}\" \"$@\"
")
    file(CHMOD "${_out}/run.sh" PERMISSIONS
         OWNER_READ OWNER_WRITE OWNER_EXECUTE
         GROUP_READ GROUP_EXECUTE
         WORLD_READ WORLD_EXECUTE)
endif()

message(STATUS "[bundle] output/${BUNDLE_CONF} 装配完成")
