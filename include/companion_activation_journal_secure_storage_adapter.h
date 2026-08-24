#ifndef COMPANION_ACTIVATION_JOURNAL_SECURE_STORAGE_ADAPTER_H
#define COMPANION_ACTIVATION_JOURNAL_SECURE_STORAGE_ADAPTER_H

#include "companion_activation_journal.h"

// 激活日志授权信封只有一个固定作用域：整台机器同时只允许一笔激活事务。
class SecureStorageCompanionActivationJournalAdapter final
    : public CompanionActivationJournalSecureStore
{
public:
    ReadState readFresh(QByteArray *value, QString *errorCode) override;
    WriteOutcome write(const QByteArray &value, QString *errorCode) override;

    static QString authorityScope();
};

#endif // COMPANION_ACTIVATION_JOURNAL_SECURE_STORAGE_ADAPTER_H
