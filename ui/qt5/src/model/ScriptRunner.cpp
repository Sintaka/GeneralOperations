#include "model/ScriptRunner.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcessEnvironment>

QStringList ScriptRunner::pythonCandidates() const
{
    // 发行包自带的运行时优先（见 docs/RELEASE.md 的 bundle 布局）：
    //   Windows ：<exe目录>/python/          —— 官方 embeddable 包，构建期展开
    //   Linux   ：<exe目录>/runtime/venv/    —— run.sh 首次运行时建
    // 都不在才退回系统解释器；Windows 上官方安装器默认只注册 py 启动器
    // 而不放 python.exe 到 PATH，所以 python 和 py 都要试。
    const QString appDir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
    return {
        appDir + QStringLiteral("/python/python.exe"),
        QStringLiteral("python"),
        QStringLiteral("py"),
    };
#else
    return {
        appDir + QStringLiteral("/runtime/venv/bin/python"),
        QStringLiteral("python3"),
    };
#endif
}

ScriptRunner::ScriptRunner(const ScriptRegistry *registry, QObject *parent)
    : QObject(parent)
    , m_registry(registry)
{
    // running 属性直接读 m_proc.state()，stateChanged 就是它的 NOTIFY。
    connect(&m_proc, &QProcess::stateChanged, this, [this] {
        emit runningChanged();
        if (m_proc.state() == QProcess::NotRunning)
            flushTailBuffer();
    });

    // stdout / stderr 合流显示：脚本按 SPEC 只往 stdout 写进度，stderr
    // 是 traceback —— 对用户来说都是"这次运行说了什么"，分开反而漏信息。
    connect(&m_proc, &QProcess::readyReadStandardOutput, this, [this] {
        appendChunk(m_proc.readAllStandardOutput());
    });
    connect(&m_proc, &QProcess::readyReadStandardError, this, [this] {
        appendChunk(m_proc.readAllStandardError());
    });

    connect(&m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        m_lastExitCode = exitCode;
        if (exitStatus == QProcess::CrashExit)
            pushLine(QStringLiteral("（进程异常终止）"));
        m_hasRun = true;
        emit hasRunChanged();
        emit finished(exitCode);
    });

    connect(&m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        // 其它错误（Crashed 等）走 finished 通道，这里只管"没跑起来"。
        if (error != QProcess::FailedToStart)
            return;
        ++m_candidateIndex;
        if (m_candidateIndex < m_pythonCandidates.size()) {
            startProcess(m_pythonCandidates.at(m_candidateIndex), m_pendingArgs);
            return;
        }
        // 候选列表为空 = @host blender 直接启动 @blender 指定的主程序，
        // 没有"换个解释器再试"的余地。报错要把路径亮出来让用户去核对，
        // 而不是套用下面 Python 的文案 —— 那会引导用户去装 Python，南辕北辙。
        if (m_pythonCandidates.isEmpty()) {
            m_spawnError = QStringLiteral("Blender 主程序启动失败: %1。"
                                          "请检查脚本头 @blender 指向的路径是否存在。")
                               .arg(m_proc.program());
            emit spawnErrorChanged();
            emit runError(m_spawnError);
            return;
        }
        m_spawnError = QStringLiteral("没有找到可用的 Python（试过：%1）。"
                                      "请安装 Python，或在发行包中内嵌 python/ 运行时。")
                           .arg(m_pythonCandidates.join(QStringLiteral(", ")));
        emit spawnErrorChanged();
        emit runError(m_spawnError);
    });
}

void ScriptRunner::run(const QString &scriptPath, const QStringList &files,
                       const QVariantMap &params)
{
    if (running()) {
        emit runError(QStringLiteral("已有脚本在运行中"));
        return;
    }

    const ScriptManifest *manifest = nullptr;
    for (const ScriptManifest &s : m_registry->scripts()) {
        if (s.filePath == scriptPath) {
            manifest = &s;
            break;
        }
    }
    if (!manifest) {
        emit runError(QStringLiteral("脚本不存在: ") + scriptPath);
        return;
    }
    if (!manifest->isValid()) {
        // 正常的 UI 路径到不了这里（无效脚本在 Outliner 就不可选中），
        // 命令行直跑可以指到任何文件，这里兜住。
        m_spawnError = QStringLiteral("脚本声明有误: ")
                       + manifest->errors.join(QStringLiteral("; "));
        emit spawnErrorChanged();
        emit runError(m_spawnError);
        return;
    }

    if (manifest->host == ScriptManifest::Blender && manifest->blenderPath.isEmpty()) {
        // SPEC：Blender 主程序路径只来自脚本头的 @blender 键，启动器不猜
        // 安装位置。与其拿一个编造的路径跑出难查的错，不如明说缺什么、去哪补。
        m_spawnError = QStringLiteral("请在脚本头用 @blender 配置 Blender 主程序路径。");
        emit spawnErrorChanged();
        emit runError(m_spawnError);
        return;
    }

    // SPEC：参数名下划线转 --连字符，文件列表放 "--" 之后，避免文件名
    // 以连字符开头时被当成选项。只传用户改过的参数，没提到的走 argparse
    // 默认值 —— docstring 默认值与 argparse 默认值一致是 SPEC 硬要求，
    // 两边都写反而多一处不同步的机会。
    //
    // 两种 host 的参数形态差异只在开头，flags 与文件段完全一致：
    //   python : <脚本>                <flags...> -- <files...>
    //   blender: --background --python <脚本> <flags...> -- <files...>
    // Blender 的 --python 选项吃掉脚本路径，脚本自己的 argv 里 "--" 之后
    // 的才是参数与文件（见 geo.pmx2fbx.py 的解析），所以 blender 形态的
    // 脚本路径要插在 --python 之后，而不是像 python 形态那样放参数表头。
    if (manifest->host == ScriptManifest::Blender) {
        m_pendingArgs = QStringList{
            QStringLiteral("--background"),
            QStringLiteral("--python"),
            scriptPath,
        };
    } else {
        m_pendingArgs = QStringList{ scriptPath };
    }
    for (const ScriptParam &p : manifest->params) {
        if (params.contains(p.name))
            m_pendingArgs += p.toCliArgs(params.value(p.name));
    }
    m_pendingArgs += QStringLiteral("--");
    m_pendingArgs += files;

    m_hasRun = false;
    m_lastExitCode = 0;
    m_spawnError.clear();
    m_lines.clear();
    m_tail.clear();
    m_lineBuf.clear();
    emit hasRunChanged();
    emit tailChanged();
    emit spawnErrorChanged();

    // 工作目录设为脚本所在目录：脚本如果写相对路径输出（SPEC 规定要写明
    // 输出规则），落点和双击 bat 跑它的直觉一致。
    m_proc.setWorkingDirectory(QFileInfo(scriptPath).absolutePath());

    if (manifest->host == ScriptManifest::Blender) {
        // Blender 自带完整的 Python 解释器，bpy/mmd_tools 等依赖装在它自己
        // 的 site-packages 里 —— pythonCandidates 与 startProcess 里的
        // PYTHONUTF8/PYTHONPATH 等 Python 环境逻辑对它都不适用，强设反而
        // 可能污染 Blender 内部解释器的行为。直接以 @blender 声明的主程序
        // 启动；候选列表留空，FailedToStart 时 errorOccurred 会改报
        // "启动失败 + 路径"，而不是逐个换解释器重试。
        m_pythonCandidates.clear();
        m_candidateIndex = 0;
        startProcess(manifest->blenderPath, m_pendingArgs);
        return;
    }

    m_pythonCandidates = pythonCandidates();
    m_candidateIndex = 0;
    startProcess(m_pythonCandidates.first(), m_pendingArgs);
}

void ScriptRunner::cancel()
{
    if (running())
        m_proc.kill();
}

void ScriptRunner::startProcess(const QString &program, const QStringList &args)
{
    // Python 3.7+ 在管道下默认用本地编码（中文 Windows 是 GBK）写 stdout，
    // 强制成 UTF-8，C++ 这边统一按 UTF-8 解码。PYTHONIOENCODING 兜底
    // 老版本，两个同时设不冲突。
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    // 混用系统 Python 时别让它读到用户级的 PYTHONPATH，内嵌运行时同理 ——
    // 脚本依赖一律以本包 site-packages 为准。
    env.remove(QStringLiteral("PYTHONPATH"));
    env.insert(QStringLiteral("PYTHONNOUSERSITE"), QStringLiteral("1"));
    m_proc.setProcessEnvironment(env);

    m_proc.start(program, args);
}

void ScriptRunner::appendChunk(const QByteArray &chunk)
{
    m_lineBuf += chunk;

    int start = 0;
    for (int i = 0; i < m_lineBuf.size(); ++i) {
        if (m_lineBuf[i] != '\n')
            continue;
        int end = i;
        if (end > start && m_lineBuf[end - 1] == '\r')
            --end;
        pushLine(QString::fromUtf8(m_lineBuf.constData() + start, end - start));
        start = i + 1;
    }
    if (start > 0)
        m_lineBuf.remove(0, start);
}

void ScriptRunner::pushLine(const QString &line)
{
    m_lines.append(line);
    while (m_lines.size() > kTailLines)
        m_lines.removeFirst();
    m_tail = m_lines.join(QStringLiteral("\n"));
    emit tailChanged();
}

void ScriptRunner::flushTailBuffer()
{
    // 进程结束时不一定以换行收尾，把残行也吐出去。
    if (m_lineBuf.isEmpty())
        return;
    QByteArray buf = m_lineBuf;
    if (buf.endsWith('\r'))
        buf.chop(1);
    m_lineBuf.clear();
    pushLine(QString::fromUtf8(buf));
}
