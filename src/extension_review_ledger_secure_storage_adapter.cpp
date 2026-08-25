#include "extension_review_ledger_secure_storage_adapter.h"

#include "authority_slot_publication.h"
#include "secure_storage.h"

namespace {

const char kSlotAScope[] = "extensions/review-ledger-authority/slot-a/v1";
const char kSlotBScope[] = "extensions/review-ledger-authority/slot-b/v1";
// 这些常量参与已持久化的字节，因此不能更改。
const char kFrameSchema[] = "aegisy-extension-review-ledger-authority-slot/0.1";
const char kDigestDomain[] =
    "aegisy-extension-review-ledger-authority-slot-digest/0.1\0";
constexpr int kMaximumAuthorityBytes = 32 * 1024;

AuthoritySlotDomain domain()
{
    AuthoritySlotDomain value;
    value.frameSchema = QByteArray(kFrameSchema, sizeof(kFrameSchema) - 1);
    value.digestDomain = QByteArray(kDigestDomain, sizeof(kDigestDomain) - 1);
    value.errorPrefix = QStringLiteral("extension-review-authority-slot-");
    return value;
}

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

// 绕过进程缓存：后端被锁定时不能被误读成"从未复核过"。
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
    // 复核记录是新增子系统，没有迁移前的单槽授权可以采纳。
    return AuthoritySlotPublication::select(
        domain(), readSlot(QString::fromLatin1(kSlotAScope)),
        readSlot(QString::fromLatin1(kSlotBScope)), AuthoritySlotInput{});
}

} // namespace

QString SecureStorageExtensionReviewLedgerAdapter::authoritySlotAScope()
{
    return QString::fromLatin1(kSlotAScope);
}

QString SecureStorageExtensionReviewLedgerAdapter::authoritySlotBScope()
{
    return QString::fromLatin1(kSlotBScope);
}

ExtensionReviewLedgerSecureStore::ReadState
SecureStorageExtensionReviewLedgerAdapter::readFresh(
    QByteArray *value, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    if (!value) {
        fail(errorCode, QStringLiteral("extension-review-secure-target-invalid"));
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
    fail(errorCode, QStringLiteral("extension-review-secure-state-invalid"));
    return ReadState::Invalid;
}

ExtensionReviewLedgerSecureStore::WriteOutcome
SecureStorageExtensionReviewLedgerAdapter::write(
    const QByteArray &value, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    if (value.isEmpty() || value.size() > kMaximumAuthorityBytes) {
        fail(errorCode, QStringLiteral("extension-review-secure-write-invalid"));
        return WriteOutcome::DefiniteFailure;
    }
    const AuthoritySlotSelection selection = currentSelection();
    if (selection.state == AuthoritySlotSelectionState::Unavailable
            || selection.state == AuthoritySlotSelectionState::Invalid) {
        // 当前授权读不出来时不能发布：那会用一个可能更旧的代号覆盖对端。
        fail(errorCode, selection.errorCode);
        return WriteOutcome::DefiniteFailure;
    }
    const QByteArray framed = AuthoritySlotPublication::frame(
        domain(), selection.writeGeneration, value);
    QString decoded;
    if (framed.isEmpty() || !strictUtf8(framed, &decoded)) {
        fail(errorCode, QStringLiteral("extension-review-secure-write-invalid"));
        return WriteOutcome::DefiniteFailure;
    }
    // 只写入持有较旧代号的槽位；当前选中的代号在对端保持完好，因此一次被打断
    // 的写入只表现为"这次发布没有生效"。
    if (!SecureStorage::saveEncrypted(slotScope(selection.writeSlot), decoded)) {
        fail(errorCode,
             QStringLiteral("extension-review-secure-write-outcome-unknown"));
        return WriteOutcome::OutcomeUnknown;
    }
    return WriteOutcome::Committed;
}
