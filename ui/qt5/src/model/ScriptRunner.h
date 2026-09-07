#pragma once

#include "core/ScriptRegistry.h"
#include "model/ProcessTreeController.h"

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QStringList>

namespace script_runner_detail {

int normalizedExitCode(int exitCode, QProcess::ExitStatus exitStatus);

/// 有界的增量 UTF-8 行缓冲。独立成纯 helper，便于确定性测试块边界和截断语义。
class OutputBuffer
{
public:
    explicit OutputBuffer(int maxChars, int maxLines);

    void reset();
    bool ingest(const QByteArray &chunk);
    bool appendLine(const QString &line);
    bool flush();
    QString tail() const;
    bool truncated() const { return m_truncated; }

private:
    void appendText(const QString &text);
    void completeCurrentLine();
    void enforceLimits();
    int retainedChars() const;

    int m_maxChars;
    int m_maxLines;
    QByteArray m_utf8Carry;
    QString m_currentLine;
    QStringList m_lines;
    bool m_pendingCr = false;
    bool m_truncated = false;
};

} // namespace script_runner_detail

/// 用 QProcess 托管脚本根进程；Windows 上额外以 Job Object 托管完整进程树。
class ScriptRunner : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(bool hasRun READ hasRun NOTIFY hasRunChanged)
    Q_PROPERTY(bool cancelled READ cancelled NOTIFY cancelledChanged)
    Q_PROPERTY(int lastExitCode READ lastExitCode NOTIFY lastExitCodeChanged)
    Q_PROPERTY(QString tail READ tail NOTIFY tailChanged)
    Q_PROPERTY(QString spawnError READ spawnError NOTIFY spawnErrorChanged)

public:
    explicit ScriptRunner(const ScriptRegistry *registry, QObject *parent = nullptr);
    ~ScriptRunner() override;

    bool running() const { return m_runActive; }
    bool hasRun() const { return m_hasRun; }
    bool cancelled() const { return m_cancelled; }
    int lastExitCode() const { return m_lastExitCode; }
    QString tail() const { return m_output.tail(); }
    QString spawnError() const { return m_spawnError; }

    Q_INVOKABLE void run(const QString &scriptPath, const QStringList &files,
                         const QVariantMap &params = QVariantMap());
    Q_INVOKABLE void cancel();

signals:
    void runningChanged();
    void hasRunChanged();
    void cancelledChanged();
    void lastExitCodeChanged();
    void tailChanged();
    void spawnErrorChanged();
    /// 已启动且受管的进程结束，或启动阶段取消完成；参数与 lastExitCode 一致。
    void finished(int exitCode);
    /// 脚本未执行（预检、进程创建或 Windows containment 失败）。
    void runError(const QString &reason);

private:
    void startProcess(const QString &program, const QStringList &args);
    void readProcessOutput();
    void finishStartedProcess(int exitCode, QProcess::ExitStatus exitStatus);
    void finishCancelledBeforeStart();
    void finishStartupFailure(const QString &reason);
    void failToStart();
    void failPreflight(const QString &reason);
    void resetForRun();
    void reportRunError(const QString &reason);
    QStringList pythonCandidates() const;

    const ScriptRegistry *m_registry;
    QProcess m_proc;
    ProcessTreeController m_processTree;

    QStringList m_pythonCandidates;
    int m_candidateIndex = 0;
    QStringList m_pendingArgs;
    QStringList m_candidateDiagnostics;
    QString m_startFailureReason;

    bool m_runActive = false;
    bool m_terminalizing = false;
    bool m_reportingRunError = false;
    QString m_reportingRunErrorReason;
    QStringList m_pendingRunErrors;
    bool m_hasRun = false;
    bool m_cancelRequested = false;
    bool m_cancelled = false;
    bool m_cancelMarkerWritten = false;
    bool m_cancelNotifying = false;
    bool m_deferredFinish = false;
    bool m_deferredCancelledBeforeStart = false;
    bool m_deferredStartupFailure = false;
    QString m_deferredStartupFailureReason;
    int m_deferredExitCode = 0;
    QProcess::ExitStatus m_deferredExitStatus = QProcess::NormalExit;
    quint64 m_runGeneration = 0;
    int m_lastExitCode = 0;
    QString m_spawnError;

    static constexpr int kLogCapacityChars = 64 * 1024;
    // 行数只防御海量空行；普通日志主要由字符容量约束，不再截成旧版 12 行。
    static constexpr int kLogCapacityLines = 2048;
    script_runner_detail::OutputBuffer m_output =
        script_runner_detail::OutputBuffer(kLogCapacityChars, kLogCapacityLines);
};
