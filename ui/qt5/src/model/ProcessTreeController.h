#pragma once

#include <QProcess>
#include <QString>

/// 管理一次 QProcess 启动的取消边界。
///
/// Windows 上根进程以 suspended 创建，只有成功加入 KILL_ON_JOB_CLOSE Job 后才
/// resume，因此可托管并取消完整进程树。其它平台只保证取消 QProcess 根进程。
class ProcessTreeController
{
public:
    enum class StartResult {
        Managed,
        Cancelled,
        ContainmentFailed
    };

    ProcessTreeController() = default;
    ~ProcessTreeController();

    ProcessTreeController(const ProcessTreeController &) = delete;
    ProcessTreeController &operator=(const ProcessTreeController &) = delete;

    /// 必须紧挨 QProcess::start 前调用；false 表示隔离设施初始化失败，禁止启动。
    bool prepare(QProcess &process);
    /// 接在 QProcess::started 上；Windows 返回前一定会 resume 或终止 suspended 根进程。
    StartResult processStarted(QProcess &process, bool cancelRequested);
    void processFailedToStart(QProcess &process);
    void processFinished(QProcess &process);

    void terminate(QProcess &process);
    void shutdown(QProcess &process);
    QString errorString() const { return m_errorString; }

#ifdef SCRIPT_RUNNER_ENABLE_TEST_HOOKS
    enum class TestFailure {
        None,
        CreateJob,
        SetJobInformation,
        AssignProcess
    };
    static void failNextForTest(TestFailure failure);
#endif

private:
#ifdef Q_OS_WIN
    void releaseJob();

    void *m_job = nullptr;
    bool m_assigned = false;
#endif
    QString m_errorString;
};
