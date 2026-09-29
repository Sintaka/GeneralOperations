#include "model/ScriptRunner.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

#ifndef GO_DEV_SCRIPTS_DIR
#  define GO_DEV_SCRIPTS_DIR ""
#endif

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

bool copyRunnerRuntime(const QString &targetDirectory)
{
    const QStringList names{QStringLiteral("Qt5Core.dll"),
                            QStringLiteral("libgcc_s_seh-1.dll"),
                            QStringLiteral("libstdc++-6.dll"),
                            QStringLiteral("libwinpthread-1.dll")};
    const QString sourceDirectory = QCoreApplication::applicationDirPath();
    for (const QString &name : names) {
        const QString source = sourceDirectory + QLatin1Char('/') + name;
        const QString target = targetDirectory + QLatin1Char('/') + name;
        if (QFileInfo::exists(target))
            continue;
        if (!QFile::copy(source, target))
            return false;
    }
    return true;
}

/// 错误列表里是否存在含 needle 的条目。manifest 同一条声明可能引发多条
/// 措辞相近的错误，按子串匹配而不是整串相等，断言不绑死文案顺序。
bool anyErrorContains(const QStringList &errors, const QString &needle)
{
    for (const QString &e : errors) {
        if (e.contains(needle))
            return true;
    }
    return false;
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

bool writePythonManifest(const QString &path, const QString &name,
                         const QString &requirements)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    const QByteArray source = QStringLiteral(
        "\"\"\"\n"
        "@name %1\n"
        "@group Tests\n"
        "@desc Python wrapper test helper\n"
        "@accepts file\n"
        "@requires %2\n"
        "@param threshold : int : 12 : Threshold\n"
        "\"\"\"\n")
                                   .arg(name, requirements)
                                   .toUtf8();
    return file.write(source) == source.size();
}

bool writeExeManifest(const QString &path, const QString &name, const QString &exeName)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    const QByteArray source = QStringLiteral(
        "\"\"\"\n"
        "@name %1\n"
        "@group Tests\n"
        "@desc exe host test helper\n"
        "@accepts file\n"
        "@host exe\n"
        "@exe %2\n"
        "\"\"\"\n")
                                  .arg(name, exeName)
                                  .toUtf8();
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

void testPythonLauncherRequest()
{
    using namespace script_runner_detail;

    g_case = "python_launcher_command_arguments";
    const QString helperPath = QStringLiteral("C:/dev/runtime/python-launcher.ps1");
    GO_CHECK(pythonLauncherArguments(helperPath)
             == (QStringList{QStringLiteral("-NoProfile"),
                             QStringLiteral("-NonInteractive"),
                             QStringLiteral("-ExecutionPolicy"),
                             QStringLiteral("Bypass"),
                             QStringLiteral("-File"), helperPath}));

    g_case = "python_launcher_json_preserves_cli_arguments";
    const QString scriptPath = QStringLiteral("C:/脚本/Format Convert.py");
    const QString scriptsDir = QStringLiteral("C:/脚本");
    const QStringList args{QStringLiteral("--threshold"), QStringLiteral("7"),
                           QStringLiteral("--"), QStringLiteral("- input_日本 \"one\".png")};
    ScriptManifest firstScript;
    firstScript.requires_ = {
        {QStringLiteral("Pillow"), false},
        {QStringLiteral("OpenImageIO==3.1.14.0"), true},
        {QStringLiteral("NumPy"), true},
        {QStringLiteral("PILLOW"), true},
        {QStringLiteral("日本語包==1.0"), true}
    };
    ScriptManifest secondScript;
    secondScript.requires_ = {
        {QStringLiteral("openimageio==3.1.14.0"), false},
        {QStringLiteral("numpy"), true},
        {QStringLiteral("SharedPackage"), true}
    };
    const QVector<ScriptManifest> scripts{firstScript, secondScript};
    const QJsonDocument request = QJsonDocument::fromJson(
        pythonLauncherRequestJson(scriptPath, args, scriptsDir, scripts));
    GO_CHECK(request.isObject());
    const QJsonObject object = request.object();
    GO_CHECK(object.size() == 4);
    GO_CHECK(object.contains(QStringLiteral("script")));
    GO_CHECK(object.contains(QStringLiteral("args")));
    GO_CHECK(object.contains(QStringLiteral("scriptsDir")));
    GO_CHECK(object.contains(QStringLiteral("requirements")));
    GO_CHECK(object.value(QStringLiteral("script")).toString()
             == QDir::cleanPath(QFileInfo(scriptPath).absoluteFilePath()));
    GO_CHECK(object.value(QStringLiteral("scriptsDir")).toString()
             == QDir::cleanPath(QFileInfo(scriptsDir).absoluteFilePath()));
    QStringList decodedArgs;
    for (const QJsonValue &value : object.value(QStringLiteral("args")).toArray())
        decodedArgs << value.toString();
    GO_CHECK(decodedArgs == args);
    QStringList decodedRequirements;
    for (const QJsonValue &value : object.value(QStringLiteral("requirements")).toArray())
        decodedRequirements << value.toString();
    const QStringList expectedDecodedRequirements{
        QStringLiteral("Pillow"),
        QStringLiteral("OpenImageIO==3.1.14.0?"),
        QStringLiteral("NumPy?"),
        QStringLiteral("PILLOW?"),
        QStringLiteral("日本語包==1.0?"),
        QStringLiteral("openimageio==3.1.14.0"),
        QStringLiteral("numpy?"),
        QStringLiteral("SharedPackage?")
    };
    GO_CHECK(decodedRequirements == expectedDecodedRequirements);

    g_case = "scripts_root_follows_group_path_contract";
    QTemporaryDir temp;
    GO_CHECK(temp.isValid());
    const QString root = temp.path() + QStringLiteral("/scripts");
    const QString nested = root + QStringLiteral("/Image/Format Convert/job.py");
    const QString rootScript = root + QStringLiteral("/os.FlattenFolderHierarchy.py");
    GO_CHECK(scriptsRootForManifest(nested, QStringLiteral("Image/Format Convert"))
             == QDir::cleanPath(root));
    GO_CHECK(scriptsRootForManifest(rootScript, QStringLiteral("System"))
             == QDir::cleanPath(root));
    GO_CHECK(scriptsRootForManifest(nested, QStringLiteral("System"))
             == QDir::cleanPath(QFileInfo(nested).absolutePath()));
}

void testExeHostManifest()
{
    g_case = "exe_host_parses_exe_name";
    const ScriptManifest ok = ScriptManifest::fromSource(QStringLiteral(
        "\"\"\"\n"
        "@name E\n"
        "@group Tests\n"
        "@desc d\n"
        "@accepts file\n"
        "@host exe\n"
        "@exe go_pmx2glb\n"
        "\"\"\"\n"), QStringLiteral("mem://ok.py"));
    GO_CHECK(ok.isValid());
    GO_CHECK(ok.host == ScriptManifest::Exe);
    GO_CHECK(ok.exeName == QStringLiteral("go_pmx2glb"));

    g_case = "exe_name_allows_dots_hyphens_underscores";
    const ScriptManifest dotted = ScriptManifest::fromSource(QStringLiteral(
        "\"\"\"\n"
        "@name E\n"
        "@group Tests\n"
        "@desc d\n"
        "@accepts file\n"
        "@host exe\n"
        "@exe go_pmx2glb.v2-beta\n"
        "\"\"\"\n"), QStringLiteral("mem://dotted.py"));
    GO_CHECK(dotted.isValid());
    GO_CHECK(dotted.exeName == QStringLiteral("go_pmx2glb.v2-beta"));

    g_case = "exe_host_without_exe_key_is_invalid";
    const ScriptManifest missing = ScriptManifest::fromSource(QStringLiteral(
        "\"\"\"\n"
        "@name E\n"
        "@group Tests\n"
        "@desc d\n"
        "@accepts file\n"
        "@host exe\n"
        "\"\"\"\n"), QStringLiteral("mem://missing.py"));
    GO_CHECK(!missing.isValid());
    GO_CHECK(missing.errors.contains(QStringLiteral(
        "@host exe 需要在脚本头用 @exe 声明后端可执行文件名")));

    g_case = "bare_exe_key_is_missing_name";
    const ScriptManifest bare = ScriptManifest::fromSource(QStringLiteral(
        "\"\"\"\n"
        "@name E\n"
        "@group Tests\n"
        "@desc d\n"
        "@accepts file\n"
        "@host exe\n"
        "@exe\n"
        "\"\"\"\n"), QStringLiteral("mem://bare.py"));
    GO_CHECK(!bare.isValid());
    GO_CHECK(bare.errors.contains(QStringLiteral("@exe 缺少可执行文件名")));

    g_case = "exe_name_rejects_illegal_characters";
    // 空白会进值（kvRe 把行内剩余全部内容当值），要按非法字符拦下。
    const ScriptManifest illegal = ScriptManifest::fromSource(QStringLiteral(
        "\"\"\"\n"
        "@name E\n"
        "@group Tests\n"
        "@desc d\n"
        "@accepts file\n"
        "@host exe\n"
        "@exe go pmx\n"
        "\"\"\"\n"), QStringLiteral("mem://illegal.py"));
    GO_CHECK(!illegal.isValid());
    GO_CHECK(anyErrorContains(illegal.errors, QStringLiteral("@exe 名字含非法字符")));

    g_case = "exe_name_rejects_path_segments";
    const ScriptManifest parent = ScriptManifest::fromSource(QStringLiteral(
        "\"\"\"\n"
        "@name E\n"
        "@group Tests\n"
        "@desc d\n"
        "@accepts file\n"
        "@host exe\n"
        "@exe ..\n"
        "\"\"\"\n"), QStringLiteral("mem://parent.py"));
    GO_CHECK(!parent.isValid());
    GO_CHECK(anyErrorContains(parent.errors, QStringLiteral("@exe 名字含非法字符")));

    g_case = "default_host_still_python";
    const ScriptManifest defaultHost = ScriptManifest::fromSource(QStringLiteral(
        "\"\"\"\n"
        "@name E\n"
        "@group Tests\n"
        "@desc d\n"
        "@accepts file\n"
        "\"\"\"\n"), QStringLiteral("mem://default.py"));
    GO_CHECK(defaultHost.isValid());
    GO_CHECK(defaultHost.host == ScriptManifest::Python);
    GO_CHECK(defaultHost.exeName.isEmpty());
}

void testSharedParserOnExistingScripts()
{
    const QDir scriptsRoot(QString::fromUtf8(GO_DEV_SCRIPTS_DIR));

    g_case = "shared_manifest_parses_exr2png_declaration_and_parameters";
    const ScriptManifest exr = ScriptManifest::fromFile(scriptsRoot.filePath(
        QStringLiteral("Image/Format Convert/img.EXR2PNG_Large.py")));
    GO_CHECK(exr.isValid());
    GO_CHECK(exr.name == QStringLiteral("EXR to PNG (Large / 4K)"));
    GO_CHECK(exr.group == QStringLiteral("Image/Format Convert"));
    GO_CHECK(exr.accepts == ScriptManifest::File);
    GO_CHECK(exr.acceptsExtension(QStringLiteral("EXR")));
    GO_CHECK(exr.params.size() == 7);
    GO_CHECK(exr.requires_.size() == 8);
    if (exr.requires_.size() == 8) {
        GO_CHECK(exr.requires_.at(5).name == QStringLiteral("OpenImageIO==3.1.14.0"));
        GO_CHECK(exr.requires_.at(5).optional);
        GO_CHECK(!exr.requires_.at(0).optional);
    }
    if (exr.params.size() == 7) {
        const ScriptParam &target = exr.params.at(0);
        GO_CHECK(target.name == QStringLiteral("target"));
        GO_CHECK(target.type == ScriptParam::Int);
        GO_CHECK(target.defaultValue.type() == QVariant::Int);
        GO_CHECK(target.defaultValue.toInt() == 4096);
        GO_CHECK(target.hasRange);
        GO_CHECK(target.minValue == 8.0);
        GO_CHECK(target.maxValue == 16384.0);
        GO_CHECK(target.presets.size() == 5);
        GO_CHECK(target.presets.at(2).toDouble() == 4096.0);

        const ScriptParam &noMip = exr.params.at(5);
        GO_CHECK(noMip.name == QStringLiteral("no_mip"));
        GO_CHECK(noMip.type == ScriptParam::Bool);
        GO_CHECK(!noMip.defaultValue.toBool());
    }

    g_case = "shared_manifest_preserves_float_defaults_and_presets";
    const ScriptManifest upscale = ScriptManifest::fromFile(scriptsRoot.filePath(
        QStringLiteral("Image/ReSize/img.RealESRGAN_Upscale.py")));
    GO_CHECK(upscale.isValid());
    GO_CHECK(upscale.params.size() >= 1);
    if (!upscale.params.isEmpty()) {
        const ScriptParam &scale = upscale.params.at(0);
        GO_CHECK(scale.name == QStringLiteral("scale"));
        GO_CHECK(scale.type == ScriptParam::Float);
        GO_CHECK(scale.defaultValue.type() == QVariant::Double);
        GO_CHECK(scale.defaultValue.toDouble() == 2.0);
        GO_CHECK(scale.hasRange);
        GO_CHECK(scale.minValue == 1.0);
        GO_CHECK(scale.maxValue == 4.0);
        GO_CHECK(scale.presets.size() == 3);
    }

    g_case = "shared_manifest_parses_pmx2fbx_blender_declaration";
    const ScriptManifest pmx = ScriptManifest::fromFile(scriptsRoot.filePath(
        QStringLiteral("Geometry/Format Convert/geo.pmx2fbx.py")));
    GO_CHECK(pmx.isValid());
    GO_CHECK(pmx.name == QStringLiteral("PMX to FBX"));
    GO_CHECK(pmx.group == QStringLiteral("Geometry/Format Convert"));
    GO_CHECK(pmx.host == ScriptManifest::Blender);
    GO_CHECK(pmx.blenderPath
             == QStringLiteral("C:/Program Files/Blender Foundation/Blender 5.2/blender.exe"));
    GO_CHECK(pmx.extensions == QStringList{QStringLiteral(".pmx")});
    GO_CHECK(!pmx.multi);
}

#ifdef Q_OS_WIN
void testScriptRunnerCancellation()
{
    QTemporaryDir temp;
    GO_CHECK(temp.isValid());
    if (!temp.isValid())
        return;

    const QString releaseHelper = QCoreApplication::applicationDirPath()
        + QStringLiteral("/python-launcher.ps1");
    bool createdReleaseHelper = false;
    if (!QFileInfo::exists(releaseHelper)) {
        QFile helper(releaseHelper);
        if (helper.open(QIODevice::WriteOnly | QIODevice::Text)) {
            helper.write("# test sentinel\n");
            helper.close();
            createdReleaseHelper = true;
        }
    }
    GO_CHECK(QFileInfo::exists(releaseHelper));

    const QString helperManifest = temp.filePath(QStringLiteral("helper.py"));
    const QString missingManifest = temp.filePath(QStringLiteral("missing.py"));
    const QString pythonManifest = temp.filePath(QStringLiteral("python.py"));
    const QString pythonExtraManifest = temp.filePath(QStringLiteral("z-python-extra.py"));
    const QString executable = QCoreApplication::applicationFilePath();
    qputenv("SCRIPT_RUNNER_TEST_EXE", QDir::toNativeSeparators(executable).toLocal8Bit());
    const QString fakePowerShell = temp.filePath(QStringLiteral("powershell.exe"));
    GO_CHECK(QFile::copy(executable, fakePowerShell));
    GO_CHECK(copyRunnerRuntime(temp.path()));
    const QByteArray originalPath = qgetenv("PATH");
    const QByteArray testRuntimeDir =
        QDir::toNativeSeparators(QCoreApplication::applicationDirPath()).toLocal8Bit();
    const QByteArray separator(1, QDir::listSeparator().toLatin1());
    qputenv("PATH", QDir::toNativeSeparators(temp.path()).toLocal8Bit()
                        + separator + testRuntimeDir + separator + originalPath);

    GO_CHECK(writeManifest(helperManifest, QStringLiteral("Helper"), executable));
    GO_CHECK(writeManifest(missingManifest, QStringLiteral("Missing"),
                           temp.filePath(QStringLiteral("does-not-exist.exe"))));
    GO_CHECK(writePythonManifest(pythonManifest, QStringLiteral("PythonFallback"),
                                 QStringLiteral("Pillow OpenImageIO==3.1.14.0? SharedLib?")));
    GO_CHECK(writePythonManifest(pythonExtraManifest, QStringLiteral("PythonExtra"),
                                 QStringLiteral("Pillow? OptionalOnly?")));
    // exe 宿主的两个 manifest 必须在 registry.scan 之前落盘 ——
    // run() 按 filePath 在扫描结果里查脚本，scan 之后再写就查不到了。
    const QString exeMissingManifest = temp.filePath(QStringLiteral("exe-missing.py"));
    const QString exeOkManifest = temp.filePath(QStringLiteral("exe-ok.py"));
    GO_CHECK(writeExeManifest(exeMissingManifest, QStringLiteral("ExeMissing"),
                              QStringLiteral("go-test-no-such-backend")));
    GO_CHECK(writeExeManifest(exeOkManifest, QStringLiteral("ExeOk"),
                              QStringLiteral("go-test-fake-backend")));

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

    g_case = "python_wrapper_receives_json_request";
    const int errorsBeforeWrapper = runErrorCount;
    const QString wrapperInput = temp.filePath(QStringLiteral("- input_日本.png"));
    QVariantMap wrapperParams;
    wrapperParams.insert(QStringLiteral("threshold"), 7);
    runner.run(pythonManifest, QStringList() << wrapperInput, wrapperParams);
    GO_CHECK(waitUntil([&runner] { return !runner.running(); }, std::chrono::milliseconds(7000)));
    GO_CHECK(runner.hasRun());
    GO_CHECK(!runner.cancelled());
    GO_CHECK(runner.lastExitCode() == 0);
    GO_CHECK(runner.spawnError().isEmpty());
    GO_CHECK(runner.tail() == QStringLiteral("PYTHON_WRAPPER_OK"));
    GO_CHECK(finishedCount == 3);
    GO_CHECK(runErrorCount == errorsBeforeWrapper);

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

    g_case = "exe_host_missing_backend_preflights";
    // 测试目标不定义 GO_DEV_BACKEND_DIR，appDir 下也没有 tools/go-test-no-such-backend，
    // 两条查找路径都落空 → 必须在 resetForRun 之前同步 failPreflight。
    const int errorsBeforeExe = runErrorCount;
    runner.run(exeMissingManifest, QStringList());
    GO_CHECK(!runner.running());
    GO_CHECK(!runner.hasRun());
    GO_CHECK(runner.spawnError().contains(QStringLiteral(
        "找不到 @exe 声明的后端可执行文件 go-test-no-such-backend")));
    GO_CHECK(runner.spawnError().contains(QStringLiteral("tools/go-test-no-such-backend")));
    GO_CHECK(runErrorCount == errorsBeforeExe + 1);
    GO_CHECK(errorStateCommitted);

    g_case = "exe_host_runs_backend_from_tools_dir";
    // 把测试二进制本身当作"后端 exe"，放到发行形态约定的
    // <appDir>/tools/<名字>/<名字>.exe，验证 exe 宿主的定位与 argv 同构：
    // 子进程只收到 "-- <文件>"（无脚本路径、无 --background），按文件名
    // 里的 exe-host-ok 分派（见 main() 的探测分支）。
    const QString toolsDir = QCoreApplication::applicationDirPath() + QStringLiteral("/tools");
    const QString backendDir = toolsDir + QStringLiteral("/go-test-fake-backend");
    const QString backendExe = backendDir + QStringLiteral("/go-test-fake-backend.exe");
    GO_CHECK(QDir().mkpath(backendDir));
    QFile::remove(backendExe);
    GO_CHECK(QFile::copy(QCoreApplication::applicationFilePath(), backendExe));
    GO_CHECK(copyRunnerRuntime(backendDir));
    const int errorsBeforeOk = runErrorCount;
    runner.run(exeOkManifest, QStringList() << temp.filePath(QStringLiteral("exe-host-ok")));
    GO_CHECK(waitUntil([&runner] { return !runner.running(); }, std::chrono::milliseconds(5000)));
    GO_CHECK(runner.hasRun());
    GO_CHECK(!runner.cancelled());
    GO_CHECK(runner.lastExitCode() == 0);
    GO_CHECK(runner.tail() == QStringLiteral("EXE_HOST_OK"));
    GO_CHECK(runner.spawnError().isEmpty());
    GO_CHECK(finishedCount == 8);
    GO_CHECK(runErrorCount == errorsBeforeOk);
    // 收尾：探测目录是本用例自建的，用完即删；杀毒软件短暂持锁删不掉时
    // 只留下残留，下次运行会先 removeRecursively，不影响可重复性。
    QFile::remove(backendExe);
    QDir(backendDir).removeRecursively();
    QDir(toolsDir).rmdir(toolsDir);

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

    g_case = "python_wrapper_cancellation_kills_process_tree";
    const QString wrapperTreeSentinel = temp.filePath(QStringLiteral("python-wrapper-tree"));
    const QString wrapperTreeReady = wrapperTreeSentinel + QStringLiteral(".ready");
    runner.run(pythonManifest, QStringList() << wrapperTreeSentinel, wrapperParams);
    GO_CHECK(waitForFile(wrapperTreeReady, std::chrono::milliseconds(7000)));
    runner.cancel();
    GO_CHECK(waitUntil([&runner] { return !runner.running(); }, std::chrono::milliseconds(7000)));
    GO_CHECK(runner.cancelled());
    GO_CHECK(runner.hasRun());
    GO_CHECK(runner.tail().count(QStringLiteral("^C")) == 1);
    GO_CHECK(finishedCount == 9);
    waitForDuration(std::chrono::milliseconds(2300));
    GO_CHECK(!QFile::exists(wrapperTreeSentinel));
    if (createdReleaseHelper)
        QFile::remove(releaseHelper);
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
        qEnvironmentVariable("SCRIPT_RUNNER_TEST_EXE",
                             QCoreApplication::applicationFilePath()),
        childArguments, QString(), &childPid);
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
    const QString executableName = QFileInfo(app.applicationFilePath()).fileName();
    if (executableName.compare(QStringLiteral("powershell.exe"), Qt::CaseInsensitive) == 0) {
        QByteArray payload;
        char buffer[4096];
        for (;;) {
            const size_t count = std::fread(buffer, 1, sizeof(buffer), stdin);
            if (count == 0)
                break;
            payload.append(buffer, static_cast<int>(count));
        }
        QJsonParseError parseError;
        const QJsonDocument request = QJsonDocument::fromJson(payload, &parseError);
        if (parseError.error != QJsonParseError::NoError || !request.isObject()) {
            std::printf("WRAPPER_BAD_JSON");
            return 51;
        }

        const QJsonObject object = request.object();
        const QString script = object.value(QStringLiteral("script")).toString();
        const QString scriptsDir = object.value(QStringLiteral("scriptsDir")).toString();
        const QJsonArray args = object.value(QStringLiteral("args")).toArray();
        const QJsonArray requirements = object.value(QStringLiteral("requirements")).toArray();
        const QJsonArray expectedRequirements{
            QStringLiteral("Pillow"),
            QStringLiteral("OpenImageIO==3.1.14.0?"),
            QStringLiteral("SharedLib?"),
            QStringLiteral("Pillow?"),
            QStringLiteral("OptionalOnly?")
        };
        if (!QFileInfo(script).isAbsolute()
            || QFileInfo(scriptsDir).absoluteFilePath()
                   != QFileInfo(script).absolutePath()
            || object.size() != 4
            || requirements != expectedRequirements
            || args.size() != 4
            || args.at(0).toString() != QStringLiteral("--threshold")
            || args.at(1).toString() != QStringLiteral("7")
            || args.at(2).toString() != QStringLiteral("--")) {
            std::printf("WRAPPER_BAD_FIELDS script=%s root=%s scriptRoot=%s count=%d requirements=%d",
                        qPrintable(script), qPrintable(scriptsDir),
                        qPrintable(QFileInfo(script).absolutePath()), args.size(),
                        requirements.size());
            return 52;
        }

        const QString fileArg = args.at(3).toString();
        if (QFileInfo(fileArg).fileName().contains(QStringLiteral("python-wrapper-tree")))
            return runTreeRoot(fileArg);
        if (!QFileInfo(fileArg).fileName().contains(QStringLiteral("input_日本.png"))) {
            std::printf("WRAPPER_BAD_FILE %s", qPrintable(fileArg));
            return 53;
        }
        std::printf("PYTHON_WRAPPER_OK");
        std::fflush(stdout);
        return 0;
    }
    const QStringList args = app.arguments();
    if (args.contains(QStringLiteral("--tree-child")))
        return runTreeChild(args.constLast());
    if (args.contains(QStringLiteral("--background")))
        return runTreeRoot(args.constLast());
    // exe 宿主探测分支：@host exe 的 argv 与 python 宿主同构 —— 没有
    // --background/--tree-child 这类分派标记，只有 "--" 和其后的文件，
    // 所以按末位文件名里的 "exe-host-ok" 识别（哨兵命名见 exe 宿主用例）。
    if (!args.isEmpty() && QFileInfo(args.constLast()).fileName().contains(
            QStringLiteral("exe-host-ok"))) {
        std::printf("EXE_HOST_OK");
        std::fflush(stdout);
        return 0;
    }

    testOutputBuffer();
    testPythonLauncherRequest();
    testExeHostManifest();
    testSharedParserOnExistingScripts();
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
