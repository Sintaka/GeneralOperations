// 切片 2：脚本树从 C++ 模型喂给 QML ListView。
//
// core/ 是纯 C++（解析、扫描），model/ 用 QAbstractListModel 做桥，
// QML 只认模型不认 core。依赖方向单向：qml → model → core。

#include "debug/DropDebugLogger.h"
#include "model/ScriptListModel.h"
#include "model/ScriptRunner.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif

namespace {

/// 开发期脚本目录由 CMake 注入（见 ui/qt5/CMakeLists.txt 的 GO_DEV_SCRIPTS_DIR）。
/// #ifndef 兜底定义为空串：保证脱离构建系统单独编译时也能过预处理，而不是炸在
/// 未定义宏上；空串在下方解析链里会被跳过，落到 c) 的报错分支。
#ifndef GO_DEV_SCRIPTS_DIR
#define GO_DEV_SCRIPTS_DIR ""
#endif

/// 找 scripts/ 目录。monorepo 里脚本实体统一住在内核 core/python/scripts，
/// 前端不持有脚本副本，解析链相应改为（顺序有意发行优先，见
/// docs/ARCHITECTURE.md「脚本目录解析链」——任何改动最终都要通过发行形态验证）：
///   a) exe 同级 scripts/ —— 发行形态：bundle 管线把 core/python/scripts 拷到 exe 旁；
///   b) GO_DEV_SCRIPTS_DIR 宏 —— 开发形态：CMake 编译期注入的内核脚本绝对路径，
///      让 build/ 里的裸 exe 不经装配也能跑起来；
///   c) 都不存在 → 返回 a) 的路径，让 ScriptRegistry 报"目录不存在"。
/// 旧版"向上 cdUp 两层找 scripts"的分支已删：monorepo 里 build/<preset> 上两层
/// 是仓库根，scripts/ 已不在那里（脚本搬进了 core/python/scripts）。
QString resolveScriptsDir()
{
    const QString beside = QCoreApplication::applicationDirPath() + QStringLiteral("/scripts");
    if (QFileInfo(beside).isDir())
        return beside;

    const QString dev = QStringLiteral(GO_DEV_SCRIPTS_DIR);
    if (!dev.isEmpty() && QFileInfo(dev).isDir())
        return dev;

    // 找不到就返回同级路径，让 ScriptRegistry 报"目录不存在"，
    // UI 上能看到原因，而不是静默显示一个空列表。
    return beside;
}

/// 给窗口申请 Win11 系统圆角。
///
/// FramelessWindowHint 的窗口 Windows 不会自动圆角（自动圆角只给带
/// WS_THICKFRAME 的常规窗口），无边框的要显式向 DWM 申请。
///
/// 【为什么用 DWM 而不是 QML 侧透明自绘】自绘要求窗口级半透明合成，
/// 四角抗锯齿、resize 时的边缘质量、拖动中的重绘全要自己扛；DWM 圆角
/// 是系统合成的，全部白拿，观感和系统其它窗口一致。
///
/// 【降级】Win10 没有这个属性，DwmSetWindowAttribute 返回失败码，
/// 窗口保持直角 —— 圆角是纯增益，不是功能依赖，静默降级即可。
///
/// 33 = DWMWA_WINDOW_CORNER_PREFERENCE，2 = DWMWCP_ROUND。
/// 这两个常量是 Win11 SDK 才加入的，MinGW 8.1 的 dwmapi.h 里没有，只能写数值。
void applyRoundedCorners(QWindow *window)
{
#ifdef Q_OS_WIN
    const int preference = 2;   // DWMWCP_ROUND：系统默认圆角半径
    const HRESULT hr = DwmSetWindowAttribute(reinterpret_cast<HWND>(window->winId()),
                                             33, &preference, sizeof(preference));
    // 结果打进启动日志：hr = 0 就是已应用；非 0（Win10 或远程会话）是
    // 预期内的降级，留着这行以后排查"为什么没圆角"不用翻源码。
    qInfo().noquote() << "[窗口] 系统圆角申请 hr =" << QString::number(quintptr(hr), 16);
#else
    Q_UNUSED(window)
#endif
}

/// 补回 WS_MINIMIZEBOX | WS_SYSMENU：修"Win11 点任务栏不最小化"。
///
/// 【为什么】点任务栏图标时，Explorer 是向窗口发 SC_MINIMIZE 系统命令，
/// 而只有带 WS_MINIMIZEBOX/WS_SYSMENU 样式的窗口才会执行它。
/// FramelessWindowHint 建窗口时把这两个样式一起剥掉了，任务栏点击就哑了
/// ——窗口本身收不到命令，QML 侧无论如何都拦不到。
///
/// 【为什么安全】这两个样式只控制系统菜单行为，不带来任何非客户区外观
/// （标题栏早被 FramelessWindowHint 去掉了），窗口视觉零变化。
void enableTaskbarMinimize(QWindow *window)
{
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_MINIMIZEBOX | WS_SYSMENU);
#else
    Q_UNUSED(window)
#endif
}

} // namespace

int main(int argc, char *argv[])
{
    // 必须在 QGuiApplication 构造之前，否则无效。
    // 车机/多显示器环境下缩放差异很大，这是兼容性基线而非可选优化。
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    QGuiApplication app(argc, argv);

    ScriptListModel scriptModel;
    const QString scriptsDir = resolveScriptsDir();
    scriptModel.load(scriptsDir);
    qInfo() << "[启动] scripts 目录:" << scriptsDir
            << "找到" << scriptModel.rowCount() << "个脚本";

    // 拖放执行。runner 挂在模型旁边而不是塞进模型：模型是只读投影，
    // 进程托管是另一份职责；两者共享同一个 registry（只读）。
    // 生命周期在 engine 之前声明，QML 上下文属性指向栈对象是安全的 ——
    // QGuiApplication::exec() 期间两者都活着。
    ScriptRunner scriptRunner(&scriptModel.registry());

    QQmlApplicationEngine engine;

    // qrc 里的 App 模块（src/qml/App/qmldir）要靠这条才能被 import 解析到。
    // 少了它就是 "module App is not installed"。
    engine.addImportPath(QStringLiteral("qrc:/qml"));

    engine.rootContext()->setContextProperty(QStringLiteral("scriptModel"), &scriptModel);
    engine.rootContext()->setContextProperty(QStringLiteral("scriptRunner"), &scriptRunner);

    // ---- 拖放诊断模式（--dropdebug）----
    // 平台层事件过滤器 + QML 全屏 DropArea 双通道记录拖放的每一层，
    // 写 exe 旁的 dropdebug.log。普通启动时 logger 是哑实例，
    // QML 侧的 debugLogger 调用是空操作，无副作用。
    const bool dropDebug = QGuiApplication::arguments()
                               .contains(QStringLiteral("--dropdebug"));
    DropDebugLogger dropDebugLogger(dropDebug);
    engine.rootContext()->setContextProperty(QStringLiteral("debugLogger"), &dropDebugLogger);
    engine.rootContext()->setContextProperty(QStringLiteral("dropDebugMode"), dropDebug);

    // ---- 命令行直跑（调试入口）----
    // launcher.exe <脚本> <文件...>：启动即选中脚本并按当前参数执行，
    // 跳过 UI 选择/拖放。脚本参数支持 绝对路径 / 文件名 / @name 三种写法
    // （匹配规则见 ScriptListModel::resolveScript）。没有参数就是普通启动。
    //
    // 它同时是自动化验证的抓手：真实 OLE 拖拽没法在测试里模拟，
    // 执行管线的效果检查都走这条路。
    const QStringList positional = [] {
        QStringList raw = QGuiApplication::arguments().mid(1);
        // 以 -- 开头的是开关（如 --dropdebug），不进"脚本 文件..."序列。
        raw.removeAll(QStringLiteral("--dropdebug"));
        return raw;
    }();
    QVariantMap startupRequest;
    if (!positional.isEmpty()) {
        QVariantList cliFiles;
        for (int i = 1; i < positional.size(); ++i)
            cliFiles.append(positional.at(i));
        startupRequest.insert(QStringLiteral("script"), positional.first());
        startupRequest.insert(QStringLiteral("files"), cliFiles);
        qInfo() << "[启动] 命令行直跑:" << positional.first()
                << "文件数" << cliFiles.size();
    }
    engine.rootContext()->setContextProperty(QStringLiteral("startupRequest"), startupRequest);

    // QML 的错误默认只打到 stderr。Debug 构建是控制台子系统（见 CMakeLists
    // 的 WIN32_EXECUTABLE 设置）所以能看见，但这里显式接一次 warnings 信号，
    // 保证即使将来改成 GUI 子系统也不会把错误吞掉。
    QObject::connect(&engine, &QQmlApplicationEngine::warnings,
                     [](const QList<QQmlError> &warnings) {
                         for (const QQmlError &e : warnings)
                             qWarning().noquote() << "[QML]" << e.toString();
                     });

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [](QObject *obj, const QUrl &url) {
                         if (!obj) {
                             qCritical().noquote() << "[致命] QML 加载失败:" << url.toString();
                             QCoreApplication::exit(-1);
                         }
                     });

    engine.load(QUrl(QStringLiteral("qrc:/qml/main.qml")));

    if (engine.rootObjects().isEmpty()) {
        qCritical() << "[致命] 没有根对象，退出。上面的 [QML] 行是原因。";
        return -1;
    }

    // 根对象就是 main.qml 的 Window。此时窗口已按 visible:true 加载，
    // winId() 能拿到原生句柄。
    for (QObject *obj : engine.rootObjects()) {
        if (QQuickWindow *w = qobject_cast<QQuickWindow *>(obj)) {
            applyRoundedCorners(w);
            enableTaskbarMinimize(w);
            // 运行时窗口图标：任务栏、Alt+Tab、窗口切换器显示的是这一份。
            // exe 文件本身的图标由 app.rc 在链接期写进 PE 资源，那是另一条
            // 独立通道 —— 资源管理器里看文件用前者，程序跑起来看任务栏用
            // 后者，缺一个都算"没图标"。窗口级外观统一在 C++ 侧收口（圆角、
            // 最小化样式也在这），不往 QML 里再散一份资源路径。
            w->setIcon(QIcon(QStringLiteral(":/assets/app.ico")));
            if (dropDebug)
                dropDebugLogger.attachWindow(w);
        }
    }

    return app.exec();
}
