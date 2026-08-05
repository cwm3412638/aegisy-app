#include "tool_manager.h"
#include "process_command.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>

#include <iostream>

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
        std::cerr << "Windows batch command line escaped quotes incorrectly\n";
        return 1;
    }

#ifdef Q_OS_WIN
    QTemporaryDir batchFixture(
        QDir::tempPath() + QStringLiteral("/aegisy command test XXXXXX"));
    const QString batchPath = batchFixture.filePath(QStringLiteral("version.cmd"));
    QFile batchFile(batchPath);
    if (!batchFixture.isValid() || !batchFile.open(QIODevice::WriteOnly)
            || batchFile.write("@echo off\r\necho 7.8.9\r\n") < 0) {
        std::cerr << "failed to create Windows batch command fixture\n";
        return 1;
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
        std::cerr << "failed to execute a batch command from a path with spaces\n";
        return 1;
    }

    // npm creates an extensionless POSIX shim next to the Windows .cmd shim.
    // Detection must ignore the former because QProcess/CreateProcess cannot
    // execute it directly.
    QTemporaryDir npmFixture(
        QDir::tempPath() + QStringLiteral("/aegisy npm shim test XXXXXX"));
    const QString npmBin = npmFixture.filePath(QStringLiteral("npm"));
    if (!npmFixture.isValid() || !QDir().mkpath(npmBin)) {
        std::cerr << "failed to create npm shim fixture\n";
        return 1;
    }
    QFile posixShim(npmBin + QStringLiteral("/codex"));
    QFile cmdShim(npmBin + QStringLiteral("/codex.cmd"));
    if (!posixShim.open(QIODevice::WriteOnly)
            || posixShim.write("#!/bin/sh\necho wrong-shim\n") < 0
            || !cmdShim.open(QIODevice::WriteOnly)
            || cmdShim.write("@echo off\r\necho codex-cli 9.8.7\r\n") < 0) {
        std::cerr << "failed to write npm shim fixture\n";
        return 1;
    }
    posixShim.close();
    cmdShim.close();

    const bool hadAppData = qEnvironmentVariableIsSet("APPDATA");
    const QByteArray previousAppData = qgetenv("APPDATA");
    qputenv("APPDATA", npmFixture.path().toUtf8());
    ToolManager shimManager;
    const QString resolvedCodex = shimManager.resolvedExecutable(AiTool::CodexCli, 500);
    if (hadAppData) qputenv("APPDATA", previousAppData);
    else qunsetenv("APPDATA");

    if (QFileInfo(resolvedCodex).absoluteFilePath()
            != QFileInfo(cmdShim.fileName()).absoluteFilePath()) {
        std::cerr << "Windows command detection did not prefer the npm .cmd shim\n";
        return 1;
    }
    QProcess shimProcess;
    shimProcess.setProcessChannelMode(QProcess::MergedChannels);
    ProcessCommand::start(&shimProcess, resolvedCodex, { QStringLiteral("--version") });
    if (!shimProcess.waitForStarted(2000) || !shimProcess.waitForFinished(5000)
            || shimProcess.exitStatus() != QProcess::NormalExit
            || shimProcess.exitCode() != 0
            || !ProcessCommand::decodeOutput(shimProcess.readAll())
                    .contains(QStringLiteral("9.8.7"))) {
        std::cerr << "failed to execute the resolved npm .cmd shim\n";
        return 1;
    }
#endif

    ToolManager manager;
    const QList<RuntimeStatus> runtimes = manager.detectRuntimes(300);

    if (runtimes.size() != 5) {
        std::cerr << "expected five runtime definitions\n";
        return 1;
    }

    QSet<QString> ids;
    QSet<QString> requiredIds;
    for (const RuntimeStatus &runtime : runtimes) {
        ids.insert(runtime.id);
        if (runtime.required) {
            requiredIds.insert(runtime.id);
        }
        if (runtime.installed != !runtime.executablePath.isEmpty()) {
            std::cerr << "installed state does not match executable path\n";
            return 1;
        }
    }

    const QSet<QString> expectedIds = {
        QStringLiteral("node"), QStringLiteral("npm"), QStringLiteral("git"),
        QStringLiteral("pnpm"), QStringLiteral("bun") };
    const QSet<QString> expectedRequired = {
        QStringLiteral("node"), QStringLiteral("npm"), QStringLiteral("git") };
    if (ids != expectedIds || requiredIds != expectedRequired) {
        std::cerr << "runtime metadata does not match the supported registry\n";
        return 1;
    }

    // Regression: an interrupted global npm upgrade may leave package metadata
    // behind after the CLI shim has disappeared. That state must offer repair,
    // not be reported as an installed/runnable CLI.
    if (!manager.resolvedRuntimeCommand(
#ifdef Q_OS_WIN
            QStringLiteral("npm.cmd"), 500
#else
            QStringLiteral("npm"), 500
#endif
            ).isEmpty()
            && manager.resolvedExecutable(AiTool::OpenCode, 300).isEmpty()) {
        QTemporaryDir prefix;
        if (!prefix.isValid()) {
            std::cerr << "failed to create npm fixture prefix\n";
            return 1;
        }
#ifdef Q_OS_WIN
        const QString packageDirectory = prefix.path()
            + QStringLiteral("/node_modules/opencode-ai");
#else
        const QString packageDirectory = prefix.path()
            + QStringLiteral("/lib/node_modules/opencode-ai");
#endif
        if (!QDir().mkpath(packageDirectory)) {
            std::cerr << "failed to create npm package fixture\n";
            return 1;
        }
        QFile packageFile(packageDirectory + QStringLiteral("/package.json"));
        if (!packageFile.open(QIODevice::WriteOnly)
                || packageFile.write(
                    "{\"name\":\"opencode-ai\",\"version\":\"9.8.7\"}") < 0) {
            std::cerr << "failed to write npm package fixture\n";
            return 1;
        }
        packageFile.close();

        const bool hadPrefix = qEnvironmentVariableIsSet("npm_config_prefix");
        const QByteArray previousPrefix = qgetenv("npm_config_prefix");
        qputenv("npm_config_prefix", prefix.path().toUtf8());

        // Cold npm on a loaded CI runner can take far longer than the
        // interactive fast-detect budget to answer `npm list -g`.
        const ToolStatus damaged = manager.detectFast(AiTool::OpenCode, 20000);
        if (damaged.installed || !damaged.repairRequired
                || damaged.version != QStringLiteral("9.8.7")
                || damaged.installationIssue.isEmpty()) {
            std::cerr << "npm package residue was not classified as repairable damage\n";
            return 1;
        }

        bool asyncInstalled = true;
        QString asyncVersion;
        QEventLoop loop;
        QObject::connect(&manager, &ToolManager::toolVersionDetected, &loop,
            [&loop, &asyncInstalled, &asyncVersion](AiTool tool, bool installed,
                                                   const QString &version) {
                if (tool != AiTool::OpenCode) return;
                asyncInstalled = installed;
                asyncVersion = version;
                loop.quit();
            });
        QTimer::singleShot(6000, &loop, &QEventLoop::quit);
        manager.detectVersion(AiTool::OpenCode);
        loop.exec();

        if (hadPrefix) qputenv("npm_config_prefix", previousPrefix);
        else qunsetenv("npm_config_prefix");

        if (asyncInstalled || asyncVersion != QStringLiteral("9.8.7")) {
            std::cerr << "async CLI detection hid the repairable npm residue\n";
            return 1;
        }
    }
    return 0;
}
