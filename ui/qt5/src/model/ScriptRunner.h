#pragma once

#include "core/ScriptRegistry.h"

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QStringList>

/// 用 QProcess 托管一个脚本进程。
///
/// 命令行组装遵循 docs/SCRIPT_SPEC.md 的映射规则：
///   python <脚本> [--flags] -- <file...>
/// 参数面板还没建，当前只跑默认值 —— docstring 默认值与 argparse 默认值
/// 一致是 SPEC 的硬要求，所以不传 --flag 就是声明里的默认行为。
///
/// 单进程：启动器一次只跑一个脚本。运行中再 run() 会拒绝 —— 拒绝而不是
/// 排队，因为拖拽语义下用户想要的永远是"跑刚拖进来的这批"，排队反而
/// 需要先做取消设计，这个切片不做。
class ScriptRunner : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    /// 曾经跑完过一次（含失败）。QML 拿它决定要不要显示输出区。
    Q_PROPERTY(bool hasRun READ hasRun NOTIFY hasRunChanged)
    Q_PROPERTY(int lastExitCode READ lastExitCode NOTIFY finished)
    /// 最近 N 行输出（stdout/stderr 合流，见 appendLines）。重跑即清空。
    Q_PROPERTY(QString tail READ tail NOTIFY tailChanged)
    /// 进程没跑起来的原因（找不到 Python 等）。空串 = 无错。
    Q_PROPERTY(QString spawnError READ spawnError NOTIFY spawnErrorChanged)

public:
    explicit ScriptRunner(const ScriptRegistry *registry, QObject *parent = nullptr);

    bool running() const { return m_proc.state() != QProcess::NotRunning; }
    bool hasRun() const { return m_hasRun; }
    int lastExitCode() const { return m_lastExitCode; }
    QString tail() const { return m_tail; }
    QString spawnError() const { return m_spawnError; }

    /// 按 SPEC 组装命令行并启动。params 是"要覆盖默认值的参数"（键 =
    /// 参数名，如 max_pixels）；没提到的参数不传 flag，argparse 走默认值。
    Q_INVOKABLE void run(const QString &scriptPath, const QStringList &files,
                         const QVariantMap &params = QVariantMap());

    /// 终止当前进程。脚本可能正写到一半，这本身是破坏性操作，
    /// 只允许挂在显式的界面上。
    Q_INVOKABLE void cancel();

signals:
    void runningChanged();
    void hasRunChanged();
    void tailChanged();
    void spawnErrorChanged();
    /// 进程正常退出（含非零退出码，失败也是 finished，不是 runError）。
    void finished(int exitCode);
    /// 根本没跑起来。
    void runError(const QString &reason);

private:
    void startProcess(const QString &program, const QStringList &args);
    void appendChunk(const QByteArray &chunk);
    void pushLine(const QString &line);
    void flushTailBuffer();
    QStringList pythonCandidates() const;

    const ScriptRegistry *m_registry;
    QProcess m_proc;

    /// Python 解析顺序：发行包内嵌的运行时优先，系统解释器兜底。
    /// 内嵌不可用时 FailedToStart 会沿列表逐个降级（见 errorOccurred）。
    QStringList m_pythonCandidates;
    int m_candidateIndex = 0;
    QStringList m_pendingArgs;

    bool m_hasRun = false;
    int m_lastExitCode = 0;
    QString m_tail;
    QString m_spawnError;

    /// 行缓冲：readyRead 的块边界不保证对齐换行，攒到整行再发。
    QByteArray m_lineBuf;
    QStringList m_lines;

    /// 输出区只留最近几行 —— 脚本输出可能上千行，QML 的 Text 挂不住。
    static constexpr int kTailLines = 12;
};
