#include "tool_manager.h"

#include <QCoreApplication>
#include <QSet>
#include <QString>

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
    return 0;
}
