#include "companion_activation_journal_secure_storage_adapter.h"

#include "companion_activation_authority_slots.h"
#include "secure_storage.h"

namespace {

const char kAuthorityScope[] = "companion/activation-journal-authority/v1";
const char kSlotAScope[] = "companion/activation-journal-authority/slot-a/v1";
const char kSlotBScope[] = "companion/activation-journal-authority/slot-b/v1";
constexpr int kMaximumAuthorityBytes = 32 * 1024;

void fail(QString *errorCode, const QString &code)
{
    if (errorCode) *errorCode = code;
}

bool strictUtf8(const QString &value, QByteArray *bytes)
{
    if (!bytes) return false;
    *bytes = value.toUtf8();
    return !bytes->isEmpty() && bytes->size() <= kMaximumAuthorityBytes
        && !bytes->contains('\0')
        && QString::fromUtf8(bytes->constData(), bytes->size()) == value;
}

bool strictUtf8(const QByteArray &bytes, QString *value)
{
    if (!value || bytes.isEmpty() || bytes.size() > kMaximumAuthorityBytes
            || bytes.contains('\0')) {
        return false;
    }
    const QString decoded = QString::fromUtf8(bytes.constData(), bytes.size());
    if (decoded.toUtf8() != bytes) return false;
    *value = decoded;
    return true;
}

// 绕过进程缓存：后端被锁定时不能被误读成首次安装。
AuthoritySlotInput readSlot(const QString &scope)
{
    AuthoritySlotInput input;
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

QString slotScope(AuthoritySlotName slot)
{
    return slot == AuthoritySlotName::SlotA
        ? QString::fromLatin1(kSlotAScope) : QString::fromLatin1(kSlotBScope);
}

AuthoritySlotSelection currentSelection()
{
    return CompanionActivationAuthoritySlots::select(
        readSlot(QString::fromLatin1(kSlotAScope)),
        readSlot(QString::fromLatin1(kSlotBScope)),
        readSlot(QString::fromLatin1(kAuthorityScope)));
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

CompanionActivationJournalSecureStore::ReadState
SecureStorageCompanionActivationJournalAdapter::readFresh(
    QByteArray *value, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    if (!value) {
        fail(errorCode, QStringLiteral("activation-journal-secure-target-invalid"));
        return ReadState::Invalid;
    }
    value->clear();
    const AuthoritySlotSelection selection = currentSelection();
    switch (selection.state) {
    case AuthoritySlotSelectionState::Missing:
        return ReadState::Missing;
    case AuthoritySlotSelectionState::Unavailable:
        fail(errorCode, selection.errorCode);
        return ReadState::Unavailable;
    case AuthoritySlotSelectionState::Invalid:
        fail(errorCode, selection.errorCode);
        return ReadState::Invalid;
    case AuthoritySlotSelectionState::Found:
        *value = selection.payload;
        return ReadState::Found;
    }
    fail(errorCode, QStringLiteral("activation-journal-secure-state-invalid"));
    return ReadState::Invalid;
}

CompanionActivationJournalSecureStore::WriteOutcome
SecureStorageCompanionActivationJournalAdapter::write(
    const QByteArray &value, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    if (value.isEmpty() || value.size() > kMaximumAuthorityBytes) {
        fail(errorCode, QStringLiteral("activation-journal-secure-write-invalid"));
        return WriteOutcome::DefiniteFailure;
    }
    const AuthoritySlotSelection selection = currentSelection();
    if (selection.state == AuthoritySlotSelectionState::Unavailable) {
        fail(errorCode, selection.errorCode);
        return WriteOutcome::DefiniteFailure;
    }
    if (selection.state == AuthoritySlotSelectionState::Invalid) {
        fail(errorCode, selection.errorCode);
        return WriteOutcome::DefiniteFailure;
    }
    const QByteArray framed = CompanionActivationAuthoritySlots::frame(
        selection.writeGeneration, value);
    QString decoded;
    if (framed.isEmpty() || !strictUtf8(framed, &decoded)) {
        fail(errorCode, QStringLiteral("activation-journal-secure-write-invalid"));
        return WriteOutcome::DefiniteFailure;
    }
    // 只写入持有较旧代号的槽位；当前选中的代号在对端保持完好，因此一次被打断
    // 的写入只表现为"这次发布没有生效"。
    if (!SecureStorage::saveEncrypted(slotScope(selection.writeSlot), decoded)) {
        fail(errorCode,
             QStringLiteral("activation-journal-secure-write-outcome-unknown"));
        return WriteOutcome::OutcomeUnknown;
    }
    if (selection.legacyPending) {
        // 新槽位已确认后才移除迁移来源；失败只是遗留清理，不影响授权。
        SecureStorage::remove(QString::fromLatin1(kAuthorityScope));
    }
    return WriteOutcome::Committed;
}
