#ifndef EXTENSION_STAGING_BACKUP_RETENTION_H
#define EXTENSION_STAGING_BACKUP_RETENTION_H

#include "extension_staging_backup_inventory.h"

#include <QList>
#include <QString>

// 捕获成功后的保留期修剪：唯一共享入口。`planRetention`/`applyRetention` 早已存在且已
// 测试，但没有任何触发器——备份只增不删。本组件把"捕获成功后修剪该主体"收敛成一处，
// MCP 保存接线与恢复编排器两个调用点只消费它，绝不各自复制修剪逻辑；修剪的唯一写入仍
// 是存储的身份绑定验证删除（经 `applyRetention` 逐条组合），不修改扩展来源树，不安装、
// 不启用、不执行任何东西。
//
// 诚实语义（本组件存在的理由）：
//
// - 修剪是捕获成功之后的后续清理。捕获已经发生、备份已经存在——修剪的任何失败都绝不
//   代表捕获失败，调用方绝不允许把本结果翻转成捕获/保存/恢复的失败。
// - 计划失败（退化清点等）= 零删除 + 诊断逐字透传（`planFailed` + `planError`）：
//   退化的清点恰恰是绝不该动删除的状态。
// - apply 的逐条结果如实汇总：删了几份（`removedCount`）、损坏条目按证据原地保留几份
//   （`corruptKeptCount`——验证删除路径无法认证损坏清单，物理清除待证据处理决策，如实
//   报告而非静默丢弃）、其余失败逐条携带备份 id 与诊断（`failures`）。绝不整体静默
//   成败。
// - 修剪只动计划针对的那一个主体：`applyRetention` 的验证删除只按精确备份 id 删除，
//   其他主体的备份绝不被触碰。该主体最近一份完整备份无条件保留并回显在
//   `newestVerifiedKept`——那是显式的、单独报告的保留决策。
struct ExtensionStagingBackupRetentionRun {
    // 计划阶段失败（退化清点、请求无效）：零删除，planError 逐字携带下层的原诊断。
    bool planFailed = false;
    QString planError;
    // 计划成功后 apply 的逐条汇总：实际删除的完整备份数。
    int removedCount = 0;
    // 损坏条目被验证删除路径如实拒绝：证据原地保留（物理清除待证据处理决策）。
    int corruptKeptCount = 0;
    // Removed 与 CorruptRefused 之外的逐条失败，各自携带备份 id 与诊断。
    QList<ExtensionStagingRetentionApplyEntry> failures;
    // 无条件保留的最近一份完整备份（计划成功且该主体有完整备份时非空）。
    QString newestVerifiedKept;
};

class ExtensionStagingBackupRetention
{
public:
    // 在一次成功的捕获之后对该主体执行保留期修剪。返回值如实描述计划与逐条删除的
    // 真相：计划失败是零删除加诊断，逐条失败逐条可见；本函数的任何失败都绝不代表
    // 捕获本身失败。全部工作同步发生在调用点（调用方都是应用模态路径，不引入任何
    // 新的异步）。
    static ExtensionStagingBackupRetentionRun pruneAfterCapture(
        const QString &backupRoot, ConfigurationBackupKeyProvider *keyProvider,
        const QString &subject);
};

#endif // EXTENSION_STAGING_BACKUP_RETENTION_H
