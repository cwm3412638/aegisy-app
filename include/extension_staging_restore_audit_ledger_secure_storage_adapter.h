#ifndef EXTENSION_STAGING_RESTORE_AUDIT_LEDGER_SECURE_STORAGE_ADAPTER_H
#define EXTENSION_STAGING_RESTORE_AUDIT_LEDGER_SECURE_STORAGE_ADAPTER_H

#include "extension_staging_restore_audit_ledger_store.h"
#include "secure_storage_authority_slot_adapter.h"

// 恢复审批审计链的授权信封（HMAC 密钥、已提交代号与身份）落在平台安全存储里，并通过
// A/B 双槽发布：密钥不存在于任何其他位置，因此一次被打断的写入若销毁唯一副本，所有
// 已存审计记录将永久无法认证。双槽把这种损坏降级为"上一次发布没有生效"，也就是
// ExtensionStagingRestoreAuditLedgerStore 已经能确定性处理的中断点。
//
// 作用域、模式串与摘要域都独立于复核记录、启用授权与激活日志，因此各子系统的槽位
// 字节不能互相冒充。这一点在这里尤其重要：把一份复核或启用授权搬进恢复审计的位置，
// 就等于把"我看过这份内容"或"我要求运行这份内容"伪造成"用户同意把这份备份写回
// 目标"。
//
// 这一层只搬字节：它不判定批准是否有效、不执行恢复、不授予任何权限。
class SecureStorageExtensionRestoreAuditLedgerAdapter final
    : public ExtensionStagingRestoreAuditSecureStore
{
public:
    ReadState readFresh(QByteArray *value, QString *errorCode) override;
    WriteOutcome write(const QByteArray &value, QString *errorCode) override;

    static QString authoritySlotAScope();
    static QString authoritySlotBScope();

    // 暴露实际生效的作用域与域串，供测试独立比对。测试若自带一份域串副本，则改动
    // 适配器里的常量不会让任何断言失败，而这些常量参与被持久化的字节。
    static SecureStorageAuthoritySlotScopes authoritySlotScopes();
};

#endif // EXTENSION_STAGING_RESTORE_AUDIT_LEDGER_SECURE_STORAGE_ADAPTER_H
