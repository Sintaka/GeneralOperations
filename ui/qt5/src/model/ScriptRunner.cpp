#include "model/ScriptRunner.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QTimer>

#include <algorithm>

namespace {

const QString &truncationMarker()
{
    static const QString marker = QStringLiteral("（输出已截断，仅显示最后部分）");
    return marker;
}

int incompleteUtf8SuffixLength(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return 0;

    int lead = bytes.size() - 1;
    while (lead >= 0 && (static_cast<unsigned char>(bytes.at(lead)) & 0xc0u) == 0x80u)
        --lead;
    if (lead < 0)
        return 0; // 只有 continuation bytes：非法输入，交给 fromUtf8 产出替换字符。

    const unsigned char first = static_cast<unsigned char>(bytes.at(lead));
    int expected = 0;
    if (first >= 0xc2u && first <= 0xdfu)
        expected = 2;
    else if (first >= 0xe0u && first <= 0xefu)
        expected = 3;
    else if (first >= 0xf0u && first <= 0xf4u)
        expected = 4;

    const int available = bytes.size() - lead;
    if (expected == 0 || available >= expected)
        return 0;

    // 只保留仍可能成为合法 UTF-8 的前缀；已经越界的第二字节应立即交给
    // fromUtf8 产出替换字符，不能假装“还缺下一个 chunk”。
    if (available >= 2) {
        const unsigned char second = static_cast<unsigned char>(bytes.at(lead + 1));
        const bool continuation = second >= 0x80u && second <= 0xbfu;
        const bool scalarRange = (first != 0xe0u || second >= 0xa0u)
                                 && (first != 0xedu || second <= 0x9fu)
                                 && (first != 0xf0u || second >= 0x90u)
                                 && (first != 0xf4u || second <= 0x8fu);
        if (!continuation || !scalarRange)
            return 0;
    }
    return available;
}

} // namespace

namespace script_runner_detail {

int normalizedExitCode(int exitCode, QProcess::ExitStatus exitStatus)
{
    return exitStatus == QProcess::CrashExit && exitCode == 0 ? 1 : exitCode;
}

OutputBuffer::OutputBuffer(int maxChars, int maxLines)
    : m_maxChars((std::max)(1, maxChars))
    , m_maxLines((std::max)(1, maxLines))
{
}

void OutputBuffer::reset()
{
    m_utf8Carry.clear();
    m_currentLine.clear();
    m_lines.clear();
    m_pendingCr = false;
    m_truncated = false;
}

bool OutputBuffer::ingest(const QByteArray &chunk)
{
    if (chunk.isEmpty())
        return false;

    const QString before = tail();
    QByteArray bytes = m_utf8Carry;
    bytes += chunk;
    const int carryLength = incompleteUtf8SuffixLength(bytes);
    m_utf8Carry = carryLength > 0 ? bytes.right(carryLength) : QByteArray();
    if (carryLength > 0)
        bytes.chop(carryLength);
    if (!bytes.isEmpty())
        appendText(QString::fromUtf8(bytes));
    enforceLimits();
    return before != tail();
}

bool OutputBuffer::appendLine(const QString &line)
{
    const QString before = tail();
    if (!m_utf8Carry.isEmpty()) {
        // 外部插入的状态行不是进程字节流的一部分；取消时未完成的 UTF-8
        // scalar 不可能再安全补齐，直接丢弃比误解码成替换字符更准确。
        m_utf8Carry.clear();
    }
    if (m_pendingCr) {
        // 行尾孤立 CR 视作终止符；这也保证取消标记前不会残留控制符。
        m_pendingCr = false;
        if (!m_currentLine.isEmpty())
            completeCurrentLine();
    } else if (!m_currentLine.isEmpty()) {
        completeCurrentLine();
    }
    m_lines.append(line);
    enforceLimits();
    return before != tail();
}

bool OutputBuffer::flush()
{
    const QString before = tail();
    if (!m_utf8Carry.isEmpty()) {
        appendText(QString::fromUtf8(m_utf8Carry));
        m_utf8Carry.clear();
    }
    m_pendingCr = false;
    if (!m_currentLine.isEmpty())
        completeCurrentLine();
    enforceLimits();
    return before != tail();
}

QString OutputBuffer::tail() const
{
    QStringList visible = m_lines;
    if (!m_currentLine.isEmpty())
        visible.append(m_currentLine);
    if (m_truncated)
        visible.prepend(truncationMarker());
    return visible.join(QLatin1Char('\n'));
}

void OutputBuffer::appendText(const QString &text)
{
    for (const QChar &ch : text) {
        if (m_pendingCr) {
            m_pendingCr = false;
            completeCurrentLine();
            if (ch == QLatin1Char('\n'))
                continue;
        }

        if (ch == QLatin1Char('\r')) {
            m_pendingCr = true;
        } else if (ch == QLatin1Char('\n')) {
            completeCurrentLine();
        } else {
            m_currentLine.append(ch);
        }
    }
}

void OutputBuffer::completeCurrentLine()
{
    m_lines.append(m_currentLine);
    m_currentLine.clear();
    enforceLimits();
}

int OutputBuffer::retainedChars() const
{
    int count = m_currentLine.size();
    for (const QString &line : m_lines)
        count += line.size();
    return count;
}

void OutputBuffer::enforceLimits()
{
    bool dropped = false;
    const int lineLimit = m_truncated ? (std::max)(0, m_maxLines - 1) : m_maxLines;
    auto contentLineCount = [this] {
        return m_lines.size() + (m_currentLine.isEmpty() ? 0 : 1);
    };

    while (contentLineCount() > lineLimit) {
        if (!m_lines.isEmpty())
            m_lines.removeFirst();
        else
            m_currentLine.clear();
        dropped = true;
    }

    while (retainedChars() > m_maxChars) {
        if (m_lines.size() > 1 || (!m_lines.isEmpty() && !m_currentLine.isEmpty())) {
            m_lines.removeFirst();
        } else {
            QString *text = m_lines.isEmpty() ? &m_currentLine : &m_lines[0];
            int start = text->size() - m_maxChars;
            // QString::right() 按 UTF-16 code unit 裁剪；起点若落在代理对中间，
            // 丢掉整个 scalar，而不是留下孤立 low surrogate。
            if (start > 0 && start < text->size()
                && text->at(start).isLowSurrogate()
                && text->at(start - 1).isHighSurrogate()) {
                ++start;
            }
            *text = text->mid(start);
        }
        dropped = true;
    }

    if (dropped && !m_truncated) {
        m_truncated = true;
        // 提示本身占一行；首次截断后再收紧一次，保证总可见行数仍不越界。
        enforceLimits();
    }
}

} // namespace script_runner_detail

QStringList ScriptRunner::pythonCandidates() const
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
#ifdef Q_OS_WIN
    candidates << appDir + QStringLiteral("/python/python.exe")
               << QStringLiteral("python")
               << QStringLiteral("py");
#else
    candidates << appDir + QStringLiteral("/runtime/venv/bin/python")
               << QStringLiteral("python3");
#endif
    return candidates;
}

ScriptRunner::ScriptRunner(const ScriptRegistry *registry, QObject *parent)
    : QObject(parent)
    , m_registry(registry)
{
    m_proc.setProcessChannelMode(QProcess::MergedChannels);

    connect(&m_proc, &QProcess::started, this, [this] {
        const ProcessTreeController::StartResult result =
            m_processTree.processStarted(m_proc, m_cancelRequested);
        if (result == ProcessTreeController::StartResult::ContainmentFailed)
            m_startFailureReason = m_processTree.errorString();
    });
    connect(&m_proc, &QProcess::readyReadStandardOutput,
            this, &ScriptRunner::readProcessOutput);

    connect(&m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &ScriptRunner::finishStartedProcess);

    connect(&m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || !m_runActive)
            return;

        m_candidateDiagnostics.append(
            QStringLiteral("%1：%2").arg(m_proc.program()).arg(m_proc.errorString()));
        m_processTree.processFailedToStart(m_proc);
        const quint64 generation = m_runGeneration;
        // Qt 5.15 在 FailedToStart 信号返回后才把内部状态切回 NotRunning。
        // 候选重试和最终失败都排到下一轮，避免旧启动栈覆盖 QProcess 新状态，
        // generation 则挡掉析构或后续任务留下的陈旧回调。
        QTimer::singleShot(0, this, [this, generation] {
            if (!m_runActive || generation != m_runGeneration)
                return;
            if (m_cancelRequested) {
                finishCancelledBeforeStart();
                return;
            }

            ++m_candidateIndex;
            if (m_candidateIndex < m_pythonCandidates.size()) {
                startProcess(m_pythonCandidates.at(m_candidateIndex), m_pendingArgs);
                return;
            }
            failToStart();
        });
    });
}

ScriptRunner::~ScriptRunner()
{
    disconnect(&m_proc, nullptr, this, nullptr);
    m_processTree.shutdown(m_proc);
}

void ScriptRunner::run(const QString &scriptPath, const QStringList &files,
                       const QVariantMap &params)
{
    if (m_runActive || m_terminalizing) {
        reportRunError(QStringLiteral("已有脚本在运行中"));
        return;
    }

    const ScriptManifest *manifest = nullptr;
    for (const ScriptManifest &script : m_registry->scripts()) {
        if (script.filePath == scriptPath) {
            manifest = &script;
            break;
        }
    }
    if (!manifest) {
        failPreflight(QStringLiteral("脚本不存在: ") + scriptPath);
        return;
    }
    if (!manifest->isValid()) {
        failPreflight(QStringLiteral("脚本声明有误: ")
                      + manifest->errors.join(QStringLiteral("; ")));
        return;
    }
    if (manifest->host == ScriptManifest::Blender && manifest->blenderPath.isEmpty()) {
        failPreflight(QStringLiteral("请在脚本头用 @blender 配置 Blender 主程序路径。"));
        return;
    }

    if (manifest->host == ScriptManifest::Blender) {
        m_pendingArgs.clear();
        m_pendingArgs << QStringLiteral("--background")
                      << QStringLiteral("--python") << scriptPath;
    } else {
        m_pendingArgs.clear();
        m_pendingArgs << scriptPath;
    }
    for (const ScriptParam &param : manifest->params) {
        if (params.contains(param.name))
            m_pendingArgs += param.toCliArgs(params.value(param.name));
    }
    m_pendingArgs += QStringLiteral("--");
    m_pendingArgs += files;

    resetForRun();
    m_proc.setWorkingDirectory(QFileInfo(scriptPath).absolutePath());
    if (m_cancelRequested) {
        // reset 信号的直接槽可能在 QProcess::start 前取消；此时根本不创建进程。
        finishCancelledBeforeStart();
        return;
    }

    if (manifest->host == ScriptManifest::Blender) {
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
    if (!m_runActive || m_cancelRequested)
        return;

    // 取消事务先完整提交，再向外通知；嵌套事件循环返回后不再访问 m_proc，避免
    // 陈旧 cancel 栈误杀下一 generation。真正的 terminate 必须在首个信号之前。
    const QByteArray pendingOutput = m_proc.readAllStandardOutput();
    m_cancelRequested = true;
    const bool tailChangedByOutput = m_output.ingest(pendingOutput);
    const bool shouldNotifyCancelled = !m_cancelled;
    m_cancelled = true;
    bool tailChangedByMarker = false;
    if (!m_cancelMarkerWritten) {
        m_cancelMarkerWritten = true;
        tailChangedByMarker = m_output.appendLine(QStringLiteral("^C"));
    }
    m_cancelNotifying = true;
    m_processTree.terminate(m_proc);

    if (tailChangedByOutput || tailChangedByMarker)
        emit tailChanged();
    if (shouldNotifyCancelled)
        emit cancelledChanged();
    m_cancelNotifying = false;

    if (m_deferredFinish) {
        const int exitCode = m_deferredExitCode;
        const QProcess::ExitStatus exitStatus = m_deferredExitStatus;
        m_deferredFinish = false;
        finishStartedProcess(exitCode, exitStatus);
    } else if (m_deferredCancelledBeforeStart) {
        m_deferredCancelledBeforeStart = false;
        finishCancelledBeforeStart();
    } else if (m_deferredStartupFailure) {
        const QString reason = m_deferredStartupFailureReason;
        m_deferredStartupFailure = false;
        m_deferredStartupFailureReason.clear();
        finishStartupFailure(reason);
    }
}

void ScriptRunner::startProcess(const QString &program, const QStringList &args)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.remove(QStringLiteral("PYTHONPATH"));
    env.insert(QStringLiteral("PYTHONNOUSERSITE"), QStringLiteral("1"));
    m_proc.setProcessEnvironment(env);
    m_startFailureReason.clear();

    if (!m_processTree.prepare(m_proc)) {
        const quint64 generation = m_runGeneration;
        const QString reason = m_processTree.errorString();
        QTimer::singleShot(0, this, [this, generation, reason] {
            if (!m_runActive || generation != m_runGeneration)
                return;
            if (m_cancelRequested)
                finishCancelledBeforeStart();
            else
                finishStartupFailure(reason);
        });
        return;
    }
    m_proc.start(program, args);
}

void ScriptRunner::readProcessOutput()
{
    const QByteArray chunk = m_proc.readAllStandardOutput();
    if (m_cancelRequested)
        return;
    if (m_output.ingest(chunk))
        emit tailChanged();
}

void ScriptRunner::finishStartedProcess(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (!m_runActive)
        return;
    if (m_cancelNotifying) {
        m_deferredFinish = true;
        m_deferredExitCode = exitCode;
        m_deferredExitStatus = exitStatus;
        return;
    }

    // 终态事务先锁住状态，并在任何通知前提交所有可观察属性。
    m_runActive = false;
    m_terminalizing = true;

    bool tailDidChange = false;
    if (!m_cancelRequested)
        tailDidChange = m_output.ingest(m_proc.readAllStandardOutput());
    else
        m_proc.readAllStandardOutput();
    m_processTree.processFinished(m_proc);
    if (!m_cancelRequested)
        tailDidChange = m_output.ingest(m_proc.readAllStandardOutput()) || tailDidChange;
    else
        m_proc.readAllStandardOutput();
    tailDidChange = m_output.flush() || tailDidChange;

    if (!m_startFailureReason.isEmpty()) {
        const QString reason = m_startFailureReason;
        m_startFailureReason.clear();
        m_lastExitCode = 1;
        m_spawnError = reason;
        if (tailDidChange)
            emit tailChanged();
        emit lastExitCodeChanged();
        emit spawnErrorChanged();
        reportRunError(reason);
        emit runningChanged();
        m_terminalizing = false;
        return;
    }

    const bool crashed = exitStatus == QProcess::CrashExit;
    // Windows 的 CrashExit 可能携带 0；QML 只看 lastExitCode，必须保证不显示成功。
    m_lastExitCode = script_runner_detail::normalizedExitCode(exitCode, exitStatus);
    if (!m_cancelRequested && crashed)
        tailDidChange = m_output.appendLine(QStringLiteral("（进程异常终止）")) || tailDidChange;
    m_hasRun = true;

    if (tailDidChange)
        emit tailChanged();
    emit lastExitCodeChanged();
    emit hasRunChanged();
    emit finished(m_lastExitCode);
    emit runningChanged();
    m_terminalizing = false;
}

void ScriptRunner::finishCancelledBeforeStart()
{
    if (!m_runActive)
        return;
    if (m_cancelNotifying) {
        m_deferredCancelledBeforeStart = true;
        return;
    }

    m_runActive = false;
    m_terminalizing = true;
    const bool tailDidChange = m_output.flush();
    m_lastExitCode = 1;
    m_hasRun = true;
    if (tailDidChange)
        emit tailChanged();
    emit lastExitCodeChanged();
    emit hasRunChanged();
    emit finished(m_lastExitCode);
    emit runningChanged();
    m_terminalizing = false;
}

void ScriptRunner::finishStartupFailure(const QString &reason)
{
    if (!m_runActive)
        return;
    if (m_cancelNotifying) {
        m_deferredStartupFailure = true;
        m_deferredStartupFailureReason = reason;
        return;
    }

    m_runActive = false;
    m_terminalizing = true;
    m_startFailureReason.clear();
    m_lastExitCode = 1;
    m_spawnError = reason;
    emit lastExitCodeChanged();
    emit spawnErrorChanged();
    reportRunError(reason);
    emit runningChanged();
    m_terminalizing = false;
}

void ScriptRunner::failToStart()
{
    if (!m_runActive)
        return;

    QString error;
    if (m_pythonCandidates.isEmpty()) {
        error = QStringLiteral("Blender 主程序启动失败: %1。"
                               "请检查脚本头 @blender 指向的路径是否存在。")
                    .arg(m_proc.program());
    } else {
        error = QStringLiteral("没有找到可用的 Python（试过：%1）。"
                               "请安装 Python，或在发行包中内嵌 python/ 运行时。")
                    .arg(m_pythonCandidates.join(QStringLiteral(", ")));
    }
    if (!m_candidateDiagnostics.isEmpty())
        error += QStringLiteral("\n启动诊断：")
               + m_candidateDiagnostics.join(QStringLiteral("；"));
    finishStartupFailure(error);
}

void ScriptRunner::failPreflight(const QString &reason)
{
    // 即使没有进入 active run，也先提交用户可见状态，再发 runError。
    m_terminalizing = true;
    m_hasRun = false;
    m_cancelRequested = false;
    m_cancelled = false;
    m_lastExitCode = 1;
    m_spawnError = reason;
    m_output.reset();
    emit hasRunChanged();
    emit cancelledChanged();
    emit lastExitCodeChanged();
    emit tailChanged();
    emit spawnErrorChanged();
    reportRunError(reason);
    m_terminalizing = false;
}

void ScriptRunner::reportRunError(const QString &reason)
{
    if (m_reportingRunError) {
        // 只抑制完全相同的递归错误；不同的终态错误排队，不能被外层提示吞掉。
        if (reason != m_reportingRunErrorReason && !m_pendingRunErrors.contains(reason))
            m_pendingRunErrors.append(reason);
        return;
    }

    m_reportingRunError = true;
    m_reportingRunErrorReason = reason;
    emit runError(reason);
    while (!m_pendingRunErrors.isEmpty()) {
        m_reportingRunErrorReason = m_pendingRunErrors.takeFirst();
        emit runError(m_reportingRunErrorReason);
    }
    m_reportingRunErrorReason.clear();
    m_reportingRunError = false;
}

void ScriptRunner::resetForRun()
{
    ++m_runGeneration;
    m_terminalizing = false;
    // 在任何 reset 信号前完成全部成员更新并占住 active；直接槽立刻 cancel/run
    // 都不会被 reset 后半段覆盖，也不会跨任务复用上一代候选诊断。
    m_runActive = true;
    m_hasRun = false;
    const bool cancelledWasSet = m_cancelled;
    m_cancelled = false;
    m_lastExitCode = 0;
    m_cancelRequested = false;
    m_cancelMarkerWritten = false;
    m_cancelNotifying = false;
    m_deferredFinish = false;
    m_deferredCancelledBeforeStart = false;
    m_deferredStartupFailure = false;
    m_deferredStartupFailureReason.clear();
    m_spawnError.clear();
    m_candidateDiagnostics.clear();
    m_startFailureReason.clear();
    m_output.reset();

    emit hasRunChanged();
    // reset 即使值已是 0 也通知：新任务的结果代次已经改变。
    emit lastExitCodeChanged();
    if (cancelledWasSet)
        emit cancelledChanged();
    emit tailChanged();
    emit spawnErrorChanged();
    emit runningChanged();
}
