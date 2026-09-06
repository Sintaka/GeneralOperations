# bundle.cmake —— 由 CMakeLists 的 POST_BUILD 调用（cmake -P），
# 把构建产物装配成自包含的 output/<配置>/：
#   exe + scripts/ + Qt 运行时（windeployqt）+ 相对路径 qt.conf
#   + 内嵌 Python（Windows）或 run.sh 首次运行建 venv（Linux）
#
# 全程相对路径：成品目录整体拷走/压 zip 后，任何同架构机器可直接运行。
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
#                   原样拷进装配目录 scripts/，并汇总脚本头 @requires 生成
#                   requirements.txt —— 替代旧仓库从 BUNDLE_SRC 推出的 <src>/scripts
#   BUNDLE_QMLDIR   前端 QML 源目录，windeployqt 的 --qmldir 用
#                   —— 替代旧仓库从 BUNDLE_SRC 推出的 <src>/src/qml
#   BUNDLE_CACHE    下载缓存目录（仓库根 .cache/），embeddable Python zip 与
#                   get-pip.py 只下一次 —— 替代旧仓库从 BUNDLE_SRC 推出的 <src>/.cache

set(_out "${BUNDLE_OUT}")
file(MAKE_DIRECTORY "${_out}")

# ---- 1. exe + scripts（运行期 scripts 解析优先 exe 同级，见 main.cpp）----
file(COPY "${BUNDLE_EXE}" DESTINATION "${_out}")
file(COPY "${BUNDLE_SCRIPTS}" DESTINATION "${_out}"
     FILES_MATCHING PATTERN "*.py")

# ---- 2. requirements.txt：唯一来源是脚本头的 @requires ----
# 启动器不为依赖维护第二份清单，脚本头加什么这里就出现什么。
# 可选依赖（带 ?）同样装上，装不上时脚本自身的容错逻辑兜底。
#
# 【架构约束】后端（C++ core + Python 脚本）不依赖 Qt：Qt 只存在于前端。
# 这里顺手检查脚本源码，发现 Qt 绑定就直接报错拦下。
set(_reqs "")
file(GLOB _py_files "${BUNDLE_SCRIPTS}/*.py")
foreach(_f ${_py_files})
    file(READ "${_f}" _txt)
    string(REGEX MATCH "(PySide[0-9]?|PyQt[56]?|qtpy)" _qt_hit "${_txt}")
    if(_qt_hit)
        message(FATAL_ERROR "${_f} 引用了 Qt 绑定（${_qt_hit}）。脚本层是纯后端，禁止依赖 Qt —— 否则 Python 环境就要跟着 Qt 的版本和许可走了。")
    endif()
    string(REGEX MATCHALL "@requires[ \t]+[^ \t][^\r\n]*" _lines "${_txt}")
    foreach(_line ${_lines})
        string(REGEX REPLACE "^@requires[ \t]+" "" _line "${_line}")
        string(REPLACE " " ";" _toks "${_line}")
        foreach(_tok ${_toks})
            if(_tok STREQUAL "")
                continue()
            endif()
            string(REGEX REPLACE "\\?$" "" _tok "${_tok}")
            list(APPEND _reqs "${_tok}")
        endforeach()
    endforeach()
endforeach()
set(_has_reqs 0)
if(_reqs)
    list(REMOVE_DUPLICATES _reqs)
    list(JOIN _reqs "\n" _req_text)
    file(WRITE "${_out}/requirements.txt" "${_req_text}\n")
    set(_has_reqs 1)
endif()

string(TOLOWER "${BUNDLE_CONF}" _conf_lower)

if(BUNDLE_WIN)
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

    # ---- 4. 内嵌 Python（官方 embeddable 包，构建期展开 + 装依赖）----
    # 目标机零部署：zip 解压即可跑，不依赖系统 Python。
    # 下载缓存在 .cache/（已 gitignore），构建期需联网一次。
    set(_py_dir "${_out}/python")
    set(_cache "${BUNDLE_CACHE}")
    file(MAKE_DIRECTORY "${_cache}")
    set(_py_zip "${_cache}/python-3.12.10-embed-amd64.zip")
    set(_py_zip_url "https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip")
    if(NOT EXISTS "${_py_zip}")
        message(STATUS "[bundle] 下载 Python embeddable 包（一次性，约 11MB）")
        file(DOWNLOAD "${_py_zip_url}" "${_py_zip}" SHOW_PROGRESS STATUS _dl)
        list(GET _dl 0 _dl_code)
        if(NOT _dl_code EQUAL 0)
            file(REMOVE "${_py_zip}")
            message(WARNING "Python embeddable 包下载失败，发行包将回退系统 Python")
        endif()
    endif()
    if(EXISTS "${_py_zip}" AND NOT EXISTS "${_py_dir}/python.exe")
        file(REMOVE_RECURSE "${_py_dir}")
        file(MAKE_DIRECTORY "${_py_dir}")
        # CMake 4.0 起 ARCHIVE_EXTRACT 的压缩包参数改成了 INPUT 关键字，
        # 旧的位置参数形式被移除。按运行中的 CMake 版本走对应写法。
        if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
            file(ARCHIVE_EXTRACT INPUT "${_py_zip}" DESTINATION "${_py_dir}")
        else()
            file(ARCHIVE_EXTRACT "${_py_zip}" DESTINATION "${_py_dir}")
        endif()
        # ._pth 默认注释掉 site：解开它，本地 Lib/site-packages 才进搜索路径。
        file(GLOB _pth_files "${_py_dir}/*._pth")
        foreach(_pth ${_pth_files})
            file(READ "${_pth}" _pth_txt)
            string(REPLACE "#import site" "import site" _pth_txt "${_pth_txt}")
            file(WRITE "${_pth}" "${_pth_txt}\nLib/site-packages\n")
        endforeach()
    endif()
    if(_has_reqs AND EXISTS "${_py_dir}/python.exe")
        set(_pip_stamp "${_py_dir}/.pip.stamp")
        set(_req_file "${_out}/requirements.txt")
        file(TIMESTAMP "${_req_file}" _req_ts)
        file(TIMESTAMP "${_pip_stamp}" _pip_ts)
        if(NOT EXISTS "${_pip_stamp}" OR _pip_ts STRLESS "${_req_ts}")
            set(_getpip "${_cache}/get-pip.py")
            if(NOT EXISTS "${_getpip}")
                file(DOWNLOAD "https://bootstrap.pypa.io/get-pip.py" "${_getpip}"
                     SHOW_PROGRESS STATUS _dl)
                list(GET _dl 0 _dl_code)
            else()
                set(_dl_code 0)
            endif()
            if(_dl_code EQUAL 0)
                set(_pyexe "${_py_dir}/python.exe")
                execute_process(COMMAND "${_pyexe}" "${_getpip}"
                                --no-warn-script-location -q
                                RESULT_VARIABLE _rv ERROR_VARIABLE _err)
                if(_rv EQUAL 0)
                    # 逐包安装：pip 对 -r 文件是全有或全无，一个包没有
                    # wheel 会让整批依赖都装不上（实测 PyOpenColorIO）。
                    # 单包失败只影响它自己，构建继续。
                    foreach(_pkg ${_reqs})
                        execute_process(COMMAND "${_pyexe}" -m pip install
                                        --no-warn-script-location -q "${_pkg}"
                                        RESULT_VARIABLE _rv ERROR_VARIABLE _err)
                        if(NOT _rv EQUAL 0)
                            message(WARNING "[bundle] ${_pkg} 安装失败（该脚本回退系统 Python）: ${_err}")
                        endif()
                    endforeach()
                    file(WRITE "${_pip_stamp}" "")
                else()
                    message(WARNING "[bundle] get-pip 执行失败（目标机将回退系统 Python）: ${_err}")
                endif()
            else()
                message(WARNING "[bundle] get-pip.py 下载失败，发行包将回退系统 Python")
            endif()
        endif()

        # ---- 5. 发行剪除：windeployqt 只增不删，行李在管线末尾统一清 ----
        # 依据 output 产物的 objdump 导入闭包审计（详见 docs/RELEASE.md 审计节）：
        #   - Qt5RemoteObjects + QtQml/RemoteObjects：全树唯一导入者是彼此，
        #     exe/QML 均不引用，windeployqt 扫 QtQml 时捎带的 —— 原子剪除。
        #   - virtualkeyboard/ + platforminputcontexts/：导入不存在的
        #     Qt5VirtualKeyboard.dll 的死插件；常规中文 IME 走 qwindows 的 IMM32。
        #   - qmltooling/：仅 QML 调试/profiler 会话加载，发行无用。
        #   - bearer/：Qt 5.15 默认已禁用 bearer 管理，无人静态导入。
        #   - imageformats 除 qico：应用不加载任何位图文件；qico 必须留
        #     （QIcon 读 .ico 走它，剪了窗口图标就哑）。
        # 【不能剪】Qt5Network.dll（Qt5Qml/Qt5Quick 静态导入，剪=启动失败）、
        # libEGL/libGLESv2/D3Dcompiler_47（ANGLE 渲染链）、opengl32sw.dll
        # （软件渲染兜底 —— 兼容性 > 体积，见 DEVELOPMENT.md 约束优先级）。
        # 必须放在分支末尾：exe 每次重链 windeployqt 都会把文件灌回来，
        # 这里统一清一遍才保证终态干净。pip 亦同理（安装完即卸，重装分支
        # 会先无条件补回 get-pip，自愈闭环）。
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
        file(REMOVE_RECURSE "${_py_dir}/Lib/site-packages/pip"
                            "${_py_dir}/Lib/site-packages/__pycache__"
                            "${_out}/scripts/__pycache__")
        file(GLOB _pip_leftovers
             "${_py_dir}/Lib/site-packages/pip-*.dist-info"
             "${_py_dir}/Scripts/pip*.exe"
             "${_py_dir}/Scripts/f2py*.exe"
             "${_py_dir}/Scripts/numpy-config*")
        if(_pip_leftovers)
            file(REMOVE ${_pip_leftovers})
        endif()
    endif()
else()
    # ---- Linux：Qt 运行时来自系统包；Python 依赖由 run.sh 首次运行建 venv ----
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
exec ./GeneralOperationsLauncher \"$@\"
")
    file(CHMOD "${_out}/run.sh" PERMISSIONS
         OWNER_READ OWNER_WRITE OWNER_EXECUTE
         GROUP_READ GROUP_EXECUTE
         WORLD_READ WORLD_EXECUTE)
endif()

message(STATUS "[bundle] output/${BUNDLE_CONF} 装配完成")
