#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>

namespace {

QString readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.readAll());
}

bool requireContains(const QString &text, const QString &value, const char *message)
{
    if (text.contains(value)) return true;
    QTextStream(stderr) << message << Qt::endl;
    return false;
}

bool requireAbsent(const QString &text, const QString &value, const char *message)
{
    if (!text.contains(value)) return true;
    QTextStream(stderr) << message << Qt::endl;
    return false;
}

bool require(bool condition, const char *message)
{
    if (condition) return true;
    QTextStream(stderr) << message << Qt::endl;
    return false;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() != 2) {
        QTextStream(stderr) << "source root argument is required" << Qt::endl;
        return 1;
    }

    const QDir root(application.arguments().at(1));
    const QString mainWindow = readFile(root.filePath(QStringLiteral("src/main_window.cpp")));
    const QString appMain = readFile(root.filePath(QStringLiteral("src/main.cpp")));
    const QString workbenchWindow = readFile(root.filePath(
        QStringLiteral("src/agent_workbench_window.cpp")));
    const QString connectWizard = readFile(root.filePath(
        QStringLiteral("src/connect_wizard.cpp")));
    const QString modelsDialog = readFile(root.filePath(
        QStringLiteral("src/models_dialog.cpp")));
    const QString chatDialog = readFile(root.filePath(
        QStringLiteral("src/chat_dialog.cpp")));
    const QString imageDialog = readFile(root.filePath(
        QStringLiteral("src/image_generation_dialog.cpp")));
    const QString usageDialog = readFile(root.filePath(
        QStringLiteral("src/usage_dialog.cpp")));
    const QString apiClient = readFile(root.filePath(
        QStringLiteral("src/api_client.cpp")));
    const QString apiClientHeader = readFile(root.filePath(
        QStringLiteral("include/api_client.h")));
    const QString apiKeysDialog = readFile(root.filePath(
        QStringLiteral("src/api_keys_dialog.cpp")));
    const QString apiKeysHeader = readFile(root.filePath(
        QStringLiteral("include/api_keys_dialog.h")));
    const QString managementProjection = readFile(root.filePath(
        QStringLiteral("src/companion_key_management_projection.cpp")));
    const QString mainWindowHeader = readFile(root.filePath(
        QStringLiteral("include/main_window.h")));
    const QString toolHeader = readFile(root.filePath(QStringLiteral("include/tool_manager.h")));
    const QString runtime = readFile(root.filePath(
        QStringLiteral("agent-runtime/crates/aegisy-agentd/src/lib.rs")));
    const QString proposal = readFile(root.filePath(
        QStringLiteral("openspec/changes/build-aegisy-agent-workbench/proposal.md")));
    const QString companionSpec = readFile(root.filePath(
        QStringLiteral("openspec/changes/build-aegisy-agent-workbench/specs/"
                       "aegisy-companion-control-center/spec.md")));
    if (mainWindow.isEmpty() || appMain.isEmpty()
            || workbenchWindow.isEmpty() || connectWizard.isEmpty()
            || modelsDialog.isEmpty() || chatDialog.isEmpty()
            || imageDialog.isEmpty() || usageDialog.isEmpty() || apiClient.isEmpty()
            || apiClientHeader.isEmpty() || apiKeysDialog.isEmpty()
            || apiKeysHeader.isEmpty() || managementProjection.isEmpty()
            || mainWindowHeader.isEmpty()
            || toolHeader.isEmpty() || runtime.isEmpty()
            || proposal.isEmpty() || companionSpec.isEmpty()) {
        QTextStream(stderr) << "product scope source could not be read" << Qt::endl;
        return 1;
    }

    bool valid = true;
    for (const QString &label : {
             QStringLiteral("Aegisy - 网站配套助手"),
             QStringLiteral("配置中心"),
             QStringLiteral("桌面增强"),
             QStringLiteral("插件与 Skills"),
             QStringLiteral("Codex 编程"),
         }) {
        valid &= requireContains(mainWindow, label, "companion navigation label is missing");
    }
    valid &= requireAbsent(mainWindow, QStringLiteral("\"Agent 工作台\""),
                           "generic Agent Workbench remains in primary navigation");
    valid &= requireContains(workbenchWindow, QStringLiteral("Aegisy Codex Programming"),
                             "retained Workbench preview is not Codex-scoped");
    valid &= requireContains(workbenchWindow, QStringLiteral("Model: Codex"),
                             "retained Workbench preview does not identify Codex");
    valid &= requireAbsent(workbenchWindow, QStringLiteral("Claude Opus"),
                           "retained Workbench preview advertises a deferred model runtime");
    valid &= requireContains(connectWizard,
                             QStringLiteral("companionConfigurationReceived"),
                             "ConnectWizard does not consume companion metadata");
    valid &= requireContains(connectWizard,
                             QStringLiteral("CompanionCredentialBroker::resolve"),
                             "ConnectWizard does not resolve an exact credential handle");
    valid &= requireContains(connectWizard,
                             QStringLiteral("companionModelsReceived"),
                             "ConnectWizard does not consume correlated model metadata");
    valid &= requireContains(connectWizard,
                             QStringLiteral("m_modelRequestKeyIdentity"),
                             "ConnectWizard does not bind model responses to a Key identity");
    valid &= requireAbsent(connectWizard, QStringLiteral("&ApiClient::modelsReceived"),
                           "ConnectWizard still consumes the global model signal");
    valid &= requireAbsent(connectWizard, QStringLiteral("m_allKeys"),
                           "ConnectWizard retains the raw website Key array");
    valid &= requireContains(modelsDialog,
                             QStringLiteral("companionConfigurationReceived"),
                             "ModelsDialog does not consume companion metadata");
    valid &= requireContains(modelsDialog,
                             QStringLiteral("companionModelsReceived"),
                             "ModelsDialog does not consume correlated model metadata");
    valid &= requireAbsent(modelsDialog, QStringLiteral("&ApiClient::apiKeysReceived"),
                           "ModelsDialog still consumes the raw website Key signal");
    valid &= requireAbsent(modelsDialog, QStringLiteral("&ApiClient::modelsReceived"),
                           "ModelsDialog still consumes the global model signal");
    valid &= requireContains(chatDialog,
                             QStringLiteral("companionConfigurationReceived"),
                             "ChatDialog does not consume companion metadata");
    valid &= requireContains(chatDialog,
                             QStringLiteral("companionModelsReceived"),
                             "ChatDialog does not consume correlated model metadata");
    valid &= requireContains(chatDialog,
                             QStringLiteral("sendCompanionChatMessage"),
                             "ChatDialog bypasses the companion credential broker for chat");
    valid &= requireContains(chatDialog,
                             QStringLiteral("generateCompanionImage"),
                             "ChatDialog image Skill bypasses the companion credential broker");
    valid &= requireContains(chatDialog,
                             QStringLiteral("requestCompanionPresentationPlan"),
                             "ChatDialog presentation Skill bypasses the companion credential broker");
    valid &= requireAbsent(chatDialog, QStringLiteral("&ApiClient::apiKeysReceived"),
                           "ChatDialog still consumes the raw website Key signal");
    valid &= requireAbsent(chatDialog, QStringLiteral("&ApiClient::modelsReceived"),
                           "ChatDialog still consumes the global model signal");
    valid &= requireAbsent(chatDialog, QStringLiteral("m_allApiKeys"),
                           "ChatDialog retains the raw website Key array");
    valid &= requireAbsent(chatDialog, QStringLiteral("selectedApiKey"),
                           "ChatDialog exposes credential plaintext through widget data");
    valid &= requireAbsent(chatDialog, QStringLiteral("maskedKey"),
                           "ChatDialog displays credential fragments");
    valid &= requireAbsent(chatDialog, QStringLiteral("\"key_id\""),
                           "ChatDialog persists a raw website Key identifier");
    valid &= requireAbsent(chatDialog,
                           QStringLiteral("m_apiClient->sendChatMessage("),
                           "ChatDialog calls the raw chat credential API");
    valid &= requireAbsent(chatDialog,
                           QStringLiteral("m_apiClient->generateImage("),
                           "ChatDialog calls the raw image credential API");
    valid &= requireAbsent(chatDialog,
                           QStringLiteral("m_apiClient->requestPresentationPlan("),
                           "ChatDialog calls the raw presentation credential API");
    valid &= requireContains(apiClient,
                             QStringLiteral("resolveCompanionCredential"),
                             "ApiClient lacks the companion operation broker boundary");
    valid &= requireContains(imageDialog,
                             QStringLiteral("companionConfigurationReceived"),
                             "ImageGenerationDialog does not consume companion metadata");
    valid &= requireContains(imageDialog,
                             QStringLiteral("generateCompanionImage"),
                             "ImageGenerationDialog bypasses the companion credential broker");
    valid &= requireContains(imageDialog,
                             QStringLiteral("companionImageGenerated"),
                             "ImageGenerationDialog lacks correlated image results");
    valid &= requireAbsent(imageDialog, QStringLiteral("&ApiClient::apiKeysReceived"),
                           "ImageGenerationDialog still consumes the raw Key signal");
    valid &= requireAbsent(imageDialog, QStringLiteral("m_allKeys"),
                           "ImageGenerationDialog retains the raw Key inventory");
    valid &= requireAbsent(imageDialog, QStringLiteral("selectedApiKey"),
                           "ImageGenerationDialog exposes credential plaintext in UI data");
    valid &= requireAbsent(imageDialog, QStringLiteral("maskedKey"),
                           "ImageGenerationDialog displays credential fragments");
    valid &= requireAbsent(imageDialog,
                           QStringLiteral("m_apiClient->generateImage("),
                           "ImageGenerationDialog calls the raw image credential API");
    valid &= requireContains(usageDialog,
                             QStringLiteral("companionConfigurationReceived"),
                             "UsageDialog does not consume companion metadata");
    valid &= requireContains(usageDialog,
                             QStringLiteral("getCompanionApiKeyUsage"),
                             "UsageDialog bypasses the companion usage projection");
    valid &= requireContains(usageDialog,
                             QStringLiteral("CompanionUsageProjection::validate"),
                             "UsageDialog does not validate companion usage metadata");
    valid &= requireAbsent(usageDialog, QStringLiteral("&ApiClient::apiKeysReceived"),
                           "UsageDialog still consumes the raw website Key signal");
    valid &= requireAbsent(usageDialog, QStringLiteral("m_apiClient->getApiKeyUsage("),
                           "UsageDialog sends raw website Key IDs");
    valid &= requireAbsent(usageDialog, QStringLiteral("m_keyUsage"),
                           "UsageDialog retains raw-ID-keyed usage data");
    valid &= requireContains(apiKeysDialog,
                             QStringLiteral("companionKeyManagementReceived"),
                             "ApiKeysDialog does not consume companion management metadata");
    valid &= requireContains(apiKeysDialog,
                             QStringLiteral("CompanionKeyManagementProjection::validate"),
                             "ApiKeysDialog does not validate management projection");
    for (const QString &method : {
             QStringLiteral("createCompanionApiKey"),
             QStringLiteral("updateCompanionApiKey"),
             QStringLiteral("deleteCompanionApiKey"),
             QStringLiteral("testCompanionApiKey")}) {
        valid &= requireContains(apiKeysDialog, method,
                                 "ApiKeysDialog lacks a companion Key operation");
    }
    for (const QString &forbidden : {
             QStringLiteral("&ApiClient::apiKeysReceived"),
             QStringLiteral("&ApiClient::groupsReceived"),
             QStringLiteral("&ApiClient::requestFailed"),
             QStringLiteral("QClipboard"), QStringLiteral("clipboard()"),
             QStringLiteral("QApplication::clipboard"), QStringLiteral("QSettings"),
             QStringLiteral("maskedKey"), QStringLiteral("QString key;"),
             QStringLiteral("credentialHandle"),
             QStringLiteral("\"credential_handle\""),
             QStringLiteral("\"credential_state\""),
             QStringLiteral("activeKeyId"), QStringLiteral("keyActivated"),
             QStringLiteral("m_apiClient->createApiKey("),
             QStringLiteral("m_apiClient->updateApiKey("),
             QStringLiteral("m_apiClient->deleteApiKey("),
             QStringLiteral("m_apiClient->testApiKey(")}) {
        valid &= requireAbsent(apiKeysDialog + apiKeysHeader, forbidden,
                               "ApiKeysDialog retains a raw Key management boundary");
    }
    for (const QString &forbidden : {
             QStringLiteral("getGroups("), QStringLiteral("createApiKey("),
             QStringLiteral("updateApiKey("), QStringLiteral("deleteApiKey("),
             QStringLiteral("testApiKey("), QStringLiteral("getApiKeyUsage("),
             QStringLiteral("apiKeysReceived("), QStringLiteral("groupsReceived("),
             QStringLiteral("apiKeyOperationCompleted("),
             QStringLiteral("apiKeyOperationFailed("),
             QStringLiteral("apiKeyTested(")}) {
        valid &= requireAbsent(apiClientHeader, forbidden,
                               "ApiClient exposes a raw Key management API");
    }
    valid &= requireAbsent(apiClient, QStringLiteral("emit apiKeysReceived"),
                           "ApiClient still publishes the raw Key inventory");
    valid &= requireContains(apiClient, QStringLiteral("QRandomGenerator::system()"),
                             "Key management handles are not generated by the system RNG");
    for (const QString &prefix : {
             QStringLiteral("website-group-management:opaque:"),
             QStringLiteral("website-group-create:opaque:"),
             QStringLiteral("website-key-update:opaque:"),
             QStringLiteral("website-key-delete:opaque:"),
             QStringLiteral("website-key-test:opaque:")}) {
        valid &= requireContains(apiClient + managementProjection, prefix,
                                 "Key management lacks an action-scoped opaque handle");
    }
    valid &= requireAbsent(mainWindow, QStringLiteral("&ApiClient::apiKeysReceived"),
                           "MainWindow still consumes the raw Key inventory");
    valid &= requireAbsent(mainWindow + mainWindowHeader,
                           QStringLiteral("onApiKeysReceived"),
                           "MainWindow retains the raw Key inventory slot");
    valid &= requireContains(appMain,
                             QStringLiteral("remove(QStringLiteral(\"apikeys/activeKeyId\"))"),
                             "legacy raw preferred Key ID is not removed");
    valid &= requireAbsent(appMain, QStringLiteral("value(\"apikeys/activeKeyId\")"),
                           "application reads the legacy raw preferred Key ID");
    valid &= requireAbsent(appMain, QStringLiteral("setValue(\"apikeys/activeKeyId\")"),
                           "application persists a raw preferred Key ID");
    valid &= require(appMain.count(QStringLiteral("apikeys/activeKeyId")) == 1,
                     "legacy preferred Key ID has a read/write path or missing cleanup");
    valid &= require(appMain.count(QStringLiteral("apikeys/activeKey")) == 2,
                     "legacy plaintext Key cleanup changed unexpectedly");

    for (const QString &tool : {
             QStringLiteral("ClaudeCode"), QStringLiteral("CodexCli"),
             QStringLiteral("GeminiCli"), QStringLiteral("OpenCode"),
         }) {
        valid &= requireContains(toolHeader, tool,
                                 "supported configuration target is missing");
    }

    valid &= requireContains(runtime, QStringLiteral("mod codex_adapter;"),
                             "Codex adapter is not retained");
    for (const QString &adapter : {
             QStringLiteral("mod claude_adapter;"),
             QStringLiteral("mod gemini_adapter;"),
             QStringLiteral("mod acp_adapter;"),
         }) {
        valid &= requireAbsent(runtime, adapter,
                               "deferred non-Codex adapter is compiled into Runtime");
    }

    valid &= requireContains(proposal, QStringLiteral("website companion"),
                             "proposal does not define the companion direction");
    valid &= requireContains(proposal, QStringLiteral("Codex remains the only"),
                             "proposal does not constrain integrated programming");
    valid &= requireContains(companionSpec, QStringLiteral("configuration target"),
                             "companion spec does not separate config and Agent targets");
    valid &= requireContains(companionSpec, QStringLiteral("Codex-only"),
                             "companion spec does not define Codex-only programming");

    return valid ? 0 : 1;
}
