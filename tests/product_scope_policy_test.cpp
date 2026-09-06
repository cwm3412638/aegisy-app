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
    const QString workbenchWidget = readFile(root.filePath(
        QStringLiteral("src/agent_workbench_widget.cpp")));
    const QString runtimeClient = readFile(root.filePath(
        QStringLiteral("src/agent_runtime_client.cpp")));
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
    const QString recoveryController = readFile(root.filePath(
        QStringLiteral("src/extension_recovery_controller.cpp")));
    const QString recoveryPresentation = readFile(root.filePath(
        QStringLiteral("src/extension_recovery_presentation.cpp")));
    const QString updatePolicy = readFile(root.filePath(
        QStringLiteral("src/extension_update_policy.cpp")));
    const QString importPreview = readFile(root.filePath(
        QStringLiteral("src/extension_import_preview.cpp")));
    const QString scopePolicy = readFile(root.filePath(
        QStringLiteral("src/extension_scope_policy.cpp")));
    const QString instructionContext = readFile(root.filePath(
        QStringLiteral("src/instruction_context_manifest.cpp")));
    const QString mcpLifecycle = readFile(root.filePath(
        QStringLiteral("src/mcp_lifecycle_policy.cpp")));
    const QString hookEngine = readFile(root.filePath(
        QStringLiteral("src/hook_policy_engine.cpp")));
    const QString lifecycleController = readFile(root.filePath(
        QStringLiteral("src/extension_lifecycle_controller.cpp")));
    const QString lifecyclePresentation = readFile(root.filePath(
        QStringLiteral("src/extension_lifecycle_presentation.cpp")));
    const QString lifecyclePresentationHeader = readFile(root.filePath(
        QStringLiteral("include/extension_lifecycle_presentation.h")));
    const QString bundleReader = readFile(root.filePath(
        QStringLiteral("src/extension_bundle_reader.cpp")));
    const QString bundleReaderHeader = readFile(root.filePath(
        QStringLiteral("include/extension_bundle_reader.h")));
    const QString treeCapture = readFile(root.filePath(
        QStringLiteral("src/extension_tree_capture.cpp")));
    const QString treeCaptureHeader = readFile(root.filePath(
        QStringLiteral("include/extension_tree_capture.h")));
    const QString stagingSnapshot = readFile(root.filePath(
        QStringLiteral("src/extension_staging_snapshot.cpp")));
    const QString stagingSnapshotHeader = readFile(root.filePath(
        QStringLiteral("include/extension_staging_snapshot.h")));
    const QString restorePlan = readFile(root.filePath(
        QStringLiteral("src/extension_staging_restore_plan.cpp")));
    const QString restorePlanHeader = readFile(root.filePath(
        QStringLiteral("include/extension_staging_restore_plan.h")));
    const QString restorePresentation = readFile(root.filePath(
        QStringLiteral("src/extension_staging_restore_presentation.cpp")));
    const QString restorePresentationHeader = readFile(root.filePath(
        QStringLiteral("include/extension_staging_restore_presentation.h")));
    const QString restoreApproval = readFile(root.filePath(
        QStringLiteral("src/extension_staging_restore_approval.cpp")));
    const QString restoreApprovalHeader = readFile(root.filePath(
        QStringLiteral("include/extension_staging_restore_approval.h")));
    const QString restoreAuditLedger = readFile(root.filePath(
        QStringLiteral("src/extension_staging_restore_audit_ledger.cpp")));
    const QString restoreAuditLedgerHeader = readFile(root.filePath(
        QStringLiteral("include/extension_staging_restore_audit_ledger.h")));
    const QString restoreAuditStore = readFile(root.filePath(
        QStringLiteral(
            "src/extension_staging_restore_audit_ledger_store.cpp")));
    const QString restoreAuditStoreHeader = readFile(root.filePath(
        QStringLiteral(
            "include/extension_staging_restore_audit_ledger_store.h")));
    const QString restoreAuditAdapter = readFile(root.filePath(
        QStringLiteral(
            "src/extension_staging_restore_audit_ledger_secure_storage_adapter.cpp")));
    const QString restoreController = readFile(root.filePath(
        QStringLiteral("src/extension_staging_restore_controller.cpp")));
    const QString restoreControllerHeader = readFile(root.filePath(
        QStringLiteral("include/extension_staging_restore_controller.h")));
    const QString restoreExecutor = readFile(root.filePath(
        QStringLiteral("src/extension_staging_restore_executor.cpp")));
    const QString restoreExecutorHeader = readFile(root.filePath(
        QStringLiteral("include/extension_staging_restore_executor.h")));
    const QString restoreFlow = readFile(root.filePath(
        QStringLiteral("src/extension_staging_restore_flow.cpp")));
    const QString restoreFlowHeader = readFile(root.filePath(
        QStringLiteral("include/extension_staging_restore_flow.h")));
    const QString removalSequence = readFile(root.filePath(
        QStringLiteral("src/extension_removal_sequence.cpp")));
    const QString removalSequenceHeader = readFile(root.filePath(
        QStringLiteral("include/extension_removal_sequence.h")));
    const QString stagingBackupCapture = readFile(root.filePath(
        QStringLiteral("src/extension_staging_backup_capture.cpp")));
    const QString stagingBackupCaptureHeader = readFile(root.filePath(
        QStringLiteral("include/extension_staging_backup_capture.h")));
    const QString stagingBackupKeyProvider = readFile(root.filePath(
        QStringLiteral("src/extension_staging_backup_key_provider.cpp")));
    const QString stagingBackupKeyProviderHeader = readFile(root.filePath(
        QStringLiteral("include/extension_staging_backup_key_provider.h")));
    const QString stagingBackupInventory = readFile(root.filePath(
        QStringLiteral("src/extension_staging_backup_inventory.cpp")));
    const QString stagingBackupInventoryHeader = readFile(root.filePath(
        QStringLiteral("include/extension_staging_backup_inventory.h")));
    const QString stagingBackupRetention = readFile(root.filePath(
        QStringLiteral("src/extension_staging_backup_retention.cpp")));
    const QString stagingBackupRetentionHeader = readFile(root.filePath(
        QStringLiteral("include/extension_staging_backup_retention.h")));
    const QString importPresentation = readFile(root.filePath(
        QStringLiteral("src/extension_import_presentation.cpp")));
    const QString importPresentationHeader = readFile(root.filePath(
        QStringLiteral("include/extension_import_presentation.h")));
    const QString candidateBuilder = readFile(root.filePath(
        QStringLiteral("src/extension_update_candidate_builder.cpp")));
    const QString candidateBuilderHeader = readFile(root.filePath(
        QStringLiteral("include/extension_update_candidate_builder.h")));
    const QString updatePresentation = readFile(root.filePath(
        QStringLiteral("src/extension_update_presentation.cpp")));
    const QString updatePresentationHeader = readFile(root.filePath(
        QStringLiteral("include/extension_update_presentation.h")));
    const QString reviewController = readFile(root.filePath(
        QStringLiteral("src/extension_review_controller.cpp")));
    const QString extensionCenter = readFile(root.filePath(
        QStringLiteral("src/extension_center_dialog.cpp")));
    const QString extensionCenterHeader = readFile(root.filePath(
        QStringLiteral("include/extension_center_dialog.h")));
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
    const QString skillsDialog = readFile(root.filePath(
        QStringLiteral("src/skills_dialog.cpp")));
    const QString gatewayScript = readFile(root.filePath(
        QStringLiteral("assets/local_gateway.js")));
    const QString backupStoreHeader = readFile(root.filePath(
        QStringLiteral("include/configuration_backup_store.h")));
    const QString backupStoreSource = readFile(root.filePath(
        QStringLiteral("src/configuration_backup_store.cpp")));
    const QString runtime = readFile(root.filePath(
        QStringLiteral("agent-runtime/crates/aegisy-agentd/src/lib.rs")));
    const QString runtimeMain = readFile(root.filePath(
        QStringLiteral("agent-runtime/crates/aegisy-agentd/src/main.rs")));
    const QString modelCatalog = readFile(root.filePath(
        QStringLiteral("agent-runtime/crates/aegisy-agentd/src/model_catalog.rs")));
    const QString proposal = readFile(root.filePath(
        QStringLiteral("openspec/changes/build-aegisy-agent-workbench/proposal.md")));
    const QString companionSpec = readFile(root.filePath(
        QStringLiteral("openspec/changes/build-aegisy-agent-workbench/specs/"
                       "aegisy-companion-control-center/spec.md")));
    if (mainWindow.isEmpty() || appMain.isEmpty()
            || workbenchWindow.isEmpty() || workbenchWidget.isEmpty()
            || runtimeClient.isEmpty() || connectWizard.isEmpty()
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
            || instructionContext.isEmpty()
            || mcpLifecycle.isEmpty()
            || hookEngine.isEmpty()
            || lifecycleController.isEmpty()
            || lifecyclePresentation.isEmpty()
            || lifecyclePresentationHeader.isEmpty()
            || bundleReader.isEmpty() || bundleReaderHeader.isEmpty()
            || treeCapture.isEmpty() || treeCaptureHeader.isEmpty()
            || stagingSnapshot.isEmpty() || stagingSnapshotHeader.isEmpty()
            || stagingBackupInventory.isEmpty()
            || stagingBackupInventoryHeader.isEmpty()
            || stagingBackupRetention.isEmpty()
            || stagingBackupRetentionHeader.isEmpty()
            || stagingBackupKeyProvider.isEmpty()
            || stagingBackupKeyProviderHeader.isEmpty()
            || importPresentation.isEmpty()
            || importPresentationHeader.isEmpty()
            || candidateBuilder.isEmpty() || candidateBuilderHeader.isEmpty()
            || enablementWorkflow.isEmpty() || enablementController.isEmpty()
            || reviewController.isEmpty()
            || extensionCenter.isEmpty() || extensionCenterHeader.isEmpty()
            || mcpInventory.isEmpty() || mcpDialog.isEmpty()
            || codexPluginInventory.isEmpty()
            || skillExtensionInventory.isEmpty()
            || extensionCoordinator.isEmpty()
            || gatewayScript.isEmpty()
            || backupStoreHeader.isEmpty() || backupStoreSource.isEmpty()
            || runtime.isEmpty() || runtimeMain.isEmpty() || modelCatalog.isEmpty()
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
    // 备份存储已经按域参数化,而域字符串一旦发布就不能再改:它们进入 AAD、密钥作用域与清单
    // 身份,改动其中任何一个都会让既有备份全部无法解密。工具域的常量因此必须留在实现文件里
    // 集中受审,而不是散到调用方——那样"发布后不可更改"就没有任何一处可以被检查。
    valid &= requireContains(backupStoreHeader,
                             QStringLiteral("struct ConfigurationBackupStoreDomain"),
                             "the backup store is no longer parameterized by a domain");
    valid &= requireContains(
        backupStoreHeader,
        QStringLiteral("bool legacyV1MigrationEnabled = false;"),
        "a new backup domain would inherit the legacy migration write path by default");
    valid &= requireContains(backupStoreHeader,
                             QStringLiteral("static ConfigurationBackupStoreDomain toolDomain();"),
                             "the tool domain constants escaped into callers");
    valid &= requireContains(
        backupStoreHeader,
        QStringLiteral("static ConfigurationBackupStoreDomain extensionStagingDomain();"),
        "the extension staging domain has no explicit factory boundary");
    for (const QString &literal : {
             QStringLiteral("aegisy-tool-config-backup-manifest/0.2"),
             QStringLiteral("aegisy-tool-config-backup-manifest-identity/0.1"),
             QStringLiteral("tool-manager/config-backup-master/v1/"),
             QStringLiteral("configuration-backup-manifest:sha256:"),
             QStringLiteral("manifest.v2.pending")}) {
        valid &= requireContains(backupStoreSource, literal,
                                 "a published tool backup domain literal drifted");
    }
    // 内嵌 NUL 必须靠 `sizeof - 1` 保留。写成 `QByteArray(kAad)` 会静默截掉它,于是每一份
    // AAD 与每一个清单身份都变了,而既有备份要到需要回滚的那一刻才被发现无法解密。
    valid &= requireContains(backupStoreSource,
                             QStringLiteral("sizeof(kAadPrefix) - 1"),
                             "the tool AAD prefix lost its embedded NUL");
    valid &= requireContains(backupStoreSource,
                             QStringLiteral("sizeof(kIdentityDomain) - 1"),
                             "the tool identity domain lost its embedded NUL");
    // 旧版迁移的两个入口都必须按域关闭。`inventory` 那一个更危险:它只要看到一份非 v2 清单
    // 就会触发,而清点本来是只读动作。
    valid &= requireOrdered(
        backupStoreSource,
        {QStringLiteral("if (!m_domain.legacyV1MigrationEnabled) {"),
         QStringLiteral("if (!m_domain.legacyV1MigrationEnabled) {")},
        "only one of the two legacy migration entry points is gated by the domain");
    // 前缀由域提供,而不是一个共享常量:AAD 是跨域互认的最后一道防线,因为改掉它就得重新
    // 认证密文,而明文清单里的 `format` 比较任何能写目录的人都能绕过。
    valid &= requireContains(backupStoreSource,
                             QStringLiteral("QByteArray output = domain.aadPrefix;"),
                             "the AAD prefix no longer comes from the domain");
    valid &= requireContains(backupStoreSource,
                             QStringLiteral("QByteArray material = domain.identityDomain;"),
                             "the manifest identity hash domain no longer comes from the domain");
    // 代号集合不能是 `static`:它由域前缀构成,而一个静态集合会永久冻结第一个被实例化的域,
    // 于是第二个域的"存储不可用"会被读成"证据无效"。
    valid &= requireAbsent(backupStoreSource,
                           QStringLiteral("static const QSet<QString> unavailable"),
                           "the diagnostic code set froze the first instantiated domain");
    // 按主体清点的混合主体容忍钉在实现上:扫描上限放宽到与 removeVerified 同宽(分辨
    // foreign 与 corrupt 要求看到每一个目录);foreign 条目绝不未经验证就跳过——声称主体
    // 提取、以该主体做完整清单解析、通过才 continue,顺序缺一不可;作用域内份数超限单独
    // 判 Invalid,别人主体的完整备份不占额度。
    const QString storeInventoryPath = sourceRange(
        backupStoreSource,
        QStringLiteral("ConfigurationBackupInventoryResult "
                       "ConfigurationBackupStore::inventory("),
        QStringLiteral("bool ConfigurationBackupStore::removeVerified("));
    valid &= requireContains(
        storeInventoryPath, QStringLiteral("m_domain.maxBackups * 4"),
        "the scoped inventory scan ceiling drifted");
    valid &= requireOrdered(
        storeInventoryPath,
        {QStringLiteral("claimedSubject != tool"),
         QStringLiteral("parseManifest(m_domain, manifestBytes, claimedSubject"),
         QStringLiteral("continue;")},
        "a foreign backup is skipped without full validation");
    valid &= requireContains(
        storeInventoryPath,
        QStringLiteral("entries.size() > m_domain.maxBackups"),
        "the in-scope over-limit judgment is missing");
    // 这一片唯一被实例化的域仍然是工具域。产品里出现第二个域意味着扩展备份路径已经开了,
    // 而那要等权限、审批、沙箱与恢复门禁都接上调用方之后才能做。
    for (const QString &source : {toolSource, mainWindow, extensionCenter}) {
        valid &= requireAbsent(source, QStringLiteral("ConfigurationBackupStoreDomain"),
                               "a second backup domain is instantiated in the product");
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
    // 产品路径只通过持久化层接触启用授权，绝不直接使用认证编解码层：直接编解码等于绕过
    // 三阶段发布与反降级逻辑，自己造一份能通过认证的授权。
    valid &= requireAbsent(
        mainWindow,
        QStringLiteral("ExtensionEnablementLedger::"),
        "the main window encodes enablement grants itself");
    valid &= requireAbsent(
        extensionCenter,
        QStringLiteral("ExtensionEnablementLedger::"),
        "the extension center dialog encodes enablement grants itself");
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
    // 授权的持久化权威只属于控制器所在的那条工作线程路径。界面只接收与展示读出来的
    // 结果，绝不自己构造一个存储去提交:那会是第二条提交路径,而只有一条经过 CAS。
    valid &= requireContains(
        mainWindow,
        QStringLiteral("ExtensionEnablementLedgerStore store(&authority, &settings)"),
        "the grant path does not open the split-persistence grant store");
    valid &= requireAbsent(
        extensionCenter,
        QStringLiteral("ExtensionEnablementLedgerStore store"),
        "the extension center persists enablement grants itself");
    valid &= requireAbsent(
        extensionCenter,
        QStringLiteral("->replace("),
        "the extension center writes a grant set directly");
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
    // 规划层的调用方仍然只有控制器：产品路径不得自己规划启用授权，否则会出现一条绕过
    // CAS 的提交路径。
    valid &= requireAbsent(
        mainWindow,
        QStringLiteral("ExtensionEnablementWorkflow"),
        "the main window plans enablement grants outside the controller");
    valid &= requireAbsent(
        extensionCenter,
        QStringLiteral("ExtensionEnablementWorkflow::"),
        "the extension center plans enablement grants outside the controller");
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
    // 授权动作现在有调用方了，但持久化权威仍然只属于控制器：对话框不得自己规划或提交
    // 授权集合，否则一份授权的提交路径会有两条，而只有一条经过 CAS。
    valid &= requireContains(
        mainWindow,
        QStringLiteral("ExtensionEnablementController::apply(inputs, request, &store)"),
        "the grant action does not commit through the enablement controller");
    valid &= requireAbsent(
        extensionCenter,
        QStringLiteral("ExtensionEnablementController"),
        "the extension center drives enablement grants outside the controller");
    valid &= requireAbsent(
        extensionCenter,
        QStringLiteral("ExtensionEnablementLedgerStore store"),
        "the extension center persists enablement grants itself");
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
    // 启用呈现现在有调用方了，但可点击性只能来自它的判定：界面另算一遍必然会与这一层
    // 漂移，而漂移的方向是给一份没人复核过的内容提供授权按钮。
    valid &= requireAbsent(
        mainWindow,
        QStringLiteral("ExtensionEnablementPresentation"),
        "the main window renders the grant prompt outside the dialog");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("ExtensionEnablementPresentation::build("),
        "the grant action does not render through the enablement presentation");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("ExtensionEnablementPresentation::buildRevocation("),
        "grant revocation does not render through the enablement presentation");
    // 三道门禁的判定不得在界面里重算一遍。
    for (const QString &token : {
             QStringLiteral("record.trust != ExtensionTrustState::Verified"),
             QStringLiteral("record.compatibility != ExtensionCompatibilityState::Compatible")}) {
        valid &= requireAbsent(
            extensionCenter, token,
            "the extension center re-decides the enablement gates itself");
    }
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("prompt.state == ExtensionEnablementPromptState::Ready"),
        "grant eligibility does not come from the presentation verdict");
    // 授权账本读不出来时授权与撤销一律冻结：提交一份\"完整集合\"会静默撤销读不出来的
    // 那些授权，把一次篡改表述成用户主动停用。
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("m_grants.state == ExtensionEnablementLedgerStoreState::Ready"),
        "the extension center acts on an unreadable grant ledger");
    // 授权当前不会让任何内容运行，这一句必须出现在人能看到的地方。
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("当前不会让任何内容运行"),
        "the grant confirmation claims the grant starts execution");
    // 一条路径回推的快照不得改写另一条路径的账本：复核操作没有读过授权，把授权集合当成
    // 空集合会让界面显示"这些扩展没有被授权过"，而实际情况是这次操作根本没有读过授权。
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("populate(records, sourceIssueCodes, ledger, m_grants)"),
        "a review refresh overwrites the enablement grant set");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("populate(records, sourceIssueCodes, m_ledger, grants)"),
        "a grant refresh overwrites the review pin set");
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

    // 恢复的执行部分只会减少授权,永不增加。`discard` 结构上只清空、不接受任何条目,因此这
    // 条路径连"写入一份非空授权集合"这个动作都无法表达;换成 `replace` 就重新获得了这个能力,
    // 而任何能产出非空集合的恢复路径都是一条制造同意的路径。
    valid &= requireContains(
        recoveryController,
        QStringLiteral("grantStore->discard(&acknowledged, &errorCode)"),
        "the recovery executor can express writing a non-empty grant set");
    valid &= requireAbsent(
        recoveryController,
        QStringLiteral("->replace("),
        "the recovery executor can write a grant set instead of clearing one");
    // 恢复不碰复核账本:复核记录是事后审计唯一的证据来源,而清掉它并不减少任何授权——注册表
    // 的双重门禁下没有授权就不会启用。删复核记录只销毁证据,不改变安全结论。
    for (const QString &token : {
             QStringLiteral("ExtensionReviewLedgerStore"),
             QStringLiteral("ExtensionReviewController"),
             QStringLiteral("QProcess")}) {
        valid &= requireAbsent(
            recoveryController, token,
            "the recovery executor destroys audit evidence or executes something");
    }
    // 结论只能来自重新读出来的字节,而完成与否只有判定层一个来源。这一层另算一遍必然与它
    // 漂移,而漂移的方向是把一次没做完的恢复报成做完了。
    valid &= requireOrdered(
        recoveryController,
        {QStringLiteral("grantStore->discard("),
         QStringLiteral("grantStore->load()"),
         QStringLiteral("ExtensionRecoveryGate::completed(reread)")},
        "the recovery outcome is concluded from an acknowledged write");
    // 恢复的执行部分同样还没有调用方:门禁完成前不得出现可点击的恢复动作。
    for (const QString &source : {mainWindow, extensionCenter}) {
        valid &= requireAbsent(
            source,
            QStringLiteral("ExtensionRecoveryController"),
            "a recovery path reached the product before the gates exist");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_recovery_controller"),
        "the recovery executor is absent from CTest");

    // 呈现层只呈现:它不读盘、不写盘、不清空事务、不执行任何东西。
    for (const QString &token : {
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("SecureStorage"),
             QStringLiteral("ExtensionRecoveryController::apply"),
             QStringLiteral("->discard("),
             QStringLiteral("->replace(")}) {
        valid &= requireAbsent(
            recoveryPresentation, token,
            "the recovery presentation holds authority beyond rendering");
    }
    // 可确认性只能来自判定层。一个能被确认的动作就是一个会被执行的动作,而这一层自己推导
    // 必然与判定层漂移,漂移的方向是界面对一份读不到的账本提供清空动作。
    valid &= requireContains(
        recoveryPresentation,
        QStringLiteral(
            "prompt.confirmationRequired = view.assessment.operatorConfirmationRequired;"),
        "the recovery presentation re-derives confirmability instead of forwarding it");
    // 读不出来的条数不显示。可读性判定只有一个来源。
    valid &= requireContains(
        recoveryPresentation,
        QStringLiteral(
            "ExtensionRecoveryGate::authoritative(view.grantState)"),
        "the recovery presentation shows a grant count it cannot know");
    // 呈现层同样还没有调用方:门禁完成前不得出现可点击的恢复动作。
    for (const QString &source : {mainWindow, extensionCenter}) {
        valid &= requireAbsent(
            source,
            QStringLiteral("ExtensionRecoveryPresentation"),
            "a recovery surface reached the product before the gates exist");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_recovery_presentation"),
        "the recovery presentation is absent from CTest");

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
    // 产品里现在有一个只读的调用方：它判定一个候选能不能成立，然后把结论摆给人看。判定
    // 不是暂存——暂存要往磁盘上放一份候选，而在权限、审批、沙箱与恢复门禁完成之前那正是被
    // 禁止的那件事。因此这里钉住的是"只判定，不暂存"。
    valid &= requireContains(
        mainWindow,
        QStringLiteral("ExtensionUpdatePolicy::evaluate(active, candidate.candidate,"),
        "the update check does not go through the shared update policy");
    // 对话框自己绝不判定：自己判一遍必然与判定层漂移，而漂移的方向是界面提供一个判定层会
    // 拒绝的动作。
    valid &= requireAbsent(
        extensionCenter,
        QStringLiteral("ExtensionUpdatePolicy"),
        "the extension center decides whether an update holds by itself");
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

    // 项目指令与 Skill 内容都是模型可见文本,而模型可见的文本就是模型会照着做的文本。
    // 磁盘上的指令不是策略:运行时策略必须始终胜出,而被拒绝的指令**不得**被改写成一条
    // 已授权的策略,否则下一个读清单的人会看到一条看似合法的授权,其来源其实是不可信
    // 磁盘内容。
    valid &= requireContains(
        instructionContext,
        QStringLiteral("entry.policyAuthority = policyAuthority(source.kind);"),
        "an instruction source can be rewritten as trusted policy");
    valid &= requireContains(
        instructionContext,
        QStringLiteral("case InstructionSourceKind::Managed:\n        return true;"),
        "policy authority is not restricted to managed instructions");
    valid &= requireContains(
        instructionContext,
        QStringLiteral("instruction-not-policy-authority"),
        "a disk instruction can declare policy without a visible denial");
    valid &= requireContains(
        instructionContext,
        QStringLiteral("instruction-forbidden-by-runtime-policy"),
        "runtime policy does not win over model-visible guidance");
    valid &= requireContains(
        instructionContext,
        QStringLiteral("if (Safety::beyondReadOnly(behavior)) {"),
        "a behavior beyond read-only can be accepted from an instruction");
    // 拒绝可见,但指令原文仍留在链上:失败关闭不等于把证据一起丢掉。
    valid &= requireContains(
        instructionContext,
        QStringLiteral("manifest.denials.append(denial);"),
        "a denial is not recorded where a person can see it");
    valid &= requireContains(
        instructionContext,
        QStringLiteral("entries.append(entry);"),
        "denying a behavior can remove the instruction from the manifest");
    // 嵌套指令按目录深度覆盖更外层的,但不得越出自己的优先级区间。
    valid &= requireContains(
        instructionContext,
        QStringLiteral("entry.precedence += MaxDirectoryDepth - source.directoryDepth;"),
        "closer nested instructions do not take precedence");
    valid &= requireContains(
        instructionContext,
        QStringLiteral("case InstructionSourceKind::Managed:\n        return 0;"),
        "managed instructions do not hold the highest precedence");
    valid &= requireContains(
        instructionContext,
        QStringLiteral("return 100000;"),
        "an unclassified instruction source does not fall to lowest precedence");
    // 上下文预算被核算而不是被静默截断:悄悄丢掉一段指令会让清单与模型看到的内容不一致。
    valid &= requireContains(
        instructionContext,
        QStringLiteral("instruction-context-budget-exceeded"),
        "an over-budget instruction context is silently truncated");
    // Skill 调用必须留下可追溯的身份,越界权限被记录并拒绝而非静默采纳。
    valid &= requireContains(
        instructionContext,
        QStringLiteral("record.deniedPermissions.append(permission);"),
        "a Skill script permission beyond read-only is silently accepted");
    for (const QString &code : {
             QStringLiteral("instruction-source-duplicate"),
             QStringLiteral("instruction-source-path-unsafe"),
             QStringLiteral("instruction-content-identity-invalid"),
             QStringLiteral("instruction-depth-invalid"),
             QStringLiteral("instruction-source-limit"),
             QStringLiteral("instruction-skill-id-invalid"),
             QStringLiteral("instruction-skill-identity-invalid"),
             QStringLiteral("instruction-skill-path-unsafe")}) {
        valid &= requireContains(
            instructionContext, code,
            "an instruction manifest accepts sources it cannot account for");
    }
    // 这一层不加载文件、不执行任何东西、不改写策略。
    for (const QString &token : {
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("SecureStorage"),
             QStringLiteral("QFile "),
             QStringLiteral("QDir "),
             QStringLiteral("QTextStream")}) {
        valid &= requireAbsent(
            instructionContext, token,
            "the instruction manifest holds authority beyond accounting");
    }
    // 指令清单还没有调用方。
    for (const QString &source : {mainWindow, extensionCenter}) {
        valid &= requireAbsent(
            source,
            QStringLiteral("InstructionContextPolicy"),
            "an instruction path reached the product before the action is wired");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("instruction_context_manifest"),
        "the instruction context manifest is absent from CTest");

    // 一个 MCP 服务器是外部进程,因此它有自己的失败方式。最要紧的一条:依赖失败服务器的
    // 回合必须以服务器专属失败结束,既不能报成模型失败,也不能报成一次成功的工具结果。
    // 报成模型失败会让人去修提示词而问题在服务器;报成成功结果更糟——模型会把一个不存在
    // 的返回值当作事实继续推理。
    valid &= requireContains(
        mcpLifecycle,
        QStringLiteral("outcome.attributableToModel = false;"),
        "a server failure can be reported as a model failure");
    valid &= requireContains(
        mcpLifecycle,
        QStringLiteral("outcome.reportedAsSuccess = false;"),
        "a server failure can be reported as a successful tool result");
    valid &= requireContains(
        mcpLifecycle,
        QStringLiteral("outcome.attribution = McpFailureAttribution::Server;"),
        "a dependent failure is not attributed to the server");
    // 认证缺失与失败是不同结论:前者可由人补上。合并会让人对一个只差一次登录的服务器
    // 去排查故障。
    valid &= requireContains(
        mcpLifecycle,
        QStringLiteral("status.resolvableByAuthentication = true;"),
        "an authentication gap is not distinguished from a failure");
    // 只有就绪状态下工具清单可用。
    valid &= requireContains(
        mcpLifecycle,
        QStringLiteral("status.toolsAvailable = true;"),
        "the ready state does not expose its tool list");
    // 审批必须遮蔽机密,而遮蔽本身可见。
    valid &= requireContains(
        mcpLifecycle,
        QStringLiteral("prompt.redactedArguments.append(name);"),
        "a redaction is not disclosed to the approver");
    valid &= requireContains(
        mcpLifecycle,
        QStringLiteral("if (secretBearing(name)) {"),
        "secret-bearing arguments are displayed unredacted");
    // 越出只读边界的调用不提供记住选项。
    valid &= requireContains(
        mcpLifecycle,
        QStringLiteral("if (!prompt.beyondReadOnly) {"),
        "a beyond-read-only invocation can yield a reusable rule");
    valid &= requireContains(
        mcpLifecycle,
        QStringLiteral("prompt.grantsInvocation = false;"),
        "rendering an approval can grant invocation");
    // 日志有界,且截断可见。
    valid &= requireContains(
        mcpLifecycle,
        QStringLiteral("log.droppedLines = start;"),
        "log truncation is not disclosed");
    for (const QString &code : {
             QStringLiteral("mcp-not-enabled"),
             QStringLiteral("mcp-not-trusted"),
             QStringLiteral("mcp-server-exited"),
             QStringLiteral("mcp-authentication-required"),
             QStringLiteral("mcp-handshake-pending"),
             QStringLiteral("mcp-dependency-state-unknown"),
             QStringLiteral("mcp-approval-arguments-misaligned"),
             QStringLiteral("mcp-approval-argument-duplicate"),
             QStringLiteral("mcp-approval-identity-invalid"),
             QStringLiteral("mcp-approval-argument-limit")}) {
        valid &= requireContains(
            mcpLifecycle, code,
            "an MCP conclusion cannot be told apart from another");
    }
    // 这一层不启动进程、不连接网络、不执行工具。
    for (const QString &token : {
             QStringLiteral("QProcess"),
             QStringLiteral("QNetworkAccessManager"),
             QStringLiteral("QTcpSocket"),
             QStringLiteral("QSettings"),
             QStringLiteral("SecureStorage"),
             QStringLiteral("QFile "),
             QStringLiteral("QDir ")}) {
        valid &= requireAbsent(
            mcpLifecycle, token,
            "the MCP lifecycle policy holds authority beyond deciding state");
    }
    // MCP 生命周期判定还没有调用方。
    for (const QString &source : {mainWindow, extensionCenter}) {
        valid &= requireAbsent(
            source,
            QStringLiteral("McpLifecyclePolicy"),
            "an MCP lifecycle path reached the product before wiring");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("mcp_lifecycle_policy"),
        "the MCP lifecycle policy is absent from CTest");

    // 钩子是在工具执行之前运行并可以否决执行的外部命令,因此它同时是最强的安全控制点和
    // 最危险的失败点。受管安全钩子必须失败关闭:一个用来阻止危险操作的钩子如果在崩溃时
    // 放行,那么让它崩溃就成了绕过它的方法,于是它提供的保护等于零。
    valid &= requireContains(
        hookEngine,
        QStringLiteral("if (declaration.provenance == HookProvenance::Managed"),
        "a managed security hook can fail open");
    valid &= requireContains(
        hookEngine,
        QStringLiteral("hook-managed-security-fail-open"),
        "a contradictory managed security contract is silently rewritten");
    // 受信任钩子的拒绝必须真的拦下工具,并且署名到该钩子。
    valid &= requireContains(
        hookEngine,
        QStringLiteral("verdict.attributedHookId = declaration.id;"),
        "a hook verdict is not attributable in the timeline");
    valid &= requireContains(
        hookEngine,
        QStringLiteral("hook-denied"),
        "a hook denial has no diagnostic");
    // 失败关闭的结论必须与显式拒绝可分辨。
    valid &= requireContains(
        hookEngine,
        QStringLiteral("verdict.fromFailureBehavior = true;"),
        "a failure fallback cannot be told apart from an explicit decision");
    // 契约外与未分类结果都不得默认放行。
    valid &= requireContains(
        hookEngine,
        QStringLiteral("hook-contract-violation"),
        "an out-of-contract hook result is treated as an allow");
    valid &= requireContains(
        hookEngine,
        QStringLiteral("hook-outcome-unknown"),
        "an unclassified hook outcome defaults to permitting the tool");
    // 不可审查的契约不等于没有钩子。
    valid &= requireContains(
        hookEngine,
        QStringLiteral("verdict.toolMayExecute = false;\n    verdict.errorCode = code;"),
        "an unreviewable hook contract permits the tool");
    for (const QString &code : {
             QStringLiteral("hook-id-invalid"),
             QStringLiteral("hook-matcher-missing"),
             QStringLiteral("hook-command-missing"),
             QStringLiteral("hook-scope-missing"),
             QStringLiteral("hook-timeout-invalid"),
             QStringLiteral("hook-untrusted"),
             QStringLiteral("hook-timed-out"),
             QStringLiteral("hook-crashed")}) {
        valid &= requireContains(
            hookEngine, code,
            "a hook contract term can be omitted without a diagnostic");
    }
    // 无界输出不得阻塞事件循环,超限内容转为工件而不是被丢掉。
    valid &= requireContains(
        hookEngine,
        QStringLiteral("output.blockedEventLoop = false;"),
        "bounding hook output can block the Agent event loop");
    valid &= requireContains(
        hookEngine,
        QStringLiteral("output.storedAsArtifact = start > 0;"),
        "over-limit hook output is discarded instead of stored");
    // 这一层不执行命令、不启动进程。
    for (const QString &token : {
             QStringLiteral("QProcess"),
             QStringLiteral("QNetworkAccessManager"),
             QStringLiteral("QSettings"),
             QStringLiteral("SecureStorage"),
             QStringLiteral("QFile "),
             QStringLiteral("QDir "),
             QStringLiteral("system(")}) {
        valid &= requireAbsent(
            hookEngine, token,
            "the hook policy engine holds authority beyond deciding a verdict");
    }
    // 钩子策略引擎还没有调用方。
    for (const QString &source : {mainWindow, extensionCenter}) {
        valid &= requireAbsent(
            source,
            QStringLiteral("HookPolicyEngine"),
            "a hook path reached the product before the action is wired");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("hook_policy_engine"),
        "the hook policy engine is absent from CTest");

    // 更新与移除的判定到这一层才第一次改动持久状态,因此顺序本身就是安全性的一部分:
    // 先收回启用授权,再收回复核记录。授权是真正运行内容的那一半,先收回它意味着任何
    // 中间失败都停在"没有授权、复核记录尚存"上,在注册表双重门禁下那是未启用。反过来
    // 先删复核记录会短暂留下"有授权、无复核"的更坏中间态,而且抹掉的正是审计需要的证据。
    valid &= requireContains(
        lifecycleController,
        QStringLiteral("const QList<ExtensionEnablementGrant> remainingGrants ="),
        "removal does not withdraw the enablement grant first");
    valid &= requireContains(
        lifecycleController,
        QStringLiteral("extension-removal-grant-survived"),
        "a removal whose grant survived can be reported as complete");
    // 一次被确认的写入不是证据。结论只能来自重新读出来的字节。
    valid &= requireContains(
        lifecycleController,
        QStringLiteral("const ExtensionEnablementLedgerStoreResult reread = grantStore->load();"),
        "an acknowledged write is taken as proof the grant was withdrawn");
    valid &= requireContains(
        lifecycleController,
        QStringLiteral("const ExtensionReviewLedgerStoreResult reread = reviewStore->load();"),
        "an acknowledged write is taken as proof the review pin was withdrawn");
    // 读不出来的账本不算收回,也不返回内容:状态未知不是空集合。
    valid &= requireContains(
        lifecycleController,
        QStringLiteral("result.grantRevoked = ledgerUsable(snapshot.grantState)"),
        "an unusable grant ledger counts as a withdrawal");
    valid &= requireContains(
        lifecycleController,
        QStringLiteral("result.reviewRevoked = ledgerUsable(snapshot.reviewState)"),
        "an unusable review ledger counts as a withdrawal");
    valid &= requireContains(
        lifecycleController,
        QStringLiteral("if (ledgerUsable(review.state)) snapshot.pins = review.pins;"),
        "an unreadable review ledger is presented as an empty set");
    valid &= requireContains(
        lifecycleController,
        QStringLiteral("if (ledgerUsable(grants.state)) snapshot.grants = grants.grants;"),
        "an unreadable grant ledger is presented as an empty set");
    // 两半都必须确实收回才算完成。部分完成不能报成成功。
    valid &= requireContains(
        lifecycleController,
        QStringLiteral("result.outcome = (result.grantRevoked && result.reviewRevoked)"),
        "an incomplete removal can be reported as complete");
    // 身份元数据在任何结局下都保留,包括部分完成:抹掉它会让"这份内容曾被授权运行过"
    // 的历史一并消失,而移除恰好最需要留下记录。
    valid &= requireContains(
        lifecycleController,
        QStringLiteral("result.retainedIdentity = verdict.retainedIdentity;"),
        "removal discards the immutable identity metadata");
    // 候选按定义是另一份内容:暂存不为它写入任何权威,也不让它可执行。
    valid &= requireContains(
        lifecycleController,
        QStringLiteral("result.candidateExecutable = false;\n    result.inheritsTrust = false;\n    result.inheritsGrant = false;\n    result.downgrade = verdict.downgrade;"),
        "a staged candidate inherits the previous version's authority");
    valid &= requireContains(
        lifecycleController,
        QStringLiteral("extension-update-target-absent"),
        "an update to an absent target can be staged");
    // 收回按种类与 ID 绑定,因为被移除内容的摘要可能已经不可读;信任与授权的传递仍然
    // 绑定确切内容,那由 ExtensionUpdatePolicy 判定。
    valid &= requireContains(
        lifecycleController,
        QStringLiteral("if (grant.kind == kind && grant.id == id) continue;"),
        "grant withdrawal does not bind the extension kind");
    valid &= requireContains(
        lifecycleController,
        QStringLiteral("if (pin.kind == kind && pin.id == id) continue;"),
        "review withdrawal does not bind the extension kind");
    for (const QString &code : {
             QStringLiteral("extension-removal-store-unavailable"),
             QStringLiteral("extension-removal-grant-ledger-unusable"),
             QStringLiteral("extension-removal-review-ledger-unusable"),
             QStringLiteral("extension-removal-grant-write-failed"),
             QStringLiteral("extension-removal-grant-refresh-failed"),
             QStringLiteral("extension-removal-review-write-failed"),
             QStringLiteral("extension-removal-review-refresh-failed"),
             QStringLiteral("extension-removal-incomplete")}) {
        valid &= requireContains(
            lifecycleController, code,
            "a lifecycle refusal carries no diagnostic");
    }
    // 这一层不安装、不下载、不解压、不执行任何东西。
    for (const QString &token : {
             QStringLiteral("QProcess"),
             QStringLiteral("QNetworkAccessManager"),
             QStringLiteral("QFile "),
             QStringLiteral("QDir "),
             QStringLiteral("system(")}) {
        valid &= requireAbsent(
            lifecycleController, token,
            "the lifecycle controller holds authority beyond deciding and journaling");
    }
    // 生效启用那道门仍然关闭:这一层从不写 effectiveEnabled。
    valid &= requireAbsent(
        lifecycleController,
        QStringLiteral(".effectiveEnabled ="),
        "the lifecycle controller writes effective enablement");
    // 移除动作已经接到产品上，更新动作仍然没有调用方:暂存一次更新需要一份产品目前还
    // 无法构造的候选，而在能构造它之前把入口开出来只会让人以为更新可用。
    valid &= requireAbsent(
        mainWindow,
        QStringLiteral("ExtensionLifecycleController::stageUpdate"),
        "an update path reached the product before a candidate can be produced");
    valid &= requireAbsent(
        extensionCenter,
        QStringLiteral("ExtensionLifecycleController"),
        "the extension center commits lifecycle changes outside the controller");
    valid &= requireContains(
        mainWindow,
        QStringLiteral("ExtensionLifecycleController::remove("),
        "the removal action does not commit through the lifecycle controller");
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_lifecycle_controller"),
        "the extension lifecycle controller is absent from CTest");
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_lifecycle_presentation"),
        "the extension lifecycle presentation is absent from CTest");

    // 移除呈现回答的问题与另外两层不同:它要说清楚这次收回到底收回了什么。这一层存在的
    // 核心理由是一句必须说清楚的话——**移除不删除任何文件**。控制器只写两份账本，界面
    // 如果写\"删除扩展\"，人会认为磁盘上那份内容已经消失，于是停止清理，而内容还在原处，
    // 重新被复核和授权就会重新可用。
    valid &= requireContains(
        lifecyclePresentationHeader,
        QStringLiteral("bool removesSourceContent = false;"),
        "the removal plan cannot state that the disk content survives");
    valid &= requireContains(
        lifecyclePresentation,
        QStringLiteral("plan.removesSourceContent = false;"),
        "the removal plan does not pin the source content as surviving");
    // 这两个不变量必须在每一条返回路径上成立，而不是只在成功路径上被设置:一个被拒绝的
    // 计划同样不删除内容，也同样保留身份。
    valid &= requireOrdered(
        sourceRange(lifecyclePresentation,
                    QStringLiteral("ExtensionRemovalPlan reject("),
                    QStringLiteral("} // namespace")),
        {QStringLiteral("plan.removesSourceContent = false;"),
         QStringLiteral("plan.retainsIdentity = true;")},
        "a rejected removal plan may still claim it deletes content");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("不删除磁盘上的任何内容"),
        "the removal confirmation does not say the disk content survives");
    valid &= requireAbsent(
        extensionCenter,
        QStringLiteral("删除扩展"),
        "the removal confirmation calls itself a deletion");
    // 这次移除是否成立只有一个来源:判定层。呈现层自己再判一遍必然会与它漂移，而漂移的
    // 方向是界面提供一个判定层会拒绝的动作。
    valid &= requireContains(
        lifecyclePresentation,
        QStringLiteral("ExtensionUpdatePolicy::evaluateRemoval(kind, id, record)"),
        "the removal presentation re-decides the removal itself");
    valid &= requireContains(
        lifecyclePresentation,
        QStringLiteral("plan.retainedIdentity = verdict.retainedIdentity;"),
        "the removal presentation constructs a second retained identity");
    // 呈现层不写账本、不删除、不执行任何东西。
    for (const QString &token : {
             QStringLiteral(".effectiveEnabled ="),
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("QFile "),
             QStringLiteral("QDir "),
             QStringLiteral("ExtensionEnablementLedger"),
             QStringLiteral("ExtensionReviewLedger")}) {
        valid &= requireAbsent(
            lifecyclePresentation, token,
            "the removal presentation holds authority beyond rendering");
    }
    // 移除没有门禁:内容漂移、复核被撤回、来源已消失的目标都必须仍然可以被收回，否则一个
    // 被篡改的扩展将永远留着一份已认证的授权。
    for (const QString &token : {
             QStringLiteral("record->trust != ExtensionTrustState::Verified"),
             QStringLiteral("record->compatibility"),
             QStringLiteral("record->installed")}) {
        valid &= requireAbsent(
            lifecyclePresentation, token,
            "the removal presentation gates removal on trust, compatibility, or install");
    }
    valid &= requireContains(
        lifecyclePresentation,
        QStringLiteral("plan.targetAbsent = true;"),
        "removing a vanished target is not distinguishable from removing a listed one");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("ExtensionLifecyclePresentation::buildRemoval("),
        "the removal action does not render through the lifecycle presentation");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("plan.state == ExtensionRemovalPlanState::Ready"),
        "removal eligibility does not come from the presentation verdict");
    // 只发 (kind, id):被收回的内容摘要可能已经不可读，绑定摘要会让一个被篡改的扩展永远
    // 留着一份已认证的授权。
    valid &= requireContains(
        extensionCenterHeader,
        QStringLiteral("void removalRequested(ExtensionKind kind, const QString &id);"),
        "the removal request carries more than the identity it can rely on");
    // 收回确实读过并写过两份账本，因此它的刷新替换两者。
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("populate(records, sourceIssueCodes, ledger, grants)"),
        "a removal refresh does not replace both ledgers it wrote");

    const QString extensionRemovalPath = sourceRange(
        mainWindow,
        QStringLiteral("void MainWindow::startExtensionRemovalOperation("),
        QStringLiteral("void MainWindow::onHelpClicked()"));
    valid &= requireOrdered(
        sourceRange(mainWindow,
                    QStringLiteral("void MainWindow::onExtensionCenterClicked()"),
                    QStringLiteral("ExtensionInventoryInputs MainWindow::")),
        {QStringLiteral("removalRequested"),
         QStringLiteral("startExtensionRemovalOperation")},
        "extension removal UI does not confirm and dispatch removal operations");
    valid &= requireContains(
        extensionRemovalPath,
        QStringLiteral("if (!dialog || m_extensionReviewThread)"),
        "a removal operation can run concurrently with another ledger writer");
    valid &= requireContains(
        extensionRemovalPath,
        QStringLiteral("extension-removal-operation-busy"),
        "a refused concurrent removal operation carries no diagnostic");
    // 部分完成绝不能报成成功:一个只收回了授权的移除仍然留着复核记录，而人正是靠这条
    // 诊断才会去把它清掉。
    valid &= requireOrdered(
        extensionRemovalPath,
        {QStringLiteral("target->setRemovalSnapshot("),
         QStringLiteral("result.outcome != ExtensionLifecycleOutcome::Withdrawn"),
         QStringLiteral("target->showRemovalError(result.errorCode)"),
         QStringLiteral("target->setRemovalBusy(true)")},
        "a partially withdrawn removal is reported as success or left unfrozen");
    valid &= requireAbsent(
        extensionRemovalPath,
        QStringLiteral("ExtensionLifecycleOutcome::PartiallyWithdrawn"),
        "the removal path enumerates the partial outcome instead of requiring completion");

    // 读取一个包不解包。这一层只扫描一个已经存在的目录:解压就是写盘，而在权限、审批、
    // 沙箱与恢复门禁完成之前写盘正是被禁止的那件事。一个"只是为了看看里面有什么"而先解压
    // 到临时目录的读取器，已经把包里的内容落到了磁盘上。
    for (const QString &token : {
             QStringLiteral("QTemporaryDir"),
             QStringLiteral("QTemporaryFile"),
             QStringLiteral("QSaveFile"),
             QStringLiteral("mkpath"),
             QStringLiteral("mkdir"),
             QStringLiteral("QIODevice::WriteOnly"),
             QStringLiteral("QIODevice::Append"),
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("->write("),
             QStringLiteral(".write("),
             QStringLiteral("remove()"),
             QStringLiteral("rename("),
             QStringLiteral("extract"),
             QStringLiteral("unpack")}) {
        valid &= requireAbsent(
            bundleReader, token,
            "the bundle reader can write to disk before the gates exist");
    }
    valid &= requireContains(
        bundleReaderHeader,
        QStringLiteral("**读取一个包不解包。**"),
        "the bundle reader header does not state that reading never unpacks");
    // 归档必须被拒绝而不是被读:读一个归档就意味着先解压。只接受目录。
    valid &= requireContains(
        bundleReader,
        QStringLiteral("extension-bundle-root-not-directory"),
        "an archive path is not refused as a non-directory");
    // 每一个摘要都由磁盘上的字节算出。一个能自己声明摘要的包可以描述它并未携带的内容，
    // 而人恰恰是按逐组件披露做决定的:屏幕上写着这个组件的内容是 A，实际被引入的是 B。
    valid &= requireContains(
        bundleReader,
        QStringLiteral("component.contentIdentity = componentContentIdentity("
                       "tree, component.id, path);"),
        "a component digest does not come from the bytes on disk");
    valid &= requireContains(
        bundleReader,
        QStringLiteral("manifest.contentIdentity = bundleContentIdentity(tree);"),
        "the bundle digest does not come from the bytes on disk");
    // 清单里出现摘要字段一律拒绝，而不是忽略:忽略会让写清单的人以为那个字段生效了，
    // 而实际生效的是磁盘上的字节。未知字段整体被拒绝即覆盖这一点。
    valid &= requireAbsent(
        bundleReader,
        QStringLiteral("\"contentIdentity\""),
        "the bundle reader reads a declared digest out of the manifest");
    valid &= requireAbsent(
        bundleReader,
        QStringLiteral("\"sourceIdentity\""),
        "the bundle reader reads a declared source identity out of the manifest");
    valid &= requireContains(
        bundleReader,
        QStringLiteral("unknown.subtract(allowed);"),
        "unknown manifest fields are ignored instead of refused");
    // 摘要的每一段都按长度分帧:不分帧时 \"ab\"+\"c\" 与 \"a\"+\"bc\" 会算出同一个值，于是两个
    // 内容不同的包共用一份授权。分帧机制由共享树捕获层持有，pin 也必须跟到共享层:留在
    // 读取器门面上只会守住一层转换代码。
    valid &= requireContains(
        treeCapture,
        QStringLiteral("appendLength(hash, static_cast<quint64>(value.size()));"),
        "digest segments are not framed by length");
    valid &= requireOrdered(
        sourceRange(treeCapture,
                    QStringLiteral("QString ExtensionTreeCapture::contentIdentity("),
                    QStringLiteral("QString ExtensionTreeCapture::framedDigest(")),
        {QStringLiteral("appendFramed(&hash, entry.relativePath.toUtf8());"),
         QStringLiteral("appendFramed(&hash, entry.bytes);")},
        "the bundle digest concatenates paths and bytes without framing");
    valid &= requireOrdered(
        sourceRange(bundleReader,
                    QStringLiteral("QString componentContentIdentity("),
                    QStringLiteral("ExtensionComponentKind componentKindOf(")),
        {QStringLiteral("parts.append(componentId.toUtf8());"),
         QStringLiteral("parts.append(entry.relativePath.toUtf8());"),
         QStringLiteral("parts.append(entry.bytes);"),
         QStringLiteral("ExtensionTreeCapture::framedDigest(")},
        "a component digest concatenates paths and bytes without framing");
    // 域分隔是共享层的安全性质:身份域进入被摘要的字节，且未配置的域被直接拒绝而不是退回
    // 默认域。两个调用方的域常量必须彼此不同，否则它们的摘要字节可以互换。
    valid &= requireContains(
        treeCapture,
        QStringLiteral("hash.addData(domain.identityDomain);"),
        "the tree content identity does not bind the caller domain");
    valid &= requireContains(
        treeCapture,
        QStringLiteral("extension-tree-capture-domain-unconfigured"),
        "an unconfigured tree capture domain falls back to a default");
    valid &= requireContains(
        bundleReader,
        QStringLiteral("aegisy-extension-bundle-content/0.1"),
        "the bundle reader content identity domain changed");
    // 不认识的类型串保留为 Unsupported 并带上原始串:丢弃它会让包的实际行为超出预览所
    // 描述的范围，而预览层正是依据 Unsupported 决定失败关闭。
    valid &= requireContains(
        bundleReader,
        QStringLiteral("return ExtensionComponentKind::Unsupported;"),
        "an unrecognised component type is dropped instead of preserved");
    valid &= requireContains(
        bundleReader,
        QStringLiteral("component.declaredType = entry.value("
                       "QStringLiteral(\"type\")).toString();"),
        "the declared component type string is not carried through as evidence");
    valid &= requireContains(
        bundleReader,
        QStringLiteral("component.kind = componentKindOf(component.declaredType);"),
        "the component kind is not derived from the declared type");
    // 能力逐组件原样传递。两个组件各自请求"读文件"与"连网"时，汇总看起来与一个组件同时
    // 请求两者完全一样，而后者才是真正危险的组合;在这一层做任何汇总都会毁掉预览的理由。
    valid &= requireContains(
        bundleReader,
        QStringLiteral("&component.requestedCapabilities"),
        "capabilities are not read per component");
    for (const QString &token : {
             QStringLiteral("manifest.requestedCapabilities"),
             QStringLiteral("allCapabilities"),
             QStringLiteral("QSet<QString> capabilities")}) {
        valid &= requireAbsent(
            bundleReader, token,
            "the bundle reader rolls capabilities up across components");
    }
    // 符号链接必须被拒绝:跟随它会把包的边界之外的字节算进摘要，也会把包外的内容当成包
    // 里的内容披露给人。拒绝动作在共享树捕获层，逐域的诊断代码由该层按调用方前缀拼出；
    // 精确的 `skill-symlink-invalid` 与 `extension-bundle-symlink-invalid` 串由
    // `extension_tree_capture` 测试在运行时逐域锁定。
    valid &= requireContains(
        treeCapture,
        QStringLiteral("code(domain, \"symlink-invalid\")"),
        "a symlink inside a bundle is not refused");
    valid &= requireContains(
        bundleReader,
        QStringLiteral("QStringLiteral(\"extension-bundle\")"),
        "the bundle reader lost its tree-capture diagnostic prefix");
    valid &= requireContains(
        bundleReader,
        QStringLiteral("extension-bundle-root-symlink-invalid"),
        "a symlinked bundle root is not refused");
    // 文件与目录各自都必须做包含性检查:只做一边时另一边就是逃逸的入口。检查在共享树
    // 捕获层，pin 也跟到那里。
    valid &= requireContains(
        sourceRange(treeCapture,
                    QStringLiteral("bool readStableFile("),
                    QStringLiteral("} // namespace")),
        QStringLiteral("!containedBy(root, canonical)"),
        "a bundle file escaping the root is not refused");
    valid &= requireContains(
        sourceRange(treeCapture,
                    QStringLiteral("bool ExtensionTreeCapture::scanDirectory("),
                    QStringLiteral("const ExtensionTreeCaptureEntry *"
                                   "ExtensionTreeCapture::findFile(")),
        QStringLiteral("!containedBy(root, canonical)"),
        "a bundle subdirectory escaping the root is not refused");
    // 共享层同样只读：捕获一棵树绝不写盘。
    for (const QString &token : {
             QStringLiteral("QTemporaryDir"),
             QStringLiteral("QTemporaryFile"),
             QStringLiteral("QSaveFile"),
             QStringLiteral("mkpath"),
             QStringLiteral("mkdir"),
             QStringLiteral("QIODevice::WriteOnly"),
             QStringLiteral("QIODevice::Append"),
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("->write("),
             QStringLiteral(".write("),
             QStringLiteral("remove()"),
             QStringLiteral("rename(")}) {
        valid &= requireAbsent(
            treeCapture, token,
            "the shared tree capture can write to disk before the gates exist");
    }
    // 目录不存在不是错误:还没有包可以导入，与一个畸形的包必须区分开。
    valid &= requireContains(
        bundleReader,
        QStringLiteral("return failure(ExtensionBundleReadState::Empty, QString());"),
        "an absent bundle directory is reported as a malformed bundle");
    // 文本能否安全展示只有一个来源。两份副本会各自漂移，而漂移意味着读取器放行了预览会
    // 拒绝的字符，或者反过来。
    valid &= requireContains(
        bundleReader,
        QStringLiteral("Safety::safeDisplayText("),
        "the bundle reader re-implements display safety");
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_bundle_reader"),
        "the extension bundle reader is absent from CTest");
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_tree_capture"),
        "the shared extension tree capture is absent from CTest");

    // 暂存快照契约：槽 0 固定是路径清单文档，槽 1..N 按清单顺序是文件内容。清单格式串、
    // 树身份绑定与每一类失败关闭的诊断都钉在实现上：它们中的任何一个松掉，读回侧验证的
    // 就是另一份契约。
    valid &= requireContains(
        stagingSnapshotHeader,
        QStringLiteral("class ExtensionStagingSnapshot"),
        "the extension staging snapshot contract has no explicit boundary");
    valid &= requireContains(
        stagingSnapshot,
        QStringLiteral("aegisy-extension-staging-snapshot-manifest/0.1"),
        "the staging snapshot manifest format is not pinned in the builder");
    valid &= requireContains(
        stagingSnapshot,
        QStringLiteral("ConfigurationBackupStore::extensionStagingDomain()"),
        "the staging snapshot contract no longer reconciles against the "
        "staging domain bounds");
    valid &= requireContains(
        stagingSnapshot,
        QStringLiteral("ExtensionTreeCapture::contentIdentity(captureDomain, tree)"),
        "the staging snapshot does not bind the tree identity into the manifest");
    valid &= requireContains(
        stagingSnapshot,
        QStringLiteral("ExtensionTreeCapture::contentIdentity(captureDomain, rebuilt)"),
        "the staging snapshot verifier does not recompute the tree identity");
    // 诊断代号由固定前缀与逐点后缀拼成（与共享树捕获层同一惯例），因此 pin 落在
    // 前缀常量与每一处 `code("...")` 后缀上：任何一个松掉，读回侧验证的就是另一份
    // 契约。
    valid &= requireContains(
        stagingSnapshot,
        QStringLiteral("QStringLiteral(\"extension-staging-snapshot\")"),
        "the staging snapshot diagnostic prefix drifted");
    // 上限对账的每一条都是独立诊断：拒绝一棵捕获层放行但暂存域放不下的树，与拒绝一份
    // 超上限的清单，是两件事。
    for (const QString &diagnostic : {
             QStringLiteral("code(\"file-count-limit\")"),
             QStringLiteral("code(\"file-oversized\")"),
             QStringLiteral("code(\"manifest-oversized\")"),
             QStringLiteral("code(\"payload-oversized\")")}) {
        valid &= requireContains(
            stagingSnapshot, diagnostic,
            "a staging snapshot bounds-reconciliation diagnostic is missing");
    }
    for (const QString &diagnostic : {
             QStringLiteral("code(\"content-digest-mismatch\")"),
             QStringLiteral("code(\"identity-mismatch\")"),
             QStringLiteral("code(\"path-duplicate\")"),
             QStringLiteral("code(\"subject-mismatch\")"),
             QStringLiteral("code(\"manifest-canonical\")")}) {
        valid &= requireContains(
            stagingSnapshot, diagnostic,
            "a staging snapshot integrity diagnostic is missing");
    }
    // 这一层与共享树捕获层同样只读：构建只产出内存快照，验证只读内存快照，任何写盘
    // token 都意味着它长出了未被审查的持久化路径。
    for (const QString &token : {
             QStringLiteral("QTemporaryDir"),
             QStringLiteral("QTemporaryFile"),
             QStringLiteral("QSaveFile"),
             QStringLiteral("mkpath"),
             QStringLiteral("mkdir"),
             QStringLiteral("QIODevice::WriteOnly"),
             QStringLiteral("QIODevice::Append"),
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("removeRecursively")}) {
        valid &= requireAbsent(
            stagingSnapshot, token,
            "the staging snapshot contract can write to disk before the gates "
            "exist");
    }
    // 没有产品调用方：这一层出现在任何产品源里，都意味着扩展备份路径在权限、审批、
    // 沙箱与恢复门禁之前被接通了。
    for (const QString &source : {toolSource, mainWindow, mainWindowHeader,
                                  extensionCenter, workbenchWindow}) {
        valid &= requireAbsent(source, QStringLiteral("ExtensionStagingSnapshot"),
                               "the staging snapshot contract is wired into the "
                               "product before its gates exist");
        valid &= requireAbsent(source,
                               QStringLiteral("extension_staging_snapshot"),
                               "the staging snapshot contract is wired into the "
                               "product before its gates exist");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_staging_snapshot"),
        "the extension staging snapshot contract is absent from CTest");

    // 暂存恢复计划契约：只规划、绝不执行。计划层的每一道门禁都钉在实现上：先验证再
    // 计划（只消费验证侧重建的树，绝不解析未验证字节）、目标根语法与规范化形式、
    // 包含性重查、冲突拒绝、符号链接拒绝、上限重查与计划身份分帧。其中任何一处松
    // 掉，"计划是已验证快照与目标根的纯函数"就不再成立。
    valid &= requireContains(
        restorePlanHeader,
        QStringLiteral("class ExtensionStagingRestorePlanBuilder"),
        "the staging restore plan contract has no explicit boundary");
    valid &= requireContains(
        restorePlanHeader,
        QStringLiteral("class ExtensionStagingRestoreObservation"),
        "the staging restore plan has no injectable observation boundary");
    valid &= requireContains(
        restorePlan,
        QStringLiteral("ExtensionStagingSnapshot::verify(captureDomain, expectedSubject,"),
        "the restore planner plans from unverified snapshot bytes");
    valid &= requireContains(
        restorePlan,
        QStringLiteral("QStringLiteral(\"extension-staging-restore\")"),
        "the restore plan diagnostic prefix drifted");
    for (const QString &diagnostic : {
             QStringLiteral("code(\"destination-invalid\")"),
             QStringLiteral("code(\"destination-unavailable\")"),
             QStringLiteral("code(\"destination-conflict\")"),
             QStringLiteral("code(\"root-symlink\")"),
             QStringLiteral("code(\"symlink-component\")"),
             QStringLiteral("code(\"path-escapes-destination\")"),
             QStringLiteral("code(\"bounds-exceeded\")")}) {
        valid &= requireContains(
            restorePlan, diagnostic,
            "a staging restore plan refusal diagnostic is missing");
    }
    // 包含性重查是纵深防御：清单已被验证，计划层仍逐段重查每一条路径。
    valid &= requireContains(
        restorePlan,
        QStringLiteral("ExtensionTreeCapture::safeEntryName(segment)"),
        "the restore planner lost its containment re-check");
    // 上限重查按暂存域定义而不是本地副本。
    valid &= requireContains(
        restorePlan,
        QStringLiteral("ConfigurationBackupStore::extensionStagingDomain()"),
        "the restore planner no longer re-checks staging domain bounds");
    // 计划身份经共享树捕获层的长度分帧摘要绑定目标根与每一条操作。
    valid &= requireContains(
        restorePlan,
        QStringLiteral("aegisy-extension-staging-restore-plan/0.1"),
        "the restore plan identity domain changed");
    valid &= requireContains(
        restorePlan,
        QStringLiteral("ExtensionTreeCapture::framedDigest("),
        "the restore plan identity is not length-framed");
    valid &= requireContains(
        restorePlan,
        QStringLiteral("parts.append(canonical.toUtf8());"),
        "the restore plan identity does not bind the destination root");
    // already-in-place 是显式语义而不是跳过：操作仍携带期望摘要并进入计划身份。
    valid &= requireContains(
        restorePlanHeader,
        QStringLiteral("bool alreadyInPlace = false;"),
        "already-in-place is an implicit skip instead of explicit plan data");
    // 这一层与快照契约同样只读：计划是纯数据对象，任何写盘 token 都意味着它长出了
    // 未被审查的执行路径。
    for (const QString &token : {
             QStringLiteral("QTemporaryDir"),
             QStringLiteral("QTemporaryFile"),
             QStringLiteral("QSaveFile"),
             QStringLiteral("mkpath"),
             QStringLiteral("mkdir"),
             QStringLiteral("QIODevice::WriteOnly"),
             QStringLiteral("QIODevice::Append"),
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("removeRecursively"),
             QStringLiteral("->write("),
             QStringLiteral(".write("),
             QStringLiteral("rename(")}) {
        valid &= requireAbsent(
            restorePlan, token,
            "the staging restore plan can write to disk before the gates "
            "exist");
    }
    // 没有产品调用方：这一层出现在任何产品源里，都意味着扩展恢复在权限、审批、
    // 沙箱与恢复门禁之前被接通了。
    for (const QString &source : {toolSource, mainWindow, mainWindowHeader,
                                  extensionCenter, workbenchWindow}) {
        valid &= requireAbsent(source,
                               QStringLiteral("ExtensionStagingRestorePlan"),
                               "the staging restore plan is wired into the "
                               "product before its gates exist");
        valid &= requireAbsent(source,
                               QStringLiteral("extension_staging_restore_plan"),
                               "the staging restore plan is wired into the "
                               "product before its gates exist");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_staging_restore_plan"),
        "the staging restore plan contract is absent from CTest");

    // 暂存恢复呈现契约：把已构建的计划如实渲染成人可复核的提示，绝不执行。渲染出来的
    // 必须正是将会执行的，因此全部展示文本过共享展示安全层（不得有第二份安全规则）、
    // 计划身份既以两端指纹展示又以完整身份回显、mcp 主体的整文件警告是强制的、清单
    // 截断有显式标记且身份回声仍绑定完整计划、每一份提示都携带不执行披露。
    valid &= requireContains(
        restorePresentationHeader,
        QStringLiteral("class ExtensionStagingRestorePresentation"),
        "the staging restore presentation has no explicit boundary");
    valid &= requireContains(
        restorePresentation,
        QStringLiteral("QStringLiteral(\"extension-restore-presentation\")"),
        "the restore presentation diagnostic prefix drifted");
    for (const QString &diagnostic : {
             QStringLiteral("code(\"descriptor-corrupt\")"),
             QStringLiteral("code(\"descriptor-invalid\")"),
             QStringLiteral("code(\"now-invalid\")"),
             QStringLiteral("code(\"descriptor-mismatch\")"),
             QStringLiteral("code(\"destination-mismatch\")"),
             QStringLiteral("code(\"subject-invalid\")"),
             QStringLiteral("code(\"subject-unsafe\")"),
             QStringLiteral("code(\"destination-unsafe\")"),
             QStringLiteral("code(\"tree-identity-invalid\")"),
             QStringLiteral("code(\"plan-identity-invalid\")"),
             QStringLiteral("code(\"entry-path-unsafe\")"),
             QStringLiteral("code(\"entry-inconsistent\")"),
             QStringLiteral("code(\"entry-digest-invalid\")"),
             QStringLiteral("code(\"operations-unordered\")"),
             QStringLiteral("code(\"refusal-invalid\")")}) {
        valid &= requireContains(
            restorePresentation, diagnostic,
            "a staging restore presentation refusal diagnostic is missing");
    }
    // 展示安全规则只有一份：呈现层本地重新实现任何字符类别、码位或修剪检查，两份副本
    // 就会各自漂移，而漂移意味着一个界面接受了另一个界面拒绝的双向覆盖字符。
    valid &= requireContains(
        restorePresentation,
        QStringLiteral("Safety::safeDisplayText("),
        "the restore presentation does not delegate display safety");
    for (const QString &token : {
             QStringLiteral("0x2028"), QStringLiteral("0x2066"),
             QStringLiteral("0x200b"), QStringLiteral("0xfeff"),
             QStringLiteral("QChar::Other_Format"),
             QStringLiteral(".unicode()"), QStringLiteral(".trimmed()"),
             QStringLiteral("QChar::Category")}) {
        valid &= requireAbsent(
            restorePresentation, token,
            "the restore presentation re-implements the display safety rules "
            "locally");
    }
    // mcp 主体的整文件警告是强制的：恢复覆盖整个共享设置文件，包括其他服务器的配置。
    // 缺失即呈现失败，而不是少了点缀。
    valid &= requireOrdered(
        restorePresentation,
        {QStringLiteral("subject.startsWith(QStringLiteral(\"mcp:\"))"),
         QStringLiteral("ExtensionStagingRestoreWarning::SharedSettingsFileRestore"),
         QStringLiteral("prompt.sharedFileOverwriteNote = kSharedFileNote;")},
        "an mcp restore can be presented without the shared settings file "
        "warning");
    valid &= requireContains(
        restorePresentation,
        QStringLiteral("包括其中其他服务器的配置"),
        "the shared-file warning does not state whole-file overwrite semantics");
    // 人看到的身份就是复核所绑定的身份：完整身份原样回显，漂移因此可检测。
    for (const QString &token : {
             QStringLiteral("prompt.echoedPlanIdentity = plan.planIdentity;"),
             QStringLiteral("prompt.echoedTreeIdentity = plan.treeIdentity;")}) {
        valid &= requireContains(
            restorePresentation, token,
            "the restore prompt does not echo the exact identity it displayed");
    }
    // 截断仅作用于清单：固定上限、显式"以及另外 N 条"标记、以及"指纹覆盖完整计划"
    // 的绑定声明都必须在场。
    valid &= requireContains(
        restorePresentationHeader,
        QStringLiteral("static constexpr int MaxListedEntries"),
        "the per-entry listing has no fixed cap");
    valid &= requireContains(
        restorePresentation,
        QStringLiteral("prompt.truncationNote = QStringLiteral("),
        "the listing truncation has no explicit marker");
    valid &= requireContains(
        restorePresentation,
        QStringLiteral("prompt.identityBindingNote = QStringLiteral("),
        "the fingerprint binding statement is not rendered");
    // 不执行披露必须在每一份提示上：当前没有任何恢复执行路径，沉默会让人以为有。
    valid &= requireContains(
        restorePresentation,
        QStringLiteral("ExtensionStagingRestoreWarning::RestoreDoesNotExecuteYet"),
        "the restore prompt does not disclose that restores execute nothing yet");
    valid &= requireContains(
        restorePresentation,
        QStringLiteral("prompt.doesNotExecuteNote = kDoesNotExecuteNote;"),
        "the does-not-execute disclosure is not rendered as prose");
    // 构建失败的计划渲染为独立的 Refused 状态，而不是计划摘要。
    valid &= requireContains(
        restorePresentation,
        QStringLiteral("prompt.state = ExtensionStagingRestorePromptState::Refused;"),
        "a failed restore plan is rendered as an approvable plan");
    // 这一层是纯数据加格式化：任何写盘、存储访问、计划构建或审批 token 都意味着它
    // 长出了未被审查的路径。
    for (const QString &token : {
             QStringLiteral("QFile"),
             QStringLiteral("QDir"),
             QStringLiteral("QTemporaryDir"),
             QStringLiteral("QTemporaryFile"),
             QStringLiteral("QSaveFile"),
             QStringLiteral("mkpath"),
             QStringLiteral("mkdir"),
             QStringLiteral("QIODevice::WriteOnly"),
             QStringLiteral("QIODevice::Append"),
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("removeRecursively"),
             QStringLiteral("removeVerified"),
             QStringLiteral("ConfigurationBackupStore"),
             QStringLiteral("ExtensionStagingRestorePlanBuilder"),
             QStringLiteral("ExtensionApprovalPolicy"),
             QStringLiteral("ExtensionEnablementLedger"),
             QStringLiteral("->write("),
             QStringLiteral(".write("),
             QStringLiteral("rename(")}) {
        valid &= requireAbsent(
            restorePresentation, token,
            "the restore presentation holds authority beyond rendering");
    }
    // 没有产品调用方：这一层出现在任何产品源里，都意味着扩展恢复在权限、审批、
    // 沙箱与恢复门禁之前被接通了。
    for (const QString &source : {toolSource, mainWindow, mainWindowHeader,
                                  extensionCenter, workbenchWindow, mcpDialog}) {
        valid &= requireAbsent(source,
                               QStringLiteral("ExtensionStagingRestorePresentation"),
                               "the restore presentation is wired into the "
                               "product before its gates exist");
        valid &= requireAbsent(source,
                               QStringLiteral("extension_staging_restore_presentation"),
                               "the restore presentation is wired into the "
                               "product before its gates exist");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_staging_restore_presentation"),
        "the staging restore presentation is absent from CTest");
    // "仅供复核、不会执行"披露的去留由调用方的显式声明门控：默认在场（不传参的旧
    // 调用方语义一字不变），声明执行在场时省略；被拒绝的计划恒携带它。
    valid &= requireContains(
        restorePresentationHeader,
        QStringLiteral("bool executionAvailable = false"),
        "the execution-availability declaration lost its safe default");
    valid &= requireContains(
        restorePresentation,
        QStringLiteral("if (!executionAvailable)"),
        "the does-not-execute disclosure is no longer gated on the caller's "
        "declaration");

    // 暂存恢复审批策略：呈现决定"能不能问"，这一层决定"这个回答是否构成恢复授权"。
    // 凭据必须与渲染出的提示逐项对齐——主体、备份 id、目标根、回显的计划身份与树身份
    // 两者都绑定（计划身份绑定目标根与全部操作，树身份绑定内容；只绑其一就留下漂移
    // 通道）、确切的披露警告集合、高风险的逐次确认；备份的清点验证状态是没有默认值
    // 的必需输入。批准产出的是纯数据凭据：不执行、不持久化、不写任何东西。
    valid &= requireContains(
        restoreApprovalHeader,
        QStringLiteral("class ExtensionStagingRestoreApprovalPolicy"),
        "the staging restore approval policy has no explicit boundary");
    valid &= requireContains(
        restoreApproval,
        QStringLiteral("QStringLiteral(\"extension-restore-approval\")"),
        "the restore approval diagnostic prefix drifted");
    for (const QString &diagnostic : {
             QStringLiteral("code(\"declined\")"),
             QStringLiteral("code(\"prompt-unpresentable\")"),
             QStringLiteral("code(\"prompt-refused\")"),
             QStringLiteral("code(\"backup-unverified\")"),
             QStringLiteral("code(\"subject-mismatch\")"),
             QStringLiteral("code(\"backup-mismatch\")"),
             QStringLiteral("code(\"destination-mismatch\")"),
             QStringLiteral("code(\"identity-invalid\")"),
             QStringLiteral("code(\"plan-drift\")"),
             QStringLiteral("code(\"tree-drift\")"),
             QStringLiteral("code(\"warning-duplicate\")"),
             QStringLiteral("code(\"warning-undisclosed\")"),
             QStringLiteral("code(\"warning-unknown\")"),
             QStringLiteral("code(\"confirmation-required\")")}) {
        valid &= requireContains(
            restoreApproval, diagnostic,
            "a staging restore approval refusal diagnostic is missing");
    }
    // 两个身份都必须与回显逐字节对齐：计划身份绑定操作、树身份绑定内容，缺任一维
    // 都是漂移通道。
    for (const QString &token : {
             QStringLiteral(
                 "acknowledgement.approvedPlanIdentity != prompt.echoedPlanIdentity"),
             QStringLiteral(
                 "acknowledgement.approvedTreeIdentity != prompt.echoedTreeIdentity")}) {
        valid &= requireContains(
            restoreApproval, token,
            "the restore approval does not align both echoed identities");
    }
    // 备份的清点验证状态是必需参数：没有默认值，且必须等值于 ListedIntact——等值
    // 比较让任何未归类的未来状态失败关闭。
    valid &= requireContains(
        restoreApprovalHeader,
        QStringLiteral("ExtensionStagingBackupEntryVerification backupVerification"),
        "the backup verification state is not a required approval input");
    valid &= requireContains(
        restoreApproval,
        QStringLiteral("!= ExtensionStagingBackupEntryVerification::ListedIntact"),
        "an unverified backup can be approved for restore");
    // 展示安全规则只有一份：审批层本地重新实现任何字符类别、码位或修剪检查，两份
    // 副本就会各自漂移。
    valid &= requireContains(
        restoreApproval,
        QStringLiteral("Safety::hashIdentity("),
        "the restore approval does not delegate identity shape checks");
    for (const QString &token : {
             QStringLiteral("0x2028"), QStringLiteral("0x2066"),
             QStringLiteral("0x200b"), QStringLiteral("0xfeff"),
             QStringLiteral("QChar::Other_Format"),
             QStringLiteral(".unicode()"), QStringLiteral(".trimmed()"),
             QStringLiteral("QChar::Category")}) {
        valid &= requireAbsent(
            restoreApproval, token,
            "the restore approval re-implements the display safety rules "
            "locally");
    }
    // 高风险集合：共享设置文件恢复无条件确认；目标非空只有在仍有待写文件时才是
    // 冲突邻接；纯信息性警告（不执行披露、already-in-place、大型、陈旧）不要求
    // 确认；未归类的警告失败关闭为需要确认。
    valid &= requireOrdered(
        restoreApproval,
        {QStringLiteral(
             "case ExtensionStagingRestoreWarning::SharedSettingsFileRestore:"),
         QStringLiteral("return true;"),
         QStringLiteral(
             "case ExtensionStagingRestoreWarning::DestinationNotEmpty:"),
         QStringLiteral("return fileWriteCount > 0;"),
         QStringLiteral(
             "case ExtensionStagingRestoreWarning::AlreadyInPlaceFiles:"),
         QStringLiteral(
             "case ExtensionStagingRestoreWarning::RestoreDoesNotExecuteYet:"),
         QStringLiteral("return false;"),
         QStringLiteral("// 未知警告按需要确认处理"),
         QStringLiteral("return true;")},
        "the restore approval high-risk classification drifted");
    valid &= requireContains(
        restoreApproval,
        QStringLiteral("if (requiresConfirmation && !acknowledgement.highRiskConfirmed)"),
        "a high-risk restore approval can succeed without explicit confirmation");
    // 不提供任何可记住的批准范围：同一份备份对另一个目标根重新计划就是另一份
    // 计划，任何宽于确切计划身份的记住范围都会转移同意，而确切计划身份本身就是
    // 凭据绑定的内容。
    for (const QString &token : {
             QStringLiteral("RememberForThisBackup"),
             QStringLiteral("RememberForThisSubject"),
             QStringLiteral("RememberAlways")}) {
        valid &= requireAbsent(
            restoreApprovalHeader, token,
            "a remembered restore approval rule is broader than the exact "
            "plan identity");
    }
    // 审批不执行、不持久化、不写任何东西：凭据是纯数据，没有任何执行钩子。
    for (const QString &token : {
             QStringLiteral("QFile"),
             QStringLiteral("QDir"),
             QStringLiteral("QTemporaryDir"),
             QStringLiteral("QTemporaryFile"),
             QStringLiteral("QSaveFile"),
             QStringLiteral("mkpath"),
             QStringLiteral("mkdir"),
             QStringLiteral("QIODevice::WriteOnly"),
             QStringLiteral("QIODevice::Append"),
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("SecureStorage"),
             QStringLiteral("Ledger"),
             QStringLiteral("removeRecursively"),
             QStringLiteral("removeVerified"),
             QStringLiteral("ConfigurationBackupStore"),
             QStringLiteral("ExtensionStagingRestorePlanBuilder"),
             QStringLiteral("ExtensionApprovalPolicy"),
             QStringLiteral("->write("),
             QStringLiteral(".write("),
             QStringLiteral("rename("),
             QStringLiteral("effectiveEnabled")}) {
        valid &= requireAbsent(
            restoreApproval, token,
            "the restore approval holds authority beyond judging a credential");
    }
    // 产品调用方是封闭的：恢复批准对话（扩展中心，经 requiresExplicitConfirmation 委派
    // 高风险分类）与恢复编排器（flow 组件，自己的 CTest 目标钉住）。这一层出现在任何
    // 其他产品源里，都意味着扩展恢复绕过了那两处被审查的接线。
    for (const QString &source : {toolSource, mainWindow, mainWindowHeader,
                                  workbenchWindow, mcpDialog}) {
        valid &= requireAbsent(source,
                               QStringLiteral("ExtensionStagingRestoreApprovalPolicy"),
                               "the restore approval policy is wired into an "
                               "unreviewed product source");
        valid &= requireAbsent(source,
                               QStringLiteral("extension_staging_restore_approval"),
                               "the restore approval policy is wired into an "
                               "unreviewed product source");
    }
    // 对话框只做委派：高风险分类只有审批策略一份，对话框绝不另算。
    valid &= requireContains(
        extensionCenter,
        QStringLiteral(
            "ExtensionStagingRestoreApprovalPolicy::requiresExplicitConfirmation("),
        "the restore dialog re-classifies high-risk warnings locally");
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_staging_restore_approval"),
        "the staging restore approval policy is absent from CTest");

    // 暂存恢复执行器：把（凭据 + 计划 + 已验证快照）变成文件系统现实的唯一组件。
    // 它与 ToolManager 经审查的配置写入同属 COMPANION 侧、用户主导的写入类；凭据绑定
    // 是全部意义所在——给计划 A 签发的凭据永远执行不了计划 B。
    valid &= requireContains(
        restoreExecutorHeader,
        QStringLiteral("class ExtensionStagingRestoreExecutor"),
        "the staging restore executor has no explicit boundary");
    // 三件套入口：快照、计划、凭据都是必需参数，观察接口不可省。
    valid &= requireContains(
        restoreExecutorHeader,
        QStringLiteral("const ConfigurationBackupSnapshot &snapshot"),
        "the restore executor does not take the snapshot as a required input");
    valid &= requireContains(
        restoreExecutorHeader,
        QStringLiteral(
            "const ExtensionStagingRestoreApprovalVerdict &credential"),
        "the restore executor does not take the credential as a required "
        "input");
    // 快照在执行开始时重新验证：绝不相信调用方"已验证"的声称。
    valid &= requireContains(
        restoreExecutor,
        QStringLiteral("ExtensionStagingSnapshot::verify("),
        "the restore executor trusts a caller's verification claim");
    // 凭据双身份绑定：计划身份与树身份都逐字节比对，缺一维就是漂移通道。
    valid &= requireOrdered(
        restoreExecutor,
        {QStringLiteral(
             "credential.state != ExtensionStagingRestoreApprovalState::Authorized"),
         QStringLiteral(
             "credential.authorizedPlanIdentity != plan.planIdentity"),
         QStringLiteral(
             "credential.authorizedTreeIdentity != plan.treeIdentity")},
        "the restore executor does not bind both credential identities");
    // 计划必须就是从这份快照构建的那一份：操作序列按验证侧重建的树逐字段重对齐。
    valid &= requireContains(
        restoreExecutor,
        QStringLiteral("plan.treeIdentity != treeIdentity"),
        "the restore executor does not re-bind the plan to the snapshot");
    // 执行前重观察：计划与执行之间的任何漂移都在第一个字节写入之前拒绝。
    valid &= requireOrdered(
        restoreExecutor,
        {QStringLiteral("observation->canonicalRoot()"),
         QStringLiteral("canonical != plan.destinationRoot"),
         QStringLiteral("reobserveOperation(observation, operation, &error)")},
        "the restore executor executes without a pre-flight re-observation");
    // 逐条操作的包含性重查是最后一道防线；写入只落在计划的精确路径上。
    valid &= requireContains(
        restoreExecutor,
        QStringLiteral("containedRelativePath(operation.relativePath)"),
        "the restore executor lost its per-operation containment re-check");
    // 原子写惯例：QSaveFile（临时文件加提交重命名），写后重读重哈希复核。
    valid &= requireContains(
        restoreExecutor,
        QStringLiteral("QSaveFile output(absolute)"),
        "the restore executor does not write atomically via QSaveFile");
    valid &= requireContains(
        restoreExecutor,
        QStringLiteral("code(\"post-write-mismatch\")"),
        "the restore executor lost its post-write verification");
    // already-in-place 是跳过并复核，不是静默跳过：重读既有字节并重哈希。
    valid &= requireContains(
        restoreExecutor,
        QStringLiteral("SkippedVerified"),
        "already-in-place became an implicit skip in the executor");
    // 失败立即停下、绝不越过失败点继续；不做自动回滚；部分应用与完整恢复可区分。
    for (const QString &token : {
             QStringLiteral("ExtensionStagingRestoreExecutionState::Partial"),
             QStringLiteral("ExtensionStagingRestoreExecutionState::Refused"),
             QStringLiteral(
                 "ExtensionStagingRestoreExecutionState::NotStarted"),
             QStringLiteral(
                 "ExtensionStagingRestoreExecutionState::Complete")}) {
        valid &= requireContains(
            restoreExecutor, token,
            "the restore executor lost a final-state classification");
    }
    // 诊断前缀与全部代号各自独立。
    valid &= requireContains(
        restoreExecutor,
        QStringLiteral("QStringLiteral(\"extension-restore-execution\")"),
        "the restore executor diagnostic prefix drifted");
    for (const QString &diagnostic : {
             QStringLiteral("code(\"plan-invalid\")"),
             QStringLiteral("code(\"credential-not-authorized\")"),
             QStringLiteral("code(\"credential-plan-mismatch\")"),
             QStringLiteral("code(\"credential-tree-mismatch\")"),
             QStringLiteral("code(\"path-escapes-destination\")"),
             QStringLiteral("code(\"plan-snapshot-mismatch\")"),
             QStringLiteral("code(\"destination-unavailable\")"),
             QStringLiteral("code(\"destination-invalid\")"),
             QStringLiteral("code(\"root-symlink\")"),
             QStringLiteral("code(\"symlink-component\")"),
             QStringLiteral("code(\"destination-drift\")"),
             QStringLiteral("code(\"directory-create-failed\")"),
             QStringLiteral("code(\"write-failed\")")}) {
        valid &= requireContains(
            restoreExecutor, diagnostic,
            "a staging restore executor diagnostic is missing");
    }
    // 执行器是写组件，但写得有界：没有进程派生、没有网络、没有第二份写入机制，
    // 也不触碰审计链（执行结果的审计记录是另一道接线决定）。
    for (const QString &token : {
             QStringLiteral("QProcess"),
             QStringLiteral("QNetworkAccessManager"),
             QStringLiteral("QTcpSocket"),
             QStringLiteral("QUdpSocket"),
             QStringLiteral("AuditLedger"),
             QStringLiteral("removeRecursively")}) {
        valid &= requireAbsent(
            restoreExecutor, token,
            "the restore executor grew an unaudited capability");
    }
    // 没有产品调用方：执行器出现在任何产品源里，都意味着恢复在 UI 复核接线被
    // 审查之前就获得了执行能力。
    for (const QString &source : {toolSource, mainWindow, mainWindowHeader,
                                  extensionCenter, workbenchWindow,
                                  mcpDialog}) {
        valid &= requireAbsent(source,
                               QStringLiteral("ExtensionStagingRestoreExecutor"),
                               "the restore executor is wired into the "
                               "product before its wiring is reviewed");
        valid &= requireAbsent(source,
                               QStringLiteral("extension_staging_restore_executor"),
                               "the restore executor is wired into the "
                               "product before its wiring is reviewed");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_staging_restore_executor"),
        "the staging restore executor is absent from CTest");

    // 暂存恢复审批审计链：恢复审批是授权，只要它被记录，一份可编辑的普通文件就能
    // 伪造"用户同意过这次恢复"——这正是复核记录与启用授权各自获得认证账本的同一个
    // 论证。审计链绑定凭据绑定的内容，域分隔贯穿模式串/MAC 域/身份域/授权模式串/
    // QSettings 键/安全存储作用域；它只记录决定（批准与拒绝），不判定批准、不执行。
    valid &= requireContains(
        restoreAuditLedgerHeader,
        QStringLiteral("class ExtensionStagingRestoreAuditLedger"),
        "the restore audit ledger has no explicit boundary");
    valid &= requireContains(
        restoreAuditStoreHeader,
        QStringLiteral("class ExtensionStagingRestoreAuditLedgerStore"),
        "the restore audit ledger store has no explicit boundary");
    // 条目形状绑定凭据绑定的内容：主体、备份 id、目标根、计划身份与树身份两者、
    // 披露的警告集合、决定与决定时间，全部按序进入 MAC 预映像。
    valid &= requireOrdered(
        restoreAuditLedger,
        {QStringLiteral("append(&input, QByteArray::number(generation));"),
         QStringLiteral("append(&input, entry.subject.toUtf8());"),
         QStringLiteral("append(&input, entry.backupId.toUtf8());"),
         QStringLiteral("append(&input, entry.destinationRoot.toUtf8());"),
         QStringLiteral("append(&input, entry.planIdentity.toUtf8());"),
         QStringLiteral("append(&input, entry.treeIdentity.toUtf8());"),
         QStringLiteral("decisionName(entry.decision).toUtf8()"),
         QStringLiteral("decidedAtLabel(entry.decidedAt).toUtf8()")},
        "the restore audit entry shape does not bind the full credential");
    for (const QString &token : {
             QStringLiteral("QStringLiteral(\"backup_id\")"),
             QStringLiteral("QStringLiteral(\"destination_root\")"),
             QStringLiteral("QStringLiteral(\"plan_identity\")"),
             QStringLiteral("QStringLiteral(\"tree_identity\")"),
             QStringLiteral("QStringLiteral(\"warnings\")"),
             QStringLiteral("QStringLiteral(\"decision\")"),
             QStringLiteral("QStringLiteral(\"decided_at\")")}) {
        valid &= requireContains(
            restoreAuditLedger, token,
            "the restore audit entry lost a persisted field");
    }
    // 批准与拒绝都被记录：只记录批准的日志无法区分"用户拒绝了"与"从未问过用户"。
    for (const QString &token : {QStringLiteral("QStringLiteral(\"approved\")"),
                                 QStringLiteral("QStringLiteral(\"declined\")")}) {
        valid &= requireContains(
            restoreAuditLedger, token,
            "the restore audit ledger no longer records both decisions");
    }
    // 域分隔的字面量：模式串、MAC 域、身份域与身份前缀全部独立，且不得引用复核或
    // 启用两个域的任何常量。
    for (const QString &token : {
             QStringLiteral("aegisy-extension-restore-audit-ledger/0.1"),
             QStringLiteral("aegisy-extension-restore-audit-ledger-hmac/0.1"),
             QStringLiteral(
                 "aegisy-extension-restore-audit-ledger-identity/0.1"),
             QStringLiteral("extension-restore-audit-ledger:sha256:")}) {
        valid &= requireContains(
            restoreAuditLedger, token,
            "the restore audit ledger lost its own persisted domain");
    }
    for (const QString &token : {
             QStringLiteral("aegisy-extension-review-ledger"),
             QStringLiteral("aegisy-extension-enablement-ledger")}) {
        valid &= requireAbsent(
            restoreAuditLedger, token,
            "the restore audit ledger borrows another evidence domain");
    }
    // 诊断前缀与代号：载荷层与持久化层各自独立归属。
    valid &= requireContains(
        restoreAuditLedger,
        QStringLiteral("extension-restore-audit-ledger"),
        "the restore audit ledger diagnostic prefix drifted");
    for (const QString &diagnostic : {
             QStringLiteral("code(\"oversized\")"),
             QStringLiteral("code(\"key-unavailable\")"),
             QStringLiteral("code(\"record-invalid\")"),
             QStringLiteral("code(\"entry-invalid\")"),
             QStringLiteral("code(\"entry-limit\")"),
             QStringLiteral("code(\"mac-mismatch\")")}) {
        valid &= requireContains(
            restoreAuditLedger, diagnostic,
            "a restore audit ledger diagnostic is missing");
    }
    valid &= requireContains(
        restoreAuditStore,
        QStringLiteral("extension-restore-audit-store"),
        "the restore audit store diagnostic prefix drifted");
    for (const QString &diagnostic : {
             QStringLiteral("code(\"record-without-authority\")"),
             QStringLiteral("code(\"record-deleted\")"),
             QStringLiteral("code(\"record-superseded\")"),
             QStringLiteral("code(\"reserved-unresolved\")"),
             QStringLiteral("code(\"generation-conflict\")"),
             QStringLiteral("code(\"generation-exhausted\")"),
             QStringLiteral("code(\"entries-cap\")"),
             QStringLiteral("code(\"entries-invalid\")"),
             QStringLiteral("code(\"discard-not-required\")"),
             QStringLiteral("code(\"authority-invalid\")")}) {
        valid &= requireContains(
            restoreAuditStore, diagnostic,
            "a restore audit store diagnostic is missing");
    }
    // 追加语义是完整集合比较并交换加有界上限：写满以独立代号拒绝，绝不驱逐历史。
    valid &= requireOrdered(
        restoreAuditStore,
        {QStringLiteral("if (entries.size() > MaxEntries)"),
         QStringLiteral("code(\"entries-cap\")")},
        "a full restore audit log evicts history instead of refusing");
    // 持久化域：授权模式串与 QSettings 键独立。
    for (const QString &token : {
             QStringLiteral(
                 "aegisy-extension-restore-audit-ledger-authority/0.1"),
             QStringLiteral("extensions/restore-audit-ledger/record")}) {
        valid &= requireContains(
            restoreAuditStore, token,
            "the restore audit store lost its own persistence domain");
    }
    // 安全存储作用域：槽位、框架模式串、摘要域与错误前缀全部独立，不采纳任何旧
    // 授权，也不引用复核或启用的作用域命名空间。
    for (const QString &token : {
             QStringLiteral(
                 "extensions/restore-audit-ledger-authority/slot-a/v1"),
             QStringLiteral(
                 "extensions/restore-audit-ledger-authority/slot-b/v1"),
             QStringLiteral(
                 "aegisy-extension-restore-audit-ledger-authority-slot/0.1"),
             QStringLiteral(
                 "aegisy-extension-restore-audit-ledger-authority-slot-digest/0.1"),
             QStringLiteral("extension-restore-audit-authority-slot-"),
             QStringLiteral("extension-restore-audit-secure")}) {
        valid &= requireContains(
            restoreAuditAdapter, token,
            "the restore audit authority adapter lost its own persisted "
            "domain");
    }
    for (const QString &token : {QStringLiteral("review-ledger"),
                                 QStringLiteral("enablement-ledger"),
                                 QStringLiteral("value.legacyScope =")}) {
        valid &= requireAbsent(
            restoreAuditAdapter, token,
            "the restore audit authority reuses another ledger's scope "
            "namespace");
    }
    // 记录层绝不判定批准是否有效、绝不执行：两个审批域互不借用，编解码层不接触
    // 持久化，两层都没有任何执行钩子。
    for (const QString &source : {restoreAuditLedger, restoreAuditLedgerHeader,
                                  restoreAuditStore, restoreAuditStoreHeader,
                                  restoreAuditAdapter}) {
        valid &= requireAbsent(
            source, QStringLiteral("ExtensionStagingRestoreApprovalPolicy"),
            "the restore audit ledger evaluates approvals");
        valid &= requireAbsent(
            source, QStringLiteral("ExtensionStagingRestoreApprovalVerdict"),
            "the restore audit ledger evaluates approvals");
        for (const QString &token : {
                 QStringLiteral("QProcess"), QStringLiteral("QSaveFile"),
                 QStringLiteral("mkpath"), QStringLiteral("removeRecursively"),
                 QStringLiteral("rename("),
                 QStringLiteral("ConfigurationBackupStore"),
                 QStringLiteral("ExtensionStagingRestorePlanBuilder"),
                 QStringLiteral("effectiveEnabled")}) {
            valid &= requireAbsent(
                source, token,
                "the restore audit ledger holds authority beyond recording");
        }
    }
    for (const QString &source : {restoreAuditLedger,
                                  restoreAuditLedgerHeader}) {
        valid &= requireAbsent(
            source, QStringLiteral("QSettings"),
            "the restore audit codec touches persistence");
    }
    // 决定时间必须是 UTC：歧义的本地墙钟时间不属于审计记录。
    valid &= requireContains(
        restoreAuditLedger, QStringLiteral("Qt::UTC"),
        "the restore audit ledger accepts ambiguous local decision times");
    // 展示安全规则只有一份：审计层本地重新实现任何字符类别或码位检查，两份副本
    // 就会各自漂移。
    valid &= requireContains(
        restoreAuditLedger,
        QStringLiteral("Safety::hashIdentity("),
        "the restore audit ledger does not delegate identity shape checks");
    for (const QString &token : {
             QStringLiteral("0x2028"), QStringLiteral("0x2066"),
             QStringLiteral("0x200b"), QStringLiteral("0xfeff"),
             QStringLiteral("QChar::Other_Format"),
             QStringLiteral(".unicode()"), QStringLiteral(".trimmed()"),
             QStringLiteral("QChar::Category")}) {
        valid &= requireAbsent(
            restoreAuditLedger, token,
            "the restore audit ledger re-implements the display safety rules "
            "locally");
    }
    // 产品调用方是封闭的：恢复提交 worker（MainWindow，构造安全存储适配器与账本存储后
    // 交给编排器）、恢复结果报告（扩展中心，只读审计决定枚举做分支文案）与恢复审计轨迹
    // 只读视图（扩展中心，只消费 MainWindow 只读 worker 读出的 store 结果）是仅有的合法
    // 接线。账本与存储 token 出现在任何其他产品源里，都意味着恢复审计绕过了那条被审查
    // 的提交/读取路径。
    for (const QString &source : {toolSource, mainWindowHeader,
                                  workbenchWindow, mcpDialog}) {
        valid &= requireAbsent(source,
                               QStringLiteral("ExtensionStagingRestoreAuditLedger"),
                               "the restore audit ledger is wired into an "
                               "unreviewed product source");
        valid &= requireAbsent(source,
                               QStringLiteral("extension_staging_restore_audit"),
                               "the restore audit ledger is wired into an "
                               "unreviewed product source");
    }
    // 扩展中心只读审计决定枚举（declined 的如实报告），绝不触碰账本本身。
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("ExtensionStagingRestoreAuditDecision::Declined"),
        "the restore result report lost the honest declined branch");
    // MainWindow 的接线形状钉死：两半持久化都在提交 worker 内构造，绝不跨线程共享。
    valid &= requireOrdered(
        mainWindow,
        {QStringLiteral(
             "SecureStorageExtensionRestoreAuditLedgerAdapter authority;"),
         QStringLiteral(
             "ExtensionStagingRestoreAuditLedgerStore store(&authority, &settings);"),
         QStringLiteral("ExtensionStagingRestoreFlow::commit(")},
        "the restore commit worker lost its ledger construction order");
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_staging_restore_audit_ledger"),
        "the restore audit ledger is absent from CTest");

    // 恢复审批控制器：把审批策略与审计链按"读取 → 判定 → 比较并交换提交 → 重新
    // 读取"的顺序串起来。它只拥有顺序纪律：判定完全委托给审批策略（调用而不是
    // 重新实现），策略拒绝零写入，人为拒绝同样被记录但不携带授权，并发冲突以
    // 独立代号报告，提交后的重读才是生效的权威，没有任何执行路径。
    valid &= requireContains(
        restoreControllerHeader,
        QStringLiteral("class ExtensionStagingRestoreController"),
        "the restore approval controller has no explicit boundary");
    valid &= requireContains(
        restoreControllerHeader,
        QStringLiteral("struct ExtensionStagingRestoreRecordResult"),
        "the restore approval controller has no explicit result shape");
    // 委派而不是复制：判定只有审批策略一份，控制器绝不重新推导任何批准维度。
    valid &= requireContains(
        restoreController,
        QStringLiteral("ExtensionStagingRestoreApprovalPolicy::evaluate("),
        "the restore controller does not delegate the approval decision");
    for (const QString &token : {
             QStringLiteral("acknowledgement.subject != prompt.subject"),
             QStringLiteral("acknowledgement.backupId != prompt.backupId"),
             QStringLiteral("acknowledgement.destinationRoot !="),
             QStringLiteral(
                 "acknowledgement.approvedPlanIdentity != prompt.echoedPlanIdentity"),
             QStringLiteral(
                 "acknowledgement.approvedTreeIdentity != prompt.echoedTreeIdentity"),
             QStringLiteral("requiresExplicitConfirmation"),
             QStringLiteral("warning-undisclosed"),
             QStringLiteral("warning-duplicate"),
             QStringLiteral("warning-unknown"),
             QStringLiteral("backup-unverified"),
             QStringLiteral("confirmation-required"),
             QStringLiteral("plan-drift"),
             QStringLiteral("tree-drift")}) {
        valid &= requireAbsent(
            restoreController, token,
            "the restore controller re-implements the approval evaluation");
    }
    // 拒绝与人为拒绝的区分：人为拒绝是策略以 declined 代号拒绝【且】提示确实
    // 可展示——只有那时才有一个有效问题被回答了"不"；其余一切拒绝都不是决定。
    valid &= requireOrdered(
        restoreController,
        {QStringLiteral(
             "== QStringLiteral(\"extension-restore-approval-declined\")"),
         QStringLiteral("ExtensionStagingRestorePromptState::Ready"),
         QStringLiteral("if (!humanApprove && !humanDecline)")},
        "the restore controller no longer distinguishes a genuine decline "
        "from a policy refusal");
    // 策略拒绝零写入：早退在任何账本读取之前，零写入可由身份前后比较证明。
    valid &= requireOrdered(
        restoreController,
        {QStringLiteral("if (!humanApprove && !humanDecline)"),
         QStringLiteral("return result;"),
         QStringLiteral(
             "const ExtensionStagingRestoreAuditStoreResult current = store->load();")},
        "a policy refusal still touches the audit ledger");
    // 读不出的审计链阻止记录：只容忍 Ready/Empty，其余状态原样透传代号。
    valid &= requireOrdered(
        restoreController,
        {QStringLiteral(
             "current.state != ExtensionStagingRestoreAuditStoreState::Ready"),
         QStringLiteral("code(\"ledger-unusable\")"),
         QStringLiteral("return result;")},
        "an unreadable restore audit ledger no longer blocks recording");
    // 并发决定由代号比较并交换裁决：提交的是读到的代号，冲突以存储的独立代号
    // 透传报告，不静默重试。
    valid &= requireOrdered(
        restoreController,
        {QStringLiteral(
             "store->replace(next, current.generation, &committed, &errorCode)"),
         QStringLiteral("code(\"store-write-failed\")"),
         QStringLiteral(
             "const ExtensionStagingRestoreAuditStoreResult refreshed = store->load();"),
         QStringLiteral("code(\"store-refresh-failed\")")},
        "the restore controller no longer commits through the generation CAS "
        "and re-reads after commit");
    // 诊断前缀与控制器自己的代号。
    valid &= requireContains(
        restoreController,
        QStringLiteral("extension-restore-controller"),
        "the restore controller diagnostic prefix drifted");
    for (const QString &diagnostic : {
             QStringLiteral("code(\"store-unavailable\")"),
             QStringLiteral("code(\"ledger-unusable\")"),
             QStringLiteral("code(\"store-write-failed\")"),
             QStringLiteral("code(\"store-refresh-failed\")")}) {
        valid &= requireContains(
            restoreController, diagnostic,
            "a restore controller diagnostic is missing");
    }
    // 控制器不执行恢复、不写审计链之外的任何字节、不接触文件系统与计划构建：
    // 写盘只经由审计存储的 replace，任何写/执行 token 都意味着它长出了未被
    // 审查的路径。
    for (const QString &source : {restoreController, restoreControllerHeader}) {
        for (const QString &token : {
                 QStringLiteral("QFile"), QStringLiteral("QDir"),
                 QStringLiteral("QTemporaryDir"), QStringLiteral("QTemporaryFile"),
                 QStringLiteral("QSaveFile"), QStringLiteral("mkpath"),
                 QStringLiteral("mkdir"), QStringLiteral("QIODevice::WriteOnly"),
                 QStringLiteral("QIODevice::Append"), QStringLiteral("QProcess"),
                 QStringLiteral("removeRecursively"),
                 QStringLiteral("removeVerified"),
                 QStringLiteral("ConfigurationBackupStore"),
                 QStringLiteral("ExtensionStagingRestorePlanBuilder"),
                 QStringLiteral("->write("), QStringLiteral(".write("),
                 QStringLiteral("rename("),
                 QStringLiteral("effectiveEnabled")}) {
            valid &= requireAbsent(
                source, token,
                "the restore controller holds authority beyond recording");
        }
        // 展示安全规则只有一份：控制器不本地重新实现任何字符类别或码位检查。
        for (const QString &token : {
                 QStringLiteral("0x2028"), QStringLiteral("0x2066"),
                 QStringLiteral("0x200b"), QStringLiteral("0xfeff"),
                 QStringLiteral("QChar::Other_Format"),
                 QStringLiteral(".unicode()"), QStringLiteral(".trimmed()"),
                 QStringLiteral("QChar::Category")}) {
            valid &= requireAbsent(
                source, token,
                "the restore controller re-implements the display safety "
                "rules locally");
        }
    }
    // 没有产品调用方：这一层出现在任何产品源里，都意味着恢复决定的记录在恢复
    // 执行器存在之前就被接通了。
    for (const QString &source : {toolSource, mainWindow, mainWindowHeader,
                                  extensionCenter, workbenchWindow, mcpDialog}) {
        valid &= requireAbsent(source,
                               QStringLiteral("ExtensionStagingRestoreController"),
                               "the restore approval controller is wired into the "
                               "product before any restore executor exists");
        valid &= requireAbsent(source,
                               QStringLiteral("extension_staging_restore_controller"),
                               "the restore approval controller is wired into the "
                               "product before any restore executor exists");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_staging_restore_controller"),
        "the restore approval controller is absent from CTest");

    // 扩展移除计划契约：把一次移除请求翻译成完整、有序、可判定的计划——备份、
    // 授权收回、复核收回、内容删除列表、元数据保留声明。顺序由类型形状固定
    // （五个独立字段而不是可重排的步骤列表），授权收回结构上先于复核收回；
    // 撤销语义完全委派给既有工作流；纯数据——无文件系统写、无账本写、无存储、
    // 无 UI；没有任何产品调用方。
    valid &= requireContains(
        removalSequenceHeader,
        QStringLiteral("class ExtensionRemovalSequenceBuilder"),
        "the removal plan contract has no explicit boundary");
    valid &= requireContains(
        removalSequenceHeader,
        QStringLiteral("struct ExtensionRemovalSequence"),
        "the removal plan contract has no explicit plan shape");
    // 步骤是五个独立字段且声明顺序固定：计划没有可重排的步骤列表。
    valid &= requireOrdered(
        removalSequenceHeader,
        {QStringLiteral("ExtensionRemovalBackupStep backup;"),
         QStringLiteral("ExtensionRemovalGrantWithdrawalStep grantWithdrawal;"),
         QStringLiteral("ExtensionRemovalReviewWithdrawalStep reviewWithdrawal;"),
         QStringLiteral("ExtensionRemovalContentStep contentRemoval;"),
         QStringLiteral("ExtensionRemovalRetention retention;")},
        "the removal plan steps became a reorderable list");
    // 授权收回的顺序常量恒小于复核收回：类型无法表达先收回复核。
    valid &= requireOrdered(
        removalSequenceHeader,
        {QStringLiteral("struct ExtensionRemovalGrantWithdrawalStep"),
         QStringLiteral("int order = 1;"),
         QStringLiteral("struct ExtensionRemovalReviewWithdrawalStep"),
         QStringLiteral("int order = 2;")},
        "the grant-before-review ordering is no longer structural");
    // 元数据保留声明是计划数据：记录身份、最后内容身份与延后记入的备份 id。
    valid &= requireContains(
        removalSequenceHeader,
        QStringLiteral("struct ExtensionRemovalRetention"),
        "the removal plan lost its metadata retention statement");
    for (const QString &token : {QStringLiteral("mustRetain"),
                                 QStringLiteral("backupIdDeferred"),
                                 QStringLiteral("contentIdentityKnown")}) {
        valid &= requireContains(
            removalSequenceHeader, token,
            "the removal plan retention statement lost a mandate");
    }
    // 检查顺序：两份权威集合可读先于一切；种类边界先于清单解析；Managed 强制
    // 先于内容前提；备份绑定（含漂移拒绝）先于两步权威收回；授权收回的构建
    // 先于复核收回；身份分帧里授权块先于复核块。
    valid &= requireOrdered(
        removalSequence,
        {QStringLiteral("code(\"grant-ledger-unusable\")"),
         QStringLiteral("code(\"review-ledger-unusable\")"),
         QStringLiteral("code(\"codex-plugin-observation-only\")"),
         QStringLiteral("code(\"mcp-document-edit-unsupported\")"),
         QStringLiteral("code(\"target-ambiguous\")"),
         QStringLiteral("code(\"managed-mandated\")"),
         QStringLiteral("code(\"content-drift\")"),
         QStringLiteral("result.backup.possible = true;"),
         QStringLiteral("ExtensionEnablementWorkflow::plan("),
         QStringLiteral("ExtensionReviewWorkflow::plan("),
         QStringLiteral("QByteArrayLiteral(\"grant-withdrawal\")"),
         QStringLiteral("QByteArrayLiteral(\"review-withdrawal\")")},
        "the removal plan check or step ordering drifted");
    // 委派而不是复制：撤销集合只有两个工作流各一份，计划层绝不重新实现。
    valid &= requireContains(
        removalSequence,
        QStringLiteral("ExtensionEnablementWorkflow::plan("),
        "the removal plan re-implements the grant revocation semantics");
    valid &= requireContains(
        removalSequence,
        QStringLiteral("ExtensionReviewWorkflow::plan("),
        "the removal plan re-implements the review revocation semantics");
    // 诊断前缀与全部代号。
    valid &= requireContains(
        removalSequence,
        QStringLiteral("extension-removal-plan"),
        "the removal plan diagnostic prefix drifted");
    for (const QString &diagnostic : {
             QStringLiteral("code(\"grant-ledger-unusable\")"),
             QStringLiteral("code(\"review-ledger-unusable\")"),
             QStringLiteral("code(\"target-id-invalid\")"),
             QStringLiteral("code(\"codex-plugin-observation-only\")"),
             QStringLiteral("code(\"mcp-document-edit-unsupported\")"),
             QStringLiteral("code(\"kind-unmapped\")"),
             QStringLiteral("code(\"target-ambiguous\")"),
             QStringLiteral("code(\"target-record-invalid\")"),
             QStringLiteral("code(\"target-not-installed\")"),
             QStringLiteral("code(\"target-absent\")"),
             QStringLiteral("code(\"managed-mandated\")"),
             QStringLiteral("code(\"authority-path-invalid\")"),
             QStringLiteral("code(\"capture-domain-invalid\")"),
             QStringLiteral("code(\"fresh-capture-unavailable\")"),
             QStringLiteral("code(\"content-step-unbounded\")"),
             QStringLiteral("code(\"capture-path-unsafe\")"),
             QStringLiteral("code(\"identity-unavailable\")"),
             QStringLiteral("code(\"content-drift\")"),
             QStringLiteral("code(\"grant-withdrawal-unplannable\")"),
             QStringLiteral("code(\"review-withdrawal-unplannable\")"),
             QStringLiteral("code(\"backup-impossible-content-gone\")")}) {
        valid &= requireContains(
            removalSequence, diagnostic,
            "a removal plan diagnostic is missing");
    }
    // 纯数据边界：无文件系统写、无账本/存储接触、无捕获或快照组件调用、无
    // 执行 token。任何一项出现都意味着这一层长出了未被审查的写路径。
    for (const QString &source : {removalSequence, removalSequenceHeader}) {
        for (const QString &token : {
                 QStringLiteral("QSaveFile"), QStringLiteral("QTemporaryDir"),
                 QStringLiteral("QTemporaryFile"),
                 QStringLiteral("QIODevice::WriteOnly"),
                 QStringLiteral("QIODevice::Append"),
                 QStringLiteral("QProcess"),
                 QStringLiteral("QNetworkAccessManager"),
                 QStringLiteral("removeRecursively"),
                 QStringLiteral("mkpath"), QStringLiteral("mkdir("),
                 QStringLiteral("QSettings"), QStringLiteral("->write("),
                 QStringLiteral(".write("), QStringLiteral("rename("),
                 QStringLiteral("system("), QStringLiteral("->replace("),
                 QStringLiteral("->load("), QStringLiteral("QDir::"),
                 QStringLiteral("ExtensionReviewLedgerStore *"),
                 QStringLiteral("ExtensionEnablementLedgerStore *"),
                 QStringLiteral("ExtensionStagingBackupCapture"),
                 QStringLiteral("ExtensionStagingSnapshot"),
                 QStringLiteral("ConfigurationBackupStore"),
                 QStringLiteral("ExtensionLifecycleController"),
                 QStringLiteral("remaining.append"),
                 QStringLiteral("effectiveEnabled =")}) {
            valid &= requireAbsent(
                source, token,
                "the removal plan holds authority beyond planning");
        }
    }
    // 没有产品调用方：这一层出现在任何产品源里，都意味着移除执行器存在之前
    // 移除计划就被接通了。
    for (const QString &source : {toolSource, mainWindow, mainWindowHeader,
                                  extensionCenter, workbenchWindow, mcpDialog}) {
        valid &= requireAbsent(source,
                               QStringLiteral("ExtensionRemovalSequence"),
                               "the removal plan is wired into the product "
                               "before any removal executor exists");
        valid &= requireAbsent(source,
                               QStringLiteral("extension_removal_sequence"),
                               "the removal plan is wired into the product "
                               "before any removal executor exists");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_removal_sequence"),
        "the removal plan contract is absent from CTest");

    // 暂存备份捕获工作流：唯一从活着的扩展树产出暂存备份的路径。它写入的唯一目标是
    // 应用私有的加密备份存储（与工具配置备份同一类写入）；主体先于文件系统工作、种类
    // 映射封闭、每类失败各自独立诊断、降级不得静默、写盘/恢复/执行 token 缺席，全部钉
    // 在实现上。
    valid &= requireContains(
        stagingBackupCaptureHeader,
        QStringLiteral("class ExtensionStagingBackupCapture"),
        "the staging backup capture workflow has no explicit boundary");
    valid &= requireContains(
        stagingBackupCapture,
        QStringLiteral("QStringLiteral(\"extension-staging-capture\")"),
        "the staging backup capture diagnostic prefix drifted");
    // 主体语法校验必须先于种类映射，种类映射必须先于任何来源根触碰：pin 住相对顺序
    // 而不是各自存在，顺序才是"畸形主体连金丝雀路径都不碰"的证据。
    valid &= requireOrdered(
        stagingBackupCapture,
        {QStringLiteral("code(\"subject-invalid\")"),
         QStringLiteral("captureDomainForSubject(subject, &captureDomain, error)"),
         QStringLiteral("QFileInfo sourceInfo(sourceRoot)")},
        "the staging backup capture touches the filesystem before subject "
        "validation");
    // 种类映射是封闭的：技能经技能清单同一份捕获域（字节不得复制第二份），mcp 经
    // MCP 清单的备份捕获域整文件捕获（字节同样不得复制第二份），codex-plugin 以原
    // 代号拒绝，语法之外的种类失败关闭而不是落到默认域。
    valid &= requireContains(
        stagingBackupCapture,
        QStringLiteral("SkillExtensionInventory::treeCaptureDomain()"),
        "the staging backup capture carries a second copy of the skill capture "
        "domain");
    valid &= requireContains(
        stagingBackupCapture,
        QStringLiteral("McpConfigurationInventory::backupCaptureDomain()"),
        "the staging backup capture carries a second copy of the mcp backup "
        "capture domain");
    for (const QString &diagnostic : {
             QStringLiteral("code(\"subject-invalid\")"),
             QStringLiteral("code(\"codex-plugin-without-tree-source\")"),
             QStringLiteral("code(\"kind-unmapped\")"),
             QStringLiteral("code(\"root-symlink\")"),
             QStringLiteral("code(\"root-unavailable\")"),
             QStringLiteral("code(\"mcp-source-symlink\")"),
             QStringLiteral("code(\"mcp-source-missing\")"),
             QStringLiteral("code(\"mcp-source-invalid\")"),
             QStringLiteral("code(\"mcp-source-oversized\")"),
             QStringLiteral("code(\"mcp-source-unavailable\")"),
             QStringLiteral("code(\"mcp-source-drift\")"),
             QStringLiteral("code(\"prior-identity-degraded\")"),
             QStringLiteral("code(\"manifest-identity-degraded\")")}) {
        valid &= requireContains(
            stagingBackupCapture, diagnostic,
            "a staging backup capture diagnostic is missing");
    }
    // MCP 备份的诚实性钉：合成路径是固定字面量 settings.json（绝不从调用方文件名
    // 推导），读取后有漂移复查（被哈希的字节必须就是被存下的字节），上限取清单的
    // 1 MiB（比捕获层 2 MiB 与暂存域 4 MiB 都紧，更紧的一侧获胜），共享文件语义
    // 在结果上显式可见。
    valid &= requireContains(
        stagingBackupCapture,
        QStringLiteral("entry.relativePath = QStringLiteral(\"settings.json\")"),
        "the mcp backup synthetic path is derived from the caller's filename");
    valid &= requireContains(
        stagingBackupCapture,
        QStringLiteral("const QFileInfo finalInfo(path)"),
        "the mcp backup lost its post-read drift recheck");
    valid &= requireContains(
        stagingBackupCapture,
        QStringLiteral("McpConfigurationInventory::MaxFileBytes"),
        "the mcp backup does not enforce the tighter 1 MiB source bound");
    valid &= requireContains(
        stagingBackupCapture,
        QStringLiteral("built.coversSharedSettingsFile"),
        "the mcp backup hides the shared settings file semantics");
    // MCP 备份身份域是一个新身份：与清单的来源身份域逐字节不同，且两个字面量都钉在
    // 清单实现上。
    valid &= requireContains(
        mcpInventory,
        QStringLiteral("aegisy-mcp-config-backup-content/0.1"),
        "the mcp backup capture identity domain literal drifted");
    valid &= requireContains(
        mcpInventory,
        QStringLiteral("aegisy-mcp-config-source/0.1"),
        "the mcp config source identity domain literal drifted");
    // 再捕获身份比对只消费验证器重建的树，绝不自行解析清单。
    valid &= requireContains(
        stagingBackupCapture,
        QStringLiteral("ExtensionStagingSnapshot::verify(captureDomain, subject, prior,"),
        "the prior-identity comparison parses unverified manifest bytes");
    // 快照构建与存储写入必须经由既有契约层与暂存域，而不是本地重造。
    valid &= requireContains(
        stagingBackupCapture,
        QStringLiteral("ExtensionStagingSnapshot::build(captureDomain, tree, subject,"),
        "the staging backup capture bypasses the snapshot contract");
    valid &= requireContains(
        stagingBackupCapture,
        QStringLiteral("ConfigurationBackupStore::extensionStagingDomain()"),
        "the staging backup capture does not write into the staging domain");
    // 这一层自己不写盘、不恢复、不安装、不启用、不执行、不裁剪：写盘只经由存储的
    // create，任何写/执行 token 都意味着它长出了未被审查的路径。
    for (const QString &token : {
             QStringLiteral("QTemporaryDir"),
             QStringLiteral("QTemporaryFile"),
             QStringLiteral("QSaveFile"),
             QStringLiteral("mkpath"),
             QStringLiteral("mkdir"),
             QStringLiteral("QIODevice::WriteOnly"),
             QStringLiteral("QIODevice::Append"),
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("removeRecursively"),
             QStringLiteral("removeVerified"),
             QStringLiteral("migrateLegacy"),
             QStringLiteral("ExtensionStagingRestorePlanBuilder"),
             QStringLiteral("->write("),
             QStringLiteral(".write("),
             QStringLiteral("rename(")}) {
        valid &= requireAbsent(
            stagingBackupCapture, token,
            "the staging backup capture can write, restore, or prune outside the "
            "store before the gates exist");
    }
    // 产品接线被刻意收窄到两个调用方：McpConfigDialog 的保存前备份与 SkillsDialog 的
    // 删除前备份（都由 MainWindow 注入密钥来源与备份根）。这一层出现在任何其他产品源
    // 里，都意味着扩展备份捕获在权限、审批、沙箱与恢复门禁之前被接通了。
    for (const QString &source : {toolSource, mainWindowHeader,
                                  extensionCenter, workbenchWindow}) {
        valid &= requireAbsent(source,
                               QStringLiteral("ExtensionStagingBackupCapture"),
                               "the staging backup capture is wired into the "
                               "product beyond the MCP save and skill removal "
                               "guards");
        valid &= requireAbsent(source,
                               QStringLiteral("extension_staging_backup_capture"),
                               "the staging backup capture is wired into the "
                               "product beyond the MCP save and skill removal "
                               "guards");
    }
    // MCP 保存前备份接线的形状钉：主体是稳定的单一个（对话框编辑整个共享文件，备份
    // 单元也是整个文件——按单个服务器命名会是 dishonest 的暗示）；顺序是安全性质——
    // 身份复查在捕获之前，捕获在写入之前，捕获后写入前还有一次身份复查；备份失败
    // 即拒绝保存（fail-closed，与激活先例一致），且失败原因如实透出。
    valid &= requireContains(
        mcpDialog,
        QStringLiteral("QStringLiteral(\"mcp:claude-settings\")"),
        "the MCP save backup does not use the stable whole-file subject");
    valid &= requireOrdered(
        mcpDialog,
        {QStringLiteral("current.sourceIdentity != m_sourceIdentity"),
         QStringLiteral("ExtensionStagingBackupCapture::capture("),
         QStringLiteral("stillCurrent.sourceIdentity != m_sourceIdentity"),
         QStringLiteral("writeSettingsFile(root)")},
        "the MCP save backup broke the recheck-capture-recheck-write ordering");
    valid &= requireOrdered(
        mcpDialog,
        {QStringLiteral("ExtensionStagingBackupCapture::capture("),
         QStringLiteral("保存前备份失败，未确认保存"),
         QStringLiteral("return false"),
         QStringLiteral("writeSettingsFile(root)")},
        "a failed MCP save backup no longer blocks the write");
    // 空来源（文件不存在）诚实跳过捕获，而不是假装备份了一份"空"。
    valid &= requireContains(
        mcpDialog,
        QStringLiteral("current.state == McpConfigurationInventoryState::Ready"),
        "the MCP save backup fabricates a backup for an absent settings file");
    // 只接捕获与其共享修剪入口：对话框不得长出恢复、逐条删除、裁剪逻辑副本或第二份
    // 备份根（修剪只经 ExtensionStagingBackupRetention 唯一入口，钉在下方）。
    for (const QString &token : {
             QStringLiteral("removeVerified"),
             QStringLiteral("applyRetention"),
             QStringLiteral("pruneBackups"),
             QStringLiteral("ExtensionStagingRestorePlan"),
             QStringLiteral("ExtensionStagingBackupInventory"),
             QStringLiteral("restoreBackup"),
             QStringLiteral("extensions-staging"),
             QStringLiteral("AppDataLocation")}) {
        valid &= requireAbsent(mcpDialog, token,
                               "the MCP dialog grew restore, a local pruning "
                               "copy, or a second backup root beyond the "
                               "pre-save capture and its shared retention "
                               "entry point");
    }
    // MainWindow 是唯一接线点：密钥来源与备份根都取自唯一产品定义点。
    valid &= requireOrdered(
        mainWindow,
        {QStringLiteral("SecureStorageExtensionStagingBackupKeyProvider"),
         QStringLiteral("new McpConfigDialog(&stagingBackupKeyProvider"),
         QStringLiteral("extensionStagingBackupRootPath()")},
        "the MCP dialog is constructed without the staging backup wiring");
    // 暂存备份密钥来源与备份根：唯一产品定义点，作用域白名单钉在暂存域的密钥作用域
    // 前缀与主体语法上，诊断代号与工具域先例逐字同形。
    valid &= requireContains(
        stagingBackupKeyProviderHeader,
        QStringLiteral("class SecureStorageExtensionStagingBackupKeyProvider"),
        "the staging backup key provider has no explicit boundary");
    valid &= requireContains(
        stagingBackupKeyProvider,
        QStringLiteral("^aegisy/extension-staging-backup-master/v1/"),
        "the staging backup key provider accepts out-of-domain key scopes");
    for (const QString &diagnostic : {
             QStringLiteral("extension-staging-backup-key-unavailable"),
             QStringLiteral("extension-staging-backup-key-invalid"),
             QStringLiteral("extension-staging-backup-random-failed"),
             QStringLiteral("extension-staging-backup-key-write-failed")}) {
        valid &= requireContains(stagingBackupKeyProvider, diagnostic,
                                 "a staging backup key provider diagnostic is "
                                 "missing");
    }
    valid &= requireContains(
        stagingBackupKeyProvider,
        QStringLiteral("QStringLiteral(\"extensions-staging\")"),
        "the staging backup root lost its single definition point");
    valid &= requireContains(
        stagingBackupKeyProvider,
        QStringLiteral("QStringLiteral(\"/backups\")"),
        "the staging backup root left the tool backup parent convention");
    for (const QString &token : {
             QStringLiteral("QProcess"),
             QStringLiteral("QNetworkAccessManager"),
             QStringLiteral("QSaveFile"),
             QStringLiteral("removeRecursively")}) {
        valid &= requireAbsent(stagingBackupKeyProvider, token,
                               "the staging backup key provider grew network, "
                               "process, or file-mutation reach");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_staging_backup_capture"),
        "the staging backup capture workflow is absent from CTest");
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_staging_backup_capture_mcp"),
        "the mcp backup capture guards are absent from CTest");
    valid &= requireContains(
        cmake,
        QStringLiteral("mcp_config_save_backup"),
        "the MCP save backup wiring guards are absent from CTest");

    // 暂存备份浏览与恢复入口（扩展中心）。形状钉：浏览区渲染清单；恢复入口按资格缺席
    // 渲染——合格行（清单验证通过 + mcp:claude-settings + 目标可解析）有且仅有一个恢复
    // 按钮，其余行连按钮都没有；删除/裁剪/立即捕获入口仍不存在，连灰掉的都不行
    // （grant-button 先例）。诚实状态钉：损坏条目可见并标注；Invalid/Unavailable 冻结成
    // 明确的非空消息，绝不落成空清单；真空是与退化完全不同的另一句话。异步纪律钉：独立
    // 线程槽位 + 单调代号 + 析构 join，与复核工作流同一套。
    valid &= requireContains(
        extensionCenterHeader,
        QStringLiteral("void setBackupListing("),
        "the extension center has no backup browsing entry point");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("QStringLiteral(\"extensionBackupTable\")"),
        "the backup browsing table is absent");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("ExtensionStagingBackupListState::Invalid"),
        "a degraded backup store no longer freezes the view");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("ExtensionStagingBackupListState::Unavailable"),
        "an unavailable backup store no longer freezes the view");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("ExtensionStagingBackupListState::Empty"),
        "the genuinely-empty backup state lost its distinct rendering");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("ExtensionStagingBackupEntryVerification::ListedCorrupt"),
        "corrupt backups are no longer rendered and labeled");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("QStringLiteral(\"损坏\")"),
        "a corrupt backup lost its visible label");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("浏览已冻结"),
        "a degraded backup store is no longer an explicit frozen state");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("这不是空清单"),
        "a degraded backup store can now read as an empty listing");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("确认一份备份都没有"),
        "the genuinely-empty backup state lost its distinct wording");
    // 作用域诚实句：恢复入口对哪一类行提供、其余行为什么没有，必须明写在界面上。
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("恢复入口只对通过验证且目标可解析的 mcp:claude-settings "
                       "备份提供"),
        "the backup surface no longer states the restore scope honestly");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("先捕获当前状态作为新备份"),
        "the backup surface no longer states the pre-restore capture");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("整个共享设置文件"),
        "an mcp: backup no longer states the whole-shared-file semantics");
    // 恢复入口的唯一合法形状：资格谓词委派（对话框绝不本地重算资格）、按钮按行缺席
    // 渲染、restoreRequested 信号只带 (backupId, subject)。
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("ExtensionStagingRestoreFlow::isRestoreOffered(entry)"),
        "the restore button eligibility no longer delegates to the flow "
        "predicate");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("extensionBackupRestoreButton"),
        "the restore button lost its fixed object name");
    valid &= requireContains(
        extensionCenterHeader,
        QStringLiteral("void restoreRequested(const QString &backupId, "
                       "const QString &subject)"),
        "the restore request signal drifted");
    // 高风险确认分类委派给审批策略（pin 在审批策略一节）；对话框自身绝不包含计划、
    // 控制器、删除或捕获 token。
    for (const QString &token : {
             QStringLiteral("ExtensionStagingRestorePlan"),
             QStringLiteral("ExtensionStagingRestoreController"),
             QStringLiteral("removeVerified"),
             QStringLiteral("applyRetention"),
             QStringLiteral("planRetention"),
             QStringLiteral("ExtensionStagingBackupCapture"),
             QStringLiteral("backupRestoreRequested"),
             QStringLiteral("restoreBackup"),
             QStringLiteral("deleteBackup")}) {
        valid &= requireAbsent(
            extensionCenter, token,
            "the backup browsing surface grew an unaudited restore/delete/"
            "capture affordance");
        valid &= requireAbsent(
            extensionCenterHeader, token,
            "the backup browsing surface grew an unaudited restore/delete/"
            "capture affordance");
    }
    // MainWindow 接线钉：只读清点走唯一产品定义点的备份根，独立槽位 + 代号绑定 +
    // 析构 join；恢复接线只碰编排器——控制器/执行器 token 绝不进入本文件。
    valid &= requireOrdered(
        mainWindow,
        {QStringLiteral("void MainWindow::startExtensionBackupListing("),
         QStringLiteral("ExtensionStagingBackupInventory::list("),
         QStringLiteral("extensionStagingBackupRootPath()"),
         QStringLiteral("m_extensionBackupGeneration != operation"),
         QStringLiteral("m_extensionBackupThread = worker")},
        "the backup browsing worker lost its tracked-slot/generation discipline");
    valid &= requireOrdered(
        mainWindow,
        {QStringLiteral("MainWindow::~MainWindow()"),
         QStringLiteral("++m_extensionBackupGeneration"),
         QStringLiteral("m_extensionBackupThread->wait()")},
        "the backup browsing worker is no longer joined on destruction");
    // 恢复接线钉：准备 → UI 线程模态批准 → 延迟一拍提交（finished 先清槽位）→
    // 结果报告 → 清单刷新；恢复线程同样先作废代号再 join。
    valid &= requireOrdered(
        mainWindow,
        {QStringLiteral("void MainWindow::startExtensionRestorePreparation("),
         QStringLiteral("ExtensionStagingRestoreFlow::prepare("),
         QStringLiteral("target->askRestoreDecision(preparation, "
                        "&acknowledgement);"),
         QStringLiteral("QTimer::singleShot(0, window,"),
         QStringLiteral("window->startExtensionRestoreCommit(target, "
                        "preparation,")},
        "the restore preparation lost its ordered hand-off to the approval "
        "dialog and commit");
    valid &= requireOrdered(
        mainWindow,
        {QStringLiteral("void MainWindow::startExtensionRestoreCommit("),
         QStringLiteral("ExtensionStagingRestoreFlow::commit("),
         QStringLiteral("target->showRestoreResult(outcome, preparation);"),
         QStringLiteral("window->startExtensionBackupListing(target);")},
        "the restore commit lost its report-and-refresh order");
    valid &= requireOrdered(
        mainWindow,
        {QStringLiteral("MainWindow::~MainWindow()"),
         QStringLiteral("++m_extensionRestoreGeneration"),
         QStringLiteral("m_extensionRestoreThread->wait()")},
        "the restore worker is no longer joined on destruction");
    // 目标可解析门在 UI 线程：设置路径非空且父目录存在，否则连准备都不发起。
    valid &= requireContains(
        mainWindow,
        QStringLiteral("m_toolManager->configurationFiles(AiTool::ClaudeCode)"),
        "the restore destination gate lost its ToolManager authority");
    for (const QString &token : {
             QStringLiteral("ExtensionStagingBackupInventory::removeVerified"),
             QStringLiteral("ExtensionStagingBackupInventory::applyRetention"),
             QStringLiteral("ExtensionStagingBackupInventory::planRetention"),
             QStringLiteral("ExtensionStagingRestoreController"),
             QStringLiteral("ExtensionStagingRestoreExecutor"),
             QStringLiteral("ExtensionStagingRestorePlanBuilder"),
             QStringLiteral("ExtensionStagingRestorePresentation::"),
             QStringLiteral("ExtensionStagingBackupCapture::")}) {
        valid &= requireAbsent(
            mainWindow, token,
            "MainWindow bypasses the restore flow orchestrator");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_center_read_only"),
        "the extension center read-only guards are absent from CTest");

    // 暂存恢复编排器：用户发起恢复的唯一产品侧编排者。顺序纪律是安全性质——捕获先于
    // 清点与计划，记录先于凭据复核与执行；全部判定留在各自已有的层里，本组件只做
    // 顺序与诚实报告。
    valid &= requireContains(
        restoreFlowHeader,
        QStringLiteral("class ExtensionStagingRestoreFlow"),
        "the restore flow orchestrator has no explicit boundary");
    // 资格谓词是"哪一行配得恢复入口"的唯一定义点：清单身份级验证通过 + 封闭主体。
    valid &= requireContains(
        restoreFlowHeader,
        QStringLiteral("== ExtensionStagingBackupEntryVerification::ListedIntact"),
        "the restore eligibility predicate no longer requires an intact "
        "listing");
    valid &= requireContains(
        restoreFlowHeader,
        QStringLiteral("QStringLiteral(\"mcp:claude-settings\")"),
        "the restore eligibility predicate lost its closed subject");
    valid &= requireContains(
        restoreFlow,
        QStringLiteral("QStringLiteral(\"extension-restore-flow\")"),
        "the restore flow diagnostic prefix drifted");
    for (const QString &diagnostic : {
             QStringLiteral("code(\"request-invalid\")"),
             QStringLiteral("code(\"subject-unsupported\")"),
             QStringLiteral("code(\"listing-failed\")"),
             QStringLiteral("code(\"listing-degraded\")"),
             QStringLiteral("code(\"backup-vanished\")"),
             QStringLiteral("code(\"backup-not-intact\")"),
             QStringLiteral("code(\"destination-unresolvable\")"),
             QStringLiteral("code(\"not-prepared\")"),
             QStringLiteral("code(\"credential-not-authorized\")")}) {
        valid &= requireContains(
            restoreFlow, diagnostic,
            "a restore flow gate diagnostic is missing");
    }
    // 准备顺序：恢复前捕获 → 重新清点 → 读回 → 计划 → 呈现；捕获失败 return 必须先于
    // 清点（fail-closed：没有回退路径的恢复不会发生）。
    valid &= requireOrdered(
        restoreFlow,
        {QStringLiteral("ExtensionStagingBackupCapture::capture("),
         QStringLiteral("return fail(QStringLiteral(\"capture\")"),
         QStringLiteral("ExtensionStagingBackupInventory::list("),
         QStringLiteral("store.read("),
         QStringLiteral("ExtensionStagingRestorePlanBuilder::plan("),
         QStringLiteral("ExtensionStagingRestorePresentation::build(")},
        "the restore preparation order drifted");
    // 接线后呈现必须如实不再携带"仅供复核、不会执行"披露。
    valid &= requireContains(
        restoreFlow,
        QStringLiteral("/*executionAvailable=*/true"),
        "the wired prompt still claims no execution path exists");
    // 提交顺序：记录（declined 同样记录）→ 凭据 Authorized 复核 → 执行。任何一步
    // 提前都意味着执行可以绕过审计。
    valid &= requireOrdered(
        restoreFlow,
        {QStringLiteral("ExtensionStagingRestoreController::record("),
         QStringLiteral("ExtensionStagingRestoreApprovalState::Authorized"),
         QStringLiteral("ExtensionStagingRestoreExecutor::execute(")},
        "the restore commit order drifted");
    // 执行结果入链：执行之后记录结果，审计失败单独成字段报告——绝不让结果记录的
    // 失败改写执行真相。
    valid &= requireOrdered(
        restoreFlow,
        {QStringLiteral("ExtensionStagingRestoreExecutor::execute("),
         QStringLiteral("ExtensionStagingRestoreController::recordOutcome(")},
        "the restore flow does not record the execution outcome after "
        "execution");
    valid &= requireContains(
        restoreFlowHeader,
        QStringLiteral("bool outcomeRecorded = false;"),
        "the restore outcome no longer reports whether the outcome reached "
        "the ledger");
    valid &= requireContains(
        restoreFlowHeader,
        QStringLiteral("QString outcomeAuditErrorCode;"),
        "the restore outcome lost its distinct audit-failure channel");
    // 结果记录器：绑定纪律（提示身份与执行回显逐字节相等 + 已记录的 approved 决定
    // 必须在场）与独立诊断。
    valid &= requireContains(
        restoreControllerHeader,
        QStringLiteral("recordOutcome("),
        "the restore controller has no outcome recording entry point");
    for (const QString &diagnostic : {
             QStringLiteral("code(\"outcome-plan-mismatch\")"),
             QStringLiteral("code(\"outcome-without-decision\")")}) {
        valid &= requireContains(
            restoreController, diagnostic,
            "a restore outcome binding diagnostic is missing");
    }
    valid &= requireOrdered(
        restoreController,
        {QStringLiteral("approvedDecisionFound"),
         QStringLiteral("store->replace(current.entries, current.generation"),
         QStringLiteral("refreshed.state !=")},
        "the outcome recording lost its bind-then-CAS-then-reread order");
    // 编解码字节兼容钉：结果分节是顶层可选数组，空集整个省略——只含决定的载荷与
    // 旧格式逐字节一致；解析同时接受旧四键形状。
    valid &= requireContains(
        restoreAuditLedger,
        QStringLiteral("const QString kOutcomesKey"),
        "the restore audit ledger lost its outcome section key");
    valid &= requireContains(
        restoreAuditLedger,
        QStringLiteral("legacyExpected"),
        "the restore audit ledger no longer accepts the legacy "
        "decision-only payload shape");
    valid &= requireContains(
        restoreAuditLedger,
        QStringLiteral("if (!outcomes.isEmpty())"),
        "the outcome section is not conditionally omitted from the byte "
        "shape and MAC preimage");
    valid &= requireContains(
        restoreAuditLedgerHeader,
        QStringLiteral("MaxOutcomeEntries"),
        "the restore audit ledger lost its outcome entry cap");
    valid &= requireContains(
        restoreAuditStore,
        QStringLiteral("code(\"outcomes-cap\")"),
        "the restore audit store does not refuse an over-cap outcome set "
        "distinctly");
    // 结果文案钉：执行结果如实报告，审计失败单独成句附上。
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("outcome.outcomeAuditErrorCode"),
        "the restore result surface no longer reports an audit failure "
        "distinctly from the execution result");
    // MainWindow 注入结果落账时刻（提交链不自带时钟）。
    valid &= requireContains(
        mainWindow,
        QStringLiteral("QDateTime::currentDateTimeUtc(), &store"),
        "the restore commit no longer injects the outcome recording time");

    // 恢复审计轨迹只读视图（扩展中心）：条目按构造即已认证，退化冻结成明确的非空
    // 消息而绝不成空轨迹，"没有记录"只在 Empty 与已认证空两种可区分状态说出；批准
    // 无结果如实标注，Partial 必须渲染混合状态与回退备份 id；渲染有界并带显式截断
    // 标记；它是轨迹而不是控制台——没有任何动作入口。
    valid &= requireContains(
        extensionCenterHeader,
        QStringLiteral("void setRestoreAuditTrail("),
        "the extension center lost the read-only audit trail entry point");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("QStringLiteral(\"extensionRestoreAuditTable\")"),
        "the restore audit trail table is absent");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("查看已冻结"),
        "a degraded restore audit ledger no longer freezes the trail view");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("这不是没有记录"),
        "a degraded restore audit ledger can now read as an empty trail");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("从未建立"),
        "the never-created restore audit ledger lost its distinct wording");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("确认尚无任何决定记录"),
        "the certified-empty restore audit ledger lost its distinct wording");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("批准已记录，尚无执行记录"),
        "an approved-without-outcome decision can now imply execution");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("仅显示最近"),
        "the audit trail lost its explicit truncation marker");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("ExtensionStagingRestoreAuditStoreState::Invalid"),
        "an invalid restore audit ledger no longer freezes the trail view");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("ExtensionStagingRestoreAuditStoreState::Unavailable"),
        "an unavailable restore audit ledger no longer freezes the trail view");
    // 零动作钉：轨迹区没有按钮、没有单元格控件、没有清理/导出/刷新入口，对话框也
    // 绝不构造或写回账本存储（读由 MainWindow 的 worker 完成）。
    for (const QString &token : {
             QStringLiteral("extensionRestoreAuditButton"),
             QStringLiteral("m_restoreAuditTable->setCellWidget"),
             QStringLiteral("clearAudit"),
             QStringLiteral("pruneAudit"),
             QStringLiteral("exportAudit"),
             QStringLiteral("ExtensionStagingRestoreAuditLedgerStore store("),
             QStringLiteral("recordSettingsKey")}) {
        valid &= requireAbsent(
            extensionCenter, token,
            "the audit trail view grew an action or write affordance");
        valid &= requireAbsent(
            extensionCenterHeader, token,
            "the audit trail view grew an action or write affordance");
    }
    // MainWindow 读取接线钉：只读 worker 走与提交 worker 相同的两半构造，独立槽位 +
    // 代号绑定 + 析构 join；结果原样交给对话框，不做任何"没有记录"化。
    valid &= requireOrdered(
        mainWindow,
        {QStringLiteral("void MainWindow::startExtensionRestoreAuditListing("),
         QStringLiteral(
             "SecureStorageExtensionRestoreAuditLedgerAdapter authority;"),
         QStringLiteral(
             "ExtensionStagingRestoreAuditLedgerStore store(&authority, &settings);"),
         QStringLiteral("store.load()"),
         QStringLiteral("m_extensionRestoreAuditGeneration != operation"),
         QStringLiteral("target->setRestoreAuditTrail(result);"),
         QStringLiteral("m_extensionRestoreAuditThread = worker")},
        "the audit trail reader lost its tracked-slot/generation discipline");
    valid &= requireOrdered(
        mainWindow,
        {QStringLiteral("MainWindow::~MainWindow()"),
         QStringLiteral("++m_extensionRestoreAuditGeneration"),
         QStringLiteral("m_extensionRestoreAuditThread->wait()")},
        "the audit trail reader is no longer joined on destruction");
    // 恢复提交完成后轨迹视图与备份清单一同如实刷新。
    valid &= requireOrdered(
        mainWindow,
        {QStringLiteral("target->showRestoreResult(outcome, preparation);"),
         QStringLiteral("window->startExtensionRestoreAuditListing(target);"),
         QStringLiteral("window->startExtensionBackupListing(target);")},
        "the restore commit no longer refreshes the audit trail view");

    // 编排器不接触 UI、不启动子进程、不碰网络：它由 MainWindow 的 tracked worker
    // 线程调用。
    for (const QString &token : {
             QStringLiteral("QWidget"),
             QStringLiteral("QDialog"),
             QStringLiteral("QMessageBox"),
             QStringLiteral("QProcess"),
             QStringLiteral("QNetwork"),
             QStringLiteral("QTcpSocket"),
             QStringLiteral("QSettings")}) {
        valid &= requireAbsent(
            restoreFlow, token,
            "the restore flow orchestrator holds UI or process authority");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_staging_restore_flow"),
        "the staging restore flow orchestrator is absent from CTest");

    // 暂存备份清点与验证删除：唯一回答"这里有哪些备份"并裁剪它们的管理层。清点诚实性
    // （损坏可见、退化绝不成空清单、清单身份级验证固定不解密载荷）、删除只走存储的验证
    // 路径、保留期计划是纯数据、清单路径不碰密钥，全部钉在实现上。
    valid &= requireContains(
        stagingBackupInventoryHeader,
        QStringLiteral("class ExtensionStagingBackupInventory"),
        "the staging backup inventory layer has no explicit boundary");
    valid &= requireContains(
        stagingBackupInventory,
        QStringLiteral("QStringLiteral(\"extension-staging-inventory\")"),
        "the staging backup inventory diagnostic prefix drifted");
    for (const QString &diagnostic : {
             QStringLiteral("code(\"subject-invalid\")"),
             QStringLiteral("code(\"request-invalid\")"),
             QStringLiteral("code(\"root-invalid\")"),
             QStringLiteral("code(\"root-unavailable\")"),
             QStringLiteral("code(\"busy\")"),
             QStringLiteral("code(\"store-shape-invalid\")"),
             QStringLiteral("code(\"entry-directory-invalid\")"),
             QStringLiteral("code(\"entry-manifest-unreadable\")"),
             QStringLiteral("code(\"entry-manifest-invalid\")"),
             QStringLiteral("code(\"backup-id-invalid\")"),
             QStringLiteral("code(\"backup-absent\")"),
             QStringLiteral("code(\"backup-corrupt\")"),
             QStringLiteral("code(\"plan-inconsistent\")")}) {
        valid &= requireContains(
            stagingBackupInventory, diagnostic,
            "a staging backup inventory diagnostic is missing");
    }
    // 主体语法校验必须先于任何存储触碰：pin 住相对顺序而不是各自存在。
    valid &= requireOrdered(
        stagingBackupInventory,
        {QStringLiteral("code(\"subject-invalid\")"),
         QStringLiteral("QLockFile lock(")},
        "the staging backup inventory touches the store before subject "
        "validation");
    // 验证级别固定为清单身份级：清单路径不碰密钥、不解密载荷，载荷认证留给恢复路径。
    const QString stagingListPath = sourceRange(
        stagingBackupInventory,
        QStringLiteral("bool ExtensionStagingBackupInventory::list("),
        QStringLiteral("ExtensionStagingBackupRemovalResult "
                       "ExtensionStagingBackupInventory::removeVerified("));
    for (const QString &token : {
             QStringLiteral("removeVerified("),
             QStringLiteral("keyForScope"),
             QStringLiteral("EVP_Decrypt"),
             QStringLiteral("QSaveFile"),
             QStringLiteral("QIODevice::WriteOnly"),
             QStringLiteral("QIODevice::Append"),
             QStringLiteral("removeRecursively"),
             QStringLiteral("QFile::remove"),
             QStringLiteral("rename(")}) {
        valid &= requireAbsent(
            stagingListPath, token,
            "the staging backup listing path holds write or key authority");
    }
    // 损坏条目绝不静默丢弃：跳过条目的唯一理由是作用域过滤，而过滤只看声称主体。
    valid &= requireContains(
        stagingBackupInventory,
        QStringLiteral("if (!subject.isEmpty() && entry.subject != subject) "
                       "continue;"),
        "the staging backup listing can silently drop corrupt entries");
    // 与存储清点的刻意分歧钉在源码里：超限是保留期规划要修复的现实，扫描上限是
    // maxBackups 的 4 倍，而不是继承存储的超限即 Invalid。
    valid &= requireContains(
        stagingBackupInventory,
        QStringLiteral("domain.maxBackups * 4"),
        "the staging backup listing scan is unbounded");
    // 删除只有一条路径：存储的身份绑定验证删除。任何本地删除/改名 token 都意味着它长出
    // 了绕过验证的路径。
    valid &= requireContains(
        stagingBackupInventory,
        QStringLiteral("store.removeVerified(found->subject, backupId,"),
        "the staging backup removal bypasses the store's verified path");
    for (const QString &token : {
             QStringLiteral("removeRecursively"),
             QStringLiteral("QFile::remove"),
             QStringLiteral(".rmdir("),
             QStringLiteral("QSaveFile"),
             QStringLiteral("QIODevice::WriteOnly"),
             QStringLiteral("QIODevice::Append"),
             QStringLiteral("migrateLegacy"),
             QStringLiteral("rename(")}) {
        valid &= requireAbsent(
            stagingBackupInventory, token,
            "the staging backup inventory can delete outside the verified "
            "path");
    }
    // 保留期计划是纯数据：主体语法先于清点，上限取域定义而非本地副本，最近完整备份的
    // 保留是显式字段而不是隐含的名单成员资格，计划范围内没有任何删除或密钥动作。
    valid &= requireOrdered(
        stagingBackupInventory,
        {QStringLiteral("bool ExtensionStagingBackupInventory::planRetention("),
         QStringLiteral("code(\"subject-invalid\")"),
         QStringLiteral("list(backupRoot, subject, &listing, error)")},
        "the retention planner lists before subject validation");
    valid &= requireContains(
        stagingBackupInventory,
        QStringLiteral("built.maxBackups = domain.maxBackups;"),
        "the retention plan carries a local copy of the domain bound");
    valid &= requireContains(
        stagingBackupInventoryHeader,
        QStringLiteral("QString newestVerifiedKept;"),
        "the newest-verified retention decision is implicit list membership");
    const QString stagingPlanPath = sourceRange(
        stagingBackupInventory,
        QStringLiteral("bool ExtensionStagingBackupInventory::planRetention("),
        QStringLiteral("QList<ExtensionStagingRetentionApplyEntry>"));
    for (const QString &token : {
             QStringLiteral("removeVerified("),
             QStringLiteral("keyForScope"),
             QStringLiteral("QSaveFile"),
             QStringLiteral("QIODevice::WriteOnly"),
             QStringLiteral("removeRecursively"),
             QStringLiteral("QFile::remove"),
             QStringLiteral("rename(")}) {
        valid &= requireAbsent(
            stagingPlanPath, token,
            "the retention plan is not a pure data object");
    }
    // apply 只是逐条组合验证删除：每一 prune 条目一次删除、一次结果。
    valid &= requireContains(
        stagingBackupInventory,
        QStringLiteral("removeVerified(backupRoot, keyProvider, prune.backupId)"),
        "retention apply does not compose verified removal per entry");
    // 没有写入型产品调用方：这一层的删除/裁剪出现在任何产品源里，都意味着扩展备份
    // 裁剪在权限、审批、沙箱与恢复门禁之前被接通了。唯一被接通的是扩展中心的只读
    // 浏览（只调 list，删除/裁剪/规划 token 在 MainWindow 与对话框里的缺席钉在上方
    // 备份浏览区块），因此 mainWindow 与 extensionCenter 从缺席清单中剔除。
    for (const QString &source : {toolSource, mainWindowHeader,
                                  workbenchWindow}) {
        valid &= requireAbsent(source,
                               QStringLiteral("ExtensionStagingBackupInventory"),
                               "the staging backup inventory is wired into the "
                               "product before its gates exist");
        valid &= requireAbsent(source,
                               QStringLiteral("extension_staging_backup_inventory"),
                               "the staging backup inventory is wired into the "
                               "product before its gates exist");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_staging_backup_inventory"),
        "the staging backup inventory layer is absent from CTest");

    // 保留期修剪接线：捕获成功后修剪该主体的唯一共享入口。planRetention/applyRetention
    // 早已存在且已测试，本层只是把它们收敛成一个入口——两个调用点（MCP 保存、恢复编排
    // 器）只消费它，绝不复制修剪逻辑。诚实语义是硬性质：修剪是捕获成功后的后续清理，
    // 修剪的任何失败都绝不代表捕获/保存/恢复失败；计划失败零删除加诊断透传，apply 逐条
    // 如实汇总。
    valid &= requireContains(
        stagingBackupRetentionHeader,
        QStringLiteral("class ExtensionStagingBackupRetention"),
        "the post-capture retention pruning has no explicit boundary");
    valid &= requireContains(
        stagingBackupRetentionHeader,
        QStringLiteral("pruneAfterCapture("),
        "the post-capture retention pruning lost its single entry point");
    // 结果形状钉：计划失败通道、逐条汇总三通道（删除/损坏保留/失败）、无条件保留的
    // 显式回显。
    for (const QString &field : {
             QStringLiteral("bool planFailed = false;"),
             QStringLiteral("QString planError;"),
             QStringLiteral("int removedCount = 0;"),
             QStringLiteral("int corruptKeptCount = 0;"),
             QStringLiteral("QList<ExtensionStagingRetentionApplyEntry> "
                            "failures;"),
             QStringLiteral("QString newestVerifiedKept;")}) {
        valid &= requireContains(
            stagingBackupRetentionHeader, field,
            "the retention run result lost an honesty field");
    }
    // 计划先于执行；计划失败立即返回（零删除），apply 只在计划成功后可达。
    valid &= requireOrdered(
        stagingBackupRetention,
        {QStringLiteral("ExtensionStagingBackupInventory::planRetention("),
         QStringLiteral("run.planFailed = true;"),
         QStringLiteral("return run;"),
         QStringLiteral("ExtensionStagingBackupInventory::applyRetention(")},
        "the retention pruning applies over a failed plan");
    // 逐条如实汇总：损坏条目计为原地保留而不是成功，其余失败逐条携带进 failures。
    valid &= requireContains(
        stagingBackupRetention,
        QStringLiteral("++run.corruptKeptCount;"),
        "corrupt prune candidates are no longer honestly kept in place");
    valid &= requireContains(
        stagingBackupRetention,
        QStringLiteral("run.failures.append(entry);"),
        "per-entry prune failures are silently swallowed");
    // 本层不自行删除：删除只经 applyRetention 的逐条组合，无任何旁路写/删 token。
    for (const QString &token : {
             QStringLiteral("removeVerified("),
             QStringLiteral("removeRecursively"),
             QStringLiteral("QFile::remove"),
             QStringLiteral("QSaveFile"),
             QStringLiteral("QIODevice::WriteOnly")}) {
        valid &= requireAbsent(
            stagingBackupRetention, token,
            "the retention pruning can delete outside the per-entry verified "
            "path");
    }
    // MCP 保存接线：捕获 → 写入 → 修剪（捕获与保存都成功后的收尾清理）；修剪段落里
    // 不存在 return false——修剪失败绝不翻转已成功的保存；备注随保存结果上屏，措辞
    // 区分"无需修剪"与"修剪失败"。
    valid &= requireOrdered(
        mcpDialog,
        {QStringLiteral("ExtensionStagingBackupCapture::capture("),
         QStringLiteral("writeSettingsFile(root)"),
         QStringLiteral("ExtensionStagingBackupRetention::pruneAfterCapture("),
         QStringLiteral("return true;")},
        "the MCP save no longer prunes retention after a successful capture");
    const QString mcpPruneTail = sourceRange(
        mcpDialog,
        QStringLiteral("ExtensionStagingBackupRetention::pruneAfterCapture("),
        QStringLiteral("return true;"));
    valid &= requireAbsent(
        mcpPruneTail, QStringLiteral("return false"),
        "a retention prune failure can flip the successful MCP save");
    valid &= requireContains(
        mcpDialog,
        QStringLiteral("QStringLiteral(\"已保存\") + m_lastRetentionNote"),
        "the retention note no longer rides on the save result copy");
    for (const QString &wording : {
             QStringLiteral("无需修剪"),
             QStringLiteral("修剪未能执行"),
             QStringLiteral("本次保存与捕获不受影响")}) {
        valid &= requireContains(
            mcpDialog, wording,
            "the MCP save retention note lost an honest wording");
    }
    // 恢复编排器接线：捕获 → 修剪 → 呈现前重新清点；修剪段落里不存在失败返回——修剪
    // 失败绝不中止恢复准备；结果作为准备结果的独立字段携带。
    valid &= requireOrdered(
        restoreFlow,
        {QStringLiteral("ExtensionStagingBackupCapture::capture("),
         QStringLiteral("ExtensionStagingBackupRetention::pruneAfterCapture("),
         QStringLiteral("ExtensionStagingBackupInventory::list(")},
        "the restore flow no longer prunes retention between the pre-restore "
        "capture and the listing");
    const QString flowPruneTail = sourceRange(
        restoreFlow,
        QStringLiteral("ExtensionStagingBackupRetention::pruneAfterCapture("),
        QStringLiteral("ExtensionStagingBackupInventory::list("));
    valid &= requireAbsent(
        flowPruneTail, QStringLiteral("return fail("),
        "a retention prune failure can abort the restore preparation");
    valid &= requireContains(
        restoreFlowHeader,
        QStringLiteral("bool preRestoreRetentionAttempted = false;"),
        "the restore preparation lost its prune-attempted marker");
    valid &= requireContains(
        restoreFlowHeader,
        QStringLiteral("ExtensionStagingBackupRetentionRun "
                       "preRestoreRetention;"),
        "the restore preparation lost its independent prune result field");
    // 结果文案钉：修剪结果单独成句（与审计失败同例），三种现实措辞可区分。
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("preparation.preRestoreRetentionAttempted"),
        "the restore result surface no longer reports the retention prune");
    for (const QString &wording : {
             QStringLiteral("无需修剪"),
             QStringLiteral("修剪未能执行"),
             QStringLiteral("均不受影响")}) {
        valid &= requireContains(
            extensionCenter, wording,
            "the restore result surface lost an honest prune wording");
    }
    // 修剪只消费既有密钥来源：两个调用点都不发明第二份密钥或备份根。
    valid &= requireAbsent(
        mcpDialog, QStringLiteral("SecureStorageExtensionStagingBackupKeyProvider"),
        "the MCP dialog grew its own staging key source");
    valid &= requireAbsent(
        restoreFlow,
        QStringLiteral("SecureStorageExtensionStagingBackupKeyProvider"),
        "the restore flow grew its own staging key source");
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_staging_backup_retention"),
        "the retention pruning guards are absent from CTest");

    // Skill 删除前备份接线（SkillsDialog 层，对齐 MCP 保存先例）：主体是
    // `skill:<id>`，sourceRoot 是 SkillManager::skillsRoot() 下该 skill 的实际目录
    // （调用方权威目标根，与扩展清点同一来源）。守卫顺序是安全性质——内置/不存在
    // 守卫先于一切备份工作；捕获 fail-closed（失败即拒绝删除，return false 先于
    // removeSkill）；删除成功后才经共享唯一入口修剪该主体。
    valid &= requireContains(
        skillsDialog,
        QStringLiteral("QStringLiteral(\"skill:\") + skill.id"),
        "the skill removal backup does not use the per-skill subject");
    valid &= requireContains(
        skillsDialog,
        QStringLiteral("skill.id.isEmpty() || skill.builtin"),
        "the built-in/missing guards no longer precede any backup work");
    valid &= requireOrdered(
        skillsDialog,
        {QStringLiteral("ExtensionStagingBackupCapture::capture("),
         QStringLiteral("删除前备份失败，已取消删除"),
         QStringLiteral("return false"),
         QStringLiteral("m_manager->removeSkill(id, &error)")},
        "a failed skill removal backup no longer blocks the deletion");
    valid &= requireOrdered(
        skillsDialog,
        {QStringLiteral("ExtensionStagingBackupCapture::capture("),
         QStringLiteral("m_manager->removeSkill(id, &error)"),
         QStringLiteral("ExtensionStagingBackupRetention::pruneAfterCapture("),
         QStringLiteral("return true;")},
        "the skill removal no longer prunes retention after a successful "
        "capture and deletion");
    // 修剪段落里不存在 return false——修剪失败绝不翻转已成功的删除；备注随删除结果
    // 上屏，措辞区分"无需修剪"与"修剪失败"。
    const QString skillPruneTail = sourceRange(
        skillsDialog,
        QStringLiteral("ExtensionStagingBackupRetention::pruneAfterCapture("),
        QStringLiteral("return true;"));
    valid &= requireAbsent(
        skillPruneTail, QStringLiteral("return false"),
        "a retention prune failure can flip the successful skill removal");
    for (const QString &wording : {
             QStringLiteral("无需修剪"),
             QStringLiteral("修剪未能执行"),
             QStringLiteral("本次删除与捕获不受影响")}) {
        valid &= requireContains(
            skillsDialog, wording,
            "the skill removal retention note lost an honest wording");
    }
    // 删除成功后如实提示备份 id 作为回退路径，并明说恢复操作尚未提供——绝不暗示有
    // 恢复按钮存在（skill 恢复资格仍未接线，kRestorableSubject 保持
    // `mcp:claude-settings` 不变，上方的资格谓词钉守着这一点）。
    valid &= requireContains(
        skillsDialog,
        QStringLiteral("已删除。删除前已捕获暂存备份"),
        "the skill removal no longer reports the backup id as the fallback");
    valid &= requireContains(
        skillsDialog,
        QStringLiteral("恢复操作尚未提供"),
        "the skill removal copy can now imply a restore action exists");
    // 只接捕获与其共享修剪入口：对话框不得长出恢复、逐条删除、裁剪逻辑副本、第二份
    // 备份根或自己的密钥来源。
    for (const QString &token : {
             QStringLiteral("removeVerified"),
             QStringLiteral("applyRetention"),
             QStringLiteral("pruneBackups"),
             QStringLiteral("ExtensionStagingRestore"),
             QStringLiteral("ExtensionStagingBackupInventory"),
             QStringLiteral("restoreBackup"),
             QStringLiteral("SecureStorageExtensionStagingBackupKeyProvider"),
             QStringLiteral("extensions-staging"),
             QStringLiteral("AppDataLocation")}) {
        valid &= requireAbsent(skillsDialog, token,
                               "the skills dialog grew restore, a local pruning "
                               "copy, its own key source, or a second backup "
                               "root beyond the pre-removal capture and its "
                               "shared retention entry point");
    }
    // MainWindow 是唯一接线点：密钥来源与备份根都取自唯一产品定义点，与 MCP 保存
    // 接线同一来源、同一注入纪律。
    valid &= requireOrdered(
        mainWindow,
        {QStringLiteral("SecureStorageExtensionStagingBackupKeyProvider"),
         QStringLiteral("new SkillsDialog(m_skillManager, "
                        "&stagingBackupKeyProvider"),
         QStringLiteral("extensionStagingBackupRootPath()")},
        "the skills dialog is constructed without the staging backup wiring");
    valid &= requireContains(
        cmake,
        QStringLiteral("skill_removal_backup"),
        "the skill removal backup wiring guards are absent from CTest");

    // 披露不导入。这一层与它的界面都不解包、不写盘、不安装、不启用任何东西，而这两个恒假
    // 字段是显式暴露的而不是省略：界面若把"已经看过这个包的内容"说成"已经导入这个包"，人
    // 会以为磁盘上已经多了一份东西并据此往下走，比如去清理一个从未被写入的目录。
    valid &= requireContains(
        importPresentationHeader,
        QStringLiteral("bool importsBundle = false;"),
        "the disclosure cannot state that nothing was imported");
    valid &= requireContains(
        importPresentationHeader,
        QStringLiteral("bool writesToDisk = false;"),
        "the disclosure cannot state that nothing was written");
    // 这两个不变量必须在每一条返回路径上成立，而不是只在成功路径上被设置：一次被拒绝的
    // 披露同样什么都没导入，而人需要知道自己不必去清理任何东西。
    valid &= requireOrdered(
        sourceRange(importPresentation,
                    QStringLiteral("ExtensionImportDisclosure refuse("),
                    QStringLiteral("} // namespace")),
        {QStringLiteral("disclosure.importsBundle = false;"),
         QStringLiteral("disclosure.writesToDisk = false;")},
        "a refused disclosure may still claim it imported or wrote something");
    valid &= requireOrdered(
        sourceRange(importPresentation,
                    QStringLiteral("ExtensionImportDisclosure ExtensionImportPresentation::build("),
                    QStringLiteral("    return disclosure;")),
        {QStringLiteral("disclosure.importsBundle = false;"),
         QStringLiteral("disclosure.writesToDisk = false;")},
        "a successful disclosure may still claim it imported or wrote something");
    for (const QString &token : {
             QStringLiteral("QTemporaryDir"),
             QStringLiteral("QTemporaryFile"),
             QStringLiteral("QSaveFile"),
             QStringLiteral("mkpath"),
             QStringLiteral("QIODevice::WriteOnly"),
             QStringLiteral("QProcess"),
             QStringLiteral("QSettings"),
             QStringLiteral("SecureStorage"),
             QStringLiteral(".effectiveEnabled ="),
             QStringLiteral("ExtensionEnablementLedger"),
             QStringLiteral("ExtensionReviewLedger")}) {
        valid &= requireAbsent(
            importPresentation, token,
            "the import presentation holds authority beyond disclosing");
    }
    // 读取失败时绝不构造预览。一次失败读取里的清单是垃圾：对它做预览有可能算出 Ready，
    // 于是一个读不出来的包在屏幕上变成一个可以批准的包。
    valid &= requireOrdered(
        importPresentation,
        {QStringLiteral("case ExtensionBundleReadState::Empty:"),
         QStringLiteral("case ExtensionBundleReadState::Unavailable:"),
         QStringLiteral("case ExtensionBundleReadState::Invalid:"),
         QStringLiteral("case ExtensionBundleReadState::Ready:"),
         QStringLiteral("ExtensionImportPreviewBuilder::build(read.manifest)")},
        "a failed read is previewed instead of refused before the preview runs");
    // 一个读不出来的目录与一个畸形的包要求人做不同的事：一个去看权限，一个去修包。把它们
    // 并成一个"无效"会把人送去重写一个本来没问题的包。
    valid &= requireContains(
        importPresentation,
        QStringLiteral("return refuse(ExtensionImportDisclosureState::Unreadable,"),
        "an unreadable bundle is not distinguished from a malformed one");
    valid &= requireContains(
        importPresentation,
        QStringLiteral("return refuse(ExtensionImportDisclosureState::Absent, QString());"),
        "an absent bundle directory is reported as a failure");
    // 判定层的诊断原样带出。这一层再编一个自己的代号会让人拿着一个查不到出处的东西。
    valid &= requireContains(
        importPresentation,
        QStringLiteral("preview.errorCode);"),
        "the preview diagnostic is replaced by a locally invented one");
    // 失败关闭保留全部组件证据，包括那个不支持的组件：隐藏证据会让没人能判断这个包到底
    // 想做什么，而失败关闭不等于把证据一起丢掉。
    valid &= requireContains(
        importPresentation,
        QStringLiteral("disclosure.components = preview.components;"),
        "failing closed discards or filters the component evidence");
    // 能力仍然逐组件披露，这一层不做任何整包汇总。
    for (const QString &token : {
             QStringLiteral("allCapabilities"),
             QStringLiteral("QSet<QString> capabilities"),
             QStringLiteral("capabilities.unite")}) {
        valid &= requireAbsent(
            importPresentation, token,
            "the import presentation rolls capabilities up across components");
    }

    // 界面这一侧：披露区与已装扩展表分开，因为它们回答的是两个不同的问题。按钮不叫"导入"，
    // 因为叫导入会让人以为点完之后磁盘上多了一份内容。
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("ExtensionImportPresentation::stateLabel(disclosure.state)"),
        "the disclosure surface re-decides what to say about the read");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("披露扩展包内容"),
        "the disclosure action is not labelled as a disclosure");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("没有导入、安装或启用任何内容，也没有向磁盘写入任何字节"),
        "the disclosure surface does not say it imported and wrote nothing");
    // 每一次披露完整替换上一次：留着上一次的组件会让一次失败的读取看起来在描述这一次选
    // 的那个包，而屏幕上那些组件属于另一个包。
    valid &= requireOrdered(
        sourceRange(extensionCenter,
                    QStringLiteral("void ExtensionCenterDialog::setImportDisclosure("),
                    QStringLiteral("void ExtensionCenterDialog::populate(")),
        {QStringLiteral("m_importTable->setRowCount(0);"),
         QStringLiteral("for (const ExtensionComponentPreview &item : disclosure.components)")},
        "a disclosure appends to the previous bundle's component list");
    valid &= requireContains(
        extensionCenterHeader,
        QStringLiteral("void bundleDisclosureRequested();"),
        "the disclosure request does not exist as its own signal");
    // 在权限、审批、沙箱与恢复门禁完成之前没有任何东西可以被导入，而一个发不出去的信号
    // 比一个能发出去的信号安全。
    for (const QString &token : {
             QStringLiteral("importRequested"),
             QStringLiteral("installRequested"),
             QStringLiteral("extractRequested")}) {
        valid &= requireAbsent(
            extensionCenterHeader, token,
            "the extension center can request an import before the gates exist");
    }

    const QString bundleDisclosurePath = sourceRange(
        mainWindow,
        QStringLiteral("void MainWindow::startExtensionBundleDisclosure("),
        QStringLiteral("void MainWindow::startExtensionUpdateCheck("));
    // 披露不写账本，因此它用自己的线程槽位与自己的代号：让一次纯读取去阻塞一次账本写入，
    // 或者反过来，都是没有理由的。
    valid &= requireContains(
        bundleDisclosurePath,
        QStringLiteral("if (!dialog || m_extensionBundleThread) return;"),
        "two disclosures can run concurrently and race for the one visible result");
    valid &= requireAbsent(
        bundleDisclosurePath,
        QStringLiteral("m_extensionReviewThread"),
        "a pure read contends for the ledger writers' worker slot");
    // 只接受目录。读一个归档就意味着先解压到某个地方，而解压是写盘。
    valid &= requireContains(
        bundleDisclosurePath,
        QStringLiteral("QFileDialog::getExistingDirectory("),
        "the disclosure accepts an archive path it would have to unpack");
    // 读与判定都必须走共享的那两层。界面自己重新判一遍必然会与判定层漂移，而漂移的方向是
    // 屏幕上给出一个判定层会拒绝的结论。
    for (const QString &token : {
             QStringLiteral("ExtensionBundleReader::read(root)"),
             QStringLiteral("ExtensionImportPresentation::build(")}) {
        valid &= requireContains(
            bundleDisclosurePath, token,
            "the disclosure path does not read and judge through the shared layers");
    }
    // 读取与判定都在工作线程上：一个大目录的逐字节摘要会让界面停住，而一个停住的界面上那
    // 份披露看起来像是已经出结果了。
    valid &= requireOrdered(
        bundleDisclosurePath,
        {QStringLiteral("QThread::create("),
         QStringLiteral("ExtensionImportPresentation::build("),
         QStringLiteral("target->setImportDisclosure(disclosure)"),
         QStringLiteral("target->setImportBusy(false)")},
        "the disclosure is computed on the GUI thread or reported before it is read");
    for (const QString &token : {
             QStringLiteral("SecureStorage"),
             QStringLiteral("ExtensionLifecycleController"),
             QStringLiteral("ExtensionEnablementController"),
             QStringLiteral("ExtensionReviewController")}) {
        valid &= requireAbsent(
            bundleDisclosurePath, token,
            "the disclosure path writes a ledger or commits something");
    }

    // 证据必须被确立，绝不能被假定。默认填真是这一层唯一真正危险的失败方式，因为它不会
    // 报错——它会成功：判定层会一路放行，而没有任何人真的验过签名、依赖或健康。
    valid &= requireOrdered(
        candidateBuilder,
        {QStringLiteral("result.evidence.signatureValid = false;"),
         QStringLiteral("result.evidence.dependenciesSatisfied = false;"),
         QStringLiteral("result.evidence.healthy = false;")},
        "unverifiable evidence is assumed rather than left false");
    for (const QString &token : {
             QStringLiteral("signatureValid = true"),
             QStringLiteral("dependenciesSatisfied = true"),
             QStringLiteral("healthy = true")}) {
        valid &= requireAbsent(
            candidateBuilder, token,
            "the candidate builder grants itself evidence nobody established");
    }
    // 每一条返回路径都让五项证据保持假。一个被拒绝的候选没有确立任何证据。
    valid &= requireContains(
        sourceRange(candidateBuilder,
                    QStringLiteral("ExtensionUpdateCandidateResult refuse("),
                    QStringLiteral("QStringList unionOfCapabilities(")),
        QStringLiteral("result.evidence = ExtensionUpdateEvidence{};"),
        "a refused candidate may still carry established evidence");
    // "无法核查"与"核查失败"不是同一件事：一个把人送去装签名权威，一个把人送去修包。并成
    // 一句"证据不足"会让人无从判断该去哪里。
    for (const QString &token : {
             QStringLiteral("extension-update-signature-authority-absent"),
             QStringLiteral("extension-update-dependency-resolver-absent"),
             QStringLiteral("extension-update-health-probe-absent")}) {
        valid &= requireContains(
            candidateBuilderHeader, token,
            "an unverifiable evidence item has no diagnostic of its own");
    }
    // 判定用并集：兼容性门禁必须失败关闭，因此任何一个组件请求写文件就等于这个扩展请求
    // 写文件。而披露仍然逐组件保留，因为人做决定看的是逐组件披露。
    valid &= requireContains(
        candidateBuilder,
        QStringLiteral("result.candidate.requestedCapabilities = "
                       "unionOfCapabilities(read.manifest);"),
        "the compatibility gate does not receive the union of every component's request");
    valid &= requireContains(
        candidateBuilder,
        QStringLiteral("result.manifest = read.manifest;"),
        "the per-component disclosure is discarded once the union is computed");
    // 候选按定义未复核、未授权。兼容性判定绝不能读到当前版本的信任或启用状态：读到了就等于
    // 让上一版的权威决定候选的结论。共享判定层当前并不读这两个字段，因此这两行是纵深防御，
    // 而纵深防御被删掉时行为上看不出来——只能钉在源码上。
    valid &= requireOrdered(
        candidateBuilder,
        {QStringLiteral("probe.trust = ExtensionTrustState::Unverified;"),
         QStringLiteral("probe.effectiveEnabled = false;")},
        "the candidate is judged carrying the active version's trust or grant");
    valid &= requireContains(
        candidateBuilder,
        QStringLiteral("compatibility.state == ExtensionCompatibilityState::Compatible"),
        "an unknown compatibility verdict is treated as compatible");
    valid &= requireContains(
        candidateBuilder,
        QStringLiteral("ExtensionCompatibilityPolicy::evaluate(probe, host)"),
        "the candidate builder re-decides compatibility itself");
    // 候选必须描述同一个扩展。按名字放行会让任意内容顶替一份已经被复核过的内容。
    valid &= requireContains(
        candidateBuilder,
        QStringLiteral("read.manifest.id != active.id"),
        "a candidate describing another extension is accepted");
    // 摘要来自磁盘上的字节。调用方传入的摘要会让它描述它并未携带的内容。
    valid &= requireOrdered(
        candidateBuilder,
        {QStringLiteral("result.candidate.sourceIdentity = read.manifest.sourceIdentity;"),
         QStringLiteral("result.candidate.contentIdentity = read.manifest.contentIdentity;")},
        "the candidate identity does not come from the bytes on disk");
    // 读取失败时不构造候选：一次失败读取里的清单是垃圾，而用它算出的摘要会被绑定成一份
    // 授权的目标。四个读取状态各自对应一个结论。
    valid &= requireOrdered(
        candidateBuilder,
        {QStringLiteral("return refuse(ExtensionUpdateCandidateState::Absent, QString());"),
         QStringLiteral("return refuse(ExtensionUpdateCandidateState::Unreadable, read.errorCode);"),
         QStringLiteral("return refuse(ExtensionUpdateCandidateState::Rejected, read.errorCode);"),
         QStringLiteral("case ExtensionBundleReadState::Ready:")},
        "a failed read still produces a candidate, or its diagnostic is invented locally");
    // 这一层不安装、不下载、不解包、不写盘、不执行任何东西，也不写任何账本。
    for (const QString &token : {
             QStringLiteral("QTemporaryDir"),
             QStringLiteral("QSaveFile"),
             QStringLiteral("mkpath"),
             QStringLiteral("QIODevice::WriteOnly"),
             QStringLiteral("QProcess"),
             QStringLiteral("QNetworkAccessManager"),
             QStringLiteral("QSettings"),
             QStringLiteral("SecureStorage"),
             QStringLiteral(".effectiveEnabled = true"),
             QStringLiteral("ExtensionEnablementLedger"),
             QStringLiteral("ExtensionReviewLedger"),
             QStringLiteral("ExtensionLifecycleController")}) {
        valid &= requireAbsent(
            candidateBuilder, token,
            "the candidate builder holds authority beyond describing a candidate");
    }
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_update_candidate_builder"),
        "the extension update candidate builder is absent from CTest");

    // 更新呈现层。这一层存在的核心理由是：当前没有任何一次更新可以成立，而这件事必须被说
    // 清楚，不能被一个灰掉的按钮代替——只灰掉按钮会让人以为是自己这个包有问题，于是反复
    // 重做包，而真正缺的是这台机器上根本没有装签名权威。
    valid &= requireContains(
        updatePresentationHeader,
        QStringLiteral("灰掉的按钮代替"),
        "the update surface no longer states why it must name what is missing");
    // "没有人能核查"与"核查失败"必须在结构上就是两个不同的字段。并成一个布尔值就等于在
    // 屏幕上把两件要求人做不同事情的情况合成一句"证据不足"。
    valid &= requireOrdered(
        updatePresentationHeader,
        {QStringLiteral("bool established = false;"),
         QStringLiteral("bool unverifiable = false;")},
        "an unverifiable evidence item cannot be told apart from a failed check");
    valid &= requireContains(
        updatePresentation,
        QStringLiteral("item.unverifiable = !established && unverifiable;"),
        "an established evidence item can still be marked unverifiable");
    // 确立了还带诊断会让人去查一个不存在的问题。
    valid &= requireContains(
        updatePresentation,
        QStringLiteral("item.diagnostic = established ? QString() : gap;"),
        "an established evidence item still carries a diagnostic");
    // 暂存不是启用。这三个字段是显式暴露的恒定值而不是省略，并且每一条返回路径都要写出来：
    // 界面若把"更新已暂存"说成"更新已完成"，人会认为新版本正在运行，而实际运行的仍然是旧
    // 版本——或者什么都没在运行。
    for (const QString &token : {
             QStringLiteral("bool stagesOnly = true;"),
             QStringLiteral("bool replacesActiveVersion = false;"),
             QStringLiteral("bool grantsExecution = false;")}) {
        valid &= requireContains(
            updatePresentationHeader, token,
            "the update plan does not declare that staging changes nothing");
    }
    valid &= requireOrdered(
        sourceRange(updatePresentation,
                    QStringLiteral("ExtensionUpdatePlan reject("),
                    QStringLiteral("ExtensionUpdateEvidenceLine line(")),
        {QStringLiteral("plan.stagesOnly = true;"),
         QStringLiteral("plan.replacesActiveVersion = false;"),
         QStringLiteral("plan.grantsExecution = false;")},
        "a rejected update plan may still claim it replaces or grants");
    valid &= requireOrdered(
        sourceRange(updatePresentation,
                    QStringLiteral("ExtensionUpdatePlan ExtensionUpdatePresentation::buildEmpty("),
                    QStringLiteral("ExtensionUpdatePlan ExtensionUpdatePresentation::build(")),
        {QStringLiteral("plan.stagesOnly = true;"),
         QStringLiteral("plan.replacesActiveVersion = false;"),
         QStringLiteral("plan.grantsExecution = false;")},
        "an empty update plan may still claim it replaces or grants");
    // 判定只有一个来源。这一层再判一遍必然会与判定层漂移，而漂移的方向是界面提供一个判定
    // 层会拒绝的动作。
    valid &= requireContains(
        updatePresentation,
        QStringLiteral("verdict.state == ExtensionUpdateState::StagedUnreviewed"),
        "the update surface decides stageability instead of reading the verdict");
    valid &= requireContains(
        updatePresentation,
        QStringLiteral("plan.downgrade = verdict.downgrade;"),
        "the update surface re-derives the downgrade conclusion itself");
    // 逐组件披露原样带出：判定用并集，展示用逐组件。汇总会让两个组件各自请求"读文件"与
    // "连网"看起来与一个组件同时请求两者完全一样，而后者才是真正危险的组合。
    valid &= requireContains(
        updatePresentation,
        QStringLiteral("plan.components = candidate.manifest.components;"),
        "the per-component disclosure is rolled up on the update surface");
    // 产出层与判定层的诊断原样带出。这一层再编一个代号会让人拿着一个查不到出处的东西。
    valid &= requireContains(
        updatePresentation,
        QStringLiteral("plan.errorCode = candidate.errorCode;"),
        "an unread candidate gets a locally invented diagnostic");
    valid &= requireContains(
        updatePresentation,
        QStringLiteral("plan.errorCode = verdict.errorCode;"),
        "a rejected verdict gets a locally invented diagnostic");
    // 这一层不安装、不解包、不写盘、不执行，也不写任何账本。
    for (const QString &token : {
             QStringLiteral("QTemporaryDir"),
             QStringLiteral("QSaveFile"),
             QStringLiteral("mkpath"),
             QStringLiteral("QIODevice::WriteOnly"),
             QStringLiteral("QProcess"),
             QStringLiteral("QNetworkAccessManager"),
             QStringLiteral("QSettings"),
             QStringLiteral("SecureStorage"),
             QStringLiteral(".effectiveEnabled = true"),
             QStringLiteral("ExtensionEnablementLedger"),
             QStringLiteral("ExtensionReviewLedger"),
             QStringLiteral("ExtensionLifecycleController")}) {
        valid &= requireAbsent(
            updatePresentation, token,
            "the update surface holds authority beyond describing a plan");
    }

    // 界面这一侧：更新区与披露区分开，因为它们回答的是两个不同的问题；披露问"这个包里有
    // 什么"，更新问"这个包能不能替换已经在列的那一份"。
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("extensionUpdateTable"),
        "the extension center has no update evidence table");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("无人可核查"),
        "the extension center reads an absent authority as a failed check");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("不是这个包的问题"),
        "the extension center blames the bundle for an absent authority");
    valid &= requireContains(
        extensionCenter,
        QStringLiteral("没有替换当前生效的版本"),
        "the extension center does not say a check changed nothing");
    // 每一次检查完整替换上一次的证据表：留着上一次的证据会让一次失败的检查看起来在描述
    // 这一次选的那个候选包。
    valid &= requireOrdered(
        extensionCenter,
        {QStringLiteral("void ExtensionCenterDialog::setUpdatePlan("),
         QStringLiteral("m_updateTable->setRowCount(0);")},
        "a settled check may leave the previous candidate's evidence on screen");
    // 这里没有"就在这里点一下完成更新"的动作：在权限、审批、沙箱与恢复门禁完成之前没有
    // 任何东西可以被暂存到磁盘上。
    for (const QString &token : {
             QStringLiteral("updateStageRequested"),
             QStringLiteral("stageUpdateRequested"),
             QStringLiteral("ExtensionLifecycleController::stageUpdate")}) {
        valid &= requireAbsent(
            extensionCenter, token,
            "the extension center can stage an update before the gates exist");
    }
    // 检查更新这条路径只读：它读一份候选包，重新读一次清单与复核账本，然后把证据摆出来。
    // 当前生效的那一份必须重新读，而不是用对话框里那一份：用一份过期的记录去比对候选，会让
    // "内容没有变化"这个结论朝两个方向都可能出错。
    valid &= requireOrdered(
        mainWindow,
        {QStringLiteral("void MainWindow::startExtensionUpdateCheck("),
         QStringLiteral("bound.reviewPins = ledger.pins;"),
         QStringLiteral("ExtensionInventoryCoordinator::collect(bound)"),
         QStringLiteral("ExtensionUpdateCandidateBuilder::build(active, root, bound.host)")},
        "the update check compares a candidate against a stale active record");
    valid &= requireContains(
        mainWindow,
        QStringLiteral("ExtensionUpdatePresentation::build("),
        "the update check does not go through the shared presentation layer");
    // 与披露同样只接受目录：读一个归档意味着先解压到某个地方，而解压是写盘。
    valid &= requireContains(
        sourceRange(mainWindow,
                    QStringLiteral("void MainWindow::startExtensionUpdateCheck("),
                    QStringLiteral("void MainWindow::onHelpClicked()")),
        QStringLiteral("QFileDialog::getExistingDirectory("),
        "the update check accepts an archive, which would require unpacking to disk");
    const QString updateCheckPath = sourceRange(
        mainWindow,
        QStringLiteral("void MainWindow::startExtensionUpdateCheck("),
        QStringLiteral("void MainWindow::onHelpClicked()"));
    // 这条路径读复核账本，但绝不写它：一次检查不改变任何记录。它也不解包、不写盘、不执行。
    for (const QString &token : {
             QStringLiteral(".replace("),
             QStringLiteral("QTemporaryDir"),
             QStringLiteral("QSaveFile"),
             QStringLiteral("mkpath"),
             QStringLiteral("QIODevice::WriteOnly"),
             QStringLiteral("QProcess"),
             QStringLiteral("QNetworkAccessManager"),
             QStringLiteral("ExtensionLifecycleController"),
             QStringLiteral("ExtensionEnablementController"),
             QStringLiteral("ExtensionReviewController")}) {
        valid &= requireAbsent(
            updateCheckPath, token,
            "the update check writes a ledger, unpacks, or commits something");
    }
    // 账本读不出来时不带任何复核记录：把残留的那几条当成复核过，等于让一次读取失败变成
    // 一次授信。
    valid &= requireContains(
        updateCheckPath,
        QStringLiteral("bound.reviewPins.clear();"),
        "an unreadable review ledger still lends its leftover pins to the check");
    valid &= requireContains(
        cmake,
        QStringLiteral("extension_update_presentation"),
        "the extension update presentation layer is absent from CTest");

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
    // 授权路径与复核路径同构：加载账本、请求、派发。授权与复核共用同一个工作线程槽位与
    // 同一个代号，因此析构里已有的 join 覆盖两条路径，并发的两次操作也不会互相把结果从
    // 屏幕上抹掉。
    valid &= requireOrdered(
        extensionReviewPath,
        {QStringLiteral("grantStore.load()"),
         QStringLiteral("enablementRequested"),
         QStringLiteral("startExtensionEnablementOperation")},
        "extension grant UI does not load, confirm, and dispatch grant operations");
    const QString extensionGrantPath = sourceRange(
        mainWindow,
        QStringLiteral("void MainWindow::startExtensionEnablementOperation("),
        QStringLiteral("void MainWindow::onHelpClicked()"));
    // 同一个槽位:授权操作必须在复核线程仍在运行时拒绝，而不是并发启动第二个写入者。
    valid &= requireContains(
        extensionGrantPath,
        QStringLiteral("if (!dialog || m_extensionReviewThread)"),
        "a grant operation can run concurrently with a review operation");
    valid &= requireContains(
        extensionGrantPath,
        QStringLiteral("extension-enablement-operation-busy"),
        "a refused concurrent grant operation carries no diagnostic");
    // 失败之后屏幕换成重新读到的快照，并且保持冻结:继续显示提交前的乐观状态会让人以为
    // 授权已经生效。
    valid &= requireOrdered(
        extensionGrantPath,
        {QStringLiteral("target->setEnablementSnapshot("),
         QStringLiteral("if (!result.committed)"),
         QStringLiteral("target->showEnablementError(result.errorCode)"),
         QStringLiteral("target->setEnablementBusy(true)")},
        "a failed grant leaves the pre-commit state on screen or unfrozen");
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
        QStringLiteral("McpConfigurationInventory::inspectFile(path)"),
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

    // 后端封闭性：Backend 是私有的封闭枚举，模块缺席的钉板挡不住"在同一个文件里多长一个
    // 变体"。整段枚举文本按原样钉住，任何新增变体（无论叫什么都必须先改这一段）都会让
    // 本测试失败；变体构造形态与适配器类型名再各自缺席一遍，防止绕过枚举文本的等价
    // 引入。该块只有六行、结构稳定，按全文本钉住不会产生脆弱噪音。
    valid &= requireContains(
        runtime,
        QStringLiteral("enum Backend {\n    Preview,\n    Codex(CodexAdapter),\n"
                       "    Recovery(WorkbenchRecoveryDiagnostic),\n"
                       "    Unavailable(String),\n}"),
        "Runtime Backend enum is no longer the closed Codex-only variant set");
    for (const QString &forbidden : {
             QStringLiteral("Claude("),
             QStringLiteral("Gemini("),
             QStringLiteral("Acp("),
             QStringLiteral("ClaudeAdapter"),
             QStringLiteral("GeminiAdapter"),
             QStringLiteral("AcpAdapter"),
             QStringLiteral("claude_adapter"),
             QStringLiteral("gemini_adapter"),
             QStringLiteral("acp_adapter"),
         }) {
        valid &= requireAbsent(runtime, forbidden,
                               "a non-Codex backend reached the Runtime implementation");
    }
    // 活的适配器构造入口只有 with_codex() 与其带存储变体;daemon 入口恰好四条
    // Runtime::with_* 路径（两条只落到 Preview/Recovery 的存储恢复路径，两条 Codex
    // 路径），任何第五条路径或以其他适配器命名的构造函数都必须先改这里的钉板。
    valid &= requireContains(runtime, QStringLiteral("pub fn with_codex()"),
                             "Runtime lost its Codex-only construction entry");
    valid &= require(runtimeMain.count(QStringLiteral("Runtime::with_")) == 4,
                     "daemon entry gained a Runtime construction path beyond the "
                     "pinned store-recovery/Codex set");
    for (const QString &entry : {
             QStringLiteral("Runtime::with_store("),
             QStringLiteral("Runtime::with_emergency_store("),
             QStringLiteral("Runtime::with_codex("),
             QStringLiteral("Runtime::with_codex_and_store("),
         }) {
        valid &= requireContains(runtimeMain, entry,
                                 "daemon entry lost a pinned Runtime construction path");
    }
    for (const QString &forbidden : {
             QStringLiteral("with_claude"),
             QStringLiteral("with_gemini"),
             QStringLiteral("with_acp"),
         }) {
        valid &= requireAbsent(runtime, forbidden,
                               "Runtime exposes a non-Codex construction entry");
        valid &= requireAbsent(runtimeMain, forbidden,
                               "daemon entry selects a non-Codex runtime");
    }

    // 编程界面的三个源文件是用户能看到运行方身份的全部位置：窗口、部件与运行时客户端
    // 都不得出现任何非 Codex 运行方的名称或适配器标识。
    for (const QString &surface : {workbenchWindow, workbenchWidget, runtimeClient}) {
        for (const QString &advertisement : {
                 QStringLiteral("Gemini"),
                 QStringLiteral("ACP"),
                 QStringLiteral("Claude Opus"),
                 QStringLiteral("claude_adapter"),
                 QStringLiteral("gemini_adapter"),
                 QStringLiteral("acp_adapter"),
             }) {
            valid &= requireAbsent(surface, advertisement,
                                   "programming surface advertises a deferred "
                                   "non-Codex runtime");
        }
    }

    // 目录元数据惰性：RuntimeAdapterFamily 只参与目录兼容性校验，它一旦出现在拥有
    // 后端的 lib.rs 里，就意味着目录条目开始驱动运行方选择;目录模块自身也不得引用
    // 任何后端构造符号。目录侧保留的 Acp 匹配臂只产出校验错误，不构成可达路径。
    valid &= requireAbsent(runtime, QStringLiteral("RuntimeAdapterFamily"),
                           "catalog adapter family metadata drives backend selection");
    for (const QString &forbidden : {
             QStringLiteral("CodexAdapter"),
             QStringLiteral("with_backend"),
             QStringLiteral("Backend::"),
         }) {
        valid &= requireAbsent(modelCatalog, forbidden,
                               "model catalog constructs a runtime backend");
    }
    valid &= requireContains(
        modelCatalog,
        QStringLiteral("RuntimeAdapterFamily::Acp if self.protocol != \"acp\" => {"),
        "catalog adapter family no longer gates on compatibility validation only");

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
