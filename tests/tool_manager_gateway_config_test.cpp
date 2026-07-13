#include "tool_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <iostream>

namespace {

QString readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

} // namespace

int main(int argc, char *argv[])
{
    QTemporaryDir home;
    if (!home.isValid()) return 1;
    qputenv("HOME", home.path().toUtf8());
    qputenv("USERPROFILE", home.path().toUtf8());

    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AegisyTest"));
    QCoreApplication::setApplicationName(QStringLiteral("GatewayConfig"));

    ToolManager manager;
    const QString localToken = QStringLiteral("aegisy-local-test-token");
    if (!require(manager.configureGateway(
                     AiTool::CodexCli, localToken, QStringLiteral("gpt-test"), 43112),
                 "failed to write Codex gateway configuration")) {
        return 1;
    }
    const QString codexConfig = readFile(home.path() + QStringLiteral("/.codex/config.toml"));
    const QString codexAuth = readFile(home.path() + QStringLiteral("/.codex/auth.json"));
    if (!require(codexConfig.contains(QStringLiteral("127.0.0.1:43112/tools/codex/v1")),
                 "Codex gateway endpoint is missing")
        || !require(codexAuth.contains(localToken), "Codex local token is missing")) {
        return 1;
    }

    if (!require(manager.configureGateway(AiTool::ClaudeCode, localToken),
                 "failed to write Claude gateway configuration")
        || !require(readFile(home.path() + QStringLiteral("/.claude/settings.json"))
                        .contains(QStringLiteral("127.0.0.1:43112/tools/claude")),
                    "Claude gateway endpoint is missing")) {
        return 1;
    }

    if (!require(manager.configureGateway(
                     AiTool::GeminiCli, localToken, QStringLiteral("gemini-test")),
                 "failed to write Gemini gateway configuration")
        || !require(readFile(home.path() + QStringLiteral("/.gemini/.env"))
                        .contains(QStringLiteral("127.0.0.1:43112/tools/gemini")),
                    "Gemini gateway endpoint is missing")) {
        return 1;
    }

    const QString directKey = QStringLiteral("sk-direct-test");
    if (!require(manager.configure(AiTool::CodexCli, directKey, QStringLiteral("gpt-test")),
                 "failed to restore direct Codex configuration")) {
        return 1;
    }
    const QString restoredConfig = readFile(home.path() + QStringLiteral("/.codex/config.toml"));
    const QString restoredAuth = readFile(home.path() + QStringLiteral("/.codex/auth.json"));
    if (!require(restoredConfig.contains(QStringLiteral("https://aegisy.cc")),
                 "direct Aegisy endpoint was not restored")
        || !require(!restoredConfig.contains(QStringLiteral("127.0.0.1:43112")),
                    "gateway endpoint remained after direct restore")
        || !require(restoredAuth.contains(directKey), "direct API key was not restored")) {
        return 1;
    }
    return 0;
}
