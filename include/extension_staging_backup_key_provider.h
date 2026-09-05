#ifndef EXTENSION_STAGING_BACKUP_KEY_PROVIDER_H
#define EXTENSION_STAGING_BACKUP_KEY_PROVIDER_H

#include "configuration_backup_store.h"

#include <QString>

// 扩展暂存备份域的生产密钥来源与备份根位置。暂存捕获/清点/恢复组件刻意不发明位置与
// 密钥（位置权威在调用方），本头文件是产品侧的唯一答案：密钥经 SecureStorage 按暂存域
// 的密钥作用域存取（与 ToolManager 的工具配置备份密钥同一类写入），备份根沿用
// ToolManager 工具备份根的父目录惯例（AppDataLocation/backups/<作用域>）落在
// `extensions-staging` 子根——与四个工具子根不重叠，与暂存域的主体命名空间同理。
// 两者都只在这里定义一次；后续任何产品调用方（保存前备份、移除执行、恢复）都必须
// 复用本文件，不得发明第二份根或第二份密钥存取。
//
// 诊断代号沿用 ToolManager 密钥来源的先例：密钥不可得时返回的代号与存储层逐字相同
// （`extension-staging-backup-key-unavailable`），调用方按既有代号理解失败，不另造
// 本地代号。
class SecureStorageExtensionStagingBackupKeyProvider final
    : public ConfigurationBackupKeyProvider
{
public:
    bool keyForScope(const QString &scope, bool allowCreate,
                     QByteArray *key, QString *error) override;
};

// 扩展暂存备份根的唯一产品定义点。
QString extensionStagingBackupRootPath();

#endif // EXTENSION_STAGING_BACKUP_KEY_PROVIDER_H
