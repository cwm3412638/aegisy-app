#include "extension_staging_backup_retention.h"

ExtensionStagingBackupRetentionRun ExtensionStagingBackupRetention::pruneAfterCapture(
        const QString &backupRoot, ConfigurationBackupKeyProvider *keyProvider,
        const QString &subject)
{
    ExtensionStagingBackupRetentionRun run;
    ExtensionStagingRetentionPlan plan;
    QString planError;
    // 计划失败 = 零删除 + 诊断逐字透传。退化清点之上绝不产出计划，也就绝不删除。
    if (!ExtensionStagingBackupInventory::planRetention(
            backupRoot, subject, &plan, &planError)) {
        run.planFailed = true;
        run.planError = planError;
        return run;
    }
    run.newestVerifiedKept = plan.newestVerifiedKept;
    // 逐条组合验证删除并如实汇总：删了几个、损坏保留几个、失败几个各自可见，绝不
    // 整体静默成败。
    const QList<ExtensionStagingRetentionApplyEntry> outcomes =
        ExtensionStagingBackupInventory::applyRetention(backupRoot, keyProvider,
                                                        plan);
    for (const ExtensionStagingRetentionApplyEntry &entry : outcomes) {
        switch (entry.outcome) {
        case ExtensionStagingBackupRemovalOutcome::Removed:
            ++run.removedCount;
            break;
        case ExtensionStagingBackupRemovalOutcome::CorruptRefused:
            // 损坏条目无法被验证删除路径认证：如实计为原地保留的证据。
            ++run.corruptKeptCount;
            break;
        case ExtensionStagingBackupRemovalOutcome::RequestInvalid:
        case ExtensionStagingBackupRemovalOutcome::IdMalformed:
        case ExtensionStagingBackupRemovalOutcome::NotFound:
        case ExtensionStagingBackupRemovalOutcome::ListingDegraded:
        case ExtensionStagingBackupRemovalOutcome::StoreFailed:
            run.failures.append(entry);
            break;
        }
    }
    return run;
}
