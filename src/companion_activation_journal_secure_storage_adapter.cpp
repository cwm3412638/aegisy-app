#include "companion_activation_journal_secure_storage_adapter.h"

#include "secure_storage.h"

namespace {

const char kAuthorityScope[] = "companion/activation-journal-authority/v1";
constexpr int kMaximumAuthorityBytes = 16 * 1024;

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

} // namespace

QString SecureStorageCompanionActivationJournalAdapter::authorityScope()
{
    return QString::fromLatin1(kAuthorityScope);
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
    // 绕过进程缓存：后端被锁定时不能被误读成首次安装。
    const SecureStorageReadResult result =
        SecureStorage::loadEncryptedFresh(authorityScope());
    switch (result.state) {
    case SecureStorageReadState::Missing:
        fail(errorCode, result.errorCode);
        return ReadState::Missing;
    case SecureStorageReadState::Unavailable:
        fail(errorCode, result.errorCode);
        return ReadState::Unavailable;
    case SecureStorageReadState::Invalid:
        fail(errorCode, result.errorCode);
        return ReadState::Invalid;
    case SecureStorageReadState::Found:
        if (!strictUtf8(result.value, value)) {
            fail(errorCode,
                 QStringLiteral("activation-journal-secure-value-invalid"));
            return ReadState::Invalid;
        }
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
    QString decoded;
    if (!strictUtf8(value, &decoded)) {
        fail(errorCode, QStringLiteral("activation-journal-secure-write-invalid"));
        return WriteOutcome::DefiniteFailure;
    }
    // 保存失败可能已经落盘：只能报告"结果未知",由重读来分类。
    if (!SecureStorage::saveEncrypted(authorityScope(), decoded)) {
        fail(errorCode,
             QStringLiteral("activation-journal-secure-write-outcome-unknown"));
        return WriteOutcome::OutcomeUnknown;
    }
    return WriteOutcome::Committed;
}
