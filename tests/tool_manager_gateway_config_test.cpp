#include "tool_manager.h"
#include "companion_activation_journal.h"
#include "credential_metadata.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>

#include <iostream>

namespace {

QString readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

// 内存授权信封替身：这个测试验证事务编排，不依赖平台钥匙串可用性。
class MemoryJournalSecureStore final : public CompanionActivationJournalSecureStore
{
public:
    ReadState readFresh(QByteArray *value, QString *errorCode) override
    {
        if (errorCode) errorCode->clear();
        if (!value) return ReadState::Invalid;
        *value = m_value;
        return m_present ? ReadState::Found : ReadState::Missing;
    }

    WriteOutcome write(const QByteArray &value, QString *errorCode) override
    {
        if (errorCode) errorCode->clear();
        m_value = value;
        m_present = true;
        return WriteOutcome::Committed;
    }

private:
    bool m_present = false;
    QByteArray m_value;
};

bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

QByteArray recursiveBytes(const QString &root)
{
    QByteArray result;
    const QFileInfoList entries = QDir(root).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (entry.isDir()) result += recursiveBytes(entry.filePath());
        else {
            QFile file(entry.filePath());
            if (file.open(QIODevice::ReadOnly)) result += file.readAll();
        }
    }
    return result;
}

class FakeBackupKeyProvider final : public ConfigurationBackupKeyProvider
{
public:
    bool keyForScope(const QString &scope, bool allowCreate,
                     QByteArray *key, QString *error) override
    {
        if (failAll || (allowCreate && failCreate)) {
            if (error) *error = QStringLiteral("fake-backup-key-unavailable");
            return false;
        }
        if (!keys.contains(scope)) {
            if (!allowCreate) {
                if (error) *error = QStringLiteral("fake-backup-key-missing");
                return false;
            }
            keys.insert(scope, QByteArray(32, static_cast<char>(0x41 + keys.size())));
        }
        *key = keys.value(scope);
        return true;
    }

    QHash<QString, QByteArray> keys;
    bool failAll = false;
    bool failCreate = false;
};

class TemporaryHome
{
public:
    TemporaryHome()
    {
        QByteArray temporaryRoot = qgetenv("TMPDIR");
        if (temporaryRoot.isEmpty()) temporaryRoot = qgetenv("TEMP");
        if (temporaryRoot.isEmpty()) temporaryRoot = qgetenv("TMP");
        if (temporaryRoot.isEmpty()) temporaryRoot = QByteArrayLiteral(".");
        m_path = QString::fromLocal8Bit(temporaryRoot)
            + QStringLiteral("/aegisy-gateway-config-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        qputenv("HOME", m_path.toUtf8());
        qputenv("USERPROFILE", m_path.toUtf8());
        qputenv("AEGISY_CONFIG_HOME", m_path.toUtf8());
        m_valid = QDir().mkpath(m_path);
    }

    ~TemporaryHome() { QDir(m_path).removeRecursively(); }

    bool isValid() const { return m_valid; }
    QString path() const { return m_path; }

private:
    QString m_path;
    bool m_valid = false;
};

} // namespace

int main(int argc, char *argv[])
{
    // Set HOME before constructing QTemporaryDir or QCoreApplication. Qt 5 on
    // Windows may otherwise cache the real user home while resolving TempLocation.
    TemporaryHome home;
    if (!home.isValid()) return 1;

    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AegisyTest"));
    QCoreApplication::setApplicationName(QStringLiteral("GatewayConfig"));

    FakeBackupKeyProvider backupKeys;
    const QString backupRoot = home.path() + QStringLiteral("/encrypted-backups");
    ToolManager manager(nullptr, &backupKeys, backupRoot);
    const QStringList managedPaths = manager.configurationFiles(AiTool::CodexCli);
    if (managedPaths.isEmpty()
            || !QDir::cleanPath(managedPaths.first()).startsWith(
                QDir::cleanPath(home.path()))) {
        std::cerr << "temporary configuration home isolation was not applied\n";
        return 1;
    }
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
                "model_context_window = 372000\n"
                "model_auto_compact_token_limit = 372000\n"
                "disable_response_storage = true\n"
                "network_access = \"enabled\"\n"
                "windows_wsl_setup_acknowledged = true\n"
                "web_search = \"cached\"\n"
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
                "web_search_request = false\n"
                "goals = false\n") < 0) {
        return 1;
    }
    staleConfig.close();

    const QString localToken = QStringLiteral("aegisy-local-test-token");
    QString initialApplyBackupId;
    if (!require(manager.configureGateway(
                     AiTool::CodexCli, localToken, QStringLiteral("gpt-test"), 43112,
                     &initialApplyBackupId),
                 "failed to write Codex gateway configuration")) {
        std::cerr << manager.lastError().toStdString() << '\n';
        return 1;
    }
    if (!require(!initialApplyBackupId.isEmpty(),
                 "verified configuration did not return a rollback backup ID")) {
        return 1;
    }
    const QByteArray backupBytes = recursiveBytes(backupRoot);
    const ConfigBackupInventory initialInventory =
        manager.backupInventory(AiTool::CodexCli);
    if (!require(initialInventory.state == ConfigBackupSubsystemState::Ready
                    && initialInventory.backups.size() == 1,
                 "encrypted backup inventory was not ready")
            || !require(!backupBytes.contains(localToken.toUtf8())
                        && !backupBytes.contains("sk-ccswitch-test")
                        && !backupBytes.contains(home.path().toUtf8()),
                        "backup storage retained credential or HOME plaintext")) {
        return 1;
    }
    const QString codexConfig = readFile(codexConfigPath);
    const QString codexAuth = readFile(home.path() + QStringLiteral("/.codex/auth.json"));
    const qsizetype rootProvider = codexConfig.indexOf(
        QStringLiteral("model_provider = \"aegisy_local\""));
    const qsizetype firstTable = codexConfig.indexOf(QLatin1Char('['));
    if (rootProvider < 0 || rootProvider >= firstTable) {
        std::cerr << "Generated Codex config:\n"
                  << codexConfig.toStdString() << '\n';
    }
    if (!require(rootProvider >= 0 && rootProvider < firstTable,
                 "Codex managed root keys were written inside a TOML table")
        || !require(codexConfig.count(QStringLiteral("model_provider =")) == 1,
                    "stale Codex model_provider key was not removed from a TOML table")
        || !require(codexConfig.count(QStringLiteral("model_context_window = 272000")) == 1
                        && codexConfig.count(QStringLiteral(
                            "model_auto_compact_token_limit = 272000")) == 1,
                    "Codex 272K context limits were not written exactly once")
        || !require(!codexConfig.contains(QStringLiteral("372000")),
                    "stale Codex context limits were preserved")
        || !require(codexConfig.indexOf(QStringLiteral("model_context_window = 272000"))
                        < firstTable
                        && codexConfig.indexOf(QStringLiteral(
                            "model_auto_compact_token_limit = 272000")) < firstTable,
                    "Codex context limits were written inside a TOML table")
        || !require(codexConfig.contains(QStringLiteral("127.0.0.1:43112/tools/codex/v1")),
                 "Codex gateway endpoint is missing")
        || !require(codexConfig.contains(QStringLiteral("web_search = \"live\""))
                        && !codexConfig.contains(QStringLiteral("web_search_request")),
                    "Codex live web search was not enabled cleanly")
        || !require(!codexConfig.contains(QStringLiteral("disable_response_storage"))
                        && !codexConfig.contains(QStringLiteral("network_access"))
                        && !codexConfig.contains(QStringLiteral(
                            "windows_wsl_setup_acknowledged")),
                    "obsolete Codex configuration fields were not removed")
        || !require(codexConfig.contains(QStringLiteral(
                        "requires_openai_auth = false\n"
                        "request_max_retries = 4\n"
                        "stream_max_retries = 5\n"
                        "stream_idle_timeout_ms = 600000\n"
                        "supports_websockets = false\n"
                        "experimental_bearer_token = \"aegisy-local-test-token\"\n"
                        "http_headers = { \"x-openai-actor-authorization\" = \"aegisy\", "
                        "\"accept-encoding\" = \"identity\" }")),
                    "Codex third-party capability compatibility fields are missing")
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
        || !require(gatewayStatus.keyHint
                        == credentialFingerprint(localToken),
                    "Codex configured credential hint is incorrect")) {
        return 1;
    }

    const int backupsBeforeInvalidPlan =
        manager.backupInventory(AiTool::CodexCli).backups.size();
    ConfigurationApplyReceipt invalidPlan;
    if (!require(!manager.prepareConfigurationApply(
                     AiTool::CodexCli, false, QString(),
                     QStringLiteral("gpt-prepared"), &invalidPlan),
                 "empty credential produced a configuration candidate")
            || !require(!manager.prepareConfigurationApply(
                            AiTool::CodexCli, true, localToken,
                            QStringLiteral("gpt-prepared"), &invalidPlan, 0),
                        "invalid gateway port produced a configuration candidate")
            || !require(manager.backupInventory(AiTool::CodexCli).backups.size()
                            == backupsBeforeInvalidPlan,
                        "invalid candidate planning created a backup")) {
        return 1;
    }

    ConfigurationApplyReceipt prepared;
    const QString preparedAuthPath =
        home.path() + QStringLiteral("/.codex/auth.json");
    const QByteArray beforePreparedConfig = readFile(codexConfigPath).toUtf8();
    const QByteArray beforePreparedAuth = readFile(preparedAuthPath).toUtf8();
    if (!require(manager.prepareConfigurationApply(
                     AiTool::CodexCli, false,
                     QStringLiteral("prepared-direct-key"),
                     QStringLiteral("gpt-prepared"), &prepared),
                 "configuration apply receipt preparation failed")
            || !require(prepared.isPrepared()
                            && !prepared.backupManifestIdentity.isEmpty()
                            && !prepared.candidateFilesIdentity.isEmpty(),
                        "prepared receipt lacks authenticated candidate identities")
            || !require(readFile(codexConfigPath).toUtf8() == beforePreparedConfig
                            && readFile(preparedAuthPath).toUtf8()
                                == beforePreparedAuth,
                        "candidate planning changed configuration files")) {
        return 1;
    }
    QSettings activationSettings(
        home.path() + QStringLiteral("/activation-journal.ini"), QSettings::IniFormat);
    MemoryJournalSecureStore activationAuthority;
    CompanionActivationJournal activationJournal(&activationAuthority,
                                                 &activationSettings);
    CompanionActivationRecord activationRecord;
    activationRecord.transactionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    activationRecord.candidateProfileId =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    activationRecord.candidateProfileIdentity = QStringLiteral(
        "profile-activation:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    activationRecord.receipt = prepared;
    QString activationError;
    if (!require(activationJournal.create(activationRecord, &activationError),
                 "prepared receipt was not durably journaled before apply")
            || !require(!readFile(activationSettings.fileName()).contains(
                            QStringLiteral("prepared-direct-key")),
                        "activation journal persisted the transient credential")) {
        return 1;
    }
    ConfigurationApplyReceipt tampered = prepared;
    tampered.backupManifestIdentity.replace(
        QStringLiteral("sha256:"), QStringLiteral("sha256:0"));
    if (!require(!manager.applyPreparedConfiguration(
                     &tampered, QStringLiteral("prepared-direct-key"),
                     QStringLiteral("gpt-prepared")),
                 "tampered prepared receipt was applied")
            || !require(readFile(home.path() + QStringLiteral("/.codex/auth.json"))
                            .contains(localToken),
                        "tampered receipt changed the existing configuration")) {
        return 1;
    }
    ConfigurationApplyReceipt tamperedCandidate = prepared;
    tamperedCandidate.candidateFilesIdentity = QStringLiteral(
        "configuration-files:sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
    if (!require(!manager.applyPreparedConfiguration(
                     &tamperedCandidate, QStringLiteral("prepared-direct-key"),
                     QStringLiteral("gpt-prepared")),
                 "tampered candidate identity was applied")
            || !require(readFile(codexConfigPath).toUtf8() == beforePreparedConfig
                            && readFile(preparedAuthPath).toUtf8()
                                == beforePreparedAuth,
                        "tampered candidate identity changed configuration files")) {
        return 1;
    }
    if (!require(manager.applyPreparedConfiguration(
                     &prepared, QStringLiteral("prepared-direct-key"),
                     QStringLiteral("gpt-prepared")),
                 "prepared configuration apply failed")
            || !require(prepared.appliedFilesIdentity
                            == prepared.candidateFilesIdentity,
                        "applied files differ from the predeclared candidate")
            || !require(manager.rollbackPreparedConfiguration(
                            CompanionActivationJournal(&activationAuthority,
                                                       &activationSettings)
                                .load().record.receipt),
                        "prepared-stage receipt could not recover an applied candidate")
            || !require(manager.inspectConfiguration(AiTool::CodexCli).gatewayMode,
                        "prepared-stage recovery did not restore the preimage")
            || !require(manager.applyPreparedConfiguration(
                            &activationRecord.receipt,
                            QStringLiteral("prepared-direct-key"),
                            QStringLiteral("gpt-prepared")),
                        "prepared candidate could not be reapplied after crash recovery")
            || !require(activationJournal.advance(
                            activationJournal.load().record.identity,
                            CompanionActivationStage::FilesApplied,
                            activationRecord.receipt,
                            &activationRecord, &activationError),
                        "files-applied receipt was not durably journaled")
            || !require(CompanionActivationJournal(&activationAuthority,
                                                   &activationSettings)
                            .load().state
                            == CompanionActivationJournalState::Ready,
                        "journal did not survive simulated restart")
            || !require(manager.rollbackPreparedConfiguration(
                            CompanionActivationJournal(&activationAuthority,
                                                       &activationSettings)
                                .load().record.receipt),
                        "receipt-bound rollback failed")
            || !require(manager.inspectConfiguration(AiTool::CodexCli).gatewayMode
                            && readFile(home.path() + QStringLiteral("/.codex/auth.json"))
                                .contains(localToken),
                        "receipt rollback did not restore the exact gateway preimage")
            || !require(activationJournal.clear(
                            activationRecord.identity, &activationError),
                        "recovered activation journal did not clear")
            || !require(!manager.finalizePreparedConfiguration(tampered),
                        "tampered receipt was finalized")
            || !require(manager.finalizePreparedConfiguration(activationRecord.receipt),
                        "prepared receipt finalize failed")) {
        return 1;
    }

    QFile missingHeaderConfig(codexConfigPath);
    QString missingHeader = codexConfig;
    missingHeader.remove(QStringLiteral(
        "http_headers = { \"x-openai-actor-authorization\" = \"aegisy\", "
        "\"accept-encoding\" = \"identity\" }\n"));
    if (!missingHeaderConfig.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || missingHeaderConfig.write(missingHeader.toUtf8()) < 0) {
        return 1;
    }
    missingHeaderConfig.close();
    const LocalConfigurationStatus missingHeaderStatus =
        manager.inspectConfiguration(AiTool::CodexCli);
    if (!require(missingHeaderStatus.state == LocalConfigurationState::Invalid,
                 "missing Codex capability header was not classified as invalid")
        || !require(manager.configureGateway(
                        AiTool::CodexCli, localToken, QStringLiteral("gpt-test"), 43112),
                    "failed to repair missing Codex capability header")) {
        return 1;
    }

    QFile wrongContextConfig(codexConfigPath);
    QString wrongContext = codexConfig;
    wrongContext.replace(QStringLiteral("model_context_window = 272000"),
                         QStringLiteral("model_context_window = 372000"));
    if (!wrongContextConfig.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || wrongContextConfig.write(wrongContext.toUtf8()) < 0) {
        return 1;
    }
    wrongContextConfig.close();
    const LocalConfigurationStatus wrongContextStatus =
        manager.inspectConfiguration(AiTool::CodexCli);
    if (!require(wrongContextStatus.state == LocalConfigurationState::Invalid,
                 "stale Codex context limit was not classified as invalid")
        || !require(wrongContextStatus.detail.contains(QStringLiteral("272000")),
                    "stale Codex context limit did not include a repair reason")
        || !require(manager.configureGateway(
                        AiTool::CodexCli, localToken, QStringLiteral("gpt-test"), 43112),
                    "failed to repair stale Codex context limits")) {
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
                "model_context_window = 372000\n"
                "model_auto_compact_token_limit = 372000\n"
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

    if (!require(manager.configureGateway(
                     AiTool::OpenCode, localToken,
                     QStringLiteral("anthropic/claude-sonnet-4-5")),
                 "failed to write OpenCode gateway configuration")
        || !require(readFile(home.path()
                        + QStringLiteral("/.config/opencode/config.json"))
                        .contains(QStringLiteral("127.0.0.1:43112/tools/opencode")),
                    "OpenCode gateway endpoint is missing")
        || !require(manager.inspectConfiguration(AiTool::OpenCode).isReady()
                        && manager.inspectConfiguration(AiTool::OpenCode).gatewayMode,
                    "valid OpenCode gateway configuration was not recognized")) {
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
        QStringLiteral("model_provider = \"aegisy\""));
    const qsizetype restoredFirstTable = restoredConfig.indexOf(QLatin1Char('['));
    const LocalConfigurationStatus restoredStatus =
        manager.inspectConfiguration(AiTool::CodexCli);
    if (!require(restoredRootProvider >= 0 && restoredRootProvider < restoredFirstTable,
                 "direct Codex root keys were written inside a TOML table")
        || !require(restoredConfig.contains(QStringLiteral("https://aegisy.cc")),
                 "direct Aegisy endpoint was not restored")
        || !require(!restoredConfig.contains(QStringLiteral("127.0.0.1:43112")),
                    "gateway endpoint remained after direct restore")
        || !require(restoredConfig.contains(QStringLiteral(
                        "model_context_window = 272000"))
                        && restoredConfig.contains(QStringLiteral(
                            "model_auto_compact_token_limit = 272000")),
                    "direct Codex configuration omitted the 272K context limits")
        || !require(restoredConfig.contains(QStringLiteral(
                        "requires_openai_auth = false\n"
                        "request_max_retries = 4\n"
                        "stream_max_retries = 5\n"
                        "stream_idle_timeout_ms = 600000\n"
                        "supports_websockets = false\n"
                        "experimental_bearer_token = \"sk-direct-test\"\n"
                        "http_headers = { \"x-openai-actor-authorization\" = \"aegisy\", "
                        "\"accept-encoding\" = \"identity\" }")),
                    "direct Codex compatibility fields are missing")
        || !require(restoredConfig.contains(QStringLiteral("[model_providers.ccswitch]"))
                        && restoredConfig.contains(QStringLiteral(
                            "experimental_bearer_token = \"sk-ccswitch-test\"")),
                    "CC Switch provider was lost after repeated activation")
        || !require(restoredConfig.contains(QStringLiteral("shell_snapshot = true")),
                    "Codex feature flags were lost after repeated activation")
        || !require(restoredStatus.isReady() && !restoredStatus.gatewayMode,
                    "repaired direct Codex configuration was not recognized")
        || !require(restoredStatus.keyHint
                        == credentialFingerprint(QStringLiteral("sk-direct-test")),
                    "repaired direct Codex credential hint is incorrect")
        || !require(restoredAuth.contains(directKey), "direct API key was not restored")) {
        return 1;
    }

    if (!require(manager.configure(
                     AiTool::CodexCli, directKey, QStringLiteral("gpt-5.6-sol")),
                 "failed to configure GPT-5.6 Sol compatibility profile")) {
        return 1;
    }
    const QString gpt56Config = readFile(codexConfigPath);
    if (!require(gpt56Config.contains(QStringLiteral(
                        "model_reasoning_effort = \"high\"")),
                 "GPT-5.6 Sol reasoning effort was not set to high")
        || !require(gpt56Config.contains(QStringLiteral(
                        "model_context_window = 372000"))
                        && gpt56Config.contains(QStringLiteral(
                            "model_auto_compact_token_limit = 372000")),
                    "GPT-5.6 Sol did not receive the 372K client context threshold")
        || !require(manager.inspectConfiguration(AiTool::CodexCli).isReady(),
                    "GPT-5.6 Sol compatibility configuration was not recognized")
        || !require(ToolManager::configuredContextLimit(
                        AiTool::CodexCli, QStringLiteral("gpt-5.6")) == 372000,
                    "GPT-5.6 alias did not use the compatibility context threshold")
        || !require(ToolManager::configuredReasoning(
                        AiTool::CodexCli, QStringLiteral("gpt-5.6"))
                        == QStringLiteral("high"),
                    "GPT-5.6 alias did not use high reasoning")) {
        return 1;
    }

    const ConfigBackupInventory beforeRoundTrip =
        manager.backupInventory(AiTool::CodexCli);
    if (!require(beforeRoundTrip.state == ConfigBackupSubsystemState::Ready
                    && !beforeRoundTrip.backups.isEmpty(),
                 "round-trip backup inventory unavailable")) {
        return 1;
    }
    const QString restoreId = beforeRoundTrip.backups.first().id;
    const QByteArray beforeSafetyConfig = readFile(codexConfigPath).toUtf8();
    const QByteArray beforeSafetyAuth = readFile(codexAuthPath).toUtf8();
    backupKeys.failCreate = true;
    const bool unsafeRestore = manager.restoreBackup(restoreId, AiTool::CodexCli);
    backupKeys.failCreate = false;
    if (!require(!unsafeRestore,
                 "restore proceeded after safety backup key failure")
            || !require(readFile(codexConfigPath).toUtf8() == beforeSafetyConfig
                        && readFile(codexAuthPath).toUtf8() == beforeSafetyAuth,
                        "safety backup failure changed current configuration")) {
        return 1;
    }

    const QString selectedDirectory = backupRoot + QStringLiteral("/codex/") + restoreId;
    if (!writeFile(selectedDirectory + QStringLiteral("/unexpected.bin"),
                   QByteArrayLiteral("unexpected"))) {
        return 1;
    }
    if (!require(!manager.restoreBackup(restoreId, AiTool::CodexCli),
                 "invalid target inventory was restored")
            || !require(readFile(codexConfigPath).toUtf8() == beforeSafetyConfig
                        && readFile(codexAuthPath).toUtf8() == beforeSafetyAuth,
                        "target prevalidation failure changed current configuration")) {
        return 1;
    }
    if (!QFile::remove(selectedDirectory + QStringLiteral("/unexpected.bin"))) return 1;

    if (!require(manager.restoreBackup(restoreId, AiTool::CodexCli),
                 "encrypted backup round-trip restore failed")
            || !require(readFile(codexAuthPath).contains(directKey),
                        "encrypted backup round trip did not restore direct key")) {
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

    const QString invalidEvidence = backupRoot + QStringLiteral("/codex/unknown.bin");
    if (!writeFile(invalidEvidence, QByteArrayLiteral("preserve-me"))) return 1;
    const QByteArray beforeInvalidConfig = readFile(codexConfigPath).toUtf8();
    const QByteArray beforeInvalidAuth = readFile(codexAuthPath).toUtf8();
    if (!require(manager.backupInventory(AiTool::CodexCli).state
                        == ConfigBackupSubsystemState::Invalid,
                    "unknown backup evidence was not classified invalid")
            || !require(!manager.configure(
                            AiTool::CodexCli, QStringLiteral("sk-must-not-write"),
                            QStringLiteral("gpt-test")),
                        "configuration proceeded with invalid backup inventory")
            || !require(readFile(codexConfigPath).toUtf8() == beforeInvalidConfig
                        && readFile(codexAuthPath).toUtf8() == beforeInvalidAuth,
                        "invalid backup inventory allowed configuration mutation")
            || !require(QFileInfo::exists(invalidEvidence),
                        "invalid backup evidence was deleted")) {
        return 1;
    }

    const QString claudeKey = QStringLiteral("sk-current-claude");
    const QString geminiKey = QStringLiteral("sk-current-gemini");
    const QString openCodeKey = QStringLiteral("sk-current-opencode");
    if (!require(manager.configure(AiTool::ClaudeCode, claudeKey),
                 "failed to write direct Claude configuration")
        || !require(manager.configure(AiTool::GeminiCli, geminiKey),
                    "failed to write direct Gemini configuration")
        || !require(manager.configure(AiTool::OpenCode, openCodeKey),
                    "failed to write direct OpenCode configuration")
        || !require(manager.inspectConfiguration(AiTool::ClaudeCode).isReady()
                        && !manager.inspectConfiguration(AiTool::ClaudeCode).gatewayMode,
                    "valid direct Claude configuration was not recognized")
        || !require(manager.inspectConfiguration(AiTool::GeminiCli).isReady()
                        && !manager.inspectConfiguration(AiTool::GeminiCli).gatewayMode,
                    "valid direct Gemini configuration was not recognized")
        || !require(manager.inspectConfiguration(AiTool::OpenCode).isReady()
                        && !manager.inspectConfiguration(AiTool::OpenCode).gatewayMode,
                    "valid direct OpenCode configuration was not recognized")) {
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
    const QByteArray completeBackupBytes = recursiveBytes(backupRoot);
    if (!require(!completeBackupBytes.contains(claudeKey.toUtf8())
                    && !completeBackupBytes.contains(geminiKey.toUtf8())
                    && !completeBackupBytes.contains(openCodeKey.toUtf8())
                    && !completeBackupBytes.contains(directKey.toUtf8())
                    && !completeBackupBytes.contains(home.path().toUtf8()),
                 "a supported tool backup retained credential or HOME plaintext")) {
        return 1;
    }
    return 0;
}
