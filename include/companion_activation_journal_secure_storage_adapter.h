#ifndef COMPANION_ACTIVATION_JOURNAL_SECURE_STORAGE_ADAPTER_H
#define COMPANION_ACTIVATION_JOURNAL_SECURE_STORAGE_ADAPTER_H

#include "companion_activation_journal.h"

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
};

#endif // COMPANION_ACTIVATION_JOURNAL_SECURE_STORAGE_ADAPTER_H
