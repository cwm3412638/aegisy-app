#include "extension_review_ledger_store.h"

#include "extension_evidence_ledger_store.h"

namespace {

// 复核证据自己的持久化域。授权模式串与 QSettings 键都进入被持久化的字节与位置，
// 因此必须与抽取之前完全一致，否则现有安装会读不出自己的复核记录。
const char kMacDomain[] = "aegisy-extension-review-ledger-hmac/0.1\0";
const char kIdentityDomain[] = "aegisy-extension-review-ledger-identity/0.1\0";
const QString kRecordKey = QStringLiteral("extensions/review-ledger/record");

ExtensionEvidenceLedgerStoreDomain domain()
{
    ExtensionEvidenceLedgerStoreDomain value;
    value.ledger.schema = QStringLiteral("aegisy-extension-review-ledger/0.1");
    value.ledger.macDomain = QByteArray(kMacDomain, sizeof(kMacDomain) - 1);
    value.ledger.identityDomain =
        QByteArray(kIdentityDomain, sizeof(kIdentityDomain) - 1);
    value.ledger.entriesKey = QStringLiteral("pins");
    value.ledger.entryCodeNoun = QStringLiteral("pin");
    value.ledger.identityPrefix =
        QStringLiteral("extension-review-ledger:sha256:");
    value.ledger.errorPrefix = QStringLiteral("extension-review-ledger");
    value.authoritySchema =
        QStringLiteral("aegisy-extension-review-ledger-authority/0.1");
    value.recordKey = kRecordKey;
    value.errorPrefix = QStringLiteral("extension-review-store");
    value.entriesCodeNoun = QStringLiteral("pins");
    return value;
}

ExtensionEvidenceEntry toEntry(const ExtensionReviewPin &pin)
{
    ExtensionEvidenceEntry entry;
    entry.kind = pin.kind;
    entry.id = pin.id;
    entry.sourceIdentity = pin.sourceIdentity;
    entry.contentIdentity = pin.contentIdentity;
    return entry;
}

ExtensionReviewPin toPin(const ExtensionEvidenceEntry &entry)
{
    ExtensionReviewPin pin;
    pin.kind = entry.kind;
    pin.id = entry.id;
    pin.sourceIdentity = entry.sourceIdentity;
    pin.contentIdentity = entry.contentIdentity;
    return pin;
}

ExtensionReviewLedgerStoreState toState(ExtensionEvidenceLedgerStoreState state)
{
    switch (state) {
    case ExtensionEvidenceLedgerStoreState::Empty:
        return ExtensionReviewLedgerStoreState::Empty;
    case ExtensionEvidenceLedgerStoreState::Ready:
        return ExtensionReviewLedgerStoreState::Ready;
    case ExtensionEvidenceLedgerStoreState::Unavailable:
        return ExtensionReviewLedgerStoreState::Unavailable;
    case ExtensionEvidenceLedgerStoreState::OutcomeUnknown:
        return ExtensionReviewLedgerStoreState::OutcomeUnknown;
    case ExtensionEvidenceLedgerStoreState::Invalid:
        break;
    }
    return ExtensionReviewLedgerStoreState::Invalid;
}

ExtensionReviewLedgerStoreResult toResult(
    const ExtensionEvidenceLedgerStoreResult &source)
{
    ExtensionReviewLedgerStoreResult result;
    result.state = toState(source.state);
    result.generation = source.generation;
    result.identity = source.identity;
    result.errorCode = source.errorCode;
    result.pins.reserve(source.entries.size());
    for (const ExtensionEvidenceEntry &entry : source.entries) {
        result.pins.append(toPin(entry));
    }
    return result;
}

// 适配器把注入的复核安全存储原样转交给共享层：它只搬字节，不改变任何判定。
class SecureStoreAdapter : public ExtensionEvidenceLedgerSecureStore
{
public:
    explicit SecureStoreAdapter(ExtensionReviewLedgerSecureStore *target)
        : m_target(target)
    {
    }

    ReadState readFresh(QByteArray *value, QString *errorCode) override
    {
        switch (m_target->readFresh(value, errorCode)) {
        case ExtensionReviewLedgerSecureStore::ReadState::Missing:
            return ReadState::Missing;
        case ExtensionReviewLedgerSecureStore::ReadState::Found:
            return ReadState::Found;
        case ExtensionReviewLedgerSecureStore::ReadState::Unavailable:
            return ReadState::Unavailable;
        case ExtensionReviewLedgerSecureStore::ReadState::Invalid:
            break;
        }
        return ReadState::Invalid;
    }

    WriteOutcome write(const QByteArray &value, QString *errorCode) override
    {
        switch (m_target->write(value, errorCode)) {
        case ExtensionReviewLedgerSecureStore::WriteOutcome::Committed:
            return WriteOutcome::Committed;
        case ExtensionReviewLedgerSecureStore::WriteOutcome::OutcomeUnknown:
            return WriteOutcome::OutcomeUnknown;
        case ExtensionReviewLedgerSecureStore::WriteOutcome::DefiniteFailure:
            break;
        }
        return WriteOutcome::DefiniteFailure;
    }

private:
    ExtensionReviewLedgerSecureStore *m_target = nullptr;
};

} // namespace

ExtensionReviewLedgerStore::ExtensionReviewLedgerStore(
    ExtensionReviewLedgerSecureStore *secureStore, QSettings *settings)
    : m_secureStore(secureStore)
    , m_settings(settings)
{
}

QString ExtensionReviewLedgerStore::recordSettingsKey()
{
    return kRecordKey;
}

ExtensionReviewLedgerStoreResult ExtensionReviewLedgerStore::load()
{
    if (!m_secureStore) {
        ExtensionReviewLedgerStoreResult result;
        result.state = ExtensionReviewLedgerStoreState::Unavailable;
        result.errorCode = QStringLiteral("extension-review-store-unavailable");
        return result;
    }
    SecureStoreAdapter adapter(m_secureStore);
    ExtensionEvidenceLedgerStore store(domain(), &adapter, m_settings);
    return toResult(store.load());
}

bool ExtensionReviewLedgerStore::replace(
    const QList<ExtensionReviewPin> &pins, qint64 expectedGeneration,
    ExtensionReviewLedgerStoreResult *updated, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    if (updated) *updated = ExtensionReviewLedgerStoreResult{};
    if (!m_secureStore) {
        if (errorCode) {
            *errorCode = QStringLiteral("extension-review-store-unavailable");
        }
        return false;
    }
    QList<ExtensionEvidenceEntry> entries;
    entries.reserve(pins.size());
    for (const ExtensionReviewPin &pin : pins) entries.append(toEntry(pin));

    SecureStoreAdapter adapter(m_secureStore);
    ExtensionEvidenceLedgerStore store(domain(), &adapter, m_settings);
    ExtensionEvidenceLedgerStoreResult result;
    const bool committed =
        store.replace(entries, expectedGeneration, &result, errorCode);
    if (updated) *updated = toResult(result);
    return committed;
}
