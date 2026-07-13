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

    qputenv("OPENAI_API_KEY", "sk-stale-openai");
    qputenv("ANTHROPIC_AUTH_TOKEN", "sk-stale-anthropic");
    qputenv("ANTHROPIC_BASE_URL", "https://old.example.com");
    qputenv("GEMINI_API_KEY", "sk-stale-gemini");
    qputenv("GOOGLE_API_KEY", "sk-stale-google");

    const QProcessEnvironment codexEnvironment = manager.launchEnvironment(AiTool::CodexCli);
    if (!require(!codexEnvironment.contains(QStringLiteral("OPENAI_API_KEY")),
                 "Codex launch environment retained stale key")
        || !require(!codexEnvironment.contains(QStringLiteral("ANTHROPIC_AUTH_TOKEN")),
                    "Codex launch environment leaked Anthropic key")
        || !require(codexEnvironment.value(QStringLiteral("TERM"))
                        == QStringLiteral("xterm-256color"),
                    "interactive terminal type was not configured")) {
        return 1;
    }

    const QString claudeKey = QStringLiteral("sk-current-claude");
    const QString geminiKey = QStringLiteral("sk-current-gemini");
    if (!require(manager.configure(AiTool::ClaudeCode, claudeKey),
                 "failed to write direct Claude configuration")
        || !require(manager.configure(AiTool::GeminiCli, geminiKey),
                    "failed to write direct Gemini configuration")) {
        return 1;
    }
    const QProcessEnvironment claudeEnvironment = manager.launchEnvironment(AiTool::ClaudeCode);
    const QProcessEnvironment geminiEnvironment = manager.launchEnvironment(AiTool::GeminiCli);
    if (!require(!claudeEnvironment.contains(QStringLiteral("ANTHROPIC_AUTH_TOKEN")),
                 "Claude launch environment retained stale key")
        || !require(!claudeEnvironment.contains(QStringLiteral("ANTHROPIC_BASE_URL")),
                    "Claude launch environment retained stale base URL")
        || !require(!geminiEnvironment.contains(QStringLiteral("GEMINI_API_KEY")),
                    "Gemini launch environment retained stale key")
        || !require(!geminiEnvironment.contains(QStringLiteral("GOOGLE_API_KEY")),
                    "Gemini launch environment retained GOOGLE_API_KEY")) {
        return 1;
    }
    return 0;
}
