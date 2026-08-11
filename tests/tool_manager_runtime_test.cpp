#include "tool_manager.h"
#include "process_command.h"
#include "qt_test_failure_sink.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>

#ifdef Q_OS_WIN
#include <stdlib.h>
#endif

namespace {

bool setUnicodeEnvironment(const QByteArray &name, const QString &value)
{
#ifdef Q_OS_WIN
    const QString wideName = QString::fromLatin1(name);
    return _wputenv_s(
               reinterpret_cast<const wchar_t *>(wideName.utf16()),
               reinterpret_cast<const wchar_t *>(value.utf16())) == 0;
#else
    return qputenv(name.constData(), value.toUtf8());
#endif
}

bool clearUnicodeEnvironment(const QByteArray &name)
{
#ifdef Q_OS_WIN
    const QString wideName = QString::fromLatin1(name);
    return _wputenv_s(
               reinterpret_cast<const wchar_t *>(wideName.utf16()), L"") == 0;
#else
    return qunsetenv(name.constData());
#endif
}

bool previousEnvironmentValueIsRestorable(const char *name, const QString &value)
{
#ifdef Q_OS_WIN
    Q_UNUSED(name);
    // MSVCRT represents an empty value as deletion, and ToolManager reads the
    // CRT environment. Normalize an inherited empty value to unset.
    return !value.isEmpty();
#else
    Q_UNUSED(value);
    return qEnvironmentVariableIsSet(name);
#endif
}

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char *name, const QString &value)
        : m_name(name),
          m_previous(qEnvironmentVariable(name)),
          m_wasSet(previousEnvironmentValueIsRestorable(name, m_previous)),
          m_valid(setUnicodeEnvironment(m_name, value))
    {
    }

    ~ScopedEnvironmentVariable()
    {
        (void)restore();
    }

    bool isValid() const noexcept { return m_valid; }

    bool restore() noexcept
    {
        if (!m_valid) return false;
        if (!m_active) return true;
        const bool restored = m_wasSet
            ? setUnicodeEnvironment(m_name, m_previous)
            : clearUnicodeEnvironment(m_name);
        if (restored) m_active = false;
        return restored;
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable &) = delete;
    ScopedEnvironmentVariable &operator=(const ScopedEnvironmentVariable &) = delete;

private:
    QByteArray m_name;
    QString m_previous;
    bool m_wasSet = false;
    bool m_valid = false;
    bool m_active = m_valid;
};

bool writeCommandFixture(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || file.write(contents) != contents.size()) {
        return false;
    }
    file.close();
#ifndef Q_OS_WIN
    return file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                               | QFileDevice::ExeOwner);
#else
    return true;
#endif
}

int fail(aegisy::test::FailureCode code, const char *message)
{
    aegisy::test::reportFailure(code);
    aegisy::test::reportLocalDiagnostic(message);
    return 1;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);

    const QString nativeArguments = ProcessCommand::windowsBatchNativeArguments(
        QStringLiteral("C:/Program Files/nodejs/npm.cmd"),
        { QStringLiteral("install"), QStringLiteral("-g"),
          QStringLiteral("@openai/codex@latest") });
    const QString expectedArguments = QStringLiteral(
        "/D /V:OFF /S /C \"\"C:/Program Files/nodejs/npm.cmd\" "
        "\"install\" \"-g\" \"@openai/codex@latest\"\"");
    if (nativeArguments != expectedArguments
            || nativeArguments.contains(QStringLiteral("\\\""))) {
        return fail(aegisy::test::FailureCode::TOOL_COMMAND_SHAPE,
                    "Windows batch command line escaped quotes incorrectly");
    }

    constexpr const char *restoreVariableName =
        "AEGISY_TOOL_MANAGER_RESTORE_TEST";
    const QString inheritedRestoreValue = qEnvironmentVariable(restoreVariableName);
    const bool inheritedRestoreValueIsSet =
        previousEnvironmentValueIsRestorable(
            restoreVariableName, inheritedRestoreValue);
    ScopedEnvironmentVariable restoreOriginal(
        restoreVariableName, QStringLiteral("Unicode 恢复原值"));
    if (!restoreOriginal.isValid()
            || qEnvironmentVariable(restoreVariableName)
                != QStringLiteral("Unicode 恢复原值")) {
        return fail(aegisy::test::FailureCode::TOOL_NPM_RESIDUE_FIXTURE,
                    "failed to install the Unicode environment restore fixture");
    }
    {
        ScopedEnvironmentVariable restoreOverride(
            restoreVariableName, QStringLiteral("Unicode 覆盖值"));
        if (!restoreOverride.isValid()
                || qEnvironmentVariable(restoreVariableName)
                    != QStringLiteral("Unicode 覆盖值")
                || !restoreOverride.restore()
                || qEnvironmentVariable(restoreVariableName)
                    != QStringLiteral("Unicode 恢复原值")) {
            return fail(aegisy::test::FailureCode::TOOL_NPM_RESIDUE_FIXTURE,
                        "Unicode environment value did not restore exactly");
        }
    }
    if (!restoreOriginal.restore()
            || qEnvironmentVariableIsSet(restoreVariableName)
                != inheritedRestoreValueIsSet
            || (inheritedRestoreValueIsSet
                && qEnvironmentVariable(restoreVariableName)
                    != inheritedRestoreValue)) {
        return fail(aegisy::test::FailureCode::TOOL_NPM_RESIDUE_FIXTURE,
                    "inherited environment state did not restore exactly");
    }

#ifdef Q_OS_WIN
    QTemporaryDir batchFixture(
        QDir::tempPath() + QStringLiteral("/aegisy command test XXXXXX"));
    const QString batchPath = batchFixture.filePath(QStringLiteral("version.cmd"));
    QFile batchFile(batchPath);
    if (!batchFixture.isValid() || !batchFile.open(QIODevice::WriteOnly)
            || batchFile.write("@echo off\r\necho 7.8.9\r\n") < 0) {
        return fail(aegisy::test::FailureCode::TOOL_BATCH_EXECUTION,
                    "failed to create Windows batch command fixture");
    }
    batchFile.close();

    QProcess batchProcess;
    batchProcess.setProcessChannelMode(QProcess::MergedChannels);
    ProcessCommand::start(&batchProcess, batchPath, {});
    if (!batchProcess.waitForStarted(2000) || !batchProcess.waitForFinished(5000)
            || batchProcess.exitStatus() != QProcess::NormalExit
            || batchProcess.exitCode() != 0
            || !ProcessCommand::decodeOutput(batchProcess.readAll())
                    .contains(QStringLiteral("7.8.9"))) {
        return fail(aegisy::test::FailureCode::TOOL_BATCH_EXECUTION,
                    "failed to execute a batch command from a path with spaces");
    }

    // npm creates an extensionless POSIX shim next to the Windows .cmd shim.
    // Detection must ignore the former because QProcess/CreateProcess cannot
    // execute it directly.
    QTemporaryDir npmFixture(
        QDir::tempPath() + QStringLiteral("/aegisy npm shim test XXXXXX"));
    const QString npmBin = npmFixture.filePath(QStringLiteral("npm"));
    if (!npmFixture.isValid() || !QDir().mkpath(npmBin)) {
        return fail(aegisy::test::FailureCode::TOOL_SHIM_RESOLUTION,
                    "failed to create npm shim fixture");
    }
    QFile posixShim(npmBin + QStringLiteral("/codex"));
    QFile cmdShim(npmBin + QStringLiteral("/codex.cmd"));
    if (!posixShim.open(QIODevice::WriteOnly)
            || posixShim.write("#!/bin/sh\necho wrong-shim\n") < 0
            || !cmdShim.open(QIODevice::WriteOnly)
            || cmdShim.write("@echo off\r\necho codex-cli 9.8.7\r\n") < 0) {
        return fail(aegisy::test::FailureCode::TOOL_SHIM_RESOLUTION,
                    "failed to write npm shim fixture");
    }
    posixShim.close();
    cmdShim.close();

    QString resolvedCodex;
    {
        ScopedEnvironmentVariable appData("APPDATA", npmFixture.path());
        if (!appData.isValid()) {
            return fail(aegisy::test::FailureCode::TOOL_SHIM_RESOLUTION,
                        "failed to isolate the Windows npm shim environment");
        }
        ToolManager shimManager;
        resolvedCodex = shimManager.resolvedExecutable(AiTool::CodexCli, 500);
        if (!appData.restore()) {
            return fail(aegisy::test::FailureCode::TOOL_SHIM_RESOLUTION,
                        "failed to restore the Windows npm shim environment");
        }
    }

    if (QFileInfo(resolvedCodex).absoluteFilePath()
            != QFileInfo(cmdShim.fileName()).absoluteFilePath()) {
        return fail(aegisy::test::FailureCode::TOOL_SHIM_RESOLUTION,
                    "Windows command detection did not prefer the npm .cmd shim");
    }
    QProcess shimProcess;
    shimProcess.setProcessChannelMode(QProcess::MergedChannels);
    ProcessCommand::start(&shimProcess, resolvedCodex, { QStringLiteral("--version") });
    if (!shimProcess.waitForStarted(2000) || !shimProcess.waitForFinished(5000)
            || shimProcess.exitStatus() != QProcess::NormalExit
            || shimProcess.exitCode() != 0
            || !ProcessCommand::decodeOutput(shimProcess.readAll())
                    .contains(QStringLiteral("9.8.7"))) {
        return fail(aegisy::test::FailureCode::TOOL_SHIM_RESOLUTION,
                    "failed to execute the resolved npm .cmd shim");
    }
#endif

    ToolManager manager;
    const QList<RuntimeStatus> runtimes = manager.detectRuntimes(300);

    if (runtimes.size() != 5) {
        return fail(aegisy::test::FailureCode::TOOL_RUNTIME_REGISTRY,
                    "expected five runtime definitions");
    }

    QSet<QString> ids;
    QSet<QString> requiredIds;
    for (const RuntimeStatus &runtime : runtimes) {
        ids.insert(runtime.id);
        if (runtime.required) {
            requiredIds.insert(runtime.id);
        }
        if (runtime.installed != !runtime.executablePath.isEmpty()) {
            return fail(aegisy::test::FailureCode::TOOL_RUNTIME_REGISTRY,
                        "installed state does not match executable path");
        }
    }

    const QSet<QString> expectedIds = {
        QStringLiteral("node"), QStringLiteral("npm"), QStringLiteral("git"),
        QStringLiteral("pnpm"), QStringLiteral("bun") };
    const QSet<QString> expectedRequired = {
        QStringLiteral("node"), QStringLiteral("npm"), QStringLiteral("git") };
    if (ids != expectedIds || requiredIds != expectedRequired) {
        return fail(aegisy::test::FailureCode::TOOL_RUNTIME_REGISTRY,
                    "runtime metadata does not match the supported registry");
    }

    // Regression: an interrupted global npm upgrade may leave package metadata
    // behind after the CLI shim has disappeared. Use an isolated fake command
    // path so an installed developer copy of OpenCode cannot skip this fixture.
    QTemporaryDir residueFixture(
        QDir::tempPath() + QStringLiteral("/aegisy npm 残留 XXXXXX"));
#ifdef Q_OS_WIN
    const QString nodePath = residueFixture.filePath(QStringLiteral("node.cmd"));
    const QString npmPath = residueFixture.filePath(QStringLiteral("npm.cmd"));
    const QByteArray nodeFixture("@echo off\r\necho v20.0.0\r\n");
    const QByteArray npmFixtureBody(
        "@echo off\r\n"
        "if /I \"%~1\"==\"list\" if /I \"%~2\"==\"-g\" if /I \"%~3\"==\"opencode-ai\" if /I \"%~4\"==\"--depth=0\" if /I \"%~5\"==\"--json\" if \"%~6\"==\"\" (\r\n"
        "  echo {\"dependencies\":{\"opencode-ai\":{\"version\":\"9.8.7\"}}}\r\n"
        "  exit /b 0\r\n"
        ")\r\n"
        "if /I \"%~1\"==\"--version\" if \"%~2\"==\"\" (echo 10.0.0& exit /b 0)\r\n"
        "exit /b 1\r\n");
#else
    const QString nodePath = residueFixture.filePath(QStringLiteral("node"));
    const QString npmPath = residueFixture.filePath(QStringLiteral("npm"));
    const QByteArray nodeFixture("#!/bin/sh\nprintf 'v20.0.0\\n'\n");
    const QByteArray npmFixtureBody(
        "#!/bin/sh\n"
        "if [ \"$#\" -eq 5 ] && [ \"$1\" = \"list\" ] && [ \"$2\" = \"-g\" ] && [ \"$3\" = \"opencode-ai\" ] && [ \"$4\" = \"--depth=0\" ] && [ \"$5\" = \"--json\" ]; then\n"
        "  printf '%s\\n' '{\"dependencies\":{\"opencode-ai\":{\"version\":\"9.8.7\"}}}'\n"
        "  exit 0\n"
        "fi\n"
        "if [ \"$#\" -eq 1 ] && [ \"$1\" = \"--version\" ]; then printf '10.0.0\\n'; exit 0; fi\n"
        "exit 1\n");
#endif
    if (!residueFixture.isValid()
            || !writeCommandFixture(nodePath, nodeFixture)
            || !writeCommandFixture(npmPath, npmFixtureBody)) {
        return fail(aegisy::test::FailureCode::TOOL_NPM_RESIDUE_FIXTURE,
                    "failed to create isolated npm residue commands");
    }
    ScopedEnvironmentVariable isolatedPath(
        "AEGISY_TOOL_MANAGER_TEST_PATH", residueFixture.path());
    ScopedEnvironmentVariable isolatedConfigHome(
        "AEGISY_CONFIG_HOME", residueFixture.path());
    if (!isolatedPath.isValid() || !isolatedConfigHome.isValid()) {
        return fail(aegisy::test::FailureCode::TOOL_NPM_RESIDUE_FIXTURE,
                    "failed to isolate the npm residue environment");
    }

    ToolManager residueManager;
    const ToolStatus damaged = residueManager.detectFast(AiTool::OpenCode, 5000);
    if (!damaged.nodeOk || damaged.installed || !damaged.repairRequired
            || damaged.version != QStringLiteral("9.8.7")
            || damaged.installationIssue.isEmpty()) {
        return fail(aegisy::test::FailureCode::TOOL_NPM_RESIDUE_SYNC,
                    "npm package residue was not classified as repairable damage");
    }

    bool asyncObserved = false;
    bool asyncInstalled = true;
    QString asyncVersion;
    QEventLoop loop;
    QObject::connect(&residueManager, &ToolManager::toolVersionDetected, &loop,
        [&loop, &asyncObserved, &asyncInstalled, &asyncVersion](
            AiTool tool, bool installed, const QString &version) {
            if (tool != AiTool::OpenCode) return;
            asyncObserved = true;
            asyncInstalled = installed;
            asyncVersion = version;
            loop.quit();
        });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    residueManager.detectVersion(AiTool::OpenCode);
    loop.exec();

    if (!asyncObserved) {
        return fail(aegisy::test::FailureCode::TOOL_NPM_RESIDUE_TIMEOUT,
                    "async CLI residue detection did not complete before its deadline");
    }
    if (asyncInstalled || asyncVersion != QStringLiteral("9.8.7")) {
        return fail(aegisy::test::FailureCode::TOOL_NPM_RESIDUE_ASYNC,
                    "async CLI detection hid the repairable npm residue");
    }
    const bool configHomeRestored = isolatedConfigHome.restore();
    const bool pathRestored = isolatedPath.restore();
    if (!configHomeRestored || !pathRestored) {
        return fail(aegisy::test::FailureCode::TOOL_NPM_RESIDUE_FIXTURE,
                    "failed to restore the npm residue environment");
    }
    return 0;
}
