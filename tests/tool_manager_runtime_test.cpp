#include "tool_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QSet>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>

#include <iostream>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
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

        const ToolStatus damaged = manager.detectFast(AiTool::OpenCode);
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
