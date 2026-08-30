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
    const QString activationJournalAdapter = readFile(root.filePath(
        QStringLiteral("src/companion_activation_journal_secure_storage_adapter.cpp")));
    const QString authoritySlots = readFile(root.filePath(
        QStringLiteral("src/authority_slot_publication.cpp")));
    const QString activationSlotDomain = readFile(root.filePath(
        QStringLiteral("src/companion_activation_authority_slots.cpp")));
    const QString reviewLedgerAdapter = readFile(root.filePath(
        QStringLiteral("src/extension_review_ledger_secure_storage_adapter.cpp")));
    const QString enablementLedgerAdapter = readFile(root.filePath(
        QStringLiteral(
            "src/extension_enablement_ledger_secure_storage_adapter.cpp")));
    const QString secureSlotAdapter = readFile(root.filePath(
        QStringLiteral("src/secure_storage_authority_slot_adapter.cpp")));
    const QString configurationReceipt = readFile(root.filePath(
        QStringLiteral("include/configuration_apply_receipt.h")));
    const QString extensionRegistry = readFile(root.filePath(
        QStringLiteral("src/extension_registry.cpp")));
    const QString compatibilityPolicy = readFile(root.filePath(
        QStringLiteral("src/extension_compatibility_policy.cpp")));
    const QString trustPolicy = readFile(root.filePath(
        QStringLiteral("src/extension_trust_policy.cpp")));
    const QString enablementPolicy = readFile(root.filePath(
        QStringLiteral("src/extension_enablement_policy.cpp")));
    const QString reviewLedger = readFile(root.filePath(
        QStringLiteral("src/extension_review_ledger.cpp")));
    const QString evidenceLedger = readFile(root.filePath(
        QStringLiteral("src/extension_evidence_ledger.cpp")));
    const QString enablementLedger = readFile(root.filePath(
        QStringLiteral("src/extension_enablement_ledger.cpp")));
    const QString reviewLedgerStore = readFile(root.filePath(
        QStringLiteral("src/extension_review_ledger_store.cpp")));
    const QString evidenceLedgerStore = readFile(root.filePath(
        QStringLiteral("src/extension_evidence_ledger_store.cpp")));
    const QString enablementLedgerStore = readFile(root.filePath(
        QStringLiteral("src/extension_enablement_ledger_store.cpp")));
    const QString reviewWorkflow = readFile(root.filePath(
        QStringLiteral("src/extension_review_workflow.cpp")));
    const QString enablementWorkflow = readFile(root.filePath(
        QStringLiteral("src/extension_enablement_workflow.cpp")));
    const QString enablementController = readFile(root.filePath(
        QStringLiteral("src/extension_enablement_controller.cpp")));
    const QString reviewPresentation = readFile(root.filePath(
        QStringLiteral("src/extension_review_presentation.cpp")));
    const QString enablementPresentation = readFile(root.filePath(
        QStringLiteral("src/extension_enablement_presentation.cpp")));
    const QString enablementPresentationHeader = readFile(root.filePath(
        QStringLiteral("include/extension_enablement_presentation.h")));
    const QString approvalPolicy = readFile(root.filePath(
        QStringLiteral("src/extension_approval_policy.cpp")));
    const QString approvalPolicyHeader = readFile(root.filePath(
        QStringLiteral("include/extension_approval_policy.h")));
    const QString displaySafety = readFile(root.filePath(
        QStringLiteral("src/extension_display_safety.cpp")));
    const QString sandboxGate = readFile(root.filePath(
        QStringLiteral("src/execution_sandbox_gate.cpp")));
    const QString sandboxGateHeader = readFile(root.filePath(
        QStringLiteral("include/execution_sandbox_gate.h")));
    const QString recoveryGate = readFile(root.filePath(
        QStringLiteral("src/extension_recovery_gate.cpp")));
    const QString recoveryGateHeader = readFile(root.filePath(
        QStringLiteral("include/extension_recovery_gate.h")));
    const QString admissionGate = readFile(root.filePath(
        QStringLiteral("src/extension_admission_gate.cpp")));
    const QString updatePolicy = readFile(root.filePath(
        QStringLiteral("src/extension_update_policy.cpp")));
    const QString importPreview = readFile(root.filePath(
        QStringLiteral("src/extension_import_preview.cpp")));
    const QString scopePolicy = readFile(root.filePath(
        QStringLiteral("src/extension_scope_policy.cpp")));
    const QString reviewController = readFile(root.filePath(
        QStringLiteral("src/extension_review_controller.cpp")));
    const QString extensionCenter = readFile(root.filePath(
        QStringLiteral("src/extension_center_dialog.cpp")));
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
            || activationJournalAdapter.isEmpty()
            || authoritySlots.isEmpty() || activationSlotDomain.isEmpty()
            || reviewLedgerAdapter.isEmpty()
            || configurationReceipt.isEmpty()
            || extensionRegistry.isEmpty()
            || compatibilityPolicy.isEmpty() || trustPolicy.isEmpty()
            || reviewLedger.isEmpty() || reviewLedgerStore.isEmpty()
            || evidenceLedgerStore.isEmpty() || enablementLedgerStore.isEmpty()
            || reviewWorkflow.isEmpty() || reviewPresentation.isEmpty()
            || enablementPresentation.isEmpty() || displaySafety.isEmpty()
            || approvalPolicy.isEmpty() || approvalPolicyHeader.isEmpty()
            || sandboxGate.isEmpty() || sandboxGateHeader.isEmpty()
            || recoveryGate.isEmpty() || recoveryGateHeader.isEmpty()
            || admissionGate.isEmpty()
            || updatePolicy.isEmpty()
            || importPreview.isEmpty()
            || scopePolicy.isEmpty()
            || enablementWorkflow.isEmpty() || enablementController.isEmpty()
            || reviewController.isEmpty()
            || extensionCenter.isEmpty()
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
        cmake,
        QStringLiteral("authority_slot_publication"),
        "the shared authority slot publication layer is absent from CTest");
    valid &= requireContains(
        activationJournal,
        QStringLiteral("activation-journal-cas-conflict"),
        "activation journal lacks expected-identity CAS");
    valid &= requireAbsent(
        activationJournal,
        QStringLiteral("apiKey"),
        "activation journal contains a credential field");
    valid &= requireAbsent(
        activationJournalAdapter,
        QStringLiteral("apiKey"),
        "activation journal authority adapter contains a credential field");
    // 授权信封必须由安全存储持有 MAC 密钥、单调序号与已提交锚点。
    for (const QString &token : {
             QStringLiteral("aegisy-companion-activation-journal-authority/0.1"),
             QStringLiteral("aegisy-companion-activation-journal-hmac/0.3"),
             QStringLiteral("hmac_key_base64"),
             QStringLiteral("highest_serial"),
             QStringLiteral("HMAC(")}) {
        valid &= requireContains(
            activationJournal, token,
            "activation journal lacks an authenticated authority envelope");
    }
    // QSettings 单独既不能伪造也不能删除一笔事务。
    for (const QString &code : {
             QStringLiteral("activation-journal-record-without-authority"),
             QStringLiteral("activation-journal-record-deleted"),
             QStringLiteral("activation-journal-record-unauthenticated"),
             QStringLiteral("activation-journal-serial-drift"),
             QStringLiteral("activation-journal-reserved-third-state")}) {
        valid &= requireContains(
            activationJournal, code,
            "activation journal lacks an anti-forgery or anti-deletion verdict");
    }
    valid &= requireContains(
        activationJournal,
        QStringLiteral("CRYPTO_memcmp"),
        "activation journal does not compare authority MACs in constant time");
    valid &= requireContains(
        activationJournal,
        QStringLiteral("OPENSSL_cleanse"),
        "activation journal does not zeroize the authority MAC key");
    valid &= requireContains(
        activationJournal,
        QStringLiteral("RAND_bytes"),
        "activation journal does not generate a random authority MAC key");
    // 预留 -> 写记录 -> 提交：崩溃必须留下可确定性恢复的中间态。
    valid &= requireOrdered(
        activationJournal,
        {QStringLiteral("reserved.reservedPresent = true;"),
         QStringLiteral("settings->setValue(kRecordKey, targetBytes);"),
         QStringLiteral("committed.committedPresent = true;")},
        "activation journal does not reserve before writing and committing a record");
    valid &= requireContains(
        secureSlotAdapter,
        QStringLiteral("SecureStorage::loadEncryptedFresh"),
        "the shared authority slot adapter does not read past the process cache");
    valid &= requireContains(
        activationJournalAdapter,
        QStringLiteral("WriteOutcome::OutcomeUnknown"),
        "activation journal authority write cannot report an unknown outcome");
    // A/B 发布：授权载荷绝不能只有一份副本，否则一次撕裂的写入销毁 MAC 密钥。
    // 发布顺序现在由共享搬运层持有，三个子系统共用同一条路径。
    valid &= requireOrdered(
        secureSlotAdapter,
        {QStringLiteral("const AuthoritySlotSelection selection = currentSelection("),
         QStringLiteral("AuthoritySlotPublication::frame("),
         QStringLiteral("SecureStorage::saveEncrypted(slotScope(")},
        "the shared authority slot adapter publishes without selecting an A/B slot first");
    valid &= requireAbsent(
        activationJournalAdapter,
        QStringLiteral("SecureStorage::saveEncrypted(authorityScope()"),
        "activation authority still overwrites its only copy in place");
    for (const QString &token : {
             QStringLiteral("slot-a/v1"),
             QStringLiteral("slot-b/v1")}) {
        valid &= requireContains(
            activationJournalAdapter, token,
            "activation authority lacks A/B slots or legacy migration");
    }
    // 只有激活日志有迁移前的单槽授权可以采纳；采纳逻辑本身在共享层。
    valid &= requireContains(
        activationJournalAdapter,
        QStringLiteral("value.legacyScope = QString::fromLatin1(kAuthorityScope);"),
        "activation authority no longer adopts its legacy single-slot envelope");
    valid &= requireContains(
        secureSlotAdapter,
        QStringLiteral("selection.legacyPending"),
        "the shared authority slot adapter dropped legacy migration");
    // 未配置作用域必须直接拒绝，而不是回落到某个默认位置：一个漏填的门面会静默
    // 共用别人的授权信封。
    valid &= requireContains(
        secureSlotAdapter,
        QStringLiteral("secure-authority-slot-scopes-unconfigured"),
        "an unconfigured secure storage scope set falls back to a default location");
    valid &= requireContains(
        cmake,
        QStringLiteral("secure_storage_authority_slot_adapter"),
        "the shared secure storage authority slot adapter is absent from CTest");
    valid &= requireContains(
        authoritySlots,
        QStringLiteral("both-corrupt"),
        "two corrupt authority slots could degrade to an empty authority");
    valid &= requireContains(
        authoritySlots,
        QStringLiteral("corrupt-without-peer"),
        "a corrupt authority slot without a peer could degrade to empty");
    valid &= requireContains(
        authoritySlots,
        QStringLiteral("generation-conflict"),
        "conflicting same-generation authority slots could be accepted");
    valid &= requireContains(
        authoritySlots,
        QStringLiteral("selection.writeSlot = newestIsA ? AuthoritySlotName::SlotB"),
        "authority publication does not always target the peer slot");
    valid &= requireContains(
        authoritySlots,
        QStringLiteral("AuthoritySlotSelectionState::Unavailable"),
        "a locked authority slot backend could be read as first install");
    // 共享的槽位发布层被两个子系统使用，因此域分离本身是安全属性：模式串与摘要域
    // 都由调用方给出，未配置的域必须直接拒绝而不是回落到某个默认格式。
    valid &= requireContains(
        authoritySlots,
        QStringLiteral("!= QString::fromLatin1(domain.frameSchema)"),
        "authority slot frames are not bound to their caller's domain");
    valid &= requireContains(
        authoritySlots,
        QStringLiteral("QByteArray input = domain.digestDomain;"),
        "the authority slot digest is not domain separated");
    valid &= requireContains(
        authoritySlots,
        QStringLiteral("authority-slot-domain-invalid"),
        "an unconfigured authority slot domain falls back to a default format");
    // 每个子系统的域常量参与已持久化的字节，因此它们必须留在各自的门面里。
    valid &= requireContains(
        activationSlotDomain,
        QStringLiteral("aegisy-companion-activation-journal-authority-slot/0.1"),
        "the activation slot schema is no longer pinned");
    valid &= requireContains(
        activationSlotDomain,
        QStringLiteral("activation-authority-slot-"),
        "activation slot failures are no longer attributable to activation");
    // 复核记录授权同样必须双槽发布：它的 HMAC 密钥不存在于任何其他位置。发布顺序
    // 由共享层持有，门面只保留自己被持久化的作用域与域串。
    for (const QString &token : {
             QStringLiteral("extensions/review-ledger-authority/slot-a/v1"),
             QStringLiteral("extensions/review-ledger-authority/slot-b/v1"),
             QStringLiteral("aegisy-extension-review-ledger-authority-slot/0.1"),
             QStringLiteral(
                 "aegisy-extension-review-ledger-authority-slot-digest/0.1"),
             QStringLiteral("extension-review-secure")}) {
        valid &= requireContains(
            reviewLedgerAdapter, token,
            "the review ledger authority adapter lost its own persisted domain");
    }
    // 启用授权的作用域与域串必须与复核记录完全不同：把一份复核授权搬进启用授权的
    // 位置等于把"我看过这份内容"变成"我要求运行这份内容"。
    for (const QString &token : {
             QStringLiteral("extensions/enablement-ledger-authority/slot-a/v1"),
             QStringLiteral("extensions/enablement-ledger-authority/slot-b/v1"),
             QStringLiteral(
                 "aegisy-extension-enablement-ledger-authority-slot/0.1"),
             QStringLiteral(
                 "aegisy-extension-enablement-ledger-authority-slot-digest/0.1"),
             QStringLiteral("extension-enablement-secure")}) {
        valid &= requireContains(
            enablementLedgerAdapter, token,
            "the enablement ledger authority adapter lost its own persisted domain");
    }
    valid &= requireAbsent(
        enablementLedgerAdapter,
        QStringLiteral("review-ledger"),
        "the enablement ledger authority reuses the review scope namespace");
    valid &= requireAbsent(
        enablementLedgerAdapter,
        QStringLiteral("value.legacyScope ="),
        "the enablement ledger authority adopts a legacy envelope it never wrote");
    valid &= requireAbsent(
        reviewLedgerAdapter,
        QStringLiteral("value.legacyScope ="),
        "the review ledger authority adopts a legacy envelope it never wrote");
    // 适配器只搬运字节：既不判定启用，也不执行任何东西。
    valid &= requireAbsent(
        enablementLedgerAdapter,
        QStringLiteral("effectiveEnabled"),
        "the enablement ledger authority adapter grants enablement authority");
    valid &= requireAbsent(
        enablementLedgerAdapter,
        QStringLiteral("QProcess"),
        "the enablement ledger authority adapter can execute a subprocess");
    valid &= requireAbsent(
        secureSlotAdapter,
        QStringLiteral("effectiveEnabled"),
        "the shared authority slot adapter grants enablement authority");
    valid &= requireAbsent(
        secureSlotAdapter,
        QStringLiteral("QProcess"),
        "the shared authority slot adapter can execute a subprocess");
    // 共享层不得内置任何子系统的域：那等于让一个漏填的门面继承别人的授权格式。
    for (const QString &token : {
             QStringLiteral("review-ledger-authority"),
             QStringLiteral("enablement-ledger-authority"),
             QStringLiteral("activation-journal-authority")}) {
        valid &= requireAbsent(
            secureSlotAdapter, token,
            "the shared authority slot adapter hardcodes a subsystem scope");
    }
    // 复核授权不得共用激活日志的作用域或迁移路径：那会让两个子系统的授权互换。
    valid &= requireAbsent(
        reviewLedgerAdapter,
        QStringLiteral("companion/activation-journal-authority"),
        "the review ledger authority shares the activation journal scope");
    valid &= requireAbsent(
        reviewLedgerAdapter,
        QStringLiteral("legacyPending"),
        "the review ledger authority adopts a legacy single-slot envelope");
    // 适配器只搬运字节：既不解析复核记录，也不判定信任或启用。
    valid &= requireAbsent(
        reviewLedgerAdapter,
        QStringLiteral("ExtensionReviewPin"),
        "the review ledger authority adapter inspects review pins");
    valid &= requireAbsent(
        reviewLedgerAdapter,
        QStringLiteral("effectiveEnabled"),
        "the review ledger authority adapter grants enablement authority");
    valid &= requireContains(
        mainWindow,
        QStringLiteral("SecureStorageExtensionReviewLedgerAdapter"),
        "the extension center does not anchor review authority in secure storage");
    valid &= requireAbsent(
        authoritySlots,
        QStringLiteral("apiKey"),
        "authority slot publication contains a credential field");
    valid &= requireContains(
        mainWindowHeader,
        QStringLiteral("SecureStorageCompanionActivationJournalAdapter"),
        "activation journal is not anchored in platform secure storage");
    valid &= requireContains(
        activationWorkflow,
        QStringLiteral("配置事务已写入但无法认证读回"),
        "activation continues without an authenticated journal read-back");
    // 每次提交前先落盘意图，恢复才能区分"提交未发出"与"提交可能已生效"。
    valid &= requireOrdered(
        activationWorkflow,
        {QStringLiteral("CompanionActivationStage::FilesApplied"),
         QStringLiteral("CompanionActivationStage::GatewayCommitRequested"),
         QStringLiteral("m_gatewayManager->commitProfile(tool, gatewayTransactionId)"),
         QStringLiteral("CompanionActivationStage::GatewayCommitted"),
         QStringLiteral("CompanionActivationStage::ProfileCommitRequested"),
         QStringLiteral("m_profileManager->setActiveIndex(profileIndex)"),
         QStringLiteral("CompanionActivationStage::ProfileCommitted")},
        "activation commits without first persisting a commit-requested intent");
    valid &= requireContains(
        activationJournal,
        QStringLiteral("gateway-commit-requested"),
        "activation journal cannot record a requested gateway commit");
    valid &= requireContains(
        activationJournal,
        QStringLiteral("profile-commit-requested"),
        "activation journal cannot record a requested profile commit");
    const QString activationRecovery = sourceRange(
        mainWindow,
        QStringLiteral("void MainWindow::recoverPendingActivation()"),
        QStringLiteral("void MainWindow::runReviewedActivationRecovery()"));
    valid &= requireContains(
        activationRecovery,
        QStringLiteral("if (record.stage == CompanionActivationStage::FilesApplied)"),
        "recovery cannot deterministically roll back an unrequested commit");
    valid &= requireContains(
        activationRecovery,
        QStringLiteral("CompanionActivationStage::GatewayCommitRequested"),
        "recovery does not distinguish a requested gateway commit");
    valid &= requireContains(
        activationWorkflow,
        QStringLiteral("if (m_profileManager->isActive(profileIndex))"),
        "a failed profile commit can compensate over an already active candidate");
    // RecoveryRequired 必须有出口：一个经过确认、重新对齐当前状态的显式动作。
    const QString reviewedRecovery = sourceRange(
        mainWindow,
        QStringLiteral("void MainWindow::runReviewedActivationRecovery()"),
        QStringLiteral("void MainWindow::discardPendingProfileReplacement()"));
    valid &= require(
        !reviewedRecovery.isEmpty(),
        "the reviewed activation recovery action is missing");
    valid &= requireOrdered(
        reviewedRecovery,
        {QStringLiteral("if (!m_activationRecoveryRequired)"),
         QStringLiteral("m_activationJournal->load()"),
         QStringLiteral("QMessageBox::question"),
         QStringLiteral("m_toolManager->rollbackPreparedConfiguration(record.receipt)"),
         QStringLiteral("m_activationJournal->clear(record.identity"),
         QStringLiteral("m_activationRecoveryRequired = false;")},
        "reviewed recovery does not re-establish a verified state before clearing");
    valid &= requireContains(
        reviewedRecovery,
        QStringLiteral("m_gatewayManager->configureProfile(tool, active.key)"),
        "reviewed recovery leaves the gateway without the active profile");
    valid &= requireContains(
        reviewedRecovery,
        QStringLiteral("m_gatewayManager->removeProfile(tool)"),
        "reviewed recovery cannot clear a gateway profile it must not keep");
    valid &= requireAbsent(
        reviewedRecovery,
        QStringLiteral("m_profileManager->setActiveIndex"),
        "reviewed recovery infers a commit outcome instead of re-aligning state");
    valid &= requireContains(
        mainWindow,
        QStringLiteral("m_activationRecoveryButton->setVisible(m_activationRecoveryRequired)"),
        "the reviewed recovery entry point is not surfaced to the operator");
    valid &= requireContains(
        mainWindow,
        QStringLiteral("&MainWindow::runReviewedActivationRecovery"),
        "the reviewed recovery action is not connected to any control");
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
             QStringLiteral("extension_compatibility_policy"),
             QStringLiteral("extension_trust_policy"),
             QStringLiteral("extension_review_ledger"),
             QStringLiteral("extension_review_ledger_store"),
             QStringLiteral("extension_review_workflow"),
             QStringLiteral("extension_review_presentation"),
             QStringLiteral("extension_center_read_only")}) {
        valid &= requireContains(cmake, testName,
                                 "strict extension source/UI test is absent from CTest");
    }

    // 兼容性必须来自可核查的宿主证据，不能由来源自我声明，也不能因为"看起来没
    // 问题"而判定兼容。判定顺序固定：确定不兼容优先于证据不足，否则一个确定的
    // 拒绝会被降级成"未知"。
    valid &= requireOrdered(
        compatibilityPolicy,
        {QStringLiteral("extension-capability-not-granted"),
         QStringLiteral("extension-version-unreadable"),
         QStringLiteral("if (record.kind == ExtensionKind::CodexPlugin)"),
         QStringLiteral("codex-plugin-host-version-unknown"),
         QStringLiteral("return verdict(ExtensionCompatibilityState::Compatible")},
        "compatibility evaluation does not reject before falling back to unknown");
    valid &= requireContains(
        compatibilityPolicy,
        QStringLiteral("codex-plugin-host-version-unreadable"),
        "corrupt Codex host version evidence is not distinguished");
    valid &= requireContains(
        compatibilityPolicy,
        QStringLiteral("codex-plugin-version-missing"),
        "an unversioned Codex plugin can still be judged compatible");
    // 只读授权：进程执行与任何写入能力都不在授予集合内。
    valid &= requireAbsent(
        compatibilityPolicy,
        QStringLiteral("QStringLiteral(\"process\")"),
        "the compatibility host grants process execution");
    valid &= requireAbsent(
        compatibilityPolicy,
        QStringLiteral("filesystem-write"),
        "the compatibility host grants filesystem writes");
    // 判定兼容不等于授权。
    valid &= requireAbsent(
        compatibilityPolicy,
        QStringLiteral("record.effectiveEnabled = "),
        "compatibility evaluation grants enablement authority");
    valid &= requireAbsent(
        compatibilityPolicy,
        QStringLiteral("record.trust = "),
        "compatibility evaluation grants trust");
    valid &= requireContains(
        extensionCoordinator,
        QStringLiteral("ExtensionCompatibilityPolicy::apply(&snapshot.records, inputs.host)"),
        "the unified inventory does not evaluate compatibility");
    // 注册表仍然是最终闸门：启用必须同时满足已核验与兼容。
    valid &= requireContains(
        extensionRegistry,
        QStringLiteral("record.trust == ExtensionTrustState::Verified"),
        "extension enablement no longer requires verified trust");
    valid &= requireContains(
        extensionRegistry,
        QStringLiteral("record.effectiveEnabled && !enabledAllowed"),
        "extension enablement no longer requires verified+compatible evidence");
    // 宿主版本证据只能来自本机实际检测结果，不能凭空构造。
    valid &= requireContains(
        mainWindow,
        QStringLiteral("inputs.host.codexVersion ="),
        "the extension center supplies no Codex host version evidence");
    valid &= requireContains(
        mainWindow,
        QStringLiteral("ExtensionCompatibilityPolicy::defaultGrantedCapabilities()"),
        "the extension center does not use the read-only granted capability set");
    // 来源不再自我声明兼容结论，也从不自我声明信任。
    for (const QString &source : {codexPluginInventory, skillExtensionInventory,
                                  mcpInventory}) {
        valid &= requireAbsent(
            source,
            QStringLiteral("ExtensionCompatibilityState::Compatible"),
            "an extension source still asserts its own compatibility");
        valid &= requireAbsent(
            source,
            QStringLiteral("ExtensionTrustState::Verified"),
            "an extension source still asserts its own trust");
    }

    // 信任只能来自针对确切内容的人工复核。判定顺序固定：不可核查的记录与不合法
    // 的复核记录先于匹配被拒绝，冲突先于匹配被判定，否则追加一条记录即可通过。
    valid &= requireOrdered(
        trustPolicy,
        {QStringLiteral("extension-record-unverifiable"),
         QStringLiteral("extension-review-store-oversized"),
         QStringLiteral("extension-review-pin-malformed"),
         QStringLiteral("if (candidates > 1)"),
         QStringLiteral("extension-review-conflict"),
         QStringLiteral("extension-not-reviewed"),
         QStringLiteral("extension-review-content-drift"),
         QStringLiteral("extension-review-source-drift"),
         QStringLiteral("return verdict(ExtensionTrustState::Verified")},
        "trust evaluation does not fail closed before matching a review pin");
    // 复核绑定的是确切内容与来源，任何一项漂移都让复核失效。
    valid &= requireContains(
        trustPolicy,
        QStringLiteral("match->contentIdentity != record.contentIdentity"),
        "trust survives extension content drift");
    valid &= requireContains(
        trustPolicy,
        QStringLiteral("match->sourceIdentity != record.sourceIdentity"),
        "trust survives extension source drift");
    valid &= requireContains(
        trustPolicy,
        QStringLiteral("pin.kind != record.kind || pin.id != record.id"),
        "review pins are not bound to both kind and id");
    // 授予信任不等于授权启用，也不改变兼容性判定。
    valid &= requireAbsent(
        trustPolicy,
        QStringLiteral("record.effectiveEnabled = "),
        "trust evaluation grants enablement authority");
    valid &= requireAbsent(
        trustPolicy,
        QStringLiteral("record.compatibility = "),
        "trust evaluation overwrites a compatibility verdict");
    valid &= requireContains(
        extensionCoordinator,
        QStringLiteral("ExtensionTrustPolicy::apply(&snapshot.records, inputs.reviewPins)"),
        "the unified inventory does not evaluate trust");

    // 启用授权与复核记录一样绑定确切内容：内容或来源被替换后授权必须失效，否则一份
    // 被替换过的内容会直接继承前一份内容的启用授权。
    valid &= requireContains(
        enablementPolicy,
        QStringLiteral("match->contentIdentity != record.contentIdentity"),
        "enablement survives extension content drift");
    valid &= requireContains(
        enablementPolicy,
        QStringLiteral("match->sourceIdentity != record.sourceIdentity"),
        "enablement survives extension source drift");
    valid &= requireContains(
        enablementPolicy,
        QStringLiteral("grant.kind != record.kind || grant.id != record.id"),
        "enablement grants are not bound to both kind and id");
    // 同一扩展存在多条授权时判定冲突，而不是任取一条匹配的。
    valid &= requireContains(
        enablementPolicy,
        QStringLiteral("candidates > 1"),
        "a matching enablement grant can be selected out of a conflicting set");
    // 授权本身不能绕过复核、兼容性与安装三道门禁。
    valid &= requireContains(
        enablementPolicy,
        QStringLiteral("record.trust != ExtensionTrustState::Verified"),
        "an enablement grant bypasses human review");
    valid &= requireContains(
        enablementPolicy,
        QStringLiteral("record.compatibility != ExtensionCompatibilityState::Compatible"),
        "an enablement grant bypasses compatibility evidence");
    valid &= requireContains(
        enablementPolicy,
        QStringLiteral("!record.installed"),
        "an enablement grant applies to an absent extension");
    // 授予启用不改写信任与兼容性判定，两者仍由各自的策略层负责。
    valid &= requireAbsent(
        enablementPolicy,
        QStringLiteral("record.trust = "),
        "enablement evaluation grants trust");
    valid &= requireAbsent(
        enablementPolicy,
        QStringLiteral("record.compatibility = "),
        "enablement evaluation overwrites a compatibility verdict");
    // 判定启用不等于安装、执行或改写工具配置。
    for (const QString &needle : {QStringLiteral("QProcess"),
                                 QStringLiteral("QFile"),
                                 QStringLiteral("QSettings")}) {
        valid &= requireAbsent(
            enablementPolicy, needle,
            "enablement evaluation reaches disk, settings, or process execution");
    }
    // 产品路径尚未提供任何启用授权，因此每一条记录都保持未启用；启用授权的持久化与
    // UI 仍未接入，`0.4` 因此保持未勾选。
    valid &= requireAbsent(
        mainWindow,
        QStringLiteral("ExtensionEnablementPolicy"),
        "the extension center already grants extension enablement");
    valid &= requireAbsent(
        extensionCenter,
        QStringLiteral("ExtensionEnablementPolicy"),
        "the extension center dialog already grants extension enablement");
    valid &= requireAbsent(
        extensionCoordinator,
        QStringLiteral("ExtensionEnablementPolicy"),
        "the unified inventory already applies enablement grants");

    // 复核证据与启用授权共用一套认证编解码，因此这些性质现在由共享层持有，pin 也
    // 必须跟到共享层：留在门面上只会守住一层转换代码。MAC 联合覆盖代号与整个集合，
    // 只覆盖单条条目会让追加、删除或重排都无法被发现。
    valid &= requireContains(
        evidenceLedger,
        QStringLiteral("append(&input, QByteArray::number(generation))"),
        "the evidence ledger MAC does not cover the generation");
    valid &= requireContains(
        evidenceLedger,
        QStringLiteral("append(&input, QByteArray::number(static_cast<qint64>(entries.size())))"),
        "the evidence ledger MAC does not cover the entry count");
    valid &= requireContains(
        evidenceLedger,
        QStringLiteral("CRYPTO_memcmp"),
        "the evidence ledger does not compare MACs in constant time");
    // 结构校验先于认证完成，但认证必须在返回任何条目之前通过。
    valid &= requireOrdered(
        evidenceLedger,
        {QStringLiteral("\"-limit\""),
         QStringLiteral("entryCode(domain, \"invalid\")"),
         QStringLiteral("entryCode(domain, \"duplicate\")"),
         QStringLiteral("code(domain, \"mac-mismatch\")"),
         QStringLiteral("ExtensionEvidenceLedgerState::Ready")},
        "the evidence ledger returns entries before authenticating them");
    // 反降级：只有空输入能得出 Empty，损坏与不可读各有独立结论。
    valid &= requireContains(
        evidenceLedger,
        QStringLiteral("ExtensionEvidenceLedgerState::Empty"),
        "the evidence ledger cannot report an absent payload");
    valid &= requireContains(
        evidenceLedger,
        QStringLiteral("code(domain, \"key-unavailable\")"),
        "an unusable evidence ledger key does not resolve to unavailable");
    // 域分隔是共享层的安全性质：模式串、MAC 域与身份域都进入被持久化的字节，且未
    // 配置的域被直接拒绝而不是退回默认格式。否则一份复核记录的字节就能被移动到启用
    // 授权的位置，把"我看过这份内容"变成"我要求运行这份内容"。
    valid &= requireContains(
        evidenceLedger,
        QStringLiteral("QByteArray input = domain.macDomain"),
        "the evidence ledger MAC does not bind the caller domain");
    valid &= requireContains(
        evidenceLedger,
        QStringLiteral("QByteArray input = domain.identityDomain"),
        "the evidence ledger identity does not bind the caller domain");
    valid &= requireContains(
        evidenceLedger,
        QStringLiteral("!= domain.schema"),
        "the evidence ledger accepts a foreign schema");
    valid &= requireContains(
        evidenceLedger,
        QStringLiteral("if (!domain.configured()) return {}"),
        "an unconfigured evidence ledger domain falls back to a default format");
    // 两类证据的域常量必须彼此不同，否则它们的字节可以互换。
    valid &= requireContains(
        reviewLedger,
        QStringLiteral("aegisy-extension-review-ledger-hmac/0.1"),
        "the review ledger MAC domain changed");
    valid &= requireContains(
        enablementLedger,
        QStringLiteral("aegisy-extension-enablement-ledger-hmac/0.1"),
        "the enablement ledger MAC domain changed");
    valid &= requireAbsent(
        enablementLedger,
        QStringLiteral("aegisy-extension-review-ledger"),
        "the enablement ledger reuses the review evidence domain");
    // 启用授权的记录层同样只解析与认证，不获得任何启用、写入或持久化授权。
    valid &= requireAbsent(
        enablementLedger,
        QStringLiteral("effectiveEnabled"),
        "the enablement ledger grants enablement directly");
    valid &= requireAbsent(
        enablementLedger,
        QStringLiteral("QFile"),
        "the enablement ledger performs its own persistence");
    valid &= requireAbsent(
        enablementLedger,
        QStringLiteral("QSettings"),
        "the enablement ledger performs its own persistence");
    // 产品路径尚未读写启用授权载荷，因此每一条记录都保持未启用。
    valid &= requireAbsent(
        mainWindow,
        QStringLiteral("ExtensionEnablementLedger"),
        "the extension center already reads enablement grants");
    valid &= requireAbsent(
        extensionCenter,
        QStringLiteral("ExtensionEnablementLedger"),
        "the extension center dialog already reads enablement grants");
    // 记录层只解析与认证，不获得任何启用、写入或持久化授权。
    valid &= requireAbsent(
        reviewLedger,
        QStringLiteral("effectiveEnabled"),
        "the review ledger grants enablement authority");
    valid &= requireAbsent(
        reviewLedger,
        QStringLiteral("QFile"),
        "the review ledger performs its own persistence");
    valid &= requireAbsent(
        reviewLedger,
        QStringLiteral("ExtensionTrustState::Verified"),
        "the review ledger decides trust instead of carrying evidence");
    // 载荷层不得成为产品路径上的复核来源，直到人工复核流程存在。
    valid &= requireContains(
        mainWindow,
        QStringLiteral("ExtensionReviewLedgerStore store(&authority, &settings)"),
        "the extension center does not load the review ledger");

    // 持久化被拆成两半：授权（密钥与已提交代号）在安全存储，载荷字节在 QSettings。
    // 密钥绝不能被写进普通设置里。共享层负责搬字节，各自的门面只持有自己的域。
    valid &= requireContains(
        evidenceLedgerStore,
        QStringLiteral("m_settings->setValue(domain.recordKey, bytes)"),
        "the evidence ledger store does not persist its payload in settings");
    valid &= requireAbsent(
        reviewLedgerStore,
        QStringLiteral("m_settings->setValue(QStringLiteral(\"extensions/review-ledger/key"),
        "the review ledger MAC key is persisted outside secure storage");
    valid &= requireAbsent(
        evidenceLedgerStore,
        QStringLiteral("m_settings->setValue(domain.authoritySchema"),
        "the evidence ledger store writes authority material into settings");
    // 预留必须先于载荷写入落盘：否则被打断的发布只能靠推断，而不是靠磁盘上的事实。
    valid &= requireOrdered(
        evidenceLedgerStore,
        {QStringLiteral("code(domain, \"reserve-failed\")"),
         QStringLiteral("m_settings->setValue(domain.recordKey, bytes)"),
         QStringLiteral("code(domain, \"commit-unresolved\")")},
        "the evidence ledger store writes its payload before reserving it");
    // 反降级：删掉任意一半都不能读成"从未记录过"，重放旧载荷也不能复活已撤销的记录。
    valid &= requireContains(
        evidenceLedgerStore,
        QStringLiteral("code(domain, \"record-without-authority\")"),
        "an orphaned evidence payload degrades to empty");
    valid &= requireContains(
        evidenceLedgerStore,
        QStringLiteral("code(domain, \"record-deleted\")"),
        "a deleted evidence payload degrades to empty");
    valid &= requireContains(
        evidenceLedgerStore,
        QStringLiteral("code(domain, \"record-superseded\")"),
        "a replayed evidence payload is accepted as current");
    valid &= requireContains(
        evidenceLedgerStore,
        QStringLiteral("ExtensionEvidenceLedgerStoreState::OutcomeUnknown"),
        "an unresolved publication resolves to a usable evidence set");
    // 并发修改必须靠代号比较解决，而不是后写覆盖。
    valid &= requireContains(
        evidenceLedgerStore,
        QStringLiteral("code(domain, \"generation-conflict\")"),
        "the evidence ledger store overwrites concurrent commits");
    // 未配置的域被拒绝，而不是退回某个默认格式——否则一个漏填的门面会静默共用别人的域。
    valid &= requireContains(
        evidenceLedgerStore,
        QStringLiteral("extension-evidence-store-domain-unconfigured"),
        "the evidence ledger store falls back to a default domain");
    // 两个门面的持久化域必须完全不同：授权信封、载荷位置与诊断前缀都不共用。否则一份
    // 复核证据可以被搬到启用授权的位置，把"我看过这份内容"变成"我要求运行这份内容"。
    valid &= requireContains(
        reviewLedgerStore,
        QStringLiteral("aegisy-extension-review-ledger-authority/0.1"),
        "the review ledger store lost its own authority schema");
    valid &= requireContains(
        reviewLedgerStore,
        QStringLiteral("extensions/review-ledger/record"),
        "the review ledger store lost its own payload key");
    valid &= requireContains(
        enablementLedgerStore,
        QStringLiteral("aegisy-extension-enablement-ledger-authority/0.1"),
        "the enablement ledger store lost its own authority schema");
    valid &= requireContains(
        enablementLedgerStore,
        QStringLiteral("extensions/enablement-ledger/record"),
        "the enablement ledger store lost its own payload key");
    valid &= requireAbsent(
        enablementLedgerStore,
        QStringLiteral("aegisy-extension-review-ledger"),
        "the enablement ledger store reuses the review persistence domain");
    // 持久化层同样不得获得启用授权，也不得自行判定信任或兼容。
    valid &= requireAbsent(
        reviewLedgerStore,
        QStringLiteral("effectiveEnabled"),
        "the review ledger store grants enablement authority");
    valid &= requireAbsent(
        reviewLedgerStore,
        QStringLiteral("ExtensionTrustState::Verified"),
        "the review ledger store decides trust instead of carrying evidence");
    valid &= requireAbsent(
        evidenceLedgerStore,
        QStringLiteral("effectiveEnabled"),
        "the evidence ledger store grants enablement authority");
    valid &= requireAbsent(
        enablementLedgerStore,
        QStringLiteral("effectiveEnabled"),
        "the enablement ledger store grants enablement authority");
    valid &= requireAbsent(
        enablementLedgerStore,
        QStringLiteral("ExtensionEnablementPolicy::"),
        "the enablement ledger store decides enablement instead of carrying grants");
    // 启用授权还没有任何生产者：产品路径不得构造启用授权的持久化，否则在权限、审批、
    // 沙箱与恢复门禁完成之前就会有记录被真正启用。
    valid &= requireAbsent(
        mainWindow,
        QStringLiteral("ExtensionEnablementLedgerStore"),
        "the main window persists enablement grants before the gates exist");
    valid &= requireAbsent(
        extensionCenter,
        QStringLiteral("ExtensionEnablementLedgerStore"),
        "the extension center persists enablement grants before the gates exist");
    valid &= requireAbsent(
        reviewController,
        QStringLiteral("ExtensionEnablementLedgerStore"),
        "the review controller persists enablement grants before the gates exist");
    valid &= requireContains(
        reviewController,
        QStringLiteral("store->replace(plan.pins, plan.expectedGeneration"),
        "the review controller does not persist through generation CAS");

    // 启用授权的规划层与复核规划层同构：它只产出"提交后的完整集合"与一个 CAS 代号，
    // 自身不做任何持久化，也不写 effectiveEnabled。
    valid &= requireAbsent(
        enablementWorkflow,
        QStringLiteral("m_settings"),
        "the enablement workflow performs its own persistence");
    valid &= requireAbsent(
        enablementWorkflow,
        QStringLiteral("SecureStorage"),
        "the enablement workflow reaches into secure storage directly");
    valid &= requireAbsent(
        enablementWorkflow,
        QStringLiteral("effectiveEnabled"),
        "the enablement workflow writes effective enablement itself");
    valid &= requireAbsent(
        enablementWorkflow,
        QStringLiteral("QProcess"),
        "the enablement workflow executes something");
    // 授予必须与人工在屏幕上看到的确切内容一致：漂移必须失败，而不是改判到当前内容。
    valid &= requireContains(
        enablementWorkflow,
        QStringLiteral("target->contentIdentity != request.reviewedContentIdentity"),
        "granting does not compare the reviewed content against the current record");
    valid &= requireContains(
        enablementWorkflow,
        QStringLiteral("target->sourceIdentity != request.reviewedSourceIdentity"),
        "granting does not compare the reviewed source against the current record");
    // 复核与兼容必须在规划时就成立。否则一条已认证的授权会一直留在账本里，等到复核
    // 出现的那一刻自动生效——那等于预先授权将来的内容。
    valid &= requireContains(
        enablementWorkflow,
        QStringLiteral("target->trust != ExtensionTrustState::Verified"),
        "granting does not require a reviewed record");
    valid &= requireContains(
        enablementWorkflow,
        QStringLiteral("target->compatibility != ExtensionCompatibilityState::Compatible"),
        "granting does not require a compatible record");
    valid &= requireContains(
        enablementWorkflow,
        QStringLiteral("extension-enablement-target-not-installed"),
        "granting does not require an installed record");
    valid &= requireContains(
        enablementWorkflow,
        QStringLiteral("extension-enablement-target-ambiguous"),
        "granting picks one of several records with the same identity");
    // 读不出当前集合时不能规划：那会把不完整集合当成完整集合提交。
    valid &= requireContains(
        enablementWorkflow,
        QStringLiteral("extension-enablement-ledger-unusable"),
        "planning proceeds against an unreadable grant ledger");
    valid &= requireContains(
        enablementWorkflow,
        QStringLiteral("extension-enablement-ledger-grant-invalid"),
        "an invalid existing grant is laundered into a new commit");
    // 停用只依据 (kind, id)：被篡改过的扩展也必须能撤销授权。
    valid &= requireContains(
        enablementWorkflow,
        QStringLiteral("if (grant.kind == request.kind && grant.id == request.id) continue;"),
        "revocation is not keyed on kind and id alone");
    // 规划层还没有任何调用者：产品路径不得规划启用授权，直到权限、审批、沙箱与恢复
    // 门禁完成。
    valid &= requireAbsent(
        mainWindow,
        QStringLiteral("ExtensionEnablementWorkflow"),
        "the main window plans enablement grants before the gates exist");
    valid &= requireAbsent(
        extensionCenter,
        QStringLiteral("ExtensionEnablementWorkflow"),
        "the extension center plans enablement grants before the gates exist");
    valid &= requireAbsent(
        reviewController,
        QStringLiteral("ExtensionEnablementWorkflow"),
        "the review controller plans enablement grants before the gates exist");

    // 启用控制器把规划与持久化接在一起，但**不**把授权喂给清单协调器：协调器会据此
    // 写 effectiveEnabled，也就是真正运行扩展内容的权限，而那道门在权限、审批、沙箱
    // 与恢复门禁完成之前必须保持关闭。判定只作为与记录一一对应的投影返回。
    valid &= requireAbsent(
        enablementController,
        QStringLiteral("ExtensionEnablementPolicy::apply"),
        "the enablement controller writes enablement onto the records");
    valid &= requireAbsent(
        enablementController,
        QStringLiteral(".effectiveEnabled ="),
        "the enablement controller opens effective enablement");
    valid &= requireAbsent(
        enablementController,
        QStringLiteral("bound.enablementGrants"),
        "the enablement controller feeds grants into the inventory coordinator");
    valid &= requireContains(
        enablementController,
        QStringLiteral("ExtensionEnablementPolicy::evaluate(record, snapshot.grants)"),
        "the enablement controller does not project the enablement decision");
    // 提交之后必须重新读取：只有重新读到的字节才是真正生效的授权。
    valid &= requireOrdered(
        enablementController,
        {QStringLiteral("store->replace(plan.grants, plan.expectedGeneration"),
         QStringLiteral("collectWithLedger(inputs, updated)")},
        "the enablement controller trusts the plan instead of re-reading");
    valid &= requireContains(
        enablementController,
        QStringLiteral("if (!plan.changed)"),
        "the enablement controller commits an unchanged grant set");
    valid &= requireContains(
        enablementController,
        QStringLiteral("extension-enablement-ledger-unusable"),
        "the enablement controller writes against an unreadable ledger");
    // 控制器同样还没有任何调用者。
    valid &= requireAbsent(
        mainWindow,
        QStringLiteral("ExtensionEnablementController"),
        "the main window drives enablement grants before the gates exist");
    valid &= requireAbsent(
        extensionCenter,
        QStringLiteral("ExtensionEnablementController"),
        "the extension center drives enablement grants before the gates exist");
    // 协调器不得获得授权输入：那会让每一次清单收集都可能写出生效启用。
    valid &= requireAbsent(
        extensionCoordinator,
        QStringLiteral("enablementGrants"),
        "the unified inventory accepts enablement grants");

    // 人工复核被翻译成"提交后的完整集合"，因此规划层自身不做任何持久化。
    valid &= requireAbsent(
        reviewWorkflow,
        QStringLiteral("m_settings"),
        "the review workflow performs its own persistence");
    valid &= requireAbsent(
        reviewWorkflow,
        QStringLiteral("SecureStorage"),
        "the review workflow reaches into secure storage directly");
    // 批准必须与人工在屏幕上看到的确切内容一致：漂移必须失败，而不是改判到当前内容。
    valid &= requireContains(
        reviewWorkflow,
        QStringLiteral("target->contentIdentity != request.reviewedContentIdentity"),
        "approval does not compare the reviewed content against the current record");
    valid &= requireContains(
        reviewWorkflow,
        QStringLiteral("extension-review-content-drift"),
        "content that changed after review is approved anyway");
    valid &= requireContains(
        reviewWorkflow,
        QStringLiteral("extension-review-source-drift"),
        "a reviewed extension may change source without losing approval");
    // 不存在、重复或未安装的目标都不能被批准：那等于预先授权将来出现的内容。
    valid &= requireContains(
        reviewWorkflow,
        QStringLiteral("extension-review-target-absent"),
        "an absent extension can be pre-approved");
    valid &= requireContains(
        reviewWorkflow,
        QStringLiteral("extension-review-target-ambiguous"),
        "an ambiguous inventory allows picking one record to approve");
    valid &= requireContains(
        reviewWorkflow,
        QStringLiteral("extension-review-target-not-installed"),
        "an uninstalled extension can be approved");
    // 当前集合读不出来时不能规划：提交一份不完整的集合会静默删除读不出来的复核。
    valid &= requireOrdered(
        reviewWorkflow,
        {QStringLiteral("extension-review-ledger-unusable"),
         QStringLiteral("extension-review-ledger-inconsistent"),
         QStringLiteral("extension-review-request-id-invalid"),
         QStringLiteral("extension-review-ledger-pin-invalid"),
         QStringLiteral("extension-review-ledger-conflict")},
        "the review workflow plans before adjudicating the stored set");
    // 撤销只依据 (kind, id)：被篡改过的扩展必须仍然能被移除。
    valid &= requireContains(
        reviewWorkflow,
        QStringLiteral("if (pin.kind == request.kind && pin.id == request.id) continue;"),
        "revocation requires the content to still match, stranding tampered reviews");
    // 集合已满时新增必须失败，而不是挤掉一条已有复核记录。
    valid &= requireContains(
        reviewWorkflow,
        QStringLiteral("extension-review-pin-limit"),
        "a full review set silently drops a pin to make room");
    // 规划层不授予启用权，也不得进入产品路径。
    valid &= requireAbsent(
        reviewWorkflow,
        QStringLiteral("effectiveEnabled"),
        "the review workflow grants enablement authority");
    valid &= requireAbsent(
        reviewWorkflow,
        QStringLiteral("ExtensionTrustState::Verified"),
        "the review workflow decides trust instead of producing evidence");
    valid &= requireContains(
        reviewController,
        QStringLiteral("ExtensionReviewWorkflow::plan"),
        "the review controller bypasses the drift/CAS workflow plan");

    // 人工复核的结论只能和呈现给人的内容一样可靠，因此不可信的磁盘文本必须先被
    // 判定为可安全展示。不可见与双向字符会让屏幕上的名称与实际字符串不一致。
    valid &= requireContains(
        displaySafety,
        QStringLiteral("category == QChar::Other_Format"),
        "authorization text may carry format characters into a prompt");
    valid &= requireContains(
        displaySafety,
        QStringLiteral("(code >= 0x2066 && code <= 0x2069)"),
        "authorization text may carry bidirectional isolates into a prompt");
    valid &= requireContains(
        displaySafety,
        QStringLiteral("(code >= 0x200b && code <= 0x200f) || code == 0xfeff"),
        "authorization text may carry zero-width characters into a prompt");
    // 超长文本必须整体拒绝：截断会让两个不同的扩展在屏幕上看起来完全一样。
    valid &= requireContains(
        displaySafety,
        QStringLiteral("if (value.isEmpty() || value.size() > maximum) return false;"),
        "over-long authorization text is truncated instead of rejected");
    valid &= requireAbsent(
        reviewPresentation,
        QStringLiteral("elidedText"),
        "review text is elided, so two extensions can render identically");
    // 人看到的摘要就是批准所绑定的摘要，否则漂移检测形同虚设。
    valid &= requireContains(
        reviewPresentation,
        QStringLiteral("prompt.reviewedSourceIdentity = record.sourceIdentity;"),
        "the prompt does not echo the exact source identity it displayed");
    valid &= requireContains(
        reviewPresentation,
        QStringLiteral("prompt.reviewedContentIdentity = record.contentIdentity;"),
        "the prompt does not echo the exact content identity it displayed");
    // 短摘要只用于展示，且必须同时保留头尾，避免构造出的前缀碰撞看起来一致。
    valid &= requireContains(
        displaySafety,
        QStringLiteral("return hex.left(8) + QStringLiteral(\"…\") + hex.right(8);"),
        "the displayed fingerprint drops one end of the digest");
    // 冒充、越权与未解决状态必须被显式标记，而不是静默展示成普通条目。
    for (const QString &warning : {
             QStringLiteral("ExtensionReviewWarning::NameMismatchesIdentifier"),
             QStringLiteral("ExtensionReviewWarning::CapabilityNotGranted"),
             QStringLiteral("ExtensionReviewWarning::CapabilityBeyondReadOnly"),
             QStringLiteral("ExtensionReviewWarning::CompatibilityUnresolved"),
             QStringLiteral("ExtensionReviewWarning::ContentChangedSinceReview")}) {
        valid &= requireContains(
            reviewPresentation, warning,
            "a review risk is not surfaced as an explicit warning");
    }
    // 只读边界：写入与执行类能力必须被标记，展示层自身不得授予任何权限。
    valid &= requireContains(
        displaySafety,
        QStringLiteral("QStringLiteral(\"git-mutation\"), QStringLiteral(\"filesystem-write\")"),
        "write and mutation capabilities are not flagged as beyond read-only");
    valid &= requireAbsent(
        reviewPresentation,
        QStringLiteral("effectiveEnabled"),
        "the review presentation grants enablement authority");
    valid &= requireAbsent(
        reviewPresentation,
        QStringLiteral("ExtensionTrustState::Verified"),
        "the review presentation decides trust instead of describing a record");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("ExtensionReviewPresentation::build"),
        "the extension center does not render spoof-resistant review prompts");

    // 共享的可展示性判定必须只有一份：两份副本会各自漂移，于是一个界面接受了另一个
    // 界面拒绝的双向覆盖字符，同一个扩展在两处呈现不同。
    for (const QString &token : {
             QStringLiteral("safeDisplayText"),
             QStringLiteral("hashIdentity"),
             QStringLiteral("fingerprint"),
             QStringLiteral("beyondReadOnly"),
             QStringLiteral("nameAgreesWithIdentifier")}) {
        valid &= requireContains(
            displaySafety, token,
            "the shared display safety layer lost a presentation guard");
    }
    for (const QString &source : {reviewPresentation, enablementPresentation}) {
        valid &= requireContains(
            source,
            QStringLiteral("extension_display_safety.h"),
            "a prompt keeps its own copy of the display safety rules");
        // 门面不得自己判定可展示性：任何本地的字符类别或码位检查都意味着又出现了
        // 一份会独立漂移的副本。
        for (const QString &token : {
                 QStringLiteral("0x200b"), QStringLiteral("0x2066"),
                 QStringLiteral("0xfeff"), QStringLiteral("QChar::Other_Format"),
                 QStringLiteral(".unicode()"), QStringLiteral(".trimmed()"),
                 QStringLiteral("QChar::Category")}) {
            valid &= requireAbsent(
                source, token,
                "a prompt re-implements the display safety rules locally");
        }
    }
    // 展示安全层只判定可展示性：它不授权、不判定信任、不执行任何东西。
    for (const QString &token : {
             QStringLiteral("effectiveEnabled"),
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("ExtensionTrustState::Verified")}) {
        valid &= requireAbsent(
            displaySafety, token,
            "the shared display safety layer holds authority beyond rendering");
    }

    // 启用提问与复核提问不同：复核问"有人看过这份内容吗"，启用问"你要让它运行吗"。
    // 后者是更强的授权，因此三道门禁必须在提问之前满足，否则界面会邀请人授权一件此刻
    // 无法生效的事，而那份授权会以已认证的形式留在账本里等门禁出现时自动生效。
    valid &= requireOrdered(
        enablementPresentation,
        {QStringLiteral("if (!record.installed) {"),
         QStringLiteral("ExtensionEnablementBlockReason::NotInstalled"),
         QStringLiteral("if (record.trust != ExtensionTrustState::Verified) {"),
         QStringLiteral("ExtensionEnablementBlockReason::TrustMissing"),
         QStringLiteral("if (record.compatibility != ExtensionCompatibilityState::Compatible) {"),
         QStringLiteral("ExtensionEnablementBlockReason::CompatibilityMissing")},
        "the enablement prompt does not gate on installed, reviewed, and compatible in order");
    // 缺少复核与缺少兼容必须是可区分的诊断：把前者显示成后者会让人以为换台机器就能
    // 运行一份从未被人看过的内容。
    valid &= requireContains(
        enablementPresentationHeader,
        QStringLiteral("enum class ExtensionEnablementBlockReason"),
        "the enablement block reason is not an explicit enumeration");
    // 授权当前不会让任何内容运行，界面必须说明，否则人以为自己刚刚开启了执行。
    valid &= requireContains(
        enablementPresentation,
        QStringLiteral("ExtensionEnablementWarning::GrantDoesNotExecuteYet"),
        "the enablement prompt does not disclose that a grant executes nothing yet");
    // 人看到的摘要就是授权所绑定的摘要，否则漂移检测形同虚设。
    for (const QString &token : {
             QStringLiteral("prompt.reviewedSourceIdentity = record.sourceIdentity;"),
             QStringLiteral("prompt.reviewedContentIdentity = record.contentIdentity;")}) {
        valid &= requireContains(
            enablementPresentation, token,
            "the enablement prompt does not echo the exact identity it displayed");
    }
    // 撤销永远可用：被篡改、被撤回复核、来源消失的扩展都必须仍然可以收回授权。
    const QString revocationBody = enablementPresentation.mid(
        enablementPresentation.indexOf(
            QStringLiteral("ExtensionEnablementPresentation::buildRevocation")));
    valid &= requireAbsent(
        revocationBody,
        QStringLiteral("ExtensionTrustState::Verified"),
        "revocation is gated on trust, so a tampered extension could never be revoked");
    valid &= requireAbsent(
        revocationBody,
        QStringLiteral("record->installed"),
        "revocation is gated on installation, so a removed extension keeps its grant");
    valid &= requireContains(
        enablementPresentation,
        QStringLiteral("prompt.targetAbsent = true;"),
        "revoking a vanished target is not distinguishable from revoking a listed one");
    // 呈现层不得授予启用或执行任何东西。
    for (const QString &token : {
             QStringLiteral(".effectiveEnabled ="),
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("ExtensionEnablementLedger")}) {
        valid &= requireAbsent(
            enablementPresentation, token,
            "the enablement presentation holds authority beyond rendering");
    }
    // 启用呈现还没有调用方：门禁完成前不得出现可点击的启用动作。
    for (const QString &source : {mainWindow, extensionCenter}) {
        valid &= requireAbsent(
            source,
            QStringLiteral("ExtensionEnablementPresentation"),
            "a grant action reached the product path before the gates exist");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_enablement_presentation"),
        "the enablement presentation is absent from CTest");

    // 审批门禁回答的是与呈现不同的问题：呈现决定"能不能问"，审批决定"这个回答是否
    // 构成授权"。伪造或过期的批准正是把工具输出里的一段文字变成"用户要求运行这份
    // 内容"的路径，因此批准必须与当时屏幕上的内容逐项对齐。
    valid &= requireContains(
        approvalPolicy,
        QStringLiteral("extension-approval-prompt-blocked"),
        "an approval against an ungated prompt is accepted");
    valid &= requireContains(
        approvalPolicy,
        QStringLiteral("extension-approval-prompt-unpresentable"),
        "an approval against unrenderable content is accepted");
    for (const QString &code : {
             QStringLiteral("extension-approval-content-drift"),
             QStringLiteral("extension-approval-source-drift"),
             QStringLiteral("extension-approval-target-mismatch"),
             QStringLiteral("extension-approval-identity-invalid")}) {
        valid &= requireContains(
            approvalPolicy, code,
            "an approval is not bound to the exact target it displayed");
    }
    // 批准的是"我看到了这些风险并接受"，因此披露集合与确认集合必须完全一致。
    for (const QString &code : {
             QStringLiteral("extension-approval-warning-undisclosed"),
             QStringLiteral("extension-approval-warning-unknown"),
             QStringLiteral("extension-approval-warning-duplicate")}) {
        valid &= requireContains(
            approvalPolicy, code,
            "an approval need not match the risks that were disclosed");
    }
    // 高风险必须逐次显式确认，并且不产生可复用规则。
    valid &= requireContains(
        approvalPolicy,
        QStringLiteral("extension-approval-confirmation-required"),
        "a high-risk approval can succeed without explicit confirmation");
    valid &= requireContains(
        approvalPolicy,
        QStringLiteral("&& !requiresConfirmation;"),
        "a high-risk approval can produce a reusable rule");
    // 未归类的风险必须默认要求确认：新增类别不应默认变成可批量放行的。
    valid &= requireContains(
        approvalPolicy,
        QStringLiteral("// 未知风险按需要确认处理"),
        "an unclassified risk category defaults to needing no confirmation");
    // 记住的规则不得比被批准的那份确切内容更宽：按名称或标识记住会让对一份内容的同意
    // 转移到从未被看过的另一份内容上。
    valid &= requireContains(
        approvalPolicyHeader,
        QStringLiteral("RememberForThisContent"),
        "the approval scope is not bound to exact content");
    valid &= requireAbsent(
        approvalPolicyHeader,
        QStringLiteral("RememberForThisExtension"),
        "a remembered approval rule is broader than the reviewed content");
    valid &= requireAbsent(
        approvalPolicyHeader,
        QStringLiteral("RememberAlways"),
        "a blanket approval rule exists");
    // 审批不启用、不持久化、不执行任何东西。
    for (const QString &token : {
             QStringLiteral(".effectiveEnabled ="),
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("SecureStorage"),
             QStringLiteral("Ledger")}) {
        valid &= requireAbsent(
            approvalPolicy, token,
            "the approval policy holds authority beyond judging a credential");
    }
    // 审批门禁还没有调用方：门禁完成前不得出现可点击的启用动作。
    for (const QString &source : {mainWindow, extensionCenter}) {
        valid &= requireAbsent(
            source,
            QStringLiteral("ExtensionApprovalPolicy"),
            "an approval path reached the product before the gates exist");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_approval_policy"),
        "the approval policy is absent from CTest");

    // 沙箱门禁回答的是与审批不同的问题:批准表达的是意图,沙箱是操作系统层面的强制。
    // 把意图当作强制,等于在没有围栏的地方宣布已经有围栏。因此当前构建的强制证据必须
    // 保持"未验证",而不是被写成乐观的常量。
    for (const QString &token : {
             QStringLiteral("evidence.filesystem = SandboxEnforcement::Enforced"),
             QStringLiteral("evidence.process = SandboxEnforcement::Enforced"),
             QStringLiteral("evidence.network = SandboxEnforcement::Enforced"),
             QStringLiteral("evidence.releaseGateSigned = true")}) {
        valid &= requireAbsent(
            sandboxGate, token,
            "the product claims sandbox enforcement it has never delivered");
    }
    // 已证实的策略绕过必须先于其他判断阻断可写通道。
    valid &= requireContains(
        sandboxGate,
        QStringLiteral("sandbox-escape-regression-open"),
        "a demonstrated policy bypass does not block the write-capable channel");
    // 强制齐备仍然不够:该平台的可写发布门禁报告必须已经签署。
    valid &= requireContains(
        sandboxGate,
        QStringLiteral("sandbox-release-gate-unsigned"),
        "verified enforcement alone can grant writes without a release gate");
    valid &= requireContains(
        sandboxGate,
        QStringLiteral("sandbox-enforcement-incomplete"),
        "an incomplete sandbox does not fall back to read-only");
    valid &= requireContains(
        sandboxGate,
        QStringLiteral("sandbox-platform-unsupported"),
        "a platform with no enforcement mechanism is not reported as unsupported");
    // 被沙箱拒绝的动作永远不得在沙箱之外自动重试,也不得被报告成模型失败。
    valid &= requireContains(
        sandboxGate,
        QStringLiteral("value.retryOutsideSandbox = false;"),
        "a sandbox denial may be retried outside the sandbox");
    valid &= requireContains(
        sandboxGate,
        QStringLiteral("value.attributableToModel = false;"),
        "a sandbox denial may be reported as a model failure");
    // 未归类的强制状态与权限级别都必须 fail closed。
    valid &= requireContains(
        sandboxGate,
        QStringLiteral("// 未知取值按未验证处理"),
        "an unclassified enforcement state defaults to enforced");
    valid &= requireContains(
        sandboxGate,
        QStringLiteral("// 未知权限按越出只读处理"),
        "an unclassified authority level defaults to read-only");
    // 沙箱门禁不执行任何东西、不持久化、不修改策略。
    for (const QString &token : {
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("SecureStorage"),
             QStringLiteral(".effectiveEnabled =")}) {
        valid &= requireAbsent(
            sandboxGate, token,
            "the sandbox gate holds authority beyond judging enforcement evidence");
    }
    // 沙箱门禁还没有调用方:门禁完成前不得出现任何写入、命令执行或 Git mutation 路径。
    for (const QString &source : {mainWindow, extensionCenter}) {
        valid &= requireAbsent(
            source,
            QStringLiteral("ExecutionSandboxGate"),
            "a write-capable path reached the product before the sandbox exists");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("execution_sandbox_gate"),
        "the sandbox gate is absent from CTest");

    // 恢复门禁回答的是当授权账本本身不可信时该怎么办。不可读的账本在没有恢复路径时是
    // 死胡同,但恢复自身必须只能减少授权:任何能产出非空授权集合的恢复路径都是一条制造
    // 同意的路径,比它试图修复的损坏更危险。
    valid &= requireContains(
        recoveryGate,
        QStringLiteral("plan.grants.clear();"),
        "recovery reconstructs grants nobody ever authorized");
    valid &= requireAbsent(
        recoveryGate,
        QStringLiteral("plan.grants = ledger.grants"),
        "recovery copies grants out of an untrusted ledger");
    valid &= requireAbsent(
        recoveryGate,
        QStringLiteral("plan.grants.append"),
        "recovery can produce a grant");
    // 可读的账本不得被恢复触碰:那会变成一条不经审批就撤销一切的路径。
    valid &= requireContains(
        recoveryGate,
        QStringLiteral("if (authoritative(ledger.state)) {"),
        "recovery can act on a readable ledger");
    valid &= requireContains(
        recoveryGate,
        QStringLiteral("extension-recovery-not-required"),
        "a healthy ledger is offered a recovery action");
    // 读不到内容与结果未知都不允许写入,而且不能互相降级。
    for (const QString &code : {
             QStringLiteral("extension-recovery-store-unavailable"),
             QStringLiteral("extension-recovery-outcome-unknown"),
             QStringLiteral("extension-recovery-evidence-invalid"),
             QStringLiteral("extension-recovery-blocked"),
             QStringLiteral("extension-recovery-reread-required")}) {
        valid &= requireContains(
            recoveryGate, code,
            "recovery collapses distinct ledger failures into one conclusion");
    }
    // 操作者确认的必须是当下真实的损坏,而不是界面上过期的结论。
    valid &= requireContains(
        recoveryGate,
        QStringLiteral("extension-recovery-assessment-stale"),
        "a confirmation for another conclusion can be replayed");
    valid &= requireContains(
        recoveryGate,
        QStringLiteral("extension-recovery-confirmation-required"),
        "recovery can withdraw every grant without explicit confirmation");
    valid &= requireContains(
        recoveryGate,
        QStringLiteral("extension-recovery-generation-stale"),
        "recovery can overwrite a concurrent grant");
    // 事务只能在提交后重新读取并验证之后清除。
    valid &= requireContains(
        recoveryGate,
        QStringLiteral("plan.clearsTransaction = false;"),
        "recovery closes the transaction before verifying the result");
    valid &= requireContains(
        recoveryGate,
        QStringLiteral("reread.state == ExtensionEnablementLedgerStoreState::Empty"),
        "a partial recovery can be mistaken for a completed one");
    // 未知的存储状态必须按不可读处理。
    valid &= requireContains(
        recoveryGate,
        QStringLiteral("// 未知状态按不可读处理"),
        "an unclassified store state defaults to authoritative");
    // 恢复门禁不读盘、不写盘、不执行任何东西。
    for (const QString &token : {
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("SecureStorage"),
             QStringLiteral(".effectiveEnabled ="),
             QStringLiteral("->replace(")}) {
        valid &= requireAbsent(
            recoveryGate, token,
            "the recovery gate holds authority beyond planning a withdrawal");
    }
    // 恢复门禁还没有调用方:门禁完成前不得出现可点击的启用或恢复动作。
    for (const QString &source : {mainWindow, extensionCenter}) {
        valid &= requireAbsent(
            source,
            QStringLiteral("ExtensionRecoveryGate"),
            "a recovery path reached the product before the gates exist");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_recovery_gate"),
        "the recovery gate is absent from CTest");

    // 准入门禁是四道门的合取。四道门分散在四个类型里时,漏查一道不会产生编译错误,也不会
    // 产生诊断——它只是让一份授权在缺少一项前提的情况下成立。因此四道门必须都在这一层
    // 被显式征询。
    for (const QString &gate : {
             QStringLiteral("ExtensionRecoveryGate::authoritative"),
             QStringLiteral("ExtensionApprovalPolicy::evaluate"),
             QStringLiteral("ExecutionSandboxGate::beyondReadOnly")}) {
        valid &= requireContains(
            admissionGate, gate,
            "admission does not consult every gate it depends on");
    }
    for (const QString &code : {
             QStringLiteral("extension-admission-ledger-unreadable"),
             QStringLiteral("extension-admission-sandbox-unenforced"),
             QStringLiteral("extension-admission-authority-insufficient")}) {
        valid &= requireContains(
            admissionGate, code,
            "admission cannot report which gate refused it");
    }
    // 审批的诊断原样透出,而不是被折叠成一个笼统的准入失败。
    valid &= requireContains(
        admissionGate,
        QStringLiteral("return refuse(approval.errorCode);"),
        "admission hides which approval requirement failed");
    // 所需强制级别读的是呈现给人的披露,而不是可被改写的能力列表。
    valid &= requireContains(
        admissionGate,
        QStringLiteral("ExtensionEnablementWarning::CapabilityBeyondReadOnly"),
        "the required enforcement level is not derived from what was disclosed");
    valid &= requireAbsent(
        admissionGate,
        QStringLiteral("prompt.capabilities"),
        "rewriting the capability list can lower the enforcement requirement");
    // 准入不放宽审批的规则判定。
    valid &= requireContains(
        admissionGate,
        QStringLiteral("verdict.ruleGranted = approval.ruleGranted;"),
        "admission widens the reusable rule the approval layer granted");
    // 准入不安装、不写盘、不执行任何东西。
    for (const QString &token : {
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("SecureStorage"),
             QStringLiteral(".effectiveEnabled ="),
             QStringLiteral("->replace(")}) {
        valid &= requireAbsent(
            admissionGate, token,
            "the admission gate holds authority beyond composing four verdicts");
    }
    // 准入门禁还没有调用方:门禁齐备不等于产品已经开放启用动作。
    for (const QString &source : {mainWindow, extensionCenter}) {
        valid &= requireAbsent(
            source,
            QStringLiteral("ExtensionAdmissionGate"),
            "an admission path reached the product before the action is wired");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_admission_gate"),
        "the admission gate is absent from CTest");

    // 更新是内容绑定信任最危险的时刻:当前版本已被复核并可能持有授权,而候选按定义是
    // 另一份内容。让信任或授权按标识传递,就把"更新"变成让任意新内容以上一版权威运行的
    // 通道——而内容绑定身份存在的全部理由正是防住这件事。
    valid &= requireContains(
        updatePolicy,
        QStringLiteral("verdict.inheritsTrust = false;"),
        "an update can inherit the reviewed trust of other content");
    valid &= requireContains(
        updatePolicy,
        QStringLiteral("verdict.inheritsGrant = false;"),
        "an update can inherit the enablement grant of other content");
    valid &= requireAbsent(
        updatePolicy,
        QStringLiteral("inheritsTrust = true"),
        "an update path grants inherited trust");
    valid &= requireAbsent(
        updatePolicy,
        QStringLiteral("inheritsGrant = true"),
        "an update path grants inherited enablement");
    // 复核只在种类、标识、来源与内容摘要全部一致时仍然适用。
    valid &= requireContains(
        updatePolicy,
        QStringLiteral("pin.contentIdentity == candidate.contentIdentity"),
        "a review transfers without matching the exact content");
    valid &= requireContains(
        updatePolicy,
        QStringLiteral("pin.sourceIdentity == candidate.sourceIdentity"),
        "a review transfers across a change of source");
    // 校验必须逐项成立,失败时当前版本保持不变。
    for (const QString &code : {
             QStringLiteral("extension-update-signature-invalid"),
             QStringLiteral("extension-update-manifest-invalid"),
             QStringLiteral("extension-update-incompatible"),
             QStringLiteral("extension-update-dependency-unsatisfied"),
             QStringLiteral("extension-update-health-failed"),
             QStringLiteral("extension-update-content-unchanged"),
             QStringLiteral("extension-update-target-mismatch"),
             QStringLiteral("extension-update-identity-invalid")}) {
        valid &= requireContains(
            updatePolicy, code,
            "an update validation cannot report which requirement failed");
    }
    valid &= requireContains(
        updatePolicy,
        QStringLiteral("verdict.activePreserved = true;"),
        "a failed upgrade need not leave the active version unchanged");
    valid &= requireContains(
        updatePolicy,
        QStringLiteral("verdict.candidateExecutable = false;"),
        "a merely validated candidate can execute");
    // 移除停用可执行内容但保留身份历史,并且必须收回授权。
    valid &= requireContains(
        updatePolicy,
        QStringLiteral("verdict.retainsIdentityMetadata = true;"),
        "removal discards the history that content was once authorized");
    valid &= requireContains(
        updatePolicy,
        QStringLiteral("verdict.retainsGrant = false;"),
        "removal leaves a grant that renamed content could inherit");
    // 版本号只用于披露降级,不参与权威判定。
    valid &= requireContains(
        updatePolicy,
        QStringLiteral("verdict.downgrade = isDowngrade("),
        "a downgrade is not disclosed");
    // 这一层不安装、不下载、不写盘、不执行任何东西。
    for (const QString &token : {
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("SecureStorage"),
             QStringLiteral(".effectiveEnabled ="),
             QStringLiteral("QNetworkAccessManager"),
             QStringLiteral("QFile ")}) {
        valid &= requireAbsent(
            updatePolicy, token,
            "the update policy holds authority beyond judging a candidate");
    }
    // 更新策略还没有调用方:门禁齐备不等于产品已经开放更新或移除动作。
    for (const QString &source : {mainWindow, extensionCenter}) {
        valid &= requireAbsent(
            source,
            QStringLiteral("ExtensionUpdatePolicy"),
            "an update path reached the product before the action is wired");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_update_policy"),
        "the update policy is absent from CTest");

    // 一个插件包可以同时携带 Skills、hooks、MCP 配置、命令与资源,因此"导入这个包"从来
    // 不是一个决定。预览只展示包名与来源时,人批准的是一个标题,而实际被引入的是标题背后
    // 的全部可执行组件。因此披露必须逐组件,整包汇总不能替代它。
    valid &= requireContains(
        importPreview,
        QStringLiteral("if (item.beyondReadOnly) preview.anyBeyondReadOnly = true;"),
        "capability disclosure is rolled up instead of made per component");
    valid &= requireContains(
        importPreview,
        QStringLiteral("preview.components.append(item);"),
        "the preview does not list every component");
    // 不认识的可执行组件必须失败关闭,而不是被静默跳过。
    valid &= requireContains(
        importPreview,
        QStringLiteral("extension-import-unsupported-component"),
        "an unsupported executable component does not fail the import closed");
    valid &= requireContains(
        importPreview,
        QStringLiteral("// 不认识的类型按可执行处理"),
        "an unrecognized component type defaults to a harmless asset");
    valid &= requireAbsent(
        importPreview,
        QStringLiteral("continue;"),
        "the preview can skip a component instead of disclosing it");
    // 失败关闭不等于丢掉证据:声明的原始类型与内容摘要必须保留可供检视。
    valid &= requireContains(
        importPreview,
        QStringLiteral("item.declaredType = component.declaredType;"),
        "failing closed discards the evidence of what the bundle declared");
    // 组件级别的展示安全与身份检查必须成立。
    for (const QString &code : {
             QStringLiteral("extension-import-component-id-invalid"),
             QStringLiteral("extension-import-component-name-unsafe"),
             QStringLiteral("extension-import-component-identity-invalid"),
             QStringLiteral("extension-import-component-duplicate"),
             QStringLiteral("extension-import-component-capability-duplicate"),
             QStringLiteral("extension-import-component-capability-limit"),
             QStringLiteral("extension-import-no-components"),
             QStringLiteral("extension-import-component-limit")}) {
        valid &= requireContains(
            importPreview, code,
            "an import preview accepts a manifest it cannot safely display");
    }
    // 预览不安装、不启用、不解压、不执行任何东西。
    valid &= requireContains(
        importPreview,
        QStringLiteral("preview.grantsInstallation = false;"),
        "building a preview can grant installation");
    for (const QString &token : {
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("SecureStorage"),
             QStringLiteral(".effectiveEnabled ="),
             QStringLiteral("QFile "),
             QStringLiteral("QDir ")}) {
        valid &= requireAbsent(
            importPreview, token,
            "the import preview holds authority beyond describing a bundle");
    }
    // 导入预览还没有调用方。
    for (const QString &source : {mainWindow, extensionCenter}) {
        valid &= requireAbsent(
            source,
            QStringLiteral("ExtensionImportPreview"),
            "an import path reached the product before the action is wired");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_import_preview"),
        "the import preview is absent from CTest");

    // 一份启用授权回答"用户要让这份内容运行吗",但不回答"在哪里运行"。没有作用域模型时
    // 任何一次启用都是全局启用:为某个项目批准的 Skill 会在别处继续激活,而被组织策略
    // 禁止的扩展会因为某个更低层级把它打开而实际运行。
    // 优先级必须单向:Managed 最高,其后 Global、Project、Session、ChildTask。
    valid &= requireContains(
        scopePolicy,
        QStringLiteral("case ExtensionScopeLevel::Managed:\n        return 0;"),
        "managed policy does not hold the highest scope precedence");
    valid &= requireContains(
        scopePolicy,
        QStringLiteral("return 1000;"),
        "an unclassified scope level does not fall to the lowest precedence");
    // 拒绝是有方向的:更低层级的拒绝始终生效(收窄权限是安全方向),而更低层级的启用
    // 不能推翻更高层级的拒绝。归因指向优先级最高的阻挡来源。
    valid &= requireContains(
        scopePolicy,
        QStringLiteral("if (!denied || precedence(rule.level) < precedence(denyingLevel))"),
        "a denial is not attributed to the strongest blocking authority");
    valid &= requireContains(
        scopePolicy,
        QStringLiteral("extension-scope-disabled-at-level"),
        "a scope-level denial has no diagnostic");
    // Managed 强制结论先于用户层级处理,但不绕过注册表双重门禁。
    valid &= requireContains(
        scopePolicy,
        QStringLiteral("if (rule.level != ExtensionScopeLevel::Managed || !rule.mandatory) continue;"),
        "a non-managed rule can claim unoverridable mandatory status");
    valid &= requireContains(
        scopePolicy,
        QStringLiteral("extension-scope-managed-ungated"),
        "managed policy can run content that was never reviewed");
    // 作用域只收窄授权,永不创造授权。
    valid &= requireContains(
        scopePolicy,
        QStringLiteral("extension-scope-grant-absent"),
        "a scope rule can activate an extension holding no grant");
    // 子任务只接收显式声明的子集。
    valid &= requireContains(
        scopePolicy,
        QStringLiteral("!context.childTaskDeclaredIds.contains(record.id)"),
        "a child task inherits extensions it was never granted");
    for (const QString &code : {
             QStringLiteral("extension-scope-child-task-undeclared"),
             QStringLiteral("extension-scope-rule-conflict"),
             QStringLiteral("extension-scope-record-invalid"),
             QStringLiteral("extension-scope-identity-invalid"),
             QStringLiteral("extension-scope-rule-limit"),
             QStringLiteral("extension-scope-managed-blocked"),
             QStringLiteral("extension-scope-unscoped")}) {
        valid &= requireContains(
            scopePolicy, code,
            "a scope refusal cannot explain which source blocked the component");
    }
    // 规则绑定确切内容,与启用授权同构。
    valid &= requireContains(
        scopePolicy,
        QStringLiteral("rule.contentIdentity == record.contentIdentity"),
        "scope rules bind by identifier instead of exact content");
    valid &= requireContains(
        scopePolicy,
        QStringLiteral("rule.sourceIdentity == record.sourceIdentity"),
        "scope rules ignore the source identity");
    // 这一层不安装、不写盘、不执行任何东西,也不改写记录。
    for (const QString &token : {
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("SecureStorage"),
             QStringLiteral(".effectiveEnabled ="),
             QStringLiteral("QFile "),
             QStringLiteral("QDir ")}) {
        valid &= requireAbsent(
            scopePolicy, token,
            "the scope policy holds authority beyond deciding applicability");
    }
    // 作用域判定还没有调用方。
    for (const QString &source : {mainWindow, extensionCenter}) {
        valid &= requireAbsent(
            source,
            QStringLiteral("ExtensionScopePolicy"),
            "a scope path reached the product before the action is wired");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_scope_policy"),
        "the scope policy is absent from CTest");

    // 复核证据仍然不存在于产品路径中，因此没有任何扩展可被启用。
    valid &= requireContains(
        reviewController,
        QStringLiteral("bound.reviewPins = ledger.pins"),
        "fresh inventory is not bound to authenticated review pins");
    valid &= requireContains(
        reviewController,
        QStringLiteral("ExtensionInventoryCoordinator::collect(bound)"),
        "review controller does not consume the unified inventory");
    valid &= requireContains(
        mainWindow,
        QStringLiteral("QStringLiteral(\"扩展中心\")"),
        "primary extension center entry is missing");
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_review_controller"),
        "review controller TOCTOU/CAS test is absent from CTest");
    const QString extensionReviewPath = sourceRange(
        mainWindow,
        QStringLiteral("void MainWindow::onExtensionCenterClicked()"),
        QStringLiteral("void MainWindow::onHelpClicked()"));
    valid &= requireOrdered(
        extensionReviewPath,
        {QStringLiteral("ExtensionReviewController::inspect"),
         QStringLiteral("reviewRequested"),
         QStringLiteral("startExtensionReviewOperation")},
        "extension review UI does not load, confirm, and dispatch review operations");
    valid &= requireContains(
        mainWindow,
        QStringLiteral("m_extensionReviewThread->wait()"),
        "extension review worker is not joined during MainWindow destruction");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("setTextFormat(Qt::PlainText)"),
        "review confirmation is not forced to plain text");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("ExtensionReviewAction::Revoke"),
        "extension center cannot revoke stale review evidence");
    valid &= requireAbsent(
        extensionCenter,
        QStringLiteral("effectiveEnabled"),
        "review UI grants extension enablement authority");
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
