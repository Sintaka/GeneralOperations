#include "model/ScriptRunner.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#ifdef Q_OS_WIN
#  include <qt_windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace {

int g_failures = 0;
const char *g_case = "setup";

#define GO_CHECK(condition)                                                        \
    do {                                                                           \
        if (!(condition)) {                                                        \
            ++g_failures;                                                          \
            std::printf("FAIL %s:%d case=%s check=%s\n",                         \
                        __FILE__, __LINE__, g_case, #condition);                    \
        }                                                                          \
    } while (0)

bool waitUntil(const std::function<bool()> &predicate, std::chrono::milliseconds timeout)
{
    QDeadlineTimer deadline(timeout);
    while (!predicate() && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return predicate();
}

void waitForDuration(std::chrono::milliseconds duration)
{
    QDeadlineTimer deadline(duration);
    while (!deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
}

bool waitForFile(const QString &path, std::chrono::milliseconds timeout)
{
    return waitUntil([&path] { return QFileInfo::exists(path); }, timeout);
}

bool writeManifest(const QString &path, const QString &name, const QString &program)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    const QByteArray source = QStringLiteral(
        "\"\"\"\n"
        "@name %1\n"
        "@group Tests\n"
        "@desc Process tree test helper\n"
        "@accepts file\n"
        "@host blender\n"
        "@blender %2\n"
        "\"\"\"\n")
                                  .arg(name, program)
                                  .toUtf8();
    return file.write(source) == source.size();
}

bool writePythonManifest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    static const QByteArray source(
        "\"\"\"\n"
        "@name PythonFallback\n"
        "@group Tests\n"
        "@desc Candidate fallback helper\n"
        "@accepts file\n"
        "\"\"\"\n");
    return file.write(source) == source.size();
}

void testOutputBuffer()
{
    using script_runner_detail::OutputBuffer;

    g_case = "utf8_chunk_boundaries_and_final_line";
    OutputBuffer utf8(128, 12);
    const QByteArray ni = QStringLiteral("你").toUtf8();
    GO_CHECK(!utf8.ingest(ni.left(1)));
    GO_CHECK(utf8.tail().isEmpty());
    GO_CHECK(!utf8.ingest(ni.mid(1, 1)));
    GO_CHECK(utf8.tail().isEmpty());
    GO_CHECK(utf8.ingest(ni.mid(2)));
    GO_CHECK(utf8.tail() == QStringLiteral("你"));
    utf8.ingest(QByteArray("\r"));
    utf8.ingest(QByteArray("\n"));
    utf8.ingest(QStringLiteral("末尾").toUtf8());
    utf8.flush();
    GO_CHECK(utf8.tail() == QStringLiteral("你\n末尾"));
    GO_CHECK(!utf8.tail().contains(QChar::ReplacementCharacter));

    g_case = "line_limit_and_single_notice";
    OutputBuffer lines(256, 4);
    lines.ingest(QByteArray("one\ntwo\nthree\nfour\nfive\n"));
    const QString lineTail = lines.tail();
    GO_CHECK(lines.truncated());
    GO_CHECK(lineTail.count(QStringLiteral("输出已截断")) == 1);
    GO_CHECK(!lineTail.contains(QStringLiteral("one")));
    GO_CHECK(!lineTail.contains(QStringLiteral("two")));
    GO_CHECK(lineTail.endsWith(QStringLiteral("three\nfour\nfive")));
    GO_CHECK(lineTail.count(QLatin1Char('\n')) == 3);

    g_case = "character_capacity";
    OutputBuffer chars(5, 10);
    chars.ingest(QByteArray("abcdefgh"));
    GO_CHECK(chars.truncated());
    GO_CHECK(chars.tail().count(QStringLiteral("输出已截断")) == 1);
    GO_CHECK(chars.tail().endsWith(QStringLiteral("defgh")));

    g_case = "crlf_split_at_capacity";
    OutputBuffer crlf(1, 12);
    crlf.ingest(QByteArray("a\r"));
    crlf.ingest(QByteArray("\n"));
    GO_CHECK(!crlf.truncated());
    GO_CHECK(crlf.tail() == QStringLiteral("a"));

    g_case = "unicode_scalar_capacity";
    OutputBuffer scalar(1, 10);
    scalar.ingest(QByteArray::fromHex("f09f9880"));
    GO_CHECK(scalar.truncated());
    GO_CHECK(!scalar.tail().contains(QChar(0xde00)));

    g_case = "single_line_limit_includes_notice";
    OutputBuffer oneLine(8, 1);
    oneLine.ingest(QByteArray("a\nb"));
    GO_CHECK(oneLine.tail().count(QLatin1Char('\n')) == 0);
    GO_CHECK(oneLine.tail().count(QStringLiteral("输出已截断")) == 1);

    g_case = "invalid_utf8_is_not_buffered";
    OutputBuffer invalid(32, 4);
    GO_CHECK(invalid.ingest(QByteArray::fromHex("e080")));
    GO_CHECK(invalid.tail().contains(QChar::ReplacementCharacter));

    g_case = "cancel_line_strips_pending_cr";
    OutputBuffer cancelLine(32, 4);
    cancelLine.ingest(QByteArray("progress\r"));
    cancelLine.appendLine(QStringLiteral("^C"));
    GO_CHECK(cancelLine.tail() == QStringLiteral("progress\n^C"));

    g_case = "crash_exit_zero_is_never_success";
    GO_CHECK(script_runner_detail::normalizedExitCode(0, QProcess::CrashExit) != 0);
    GO_CHECK(script_runner_detail::normalizedExitCode(0, QProcess::NormalExit) == 0);
    GO_CHECK(script_runner_detail::normalizedExitCode(23, QProcess::CrashExit) == 23);
}

#ifdef Q_OS_WIN
void testScriptRunnerCancellation()
{
    QTemporaryDir temp;
    GO_CHECK(temp.isValid());
    if (!temp.isValid())
        return;

    const QString helperManifest = temp.filePath(QStringLiteral("helper.py"));
    const QString missingManifest = temp.filePath(QStringLiteral("missing.py"));
    const QString pythonManifest = temp.filePath(QStringLiteral("python.py"));
    const QString executable = QCoreApplication::applicationFilePath();
    const QString fakePython = temp.filePath(QStringLiteral("python.exe"));
    GO_CHECK(QFile::copy(executable, fakePython));
    const QByteArray originalPath = qgetenv("PATH");
    const QByteArray testRuntimeDir =
        QDir::toNativeSeparators(QCoreApplication::applicationDirPath()).toLocal8Bit();
    const QByteArray separator(1, QDir::listSeparator().toLatin1());
    qputenv("PATH", QDir::toNativeSeparators(temp.path()).toLocal8Bit()
                        + separator + testRuntimeDir + separator + originalPath);

    GO_CHECK(writeManifest(helperManifest, QStringLiteral("Helper"), executable));
    GO_CHECK(writeManifest(missingManifest, QStringLiteral("Missing"),
                           temp.filePath(QStringLiteral("does-not-exist.exe"))));
    GO_CHECK(writePythonManifest(pythonManifest));

    ScriptRegistry registry;
    registry.scan(temp.path());
    ScriptRunner runner(&registry);
    int finishedCount = 0;
    int runErrorCount = 0;
    int lastExitCodeNotifyCount = 0;
    bool errorStateCommitted = true;
    bool terminalSnapshotValid = true;
    int tailChangeCount = 0;
    QObject::connect(&runner, &ScriptRunner::tailChanged,
                     [&runner, &terminalSnapshotValid, &tailChangeCount] {
        ++tailChangeCount;
        if (!runner.running()) {
            terminalSnapshotValid = terminalSnapshotValid
                && (runner.hasRun() || !runner.spawnError().isEmpty());
        }
    });
    QObject::connect(&runner, &ScriptRunner::finished,
                     [&runner, &finishedCount, &terminalSnapshotValid](int exitCode) {
        ++finishedCount;
        terminalSnapshotValid = terminalSnapshotValid
            && !runner.running() && runner.hasRun()
            && runner.lastExitCode() == exitCode;
    });
    QObject::connect(&runner, &ScriptRunner::runError,
                     [&runner, &runErrorCount, &errorStateCommitted](const QString &reason) {
        ++runErrorCount;
        if (reason != QStringLiteral("已有脚本在运行中")) {
            errorStateCommitted = errorStateCommitted
                && !runner.running() && runner.spawnError() == reason;
        }
    });
    QObject::connect(&runner, &ScriptRunner::lastExitCodeChanged,
                     [&lastExitCodeNotifyCount] { ++lastExitCodeNotifyCount; });

    g_case = "normal_completion";
    runner.run(helperManifest,
               QStringList() << temp.filePath(QStringLiteral("normal-complete")));
    GO_CHECK(waitUntil([&runner] { return !runner.running(); }, std::chrono::milliseconds(5000)));
    GO_CHECK(runner.hasRun());
    GO_CHECK(!runner.cancelled());
    GO_CHECK(runner.lastExitCode() == 0);
    GO_CHECK(runner.tail() == QStringLiteral("NORMAL_OK"));
    GO_CHECK(finishedCount == 1);

    g_case = "crash_exit_is_failure";
    runner.run(helperManifest,
               QStringList() << temp.filePath(QStringLiteral("crash-exit")));
    GO_CHECK(waitUntil([&runner] { return !runner.running(); }, std::chrono::milliseconds(5000)));
    GO_CHECK(runner.hasRun());
    GO_CHECK(!runner.cancelled());
    GO_CHECK(runner.lastExitCode() != 0);
    GO_CHECK(runner.tail().contains(QStringLiteral("进程异常终止")));
    GO_CHECK(finishedCount == 2);
    GO_CHECK(lastExitCodeNotifyCount >= 4);

    g_case = "failed_to_start_then_new_run_resets_state";
    const int errorsBeforeMissing = runErrorCount;
    runner.run(missingManifest, QStringList());
    GO_CHECK(runner.running()); // final failure is queued, never synchronously reentrant.
    GO_CHECK(waitUntil([&runner] { return !runner.running(); }, std::chrono::milliseconds(5000)));
    GO_CHECK(!runner.spawnError().isEmpty());
    GO_CHECK(runner.spawnError().contains(QStringLiteral("启动诊断")));
    GO_CHECK(runner.spawnError().contains(QStringLiteral("does-not-exist.exe")));
    GO_CHECK(runner.spawnError().contains(QStringLiteral("failed"), Qt::CaseInsensitive)
             || runner.spawnError().contains(QStringLiteral("找不到")));
    GO_CHECK(!runner.cancelled());
    GO_CHECK(!runner.hasRun());
    GO_CHECK(runErrorCount == errorsBeforeMissing + 1);
    GO_CHECK(errorStateCommitted);

    g_case = "failed_candidate_then_python_fallback";
    const int errorsBeforeFallback = runErrorCount;
    runner.run(pythonManifest, QStringList());
    GO_CHECK(waitUntil([&runner] { return !runner.running(); }, std::chrono::milliseconds(7000)));
    GO_CHECK(runner.hasRun());
    GO_CHECK(!runner.cancelled());
    GO_CHECK(runner.lastExitCode() == 0);
    GO_CHECK(runner.spawnError().isEmpty());
    GO_CHECK(runner.tail() == QStringLiteral("PYTHON_FALLBACK_OK"));
    GO_CHECK(finishedCount == 3);
    GO_CHECK(runErrorCount == errorsBeforeFallback);

    g_case = "cancel_before_started_is_idempotent";
    const QString earlySentinel = temp.filePath(QStringLiteral("must-not-run-early"));
    QMetaObject::Connection cancelOnReset;
    cancelOnReset = QObject::connect(
        &runner, &ScriptRunner::hasRunChanged, &runner,
        [&runner, &cancelOnReset] {
            if (!runner.running() || runner.hasRun())
                return;
            QObject::disconnect(cancelOnReset);
            runner.cancel();
            runner.cancel();
        });
    QStringList earlyFiles;
    earlyFiles << earlySentinel;
    const int tailChangesBeforeEarlyCancel = tailChangeCount;
    runner.run(helperManifest, earlyFiles);
    GO_CHECK(waitUntil([&runner] { return !runner.running(); }, std::chrono::milliseconds(7000)));
    GO_CHECK(runner.hasRun());
    GO_CHECK(runner.cancelled());
    GO_CHECK(runner.tail().count(QStringLiteral("^C")) == 1);
    GO_CHECK(tailChangeCount - tailChangesBeforeEarlyCancel == 2); // reset + one committed cancel.
    GO_CHECK(!runner.tail().contains(QStringLiteral("进程异常终止")));
    GO_CHECK(finishedCount == 4);
    waitForDuration(std::chrono::milliseconds(300));
    GO_CHECK(!QFile::exists(earlySentinel));

    g_case = "job_failures_are_fail_closed";
    struct JobFailureCase {
        ProcessTreeController::TestFailure failure;
        const char *name;
    };
    const JobFailureCase jobFailures[] = {
        {ProcessTreeController::TestFailure::CreateJob, "create"},
        {ProcessTreeController::TestFailure::SetJobInformation, "set"},
        {ProcessTreeController::TestFailure::AssignProcess, "assign"}
    };
    for (const JobFailureCase &failureCase : jobFailures) {
        g_case = failureCase.name;
        const QString sentinel = temp.filePath(
            QStringLiteral("must-not-run-job-%1").arg(QString::fromLatin1(failureCase.name)));
        const int finishedBefore = finishedCount;
        const int errorsBefore = runErrorCount;
        ProcessTreeController::failNextForTest(failureCase.failure);
        QStringList files;
        files << sentinel;
        runner.run(helperManifest, files);
        GO_CHECK(waitUntil([&runner] { return !runner.running(); },
                           std::chrono::milliseconds(5000)));
        GO_CHECK(!runner.hasRun());
        GO_CHECK(!runner.cancelled());
        GO_CHECK(runner.lastExitCode() != 0);
        GO_CHECK(runner.spawnError().contains(QStringLiteral("Job Object")));
        GO_CHECK(runErrorCount == errorsBefore + 1);
        GO_CHECK(finishedCount == finishedBefore);
        GO_CHECK(errorStateCommitted);
        waitForDuration(std::chrono::milliseconds(100));
        GO_CHECK(!QFile::exists(sentinel));
    }

    g_case = "process_tree_cancellation_kills_ready_descendant";
    const QString treeSentinel = temp.filePath(QStringLiteral("tree-sentinel"));
    const QString treeReady = treeSentinel + QStringLiteral(".ready");
    runner.run(helperManifest, QStringList() << treeSentinel);
    GO_CHECK(!runner.cancelled());
    GO_CHECK(!runner.hasRun());
    GO_CHECK(runner.spawnError().isEmpty());
    GO_CHECK(waitForFile(treeReady, std::chrono::milliseconds(7000)));
    runner.cancel();
    runner.cancel();
    GO_CHECK(waitUntil([&runner] { return !runner.running(); }, std::chrono::milliseconds(7000)));
    GO_CHECK(runner.cancelled());
    GO_CHECK(runner.hasRun());
    GO_CHECK(runner.tail().count(QStringLiteral("^C")) == 1);
    GO_CHECK(!runner.tail().contains(QStringLiteral("进程异常终止")));
    GO_CHECK(finishedCount == 5);
    waitForDuration(std::chrono::milliseconds(2300));
    GO_CHECK(!QFile::exists(treeSentinel));

    g_case = "natural_root_exit_closes_tree";
    const QString naturalSentinel = temp.filePath(QStringLiteral("natural-sentinel"));
    runner.run(helperManifest, QStringList() << naturalSentinel);
    GO_CHECK(waitUntil([&runner] { return !runner.running(); }, std::chrono::milliseconds(7000)));
    GO_CHECK(runner.hasRun());
    GO_CHECK(!runner.cancelled());
    GO_CHECK(runner.lastExitCode() == 0);
    GO_CHECK(finishedCount == 6);
    waitForDuration(std::chrono::milliseconds(2300));
    GO_CHECK(!QFile::exists(naturalSentinel));

    g_case = "preflight_error_is_visible_before_signal";
    const int errorsBeforePreflight = runErrorCount;
    runner.run(temp.filePath(QStringLiteral("not-in-registry.py")), QStringList());
    GO_CHECK(!runner.running());
    GO_CHECK(runner.spawnError().contains(QStringLiteral("脚本不存在")));
    GO_CHECK(runErrorCount == errorsBeforePreflight + 1);
    GO_CHECK(errorStateCommitted);

    g_case = "direct_signal_reentry_cannot_cross_tasks";
    const QString reentrySentinel = temp.filePath(QStringLiteral("must-not-run-reentry"));
    bool armed = false;
    bool triedTail = false;
    bool triedHasRun = false;
    bool triedFinished = false;
    bool triedRunning = false;
    auto attemptReentry = [&runner, &helperManifest, &reentrySentinel](bool &tried) {
        if (tried)
            return;
        tried = true;
        QStringList files;
        files << reentrySentinel;
        runner.run(helperManifest, files);
    };
    const QMetaObject::Connection tailConnection = QObject::connect(
        &runner, &ScriptRunner::tailChanged, &runner,
        [&] { if (armed) attemptReentry(triedTail); });
    const QMetaObject::Connection hasRunConnection = QObject::connect(
        &runner, &ScriptRunner::hasRunChanged, &runner,
        [&] { if (armed && runner.hasRun()) attemptReentry(triedHasRun); });
    const QMetaObject::Connection finishedConnection = QObject::connect(
        &runner, &ScriptRunner::finished, &runner,
        [&](int) { if (armed) attemptReentry(triedFinished); });
    const QMetaObject::Connection runningConnection = QObject::connect(
        &runner, &ScriptRunner::runningChanged, &runner,
        [&] { if (armed && !runner.running()) attemptReentry(triedRunning); });
    const int errorsBeforeReentry = runErrorCount;
    QStringList normalFiles;
    normalFiles << temp.filePath(QStringLiteral("normal-complete-reentry"));
    runner.run(helperManifest, normalFiles);
    armed = true;
    GO_CHECK(waitUntil([&runner] { return !runner.running(); }, std::chrono::milliseconds(5000)));
    QObject::disconnect(tailConnection);
    QObject::disconnect(hasRunConnection);
    QObject::disconnect(finishedConnection);
    QObject::disconnect(runningConnection);
    GO_CHECK(triedTail && triedHasRun && triedFinished && triedRunning);
    GO_CHECK(runErrorCount == errorsBeforeReentry + 4);
    GO_CHECK(finishedCount == 7);
    GO_CHECK(runner.hasRun() && runner.lastExitCode() == 0);
    GO_CHECK(!QFile::exists(reentrySentinel));

    g_case = "destructor_kills_ready_descendant";
    const QString destructorSentinel = temp.filePath(QStringLiteral("destructor-sentinel"));
    const QString destructorReady = destructorSentinel + QStringLiteral(".ready");
    {
        ScriptRunner scopedRunner(&registry);
        scopedRunner.run(helperManifest, QStringList() << destructorSentinel);
        GO_CHECK(waitForFile(destructorReady, std::chrono::milliseconds(7000)));
    }
    waitForDuration(std::chrono::milliseconds(2300));
    GO_CHECK(!QFile::exists(destructorSentinel));
    GO_CHECK(terminalSnapshotValid);
}
#endif

int runTreeRoot(const QString &sentinel)
{
    if (sentinel.contains(QStringLiteral("normal-complete"))) {
        std::printf("NORMAL_OK");
        std::fflush(stdout);
        return 0;
    }
    if (sentinel.contains(QStringLiteral("crash-exit"))) {
        std::fflush(stdout);
#ifdef Q_OS_WIN
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(EXCEPTION_ACCESS_VIOLATION));
#else
        std::abort();
#endif
        return 36;
    }
    if (sentinel.contains(QStringLiteral("must-not-run"))) {
        QFile file(sentinel);
        if (!file.open(QIODevice::WriteOnly))
            return 35;
        file.write("ran");
        return 0;
    }

    qint64 childPid = 0;
    QStringList childArguments;
    childArguments << QStringLiteral("--tree-child") << sentinel;
    const bool started = QProcess::startDetached(
        QCoreApplication::applicationFilePath(), childArguments, QString(), &childPid);
    if (!started)
        return 31;
    std::printf("TREE_CHILD_STARTED %lld\n", static_cast<long long>(childPid));
    std::fflush(stdout);
    if (sentinel.contains(QStringLiteral("natural-sentinel"))) {
        const QString ready = sentinel + QStringLiteral(".ready");
        QDeadlineTimer deadline(std::chrono::seconds(5));
        while (!QFileInfo::exists(ready) && !deadline.hasExpired())
            QThread::msleep(10);
        return QFileInfo::exists(ready) ? 0 : 34;
    }
    QThread::sleep(30);
    return 0;
}

int runTreeChild(const QString &sentinel)
{
    QFile ready(sentinel + QStringLiteral(".ready"));
    if (!ready.open(QIODevice::WriteOnly))
        return 32;
    ready.write("ready");
    ready.close();

    QThread::msleep(1800);
    QFile file(sentinel);
    if (!file.open(QIODevice::WriteOnly))
        return 33;
    file.write("survived");
    file.close();
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (QFileInfo(app.applicationFilePath()).fileName().compare(
            QStringLiteral("python.exe"), Qt::CaseInsensitive) == 0) {
        std::printf("PYTHON_FALLBACK_OK"); // 无换行，顺便覆盖真实进程的末尾残行。
        std::fflush(stdout);
        return 0;
    }

    const QStringList args = app.arguments();
    if (args.contains(QStringLiteral("--tree-child")))
        return runTreeChild(args.constLast());
    if (args.contains(QStringLiteral("--background")))
        return runTreeRoot(args.constLast());

    testOutputBuffer();
#ifdef Q_OS_WIN
    testScriptRunnerCancellation();
#endif

    if (g_failures != 0) {
        std::printf("script_runner_tests: %d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("script_runner_tests: all ok\n");
    return 0;
}
