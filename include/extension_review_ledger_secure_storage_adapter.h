#ifndef EXTENSION_REVIEW_LEDGER_SECURE_STORAGE_ADAPTER_H
#define EXTENSION_REVIEW_LEDGER_SECURE_STORAGE_ADAPTER_H

#include "extension_review_ledger_store.h"
#include "secure_storage_authority_slot_adapter.h"

// 复核记录授权（HMAC 密钥、已提交代号与身份）落在平台安全存储里，并通过 A/B
// 双槽发布：密钥不存在于任何其他位置，因此一次被打断的写入若销毁唯一副本，所有
// 已存复核记录将永久无法认证。双槽把这种损坏降级为"上一次发布没有生效"，也就是
// ExtensionReviewLedgerStore 已经能确定性处理的中断点。
//
// 作用域独立于激活日志，摘要域也不同，因此两个子系统的槽位字节不能互相冒充。
class SecureStorageExtensionReviewLedgerAdapter final
    : public ExtensionReviewLedgerSecureStore
{
public:
    ReadState readFresh(QByteArray *value, QString *errorCode) override;
    WriteOutcome write(const QByteArray &value, QString *errorCode) override;

    static QString authoritySlotAScope();
    static QString authoritySlotBScope();

    // 暴露实际生效的作用域与域串，供测试独立比对。测试若自带一份域串副本，则改动
    // 适配器里的常量不会让任何断言失败，而这些常量参与已持久化的字节。
    static SecureStorageAuthoritySlotScopes authoritySlotScopes();
};

#endif // EXTENSION_REVIEW_LEDGER_SECURE_STORAGE_ADAPTER_H
