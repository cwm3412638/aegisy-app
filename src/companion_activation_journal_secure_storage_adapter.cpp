#include "companion_activation_journal_secure_storage_adapter.h"

#include "companion_activation_authority_slots.h"
#include "secure_storage_authority_slot_adapter.h"

namespace {

const char kAuthorityScope[] = "companion/activation-journal-authority/v1";
const char kSlotAScope[] = "companion/activation-journal-authority/slot-a/v1";
const char kSlotBScope[] = "companion/activation-journal-authority/slot-b/v1";

// 激活日志早于双槽发布存在，因此保留迁移前的单槽作用域：双槽发布确认之后才可移除。
// 模式串与摘要域取自 CompanionActivationAuthoritySlots，不在这里重抄一份：两份副本
// 会各自漂移，而这些字节已经被持久化，改动会让现有安装读不出自己的授权。
SecureStorageAuthoritySlotScopes scopes()
{
    SecureStorageAuthoritySlotScopes value;
    value.domain = CompanionActivationAuthoritySlots::domain();
    value.slotAScope = QString::fromLatin1(kSlotAScope);
    value.slotBScope = QString::fromLatin1(kSlotBScope);
    value.legacyScope = QString::fromLatin1(kAuthorityScope);
    value.errorPrefix = QStringLiteral("activation-journal-secure");
    return value;
}

} // namespace

QString SecureStorageCompanionActivationJournalAdapter::authorityScope()
{
    return QString::fromLatin1(kAuthorityScope);
}

QString SecureStorageCompanionActivationJournalAdapter::authoritySlotAScope()
{
    return QString::fromLatin1(kSlotAScope);
}

QString SecureStorageCompanionActivationJournalAdapter::authoritySlotBScope()
{
    return QString::fromLatin1(kSlotBScope);
}

SecureStorageAuthoritySlotScopes
SecureStorageCompanionActivationJournalAdapter::authoritySlotScopes()
{
    return scopes();
}

CompanionActivationJournalSecureStore::ReadState
SecureStorageCompanionActivationJournalAdapter::readFresh(
    QByteArray *value, QString *errorCode)
{
    switch (SecureStorageAuthoritySlotAdapter::readFresh(
                scopes(), value, errorCode)) {
    case SecureStorageAuthoritySlotReadState::Missing:
        return ReadState::Missing;
    case SecureStorageAuthoritySlotReadState::Found:
        return ReadState::Found;
    case SecureStorageAuthoritySlotReadState::Unavailable:
        return ReadState::Unavailable;
    case SecureStorageAuthoritySlotReadState::Invalid:
        break;
    }
    return ReadState::Invalid;
}

CompanionActivationJournalSecureStore::WriteOutcome
SecureStorageCompanionActivationJournalAdapter::write(
    const QByteArray &value, QString *errorCode)
{
    switch (SecureStorageAuthoritySlotAdapter::write(
                scopes(), value, errorCode)) {
    case SecureStorageAuthoritySlotWriteOutcome::Committed:
        return WriteOutcome::Committed;
    case SecureStorageAuthoritySlotWriteOutcome::OutcomeUnknown:
        return WriteOutcome::OutcomeUnknown;
    case SecureStorageAuthoritySlotWriteOutcome::DefiniteFailure:
        break;
    }
    return WriteOutcome::DefiniteFailure;
}
