#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>

#include <initializer_list>

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

QString sourceRange(const QString &text, const QString &start, const QString &end)
{
    const qsizetype startIndex = text.indexOf(start);
    if (startIndex < 0) return {};
    const qsizetype endIndex = text.indexOf(end, startIndex + start.size());
    if (endIndex < 0) return {};
    return text.mid(startIndex, endIndex - startIndex);
}

bool requireOrdered(const QString &text,
                    std::initializer_list<QString> values,
                    const char *message)
{
    qsizetype offset = 0;
    for (const QString &value : values) {
        const qsizetype index = text.indexOf(value, offset);
        if (index < 0) {
            QTextStream(stderr) << message << Qt::endl;
            return false;
        }
        offset = index + value.size();
    }
    return true;
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
    const QString secureStorageHeader = readFile(root.filePath(
        QStringLiteral("include/secure_storage.h")));
    const QString secureStorageSource = readFile(root.filePath(
        QStringLiteral("src/secure_storage.cpp")));
    const QString companionCache = readFile(root.filePath(
        QStringLiteral("src/companion_configuration_cache.cpp")));
    const QString companionCacheAdapter = readFile(root.filePath(
        QStringLiteral("src/companion_configuration_cache_secure_storage_adapter.cpp")));
    const QString companionCacheWorker = readFile(root.filePath(
        QStringLiteral("src/companion_configuration_cache_worker.cpp")));
    const QString companionCachePresentation = readFile(root.filePath(
        QStringLiteral("src/companion_configuration_cache_presentation.cpp")));
    const QString companionCachePresentationHeader = readFile(root.filePath(
        QStringLiteral("include/companion_configuration_cache_presentation.h")));
    const QString cmake = readFile(root.filePath(QStringLiteral("CMakeLists.txt")));
    const QString mainWindowHeader = readFile(root.filePath(
        QStringLiteral("include/main_window.h")));
    const QString toolHeader = readFile(root.filePath(QStringLiteral("include/tool_manager.h")));
    const QString toolSource = readFile(root.filePath(QStringLiteral("src/tool_manager.cpp")));
    const QString profileSource = readFile(root.filePath(
        QStringLiteral("src/profile_manager.cpp")));
    const QString gatewayHeader = readFile(root.filePath(
        QStringLiteral("include/gateway_manager.h")));
    const QString gatewaySource = readFile(root.filePath(
        QStringLiteral("src/gateway_manager.cpp")));
    const QString gatewayControlContract = readFile(root.filePath(
        QStringLiteral("src/gateway_control_contract.cpp")));
    const QString activationJournal = readFile(root.filePath(
        QStringLiteral("src/companion_activation_journal.cpp")));
    const QString configurationReceipt = readFile(root.filePath(
        QStringLiteral("include/configuration_apply_receipt.h")));
    const QString extensionRegistry = readFile(root.filePath(
        QStringLiteral("src/extension_registry.cpp")));
    const QString mcpInventory = readFile(root.filePath(
        QStringLiteral("src/mcp_configuration_inventory.cpp")));
    const QString codexPluginInventory = readFile(root.filePath(
        QStringLiteral("src/codex_plugin_inventory.cpp")));
    const QString skillExtensionInventory = readFile(root.filePath(
        QStringLiteral("src/skill_extension_inventory.cpp")));
    const QString extensionCoordinator = readFile(root.filePath(
        QStringLiteral("src/extension_inventory_coordinator.cpp")));
    const QString mcpDialog = readFile(root.filePath(
        QStringLiteral("src/mcp_config_dialog.cpp")));
    const QString gatewayScript = readFile(root.filePath(
        QStringLiteral("assets/local_gateway.js")));
    const QString backupStoreHeader = readFile(root.filePath(
        QStringLiteral("include/configuration_backup_store.h")));
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
            || secureStorageHeader.isEmpty() || secureStorageSource.isEmpty()
            || companionCachePresentation.isEmpty()
            || companionCachePresentationHeader.isEmpty()
            || companionCache.isEmpty() || companionCacheAdapter.isEmpty()
            || companionCacheWorker.isEmpty()
            || cmake.isEmpty()
            || mainWindowHeader.isEmpty()
            || toolHeader.isEmpty() || toolSource.isEmpty() || profileSource.isEmpty()
            || gatewayHeader.isEmpty() || gatewaySource.isEmpty()
            || gatewayControlContract.isEmpty()
            || activationJournal.isEmpty()
            || configurationReceipt.isEmpty()
            || extensionRegistry.isEmpty()
            || mcpInventory.isEmpty() || mcpDialog.isEmpty()
            || codexPluginInventory.isEmpty()
            || skillExtensionInventory.isEmpty()
            || extensionCoordinator.isEmpty()
            || gatewayScript.isEmpty()
            || backupStoreHeader.isEmpty() || runtime.isEmpty()
            || proposal.isEmpty() || companionSpec.isEmpty()) {
        QTextStream(stderr) << "product scope source could not be read" << Qt::endl;
        return 1;
    }

    bool valid = true;
    for (const QString &label : {
             QStringLiteral("Aegisy - 网站配套助手"),
             QStringLiteral("配置中心"),
             QStringLiteral("桌面增强"),
             QStringLiteral("扩展与系统"),
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
                             QStringLiteral("&ApiClient::companionConfigurationFailed"),
                             "ConnectWizard does not retire failed companion metadata");
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
                             QStringLiteral("&ApiClient::companionConfigurationFailed"),
                             "ModelsDialog does not retire failed companion metadata");
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
                             QStringLiteral("&ApiClient::companionConfigurationFailed"),
                             "ChatDialog does not retire failed companion metadata");
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
    const QString apiKeyCompletion = sourceRange(
        apiClient, QStringLiteral("void ApiClient::onApiKeysFinished()"),
        QStringLiteral("void ApiClient::onUserInfoFinished()"));
    valid &= requireContains(apiKeyCompletion,
                             QStringLiteral("failCurrentCompanionConfiguration("),
                             "API-Key completion bypasses central authority retirement");
    valid &= requireAbsent(apiKeyCompletion,
                           QStringLiteral("emit companionConfigurationFailed"),
                           "API-Key completion emits configuration failure outside retirement");
    valid &= requireContains(apiClientHeader,
                             QStringLiteral("m_currentCompanionModelProjections"),
                             "ApiClient lacks website model authority state");
    valid &= requireContains(apiClientHeader,
                             QStringLiteral("companionWebsiteModelsObserved"),
                             "ApiClient lacks the website-only model observation signal");
    valid &= require(apiClient.count(
                         QStringLiteral("emit companionWebsiteModelsObserved(")) == 1,
                     "website model observation has multiple or missing producers");
    valid &= requireContains(apiClient,
                             QStringLiteral("if (!managementKeyTest && !projectionSha256.isEmpty())"),
                             "website model observation is not isolated from management/local results");
    valid &= requireAbsent(connectWizard + modelsDialog + chatDialog,
                           QStringLiteral("companionWebsiteModelsObserved"),
                           "existing dialogs consume the display-only persistent cache signal");
    valid &= requireAbsent(mainWindow,
                           QStringLiteral("CompanionConfigProjection::saveLastValid"),
                           "MainWindow still writes the unauthenticated legacy cache");
    valid &= requireAbsent(mainWindow,
                           QStringLiteral("CompanionConfigProjection::loadLastValid"),
                           "MainWindow still reads the unauthenticated legacy cache");
    valid &= requireContains(companionCacheAdapter,
                             QStringLiteral("companion/configuration-cache-authority/v1/"),
                             "production cache adapter lacks the strict authority scope");
    valid &= requireContains(companionCacheAdapter,
                             QStringLiteral("SecureStorage::loadEncryptedFresh(scope)"),
                             "production cache adapter does not perform a typed fresh read");
    valid &= requireContains(companionCacheAdapter,
                             QStringLiteral("SecureStorage::saveEncrypted(scope, decoded)"),
                             "production cache adapter does not use SecureStorage writes");
    valid &= requireAbsent(companionCacheAdapter,
                           QStringLiteral("SecureStorage::loadEncrypted("),
                           "production cache adapter uses compatibility cached reads");
    valid &= requireAbsent(companionCacheAdapter,
                           QStringLiteral("SecureStorage::contains("),
                           "production cache adapter confuses existence with fresh state");
    valid &= requireContains(companionCacheWorker,
                             QStringLiteral("std::make_unique<QSettings>()"),
                             "cache worker does not own QSettings");
    valid &= requireContains(mainWindow,
                             QStringLiteral("moveToThread(m_companionCacheThread)"),
                             "MainWindow cache persistence is not moved off the UI thread");
    valid &= requireContains(mainWindow,
                             QStringLiteral("Qt::QueuedConnection"),
                             "MainWindow cache operations are not queued to the worker");
    const QString liveConfigurationHandler = sourceRange(
        mainWindow,
        QStringLiteral("void MainWindow::onCompanionConfigurationReceived("),
        QStringLiteral("void MainWindow::onCompanionConfigurationFailed("));
    valid &= requireOrdered(
        liveConfigurationHandler,
        {QStringLiteral("updateCompanionProjectionStatus(projection, true)"),
         QStringLiteral("worker->commitLiveConfiguration(")},
        "live configuration is not rendered before cache persistence");
    const qsizetype liveReadyIndex = liveConfigurationHandler.indexOf(
        QStringLiteral("updateCompanionProjectionStatus(projection, true)"));
    valid &= require(liveReadyIndex >= 0
                         && !liveConfigurationHandler.mid(liveReadyIndex).contains(
                             QStringLiteral("onCompanionConfigurationFailed(")),
                     "cache persistence failure is routed into live failure");
    valid &= requireContains(companionCache,
                             QStringLiteral("configuration_authority"),
                             "persistent cache authority invariant is missing");
    valid &= requireContains(
        companionCachePresentation,
        QStringLiteral("viewAccountIdentity != expectedAccountIdentity"),
        "cache presentation is not bound to the current account");
    valid &= requireContains(
        companionCachePresentation,
        QStringLiteral("view.configurationAuthority || view.configurationApplied"),
        "cache presentation does not reject authority-bearing views");
    valid &= requireAbsent(
        companionCachePresentationHeader,
        QStringLiteral("credentialHandle"),
        "cache presentation DTO exposes a credential handle");
    valid &= requireAbsent(
        companionCachePresentationHeader,
        QStringLiteral("configurationAuthority"),
        "cache presentation DTO exposes operational authority");
    valid &= requireContains(
        companionCachePresentation,
        QStringLiteral("ageForDisplay("),
        "cache presentation has no monotonic display-aging boundary");
    valid &= requireContains(mainWindow,
                             QStringLiteral("currentCompanionCachePresentation()"),
                             "MainWindow does not inject the account-bound cache view");
    valid &= requireContains(connectWizard,
                             QStringLiteral("ConnectionRowKind::CachedWebsite"),
                             "ConnectWizard lacks an isolated cached row kind");
    valid &= requireContains(connectWizard,
                             QStringLiteral("if (currentSelectionIsCached())"),
                             "ConnectWizard lacks repeated cached entry-point gates");
    valid &= requireContains(connectWizard,
                             QStringLiteral("kCachedKeyIdentityRole = Qt::UserRole + 32"),
                             "ConnectWizard cache roles overlap live roles");
    valid &= requireContains(modelsDialog,
                             QStringLiteral("SourceMode::CachedDisplay"),
                             "ModelsDialog lacks an explicit cached display mode");
    valid &= requireContains(modelsDialog,
                             QStringLiteral("m_refreshButton->setEnabled(false)"),
                             "ModelsDialog cached mode can query providers");
    valid &= requireContains(chatDialog,
                             QStringLiteral("m_sourceMode != SourceMode::LiveWebsite"),
                             "Chat cached mode lacks direct action gates");
    valid &= requireContains(chatDialog,
                             QStringLiteral("kCachedKeyIdentityRole = Qt::UserRole + 32"),
                             "Chat cache roles overlap live roles");
    valid &= requireContains(connectWizard,
                             QStringLiteral("scheduleCachedPresentationRefresh()"),
                             "ConnectWizard does not schedule cache TTL transitions");
    valid &= requireContains(modelsDialog,
                             QStringLiteral("scheduleCachedPresentationRefresh()"),
                             "ModelsDialog does not schedule cache TTL transitions");
    valid &= requireContains(chatDialog,
                             QStringLiteral("scheduleCachedPresentationRefresh()"),
                             "ChatDialog does not schedule cache TTL transitions");
    valid &= requireContains(cmake,
                             QStringLiteral("companion_cached_dialogs_projection"),
                             "cached dialog projection is absent from CTest");
    valid &= requireContains(imageDialog,
                             QStringLiteral("companionConfigurationReceived"),
                             "ImageGenerationDialog does not consume companion metadata");
    valid &= requireContains(imageDialog,
                             QStringLiteral("&ApiClient::companionConfigurationFailed"),
                             "ImageGenerationDialog does not retire failed companion metadata");
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
                             QStringLiteral("&ApiClient::companionConfigurationFailed"),
                             "UsageDialog does not retire failed companion metadata");
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
                             QStringLiteral("&ApiClient::companionConfigurationFailed"),
                             "ApiKeysDialog does not retire failed companion metadata");
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
    const QString removeTestingMacro = QStringLiteral(
        "AEGISY_SECURE_STORAGE_REMOVE_TESTING");
    valid &= require(cmake.count(removeTestingMacro) == 1,
                     "secure-storage removal hook is not target-local");
    valid &= requireContains(
        cmake,
        QStringLiteral("target_compile_definitions(AegisyCompanionKeyManagementApiTest PRIVATE\n"
                       "        AEGISY_SECURE_STORAGE_REMOVE_TESTING=1)"),
        "secure-storage removal hook is not private to the API fixture");
    valid &= requireAbsent(cmake, QStringLiteral("add_compile_definitions("),
                           "a global compile definition can expose test authority");
    valid &= require(secureStorageHeader.count(removeTestingMacro) == 1
                        && secureStorageSource.count(removeTestingMacro) == 4,
                     "secure-storage removal hook guard changed unexpectedly");
    const QString secureStorageSave = sourceRange(
        secureStorageSource, QStringLiteral("bool SecureStorage::saveEncrypted("),
        QStringLiteral("QString SecureStorage::loadEncrypted("));
    const QString secureStorageRemove = sourceRange(
        secureStorageSource, QStringLiteral("bool SecureStorage::remove("),
        QStringLiteral("#ifdef AEGISY_SECURE_STORAGE_REMOVE_TESTING\nvoid "));
    valid &= require(!secureStorageSave.isEmpty() && !secureStorageRemove.isEmpty(),
                     "secure-storage persistence functions could not be isolated");
    valid &= requireOrdered(
        secureStorageSave,
        {QStringLiteral("settings.setValue(key, encrypted.toBase64());"),
         QStringLiteral("settings.sync();"),
         QStringLiteral("if (settings.status() != QSettings::NoError)"),
         QStringLiteral("return false;"),
         QStringLiteral("cacheCredential(key, data);")},
        "Windows secure-storage save can cache before durable persistence succeeds");
    valid &= require(
        secureStorageSave.count(QStringLiteral("cacheCredential(key, data);")) == 3
            && secureStorageSave.indexOf(QStringLiteral("cacheCredential(key, data);"))
                > secureStorageSave.indexOf(
                    QStringLiteral("if (settings.status() != QSettings::NoError)")),
        "secure-storage save has a cache write before Windows persistence validation");
    valid &= requireOrdered(
        secureStorageRemove,
        {QStringLiteral("removed = deleteFromKeychain(SERVICE_NAME, key);"),
         QStringLiteral("settings.remove(key);"),
         QStringLiteral("settings.sync();"),
         QStringLiteral("removed = settings.status() == QSettings::NoError;"),
         QStringLiteral("removed = deleteFromSecretService(SERVICE_NAME, key);"),
         QStringLiteral("if (!removed) return false;"),
         QStringLiteral("removeCachedCredential(key);"),
         QStringLiteral("return true;")},
        "secure-storage remove clears cache before its platform backend succeeds");
    valid &= require(secureStorageRemove.count(
                         QStringLiteral("removeCachedCredential(key);")) == 1,
                     "secure-storage remove has a platform path that clears cache early");
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
    valid &= requireContains(toolHeader, QStringLiteral("ConfigBackupInventory"),
                             "ToolManager does not expose backup subsystem state");
    valid &= requireContains(toolSource, QStringLiteral("ConfigurationBackupStore"),
                             "ToolManager bypasses the encrypted backup store");
    valid &= requireContains(toolSource, QStringLiteral("removeVerified("),
                             "ToolManager prune bypasses verified removal");
    valid &= requireContains(toolSource,
                             QStringLiteral("tool-manager/config-backup-master/v1/"),
                             "ToolManager lacks the strict backup key scope");
    for (const QString &forbidden : {
             QStringLiteral("file_%1.bin"),
             QStringLiteral("manifest.readAll()"),
             QStringLiteral("QDir(rootPath + QLatin1Char('/') + history[i].id)"
                            ".removeRecursively()")}) {
        valid &= requireAbsent(toolSource, forbidden,
                               "ToolManager retains the plaintext backup implementation");
    }
    valid &= requireContains(mainWindow, QStringLiteral("backupInventory(tool)"),
                             "backup UI does not consume subsystem state");
    valid &= requireContains(mainWindow, QStringLiteral("OpenCode"),
                             "backup UI omits the OpenCode configuration target");

    const QString activationWorkflow = sourceRange(
        mainWindow,
        QStringLiteral("void MainWindow::processActivationQueue()"),
        QStringLiteral("void MainWindow::abortActivation("));
    valid &= requireOrdered(
        activationWorkflow,
        {QStringLiteral("const ToolStatus status = m_toolManager->detect(tool)"),
         QStringLiteral("if (!status.installed)"),
         QStringLiteral("m_toolManager->install(tool, m_activationGeneration)"),
         QStringLiteral("m_toolManager->prepareConfigurationApply("),
         QStringLiteral("m_activationJournal->create("),
         QStringLiteral("m_toolManager->applyPreparedConfiguration("),
         QStringLiteral("m_profileManager->setActiveIndex(profileIndex)")},
        "activation does not install, configure, and commit the active profile in order");
    valid &= requireOrdered(
        activationWorkflow,
        {QStringLiteral("const QString appliedCredential"),
         QStringLiteral("m_toolManager->prepareConfigurationApply("),
         QStringLiteral("m_activationJournal->create("),
         QStringLiteral("m_toolManager->applyPreparedConfiguration(")},
        "activation does not predeclare the exact candidate before journal/apply");
    valid &= requireContains(
        configurationReceipt,
        QStringLiteral("QString candidateFilesIdentity;"),
        "configuration receipt lacks a predeclared candidate identity");
    valid &= requireContains(
        toolSource,
        QStringLiteral("appliedIdentity != receipt->candidateFilesIdentity"),
        "ToolManager does not verify applied files against the predeclared candidate");
    valid &= requireContains(
        activationJournal,
        QStringLiteral("candidate_files_identity"),
        "activation journal omits the predeclared candidate files identity");
    valid &= requireContains(
        activationWorkflow,
        QStringLiteral("abortActivation(QStringLiteral("),
        "activation failures do not preserve the prior active profile");
    const QString installCompletion = sourceRange(
        mainWindow,
        QStringLiteral("void MainWindow::onInstallFinished("),
        QStringLiteral("void MainWindow::installToolEnvironment("));
    valid &= requireOrdered(
        installCompletion,
        {QStringLiteral("if (!success)"),
         QStringLiteral("abortActivation("),
         QStringLiteral("const ToolStatus status = m_toolManager->detect(tool)"),
         QStringLiteral("if (!status.installed)"),
         QStringLiteral("processActivationQueue()")},
        "installation completion can advance activation without verified CLI state");
    valid &= requireContains(
        toolSource,
        QStringLiteral("安装失败不会切换活动档案或写入配置"),
        "configuration preview omits fail-closed installation impact");
    const QString bulkActivation = sourceRange(
        mainWindow,
        QStringLiteral("void MainWindow::onBulkSwitchClicked()"),
        QStringLiteral("void MainWindow::onNewConnectClicked()"));
    valid &= requireContains(
        bulkActivation,
        QStringLiteral("confirmConfigurationPreview(previews, false)"),
        "bulk activation bypasses the combined configuration preview");
    valid &= requireContains(
        bulkActivation,
        QStringLiteral("startActivationQueue(scheduledProfiles)"),
        "bulk activation bypasses the verified activation queue");
    valid &= requireAbsent(
        bulkActivation,
        QStringLiteral("configureFromProfile("),
        "bulk activation writes configuration outside the verified queue");
    valid &= requireAbsent(
        bulkActivation,
        QStringLiteral("setActiveIndex("),
        "bulk activation commits active state outside the verified queue");
    const QString activeProfileEdit = sourceRange(
        mainWindow,
        QStringLiteral("void MainWindow::editProfile(int index)"),
        QStringLiteral("void MainWindow::deleteProfile(int index)"));
    valid &= requireContains(
        activeProfileEdit,
        QStringLiteral("dialog.setCreateReplacementOnEdit(wasActive)"),
        "active Profile edits are not staged as immutable replacements");
    valid &= requireContains(
        activeProfileEdit,
        QStringLiteral("discardPendingProfileReplacement()"),
        "cancelled active Profile replacement is not discarded");
    valid &= requireOrdered(
        activationWorkflow,
        {QStringLiteral("m_profileManager->setActiveIndex(profileIndex)"),
         QStringLiteral("finalizePendingProfileReplacement(profile.id)")},
        "active Profile replacement removes the old Profile before verified activation");
    valid &= requireContains(
        activationWorkflow,
        QStringLiteral("m_toolManager->rollbackPreparedConfiguration(receipt)"),
        "active Profile commit failure cannot restore the verified tool preimage");
    valid &= requireContains(
        toolHeader,
        QStringLiteral("QString *rollbackBackupId = nullptr"),
        "ToolManager does not retain a verified rollback receipt for Profile commit");
    valid &= requireContains(
        mainWindow,
        QStringLiteral("if (!m_profileManager->setActiveIndex(profileIndex))"),
        "MainWindow reports activation without a verified Profile commit");
    valid &= requireContains(
        mainWindow,
        QStringLiteral("if (!removal.metadataRemoved())"),
        "MainWindow clears Profile removal state without a verified metadata outcome");
    valid &= requireContains(
        mainWindow,
        QStringLiteral("ProfileRemovalState::RemovedCredentialCleanupPending"),
        "Profile removal has no typed truthfulness boundary");
    valid &= requireContains(
        profileSource,
        QStringLiteral("const auto restoreOldState = [&]()"),
        "Profile update lacks exact old-state compensation");
    valid &= requireContains(
        profileSource,
        QStringLiteral("newCredentialVerified"),
        "Profile update does not verify its fresh credential write");
    valid &= requireContains(
        toolSource,
        QStringLiteral("if (rollbackBackupId) *rollbackBackupId = receipt.backupId"),
        "ToolManager success does not publish its exact verified backup identity");
    valid &= requireOrdered(
        toolSource,
        {QStringLiteral("bool ToolManager::prepareConfigurationApply("),
         QStringLiteral("bool ToolManager::applyPreparedConfiguration("),
         QStringLiteral("bool ToolManager::rollbackPreparedConfiguration("),
         QStringLiteral("bool ToolManager::finalizePreparedConfiguration(")},
        "ToolManager does not expose the ordered prepare/apply/rollback/finalize boundary");
    valid &= requireOrdered(
        activationWorkflow,
        {QStringLiteral("m_gatewayManager->prepareProfile("),
         QStringLiteral("m_toolManager->applyPreparedConfiguration("),
         QStringLiteral("m_gatewayManager->commitProfile(")},
        "gateway profile is not prepared, file-verified, and committed in order");
    valid &= requireContains(
        activationWorkflow,
        QStringLiteral("m_gatewayManager->abortProfile("),
        "gateway candidate cannot be aborted after file apply failure");
    valid &= requireContains(
        mainWindow,
        QStringLiteral("void MainWindow::recoverPendingActivation()"),
        "MainWindow does not consume the durable activation journal at startup");
    valid &= requireContains(
        mainWindow,
        QStringLiteral("if (profileIndices.isEmpty() || m_activationRecoveryRequired)"),
        "bulk activation bypasses the recovery-required gate");
    valid &= requireContains(
        mainWindow,
        QStringLiteral("running && m_activationRecoveryRequired"),
        "gateway auto-rehydration bypasses the recovery-required gate");
    for (const QString &operation : {
             QStringLiteral("prepare-configure"), QStringLiteral("prepare-remove"),
             QStringLiteral("commit"), QStringLiteral("abort")}) {
        valid &= requireContains(gatewayScript, operation,
                                 "gateway control protocol operation is missing");
    }
    valid &= requireContains(
        gatewayScript,
        QStringLiteral("aegisy-gateway-control/0.1"),
        "gateway script lacks the strict control schema");
    valid &= requireContains(
        gatewaySource,
        QStringLiteral("generation != m_generation || process != m_process"),
        "gateway callbacks are not process-generation bound");
    valid &= requireContains(
        gatewayControlContract,
        QStringLiteral("actualKeys != expectedKeys"),
        "gateway control acknowledgement is not exact-field validated");
    valid &= requireContains(
        cmake,
        QStringLiteral("gateway_control_contract"),
        "gateway control contract is absent from CTest");
    valid &= requireContains(
        cmake,
        QStringLiteral("gateway_manager_process_matrix"),
        "gateway process timeout/exit matrix is absent from CTest");
    valid &= require(
        cmake.count(QStringLiteral("AEGISY_GATEWAY_MANAGER_PROCESS_TEST=1")) == 1,
        "gateway process injection macro is not isolated to one test target");
    const QString generationRetirement = sourceRange(
        gatewaySource,
        QStringLiteral("void GatewayManager::failCurrentGeneration("),
        QStringLiteral("void GatewayManager::clearRequestLogs()"));
    valid &= requireOrdered(
        generationRetirement,
        {QStringLiteral("++m_generation;"),
         QStringLiteral("m_expectedRequestId.clear();"),
         QStringLiteral("retiredProcess->kill();"),
         QStringLiteral("retiredProcess->waitForFinished(1000)")},
        "gateway failure does not retire identity before kill and bounded reap");
    valid &= requireContains(
        cmake,
        QStringLiteral("companion_activation_journal"),
        "activation recovery journal is absent from CTest");
    valid &= requireContains(
        activationJournal,
        QStringLiteral("activation-journal-cas-conflict"),
        "activation journal lacks expected-identity CAS");
    valid &= requireAbsent(
        activationJournal,
        QStringLiteral("apiKey"),
        "activation journal contains a credential field");
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_registry_contract"),
        "extension registry contract is absent from CTest");
    valid &= requireContains(
        extensionRegistry,
        QStringLiteral("extension-registry/0.1"),
        "extension registry schema is missing");
    for (const QString &authority : {
             QStringLiteral("install_authority"),
             QStringLiteral("enable_authority"),
             QStringLiteral("update_authority"),
             QStringLiteral("remove_authority"),
             QStringLiteral("execution_authority")}) {
        valid &= requireContains(extensionRegistry, authority,
                                 "extension registry authority boundary is missing");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("mcp_configuration_inventory"),
        "MCP configuration inventory is absent from CTest");
    valid &= requireContains(
        cmake,
        QStringLiteral("mcp_config_dialog_guard"),
        "MCP malformed/drifted save guard is absent from CTest");
    for (const QString &testName : {
             QStringLiteral("codex_plugin_inventory"),
             QStringLiteral("skill_extension_inventory"),
             QStringLiteral("extension_inventory_coordinator"),
             QStringLiteral("extension_center_read_only")}) {
        valid &= requireContains(cmake, testName,
                                 "strict extension source/UI test is absent from CTest");
    }
    valid &= requireContains(
        mainWindow,
        QStringLiteral("ExtensionInventoryCoordinator::collect(inputs)"),
        "primary extension center does not consume the unified inventory");
    valid &= requireContains(
        mainWindow,
        QStringLiteral("QStringLiteral(\"扩展中心\")"),
        "primary extension center entry is missing");
    valid &= requireContains(
        codexPluginInventory,
        QStringLiteral("StrictJsonValidator::accepts(bytes)"),
        "Codex plugin inventory does not reject ambiguous JSON");
    valid &= requireContains(
        skillExtensionInventory,
        QStringLiteral("StrictJsonValidator::accepts(bytes)"),
        "Skill inventory does not reject ambiguous manifests");
    valid &= requireContains(
        extensionCoordinator,
        QStringLiteral("QProcessEnvironment result;"),
        "Codex plugin capture inherits the full process environment");
    valid &= requireAbsent(
        extensionCoordinator,
        QStringLiteral("setProcessEnvironment(inputs.sourceEnvironment)"),
        "Codex plugin capture bypasses environment scrubbing");
    valid &= requireContains(
        extensionCoordinator,
        QStringLiteral("MaxCodexStderrBytes"),
        "Codex plugin capture does not bound stderr");
    valid &= requireContains(
        mcpDialog,
        QStringLiteral("McpConfigurationInventory::inspectFile(settingsFilePath())"),
        "MCP dialog bypasses strict source inventory");
    valid &= requireAbsent(
        mcpDialog,
        QStringLiteral("doc.isObject() ? doc.object() : QJsonObject()"),
        "malformed MCP configuration still degrades to an empty writable object");
    valid &= requireContains(
        mcpDialog,
        QStringLiteral("current.sourceIdentity != m_sourceIdentity"),
        "MCP save does not reject external source drift");
    valid &= requireContains(
        gatewaySource,
        QStringLiteral("gateway-control-timeout-outcome-unknown"),
        "gateway timeout is not classified outcome-unknown");
    valid &= requireContains(
        gatewaySource,
        QStringLiteral("gateway-runtime-stderr"),
        "gateway stderr is not reduced to a fixed classification");
    valid &= requireAbsent(
        gatewaySource,
        QStringLiteral("error.left(300)"),
        "gateway publishes dynamic stderr text");
    valid &= requireAbsent(
        gatewayScript,
        QStringLiteral("type === 'configure'"),
        "gateway retains the unacknowledged legacy configure path");
    valid &= requireContains(
        gatewayHeader,
        QStringLiteral("bool prepareProfile("),
        "GatewayManager lacks a prepare boundary");
    valid &= requireContains(
        connectWizard,
        QStringLiteral("m_editIndex < 0 || m_createReplacementOnEdit"),
        "ConnectWizard cannot create an immutable replacement for an active Profile");
    valid &= requireContains(
        connectWizard,
        QStringLiteral("m_selectedType != m_existingType"),
        "active Profile replacement can cross tool boundaries");
    valid &= requireContains(
        activationWorkflow,
        QStringLiteral("activationProfileIdentity(profile) != entry.profileIdentity"),
        "activation queue is not bound to immutable Profile content");
    valid &= requireContains(
        mainWindowHeader,
        QStringLiteral("QString profileId;"),
        "activation queue is not bound to a stable Profile UUID");
    const QString environmentCheck = sourceRange(
        mainWindow,
        QStringLiteral("void MainWindow::showEnvCheckDialog(int profileIndex)"),
        QStringLiteral("void MainWindow::onManageKeysClicked()"));
    valid &= requireAbsent(
        environmentCheck,
        QStringLiteral("installToolEnvironment(tool)"),
        "pre-activation environment review can race the activation installer");
    valid &= requireContains(
        environmentCheck,
        QStringLiteral("由激活流程先安装并验证"),
        "environment review does not delegate installation to the verified activation queue");

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
