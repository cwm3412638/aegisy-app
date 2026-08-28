#include "extension_enablement_ledger_secure_storage_adapter.h"

#include "secure_storage_authority_slot_adapter.h"

namespace {

const char kSlotAScope[] = "extensions/enablement-ledger-authority/slot-a/v1";
const char kSlotBScope[] = "extensions/enablement-ledger-authority/slot-b/v1";
// 这些常量参与已持久化的字节，因此不能更改。它们与复核记录的对应常量必须不同：
// 否则一份复核授权的槽位字节可以被搬到启用授权的作用域里通过验证。
const char kFrameSchema[] =
    "aegisy-extension-enablement-ledger-authority-slot/0.1";
const char kDigestDomain[] =
    "aegisy-extension-enablement-ledger-authority-slot-digest/0.1\0";

// 启用授权是新增子系统，没有迁移前的单槽授权可以采纳，因此 legacyScope 留空。
SecureStorageAuthoritySlotScopes scopes()
{
    SecureStorageAuthoritySlotScopes value;
    value.domain.frameSchema = QByteArray(kFrameSchema, sizeof(kFrameSchema) - 1);
    value.domain.digestDomain =
        QByteArray(kDigestDomain, sizeof(kDigestDomain) - 1);
    value.domain.errorPrefix =
        QStringLiteral("extension-enablement-authority-slot-");
    value.slotAScope = QString::fromLatin1(kSlotAScope);
    value.slotBScope = QString::fromLatin1(kSlotBScope);
    value.errorPrefix = QStringLiteral("extension-enablement-secure");
    return value;
}

} // namespace

QString SecureStorageExtensionEnablementLedgerAdapter::authoritySlotAScope()
{
    return QString::fromLatin1(kSlotAScope);
}

QString SecureStorageExtensionEnablementLedgerAdapter::authoritySlotBScope()
{
    return QString::fromLatin1(kSlotBScope);
}

SecureStorageAuthoritySlotScopes
SecureStorageExtensionEnablementLedgerAdapter::authoritySlotScopes()
{
    return scopes();
}

ExtensionEnablementLedgerSecureStore::ReadState
SecureStorageExtensionEnablementLedgerAdapter::readFresh(
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

ExtensionEnablementLedgerSecureStore::WriteOutcome
SecureStorageExtensionEnablementLedgerAdapter::write(
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
