#ifndef COMPANION_ACTIVATION_JOURNAL_SECURE_STORAGE_ADAPTER_H
#define COMPANION_ACTIVATION_JOURNAL_SECURE_STORAGE_ADAPTER_H

#include "companion_activation_journal.h"
#include "secure_storage_authority_slot_adapter.h"

// 激活日志授权信封只有一个固定作用域：整台机器同时只允许一笔激活事务。
// 载荷通过 A/B 双槽发布，因此一次被打断的安全存储写入只会让"上一次发布没有
// 生效"，而不会销毁唯一的 HMAC 密钥副本。
class SecureStorageCompanionActivationJournalAdapter final
    : public CompanionActivationJournalSecureStore
{
public:
    ReadState readFresh(QByteArray *value, QString *errorCode) override;
    WriteOutcome write(const QByteArray &value, QString *errorCode) override;

    static QString authorityScope();
    static QString authoritySlotAScope();
    static QString authoritySlotBScope();

    // 暴露实际生效的作用域与域串，供测试独立比对。测试若自带一份域串副本，则改动
    // 适配器里的常量不会让任何断言失败，而这些常量参与已持久化的字节。
    static SecureStorageAuthoritySlotScopes authoritySlotScopes();
};

#endif // COMPANION_ACTIVATION_JOURNAL_SECURE_STORAGE_ADAPTER_H
