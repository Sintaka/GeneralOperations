#include "model/ProcessTreeController.h"

#ifdef Q_OS_WIN
#  include <qt_windows.h>

#  ifdef SCRIPT_RUNNER_ENABLE_TEST_HOOKS
#    include <atomic>
#  endif
#endif

namespace {

#ifdef Q_OS_WIN
QString windowsError(const QString &operation, DWORD error)
{
    wchar_t *message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t *>(&message), 0, nullptr);
    QString detail = length > 0 && message
        ? QString::fromWCharArray(message, static_cast<int>(length)).trimmed()
        : QStringLiteral("未知 Windows 错误");
    if (message)
        LocalFree(message);
    return QStringLiteral("%1（Windows 错误 %2：%3）")
        .arg(operation).arg(error).arg(detail);
}

bool terminateSuspendedRoot(QProcess &process, HANDLE processHandle, HANDLE threadHandle)
{
    if (processHandle && TerminateProcess(processHandle, 1))
        return true;

    // CREATE_SUSPENDED 后尚无用户创建的其它线程。若进程级终止异常失败，终止
    // 唯一主线程可避免永久 suspended；QProcess::kill 再覆盖“正在退出”的竞态。
    bool terminated = threadHandle && TerminateThread(threadHandle, 1);
    if (process.state() != QProcess::NotRunning)
        process.kill();
    return terminated;
}

#  ifdef SCRIPT_RUNNER_ENABLE_TEST_HOOKS
std::atomic<int> g_testFailure{static_cast<int>(ProcessTreeController::TestFailure::None)};

bool takeTestFailure(ProcessTreeController::TestFailure failure)
{
    int expected = static_cast<int>(failure);
    return g_testFailure.compare_exchange_strong(
        expected, static_cast<int>(ProcessTreeController::TestFailure::None));
}
#  endif
#endif

} // namespace

ProcessTreeController::~ProcessTreeController()
{
#ifdef Q_OS_WIN
    releaseJob();
#endif
}

bool ProcessTreeController::prepare(QProcess &process)
{
    m_errorString.clear();
#ifdef Q_OS_WIN
    releaseJob();

    HANDLE job = nullptr;
#  ifdef SCRIPT_RUNNER_ENABLE_TEST_HOOKS
    const bool failCreate = takeTestFailure(TestFailure::CreateJob);
#  else
    const bool failCreate = false;
#  endif
    if (!failCreate)
        job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        const DWORD error = failCreate ? ERROR_NOT_ENOUGH_MEMORY : GetLastError();
        m_errorString = windowsError(QStringLiteral("无法创建进程隔离 Job Object"), error);
        process.setCreateProcessArgumentsModifier(QProcess::CreateProcessArgumentModifier());
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
#  ifdef SCRIPT_RUNNER_ENABLE_TEST_HOOKS
    const bool failSetInformation = takeTestFailure(TestFailure::SetJobInformation);
#  else
    const bool failSetInformation = false;
#  endif
    const BOOL configured = failSetInformation
        ? FALSE
        : SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                  &limits, sizeof(limits));
    if (!configured) {
        const DWORD error = failSetInformation ? ERROR_INVALID_PARAMETER : GetLastError();
        CloseHandle(job);
        m_errorString = windowsError(
            QStringLiteral("无法配置进程隔离 Job Object"), error);
        process.setCreateProcessArgumentsModifier(QProcess::CreateProcessArgumentModifier());
        return false;
    }

    m_job = job;
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_SUSPENDED;
    });
#else
    Q_UNUSED(process)
#endif
    return true;
}

ProcessTreeController::StartResult ProcessTreeController::processStarted(
    QProcess &process, bool cancelRequested)
{
#ifdef Q_OS_WIN
    // 仅在 started 后读取 Qt 5 的 PROCESS_INFORMATION；Starting 状态不保证它存在。
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    const Q_PID processInfo = process.pid();
    QT_WARNING_POP
    HANDLE processHandle = processInfo ? processInfo->hProcess : nullptr;
    HANDLE threadHandle = processInfo ? processInfo->hThread : nullptr;

#  ifdef SCRIPT_RUNNER_ENABLE_TEST_HOOKS
    const bool failAssign = takeTestFailure(TestFailure::AssignProcess);
#  else
    const bool failAssign = false;
#  endif
    const BOOL assigned = !failAssign && m_job && processHandle
        ? AssignProcessToJobObject(static_cast<HANDLE>(m_job), processHandle)
        : FALSE;
    if (!assigned) {
        const DWORD error = failAssign
            ? ERROR_ACCESS_DENIED
            : (!m_job || !processHandle ? ERROR_INVALID_HANDLE : GetLastError());
        m_errorString = windowsError(
            QStringLiteral("无法把脚本进程加入隔离 Job Object，已阻止未托管启动"), error);

        // 根线程仍为 CREATE_SUSPENDED；直接终止，绝不能 resume 后再补杀。
        terminateSuspendedRoot(process, processHandle, threadHandle);
        releaseJob();
        return StartResult::ContainmentFailed;
    }
    m_assigned = true;

    if (cancelRequested) {
        // 已在 Job 内且仍 suspended：直接终止整棵 Job，不给用户代码执行窗口。
        BOOL terminated = TerminateJobObject(static_cast<HANDLE>(m_job), 1);
        if (!terminated) {
            // 显式终止失败时关闭 KILL_ON_JOB_CLOSE 句柄，再终止 suspended 根线程。
            releaseJob();
            terminateSuspendedRoot(process, processHandle, threadHandle);
        }
        return StartResult::Cancelled;
    }

    const DWORD previousSuspendCount = threadHandle
        ? ResumeThread(threadHandle)
        : static_cast<DWORD>(-1);
    if (previousSuspendCount != 1) {
        const DWORD error = previousSuspendCount == static_cast<DWORD>(-1)
            ? GetLastError() : ERROR_INVALID_STATE;
        m_errorString = windowsError(
            QStringLiteral("无法恢复受管脚本进程，正在终止该 Job"), error);
        BOOL terminated = TerminateJobObject(static_cast<HANDLE>(m_job), 1);
        if (!terminated) {
            releaseJob();
            terminateSuspendedRoot(process, processHandle, threadHandle);
        }
        return StartResult::ContainmentFailed;
    }
    return StartResult::Managed;
#else
    if (cancelRequested) {
        process.kill();
        return StartResult::Cancelled;
    }
    return StartResult::Managed;
#endif
}

void ProcessTreeController::processFailedToStart(QProcess &process)
{
#ifdef Q_OS_WIN
    releaseJob();
    process.setCreateProcessArgumentsModifier(QProcess::CreateProcessArgumentModifier());
#else
    Q_UNUSED(process)
#endif
}

void ProcessTreeController::processFinished(QProcess &process)
{
    Q_UNUSED(process)
#ifdef Q_OS_WIN
    // 根进程自然退出后关闭 KILL_ON_JOB_CLOSE Job，清理仍存活的后代。
    releaseJob();
#endif
}

void ProcessTreeController::terminate(QProcess &process)
{
#ifdef Q_OS_WIN
    bool terminated = false;
    if (m_job && m_assigned) {
        terminated = TerminateJobObject(static_cast<HANDLE>(m_job), 1) != FALSE;
        if (!terminated) {
            // KILL_ON_JOB_CLOSE 是第二条整树路径；不退回同步 taskkill。
            releaseJob();
        }
    }
    if (!terminated && process.state() != QProcess::NotRunning)
        process.kill();
#else
    // 非 Windows 只承诺终止 QProcess 根进程，不声称覆盖其后代。
    if (process.state() != QProcess::NotRunning)
        process.kill();
#endif
}

void ProcessTreeController::shutdown(QProcess &process)
{
#ifdef Q_OS_WIN
    if (process.state() == QProcess::Starting) {
        // Qt 5/Windows 的 Starting 位于同步 CreateProcessW 调用栈中，尚不能使用
        // PROCESS_INFORMATION。有效的同线程析构只能重入在 modifier 调用之前；
        // 先撤掉 CREATE_SUSPENDED，确保外层启动栈即使继续也不会产生无人恢复的线程。
        process.setCreateProcessArgumentsModifier(QProcess::CreateProcessArgumentModifier());
        releaseJob();
        process.kill();
        return;
    }
#endif

    terminate(process);
    if (process.state() != QProcess::NotRunning) {
        process.waitForFinished(3000);
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(1000);
        }
    }
#ifdef Q_OS_WIN
    releaseJob();
    process.setCreateProcessArgumentsModifier(QProcess::CreateProcessArgumentModifier());
#endif
}

#ifdef Q_OS_WIN
void ProcessTreeController::releaseJob()
{
    if (m_job) {
        CloseHandle(static_cast<HANDLE>(m_job));
        m_job = nullptr;
    }
    m_assigned = false;
}
#endif

#ifdef SCRIPT_RUNNER_ENABLE_TEST_HOOKS
void ProcessTreeController::failNextForTest(TestFailure failure)
{
#  ifdef Q_OS_WIN
    g_testFailure.store(static_cast<int>(failure));
#  else
    Q_UNUSED(failure)
#  endif
}
#endif
