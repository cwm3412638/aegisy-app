#include "secure_storage_authority_slot_adapter.h"

#include "secure_storage.h"

namespace {

void fail(QString *errorCode, const QString &value)
{
    if (errorCode) *errorCode = value;
}

QString code(const SecureStorageAuthoritySlotScopes &scopes, const char *suffix)
{
    return scopes.errorPrefix + QLatin1Char('-') + QLatin1String(suffix);
}

bool strictUtf8(const QString &value, QByteArray *bytes)
{
    if (!bytes) return false;
    *bytes = value.toUtf8();
    return !bytes->isEmpty()
        && bytes->size() <= SecureStorageAuthoritySlotAdapter::MaxAuthorityBytes
        && !bytes->contains('\0')
        && QString::fromUtf8(bytes->constData(), bytes->size()) == value;
}

bool strictUtf8(const QByteArray &bytes, QString *value)
{
    if (!value || bytes.isEmpty()
            || bytes.size() > SecureStorageAuthoritySlotAdapter::MaxAuthorityBytes
            || bytes.contains('\0')) {
        return false;
    }
    const QString decoded = QString::fromUtf8(bytes.constData(), bytes.size());
    if (decoded.toUtf8() != bytes) return false;
    *value = decoded;
    return true;
}

// 绕过进程缓存：后端被锁定时不能被误读成"从未记录过"。
AuthoritySlotInput readSlot(const QString &scope)
{
    AuthoritySlotInput input;
    if (scope.isEmpty()) {
        input.state = AuthoritySlotReadState::Missing;
        return input;
    }
    const SecureStorageReadResult result = SecureStorage::loadEncryptedFresh(scope);
    switch (result.state) {
    case SecureStorageReadState::Missing:
        input.state = AuthoritySlotReadState::Missing;
        return input;
    case SecureStorageReadState::Unavailable:
        input.state = AuthoritySlotReadState::Unavailable;
        return input;
    case SecureStorageReadState::Invalid:
        input.state = AuthoritySlotReadState::Invalid;
        return input;
    case SecureStorageReadState::Found:
        if (!strictUtf8(result.value, &input.frame)) {
            input.state = AuthoritySlotReadState::Invalid;
            return input;
        }
        input.state = AuthoritySlotReadState::Found;
        return input;
    }
    input.state = AuthoritySlotReadState::Invalid;
    return input;
}

QString slotScope(const SecureStorageAuthoritySlotScopes &scopes,
                  AuthoritySlotName slot)
{
    return slot == AuthoritySlotName::SlotA ? scopes.slotAScope : scopes.slotBScope;
}

AuthoritySlotSelection currentSelection(
    const SecureStorageAuthoritySlotScopes &scopes)
{
    return AuthoritySlotPublication::select(
        scopes.domain, readSlot(scopes.slotAScope), readSlot(scopes.slotBScope),
        readSlot(scopes.legacyScope));
}

} // namespace

SecureStorageAuthoritySlotReadState SecureStorageAuthoritySlotAdapter::readFresh(
    const SecureStorageAuthoritySlotScopes &scopes,
    QByteArray *value, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    // 未配置的作用域被拒绝，而不是退回某个默认位置：那会让两个子系统共用一处授权。
    if (!scopes.isValid()) {
        fail(errorCode,
             QStringLiteral("secure-authority-slot-scopes-unconfigured"));
        return SecureStorageAuthoritySlotReadState::Invalid;
    }
    if (!value) {
        fail(errorCode, code(scopes, "target-invalid"));
        return SecureStorageAuthoritySlotReadState::Invalid;
    }
    value->clear();
    const AuthoritySlotSelection selection = currentSelection(scopes);
    switch (selection.state) {
    case AuthoritySlotSelectionState::Missing:
        return SecureStorageAuthoritySlotReadState::Missing;
    case AuthoritySlotSelectionState::Unavailable:
        fail(errorCode, selection.errorCode);
        return SecureStorageAuthoritySlotReadState::Unavailable;
    case AuthoritySlotSelectionState::Invalid:
        fail(errorCode, selection.errorCode);
        return SecureStorageAuthoritySlotReadState::Invalid;
    case AuthoritySlotSelectionState::Found:
        *value = selection.payload;
        return SecureStorageAuthoritySlotReadState::Found;
    }
    fail(errorCode, code(scopes, "state-invalid"));
    return SecureStorageAuthoritySlotReadState::Invalid;
}

SecureStorageAuthoritySlotWriteOutcome SecureStorageAuthoritySlotAdapter::write(
    const SecureStorageAuthoritySlotScopes &scopes,
    const QByteArray &value, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    if (!scopes.isValid()) {
        fail(errorCode,
             QStringLiteral("secure-authority-slot-scopes-unconfigured"));
        return SecureStorageAuthoritySlotWriteOutcome::DefiniteFailure;
    }
    if (value.isEmpty() || value.size() > MaxAuthorityBytes) {
        fail(errorCode, code(scopes, "write-invalid"));
        return SecureStorageAuthoritySlotWriteOutcome::DefiniteFailure;
    }
    const AuthoritySlotSelection selection = currentSelection(scopes);
    if (selection.state == AuthoritySlotSelectionState::Unavailable
            || selection.state == AuthoritySlotSelectionState::Invalid) {
        // 当前授权读不出来时不能发布：那会用一个可能更旧的代号覆盖对端。
        fail(errorCode, selection.errorCode);
        return SecureStorageAuthoritySlotWriteOutcome::DefiniteFailure;
    }
    const QByteArray framed = AuthoritySlotPublication::frame(
        scopes.domain, selection.writeGeneration, value);
    QString decoded;
    if (framed.isEmpty() || !strictUtf8(framed, &decoded)) {
        fail(errorCode, code(scopes, "write-invalid"));
        return SecureStorageAuthoritySlotWriteOutcome::DefiniteFailure;
    }
    // 只写入持有较旧代号的槽位；当前选中的代号在对端保持完好，因此一次被打断的
    // 写入只表现为"这次发布没有生效"。
    if (!SecureStorage::saveEncrypted(slotScope(scopes, selection.writeSlot),
                                      decoded)) {
        fail(errorCode, code(scopes, "write-outcome-unknown"));
        return SecureStorageAuthoritySlotWriteOutcome::OutcomeUnknown;
    }
    if (selection.legacyPending && !scopes.legacyScope.isEmpty()) {
        // 新槽位已确认后才移除迁移来源；失败只是遗留清理，不影响授权。
        SecureStorage::remove(scopes.legacyScope);
    }
    return SecureStorageAuthoritySlotWriteOutcome::Committed;
}
