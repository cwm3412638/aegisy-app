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
    const QString codexConfigPath =
        home.path() + QStringLiteral("/.codex/config.toml");
    if (!QDir().mkpath(home.path() + QStringLiteral("/.codex"))) return 1;
    QFile staleConfig(codexConfigPath);
    if (!staleConfig.open(QIODevice::WriteOnly)
            || staleConfig.write(
                "[projects.\"/tmp/aegisy-project\"]\n"
                "trust_level = \"trusted\"\n"
                "\n"
                "[tui.model_availability_nux]\n"
                "\"gpt-existing\" = 1\n"
                "\n"
                "model_provider = \"OpenAI\"\n"
                "model = \"stale-model\"\n"
                "review_model = \"stale-model\"\n"
                "model_reasoning_effort = \"xhigh\"\n"
                "disable_response_storage = true\n"
                "network_access = \"enabled\"\n"
                "windows_wsl_setup_acknowledged = true\n"
                "\n"
                "[model_providers.ccswitch]\n"
                "name = \"CC Switch\"\n"
                "base_url = \"https://api.example.com/v1\"\n"
                "wire_api = \"responses\"\n"
                "experimental_bearer_token = \"sk-ccswitch-test\"\n"
                "requires_openai_auth = true\n"
                "\n"
                "[model_providers.OpenAI]\n"
                "name = \"OpenAI\"\n"
                "base_url = \"https://aegisy.cc\"\n"
                "wire_api = \"responses\"\n"
                "requires_openai_auth = true\n"
                "\n"
                "[features]\n"
                "shell_snapshot = true\n"
                "goals = false\n") < 0) {
        return 1;
    }
    staleConfig.close();

    const QString localToken = QStringLiteral("aegisy-local-test-token");
    if (!require(manager.configureGateway(
                     AiTool::CodexCli, localToken, QStringLiteral("gpt-test"), 43112),
                 "failed to write Codex gateway configuration")) {
        std::cerr << manager.lastError().toStdString() << '\n';
        return 1;
    }
    const QString codexConfig = readFile(codexConfigPath);
    const QString codexAuth = readFile(home.path() + QStringLiteral("/.codex/auth.json"));
    const qsizetype rootProvider = codexConfig.indexOf(
        QStringLiteral("model_provider = \"aegisy_local\""));
    const qsizetype firstTable = codexConfig.indexOf(QLatin1Char('['));
    if (!require(rootProvider >= 0 && rootProvider < firstTable,
                 "Codex managed root keys were written inside a TOML table")
        || !require(codexConfig.count(QStringLiteral("model_provider =")) == 1,
                    "stale Codex model_provider key was not removed from a TOML table")
        || !require(codexConfig.contains(QStringLiteral("127.0.0.1:43112/tools/codex/v1")),
                 "Codex gateway endpoint is missing")
        || !require(codexConfig.contains(QStringLiteral(
                        "[projects.\"/tmp/aegisy-project\"]\ntrust_level = \"trusted\"")),
                    "Codex project trust configuration was not preserved")
        || !require(codexConfig.contains(QStringLiteral(
                        "[tui.model_availability_nux]\n\"gpt-existing\" = 1")),
                    "Codex model availability state was not preserved")
        || !require(codexConfig.contains(QStringLiteral(
                        "[model_providers.ccswitch]\n"
                        "name = \"CC Switch\"\n"
                        "base_url = \"https://api.example.com/v1\"")),
                    "CC Switch provider configuration was not preserved")
        || !require(codexConfig.contains(QStringLiteral(
                        "experimental_bearer_token = \"sk-ccswitch-test\"")),
                    "CC Switch provider credential was not preserved")
        || !require(codexConfig.contains(QStringLiteral("shell_snapshot = true")),
                    "non-Aegisy Codex feature flags were not preserved")
        || !require(codexConfig.contains(QStringLiteral("goals = true"))
                        && !codexConfig.contains(QStringLiteral("goals = false")),
                    "Aegisy-managed Codex goal setting was not updated")
        || !require(codexAuth.contains(localToken), "Codex local token is missing")) {
        return 1;
    }

    const LocalConfigurationStatus gatewayStatus =
        manager.inspectConfiguration(AiTool::CodexCli);
    if (!require(gatewayStatus.isReady(),
                 "valid Codex gateway configuration was not recognized")
        || !require(gatewayStatus.gatewayMode,
                    "Codex gateway configuration was classified as direct")
        || !require(gatewayStatus.keyHint == QStringLiteral("oken"),
                    "Codex configured credential hint is incorrect")) {
        return 1;
    }

    const QString codexAuthPath = home.path() + QStringLiteral("/.codex/auth.json");
    if (!QFile::remove(codexAuthPath)) return 1;
    const LocalConfigurationStatus missingAuth =
        manager.inspectConfiguration(AiTool::CodexCli);
    if (!require(missingAuth.state == LocalConfigurationState::Missing,
                 "deleted Codex auth.json was not classified as missing")
        || !require(!missingAuth.detail.isEmpty(),
                    "missing Codex auth.json did not include a repair reason")
        || !require(manager.configureGateway(
                        AiTool::CodexCli, localToken, QStringLiteral("gpt-test"), 43112),
                    "failed to repair deleted Codex auth.json")) {
        return 1;
    }

    QFile brokenConfig(codexConfigPath);
    if (!brokenConfig.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || brokenConfig.write(
                "[tui.model_availability_nux]\n"
                "\"gpt-existing\" = 1\n"
                "model_provider = \"aegisy_local\"\n"
                "model = \"gpt-test\"\n"
                "\n"
                "[model_providers.aegisy_local]\n"
                "base_url = \"http://127.0.0.1:43112/tools/codex/v1\"\n"
                "\n"
                "[model_providers.ccswitch]\n"
                "name = \"CC Switch\"\n"
                "base_url = \"https://api.example.com/v1\"\n"
                "wire_api = \"responses\"\n"
                "experimental_bearer_token = \"sk-ccswitch-test\"\n"
                "requires_openai_auth = true\n"
                "\n"
                "[features]\n"
                "shell_snapshot = true\n"
                "goals = false\n") < 0) {
        return 1;
    }
    brokenConfig.close();
    const LocalConfigurationStatus brokenToml =
        manager.inspectConfiguration(AiTool::CodexCli);
    if (!require(brokenToml.state == LocalConfigurationState::Invalid,
                 "misplaced Codex TOML root keys were not classified as invalid")
        || !require(!brokenToml.detail.isEmpty(),
                    "invalid Codex config.toml did not include a repair reason")) {
        return 1;
    }

    if (!require(manager.configureGateway(AiTool::ClaudeCode, localToken),
                 "failed to write Claude gateway configuration")
        || !require(readFile(home.path() + QStringLiteral("/.claude/settings.json"))
                        .contains(QStringLiteral("127.0.0.1:43112/tools/claude")),
                    "Claude gateway endpoint is missing")
        || !require(manager.inspectConfiguration(AiTool::ClaudeCode).isReady()
                        && manager.inspectConfiguration(AiTool::ClaudeCode).gatewayMode,
                    "valid Claude gateway configuration was not recognized")) {
        return 1;
    }

    if (!require(manager.configureGateway(
                     AiTool::GeminiCli, localToken, QStringLiteral("gemini-test")),
                 "failed to write Gemini gateway configuration")
        || !require(readFile(home.path() + QStringLiteral("/.gemini/.env"))
                        .contains(QStringLiteral("127.0.0.1:43112/tools/gemini")),
                    "Gemini gateway endpoint is missing")
        || !require(manager.inspectConfiguration(AiTool::GeminiCli).isReady()
                        && manager.inspectConfiguration(AiTool::GeminiCli).gatewayMode,
                    "valid Gemini gateway configuration was not recognized")) {
        return 1;
    }

    const QString directKey = QStringLiteral("sk-direct-test");
    if (!require(manager.configure(AiTool::CodexCli, directKey, QStringLiteral("gpt-test")),
                 "failed to restore direct Codex configuration")) {
        return 1;
    }
    const QString restoredConfig = readFile(codexConfigPath);
    const QString restoredAuth = readFile(home.path() + QStringLiteral("/.codex/auth.json"));
    const qsizetype restoredRootProvider = restoredConfig.indexOf(
        QStringLiteral("model_provider = \"OpenAI\""));
    const qsizetype restoredFirstTable = restoredConfig.indexOf(QLatin1Char('['));
    const LocalConfigurationStatus restoredStatus =
        manager.inspectConfiguration(AiTool::CodexCli);
    if (!require(restoredRootProvider >= 0 && restoredRootProvider < restoredFirstTable,
                 "direct Codex root keys were written inside a TOML table")
        || !require(restoredConfig.contains(QStringLiteral("https://aegisy.cc")),
                 "direct Aegisy endpoint was not restored")
        || !require(!restoredConfig.contains(QStringLiteral("127.0.0.1:43112")),
                    "gateway endpoint remained after direct restore")
        || !require(restoredConfig.contains(QStringLiteral("[model_providers.ccswitch]"))
                        && restoredConfig.contains(QStringLiteral(
                            "experimental_bearer_token = \"sk-ccswitch-test\"")),
                    "CC Switch provider was lost after repeated activation")
        || !require(restoredConfig.contains(QStringLiteral("shell_snapshot = true")),
                    "Codex feature flags were lost after repeated activation")
        || !require(restoredStatus.isReady() && !restoredStatus.gatewayMode,
                    "repaired direct Codex configuration was not recognized")
        || !require(restoredStatus.keyHint == QStringLiteral("test"),
                    "repaired direct Codex credential hint is incorrect")
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
                    "failed to write direct Gemini configuration")
        || !require(manager.inspectConfiguration(AiTool::ClaudeCode).isReady()
                        && !manager.inspectConfiguration(AiTool::ClaudeCode).gatewayMode,
                    "valid direct Claude configuration was not recognized")
        || !require(manager.inspectConfiguration(AiTool::GeminiCli).isReady()
                        && !manager.inspectConfiguration(AiTool::GeminiCli).gatewayMode,
                    "valid direct Gemini configuration was not recognized")) {
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
