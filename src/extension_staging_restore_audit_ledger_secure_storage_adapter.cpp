#include "extension_staging_restore_audit_ledger_secure_storage_adapter.h"

#include "secure_storage_authority_slot_adapter.h"

namespace {

const char kSlotAScope[] =
    "extensions/restore-audit-ledger-authority/slot-a/v1";
const char kSlotBScope[] =
    "extensions/restore-audit-ledger-authority/slot-b/v1";
// 这些常量参与已持久化的字节，因此不能更改。它们与复核记录、启用授权的对应常量必须
// 不同：否则那两类授权的槽位字节可以被搬到恢复审计的作用域里冒充审计授权。
const char kFrameSchema[] =
    "aegisy-extension-restore-audit-ledger-authority-slot/0.1";
const char kDigestDomain[] =
    "aegisy-extension-restore-audit-ledger-authority-slot-digest/0.1\0";

// 恢复审计是新增子系统，没有迁移前的单槽授权可以采纳，因此 legacyScope 留空。
SecureStorageAuthoritySlotScopes scopes()
{
    SecureStorageAuthoritySlotScopes value;
    value.domain.frameSchema = QByteArray(kFrameSchema, sizeof(kFrameSchema) - 1);
    value.domain.digestDomain =
        QByteArray(kDigestDomain, sizeof(kDigestDomain) - 1);
    value.domain.errorPrefix =
        QStringLiteral("extension-restore-audit-authority-slot-");
    value.slotAScope = QString::fromLatin1(kSlotAScope);
    value.slotBScope = QString::fromLatin1(kSlotBScope);
    value.errorPrefix = QStringLiteral("extension-restore-audit-secure");
    return value;
}

} // namespace

QString SecureStorageExtensionRestoreAuditLedgerAdapter::authoritySlotAScope()
{
    return QString::fromLatin1(kSlotAScope);
}

QString SecureStorageExtensionRestoreAuditLedgerAdapter::authoritySlotBScope()
{
    return QString::fromLatin1(kSlotBScope);
}

SecureStorageAuthoritySlotScopes
SecureStorageExtensionRestoreAuditLedgerAdapter::authoritySlotScopes()
{
    return scopes();
}

ExtensionStagingRestoreAuditSecureStore::ReadState
SecureStorageExtensionRestoreAuditLedgerAdapter::readFresh(
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

ExtensionStagingRestoreAuditSecureStore::WriteOutcome
SecureStorageExtensionRestoreAuditLedgerAdapter::write(
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
