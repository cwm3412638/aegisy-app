#include "extension_enablement_ledger_store.h"

#include "extension_evidence_ledger_store.h"

namespace {

// 启用授权自己的持久化域。授权模式串、载荷域与 QSettings 键全部与复核证据不同，因此
// 一份复核授权信封或载荷都无法被当作启用授权采用。
const char kMacDomain[] = "aegisy-extension-enablement-ledger-hmac/0.1\0";
const char kIdentityDomain[] = "aegisy-extension-enablement-ledger-identity/0.1\0";
const QString kRecordKey =
    QStringLiteral("extensions/enablement-ledger/record");

ExtensionEvidenceLedgerStoreDomain domain()
{
    ExtensionEvidenceLedgerStoreDomain value;
    value.ledger.schema = QStringLiteral("aegisy-extension-enablement-ledger/0.1");
    value.ledger.macDomain = QByteArray(kMacDomain, sizeof(kMacDomain) - 1);
    value.ledger.identityDomain =
        QByteArray(kIdentityDomain, sizeof(kIdentityDomain) - 1);
    value.ledger.entriesKey = QStringLiteral("grants");
    value.ledger.entryCodeNoun = QStringLiteral("grant");
    value.ledger.identityPrefix =
        QStringLiteral("extension-enablement-ledger:sha256:");
    value.ledger.errorPrefix = QStringLiteral("extension-enablement-ledger");
    value.authoritySchema =
        QStringLiteral("aegisy-extension-enablement-ledger-authority/0.1");
    value.recordKey = kRecordKey;
    value.errorPrefix = QStringLiteral("extension-enablement-store");
    value.entriesCodeNoun = QStringLiteral("grants");
    return value;
}

ExtensionEvidenceEntry toEntry(const ExtensionEnablementGrant &grant)
{
    ExtensionEvidenceEntry entry;
    entry.kind = grant.kind;
    entry.id = grant.id;
    entry.sourceIdentity = grant.sourceIdentity;
    entry.contentIdentity = grant.contentIdentity;
    return entry;
}

ExtensionEnablementGrant toGrant(const ExtensionEvidenceEntry &entry)
{
    ExtensionEnablementGrant grant;
    grant.kind = entry.kind;
    grant.id = entry.id;
    grant.sourceIdentity = entry.sourceIdentity;
    grant.contentIdentity = entry.contentIdentity;
    return grant;
}

ExtensionEnablementLedgerStoreState toState(
    ExtensionEvidenceLedgerStoreState state)
{
    switch (state) {
    case ExtensionEvidenceLedgerStoreState::Empty:
        return ExtensionEnablementLedgerStoreState::Empty;
    case ExtensionEvidenceLedgerStoreState::Ready:
        return ExtensionEnablementLedgerStoreState::Ready;
    case ExtensionEvidenceLedgerStoreState::Unavailable:
        return ExtensionEnablementLedgerStoreState::Unavailable;
    case ExtensionEvidenceLedgerStoreState::OutcomeUnknown:
        return ExtensionEnablementLedgerStoreState::OutcomeUnknown;
    case ExtensionEvidenceLedgerStoreState::Invalid:
        break;
    }
    return ExtensionEnablementLedgerStoreState::Invalid;
}

ExtensionEnablementLedgerStoreResult toResult(
    const ExtensionEvidenceLedgerStoreResult &source)
{
    ExtensionEnablementLedgerStoreResult result;
    result.state = toState(source.state);
    result.generation = source.generation;
    result.identity = source.identity;
    result.errorCode = source.errorCode;
    result.grants.reserve(source.entries.size());
    for (const ExtensionEvidenceEntry &entry : source.entries) {
        result.grants.append(toGrant(entry));
    }
    return result;
}

// 适配器把注入的安全存储原样转交给共享层：它只搬字节，不改变任何判定。
class SecureStoreAdapter : public ExtensionEvidenceLedgerSecureStore
{
public:
    explicit SecureStoreAdapter(ExtensionEnablementLedgerSecureStore *target)
        : m_target(target)
    {
    }

    ReadState readFresh(QByteArray *value, QString *errorCode) override
    {
        switch (m_target->readFresh(value, errorCode)) {
        case ExtensionEnablementLedgerSecureStore::ReadState::Missing:
            return ReadState::Missing;
        case ExtensionEnablementLedgerSecureStore::ReadState::Found:
            return ReadState::Found;
        case ExtensionEnablementLedgerSecureStore::ReadState::Unavailable:
            return ReadState::Unavailable;
        case ExtensionEnablementLedgerSecureStore::ReadState::Invalid:
            break;
        }
        return ReadState::Invalid;
    }

    WriteOutcome write(const QByteArray &value, QString *errorCode) override
    {
        switch (m_target->write(value, errorCode)) {
        case ExtensionEnablementLedgerSecureStore::WriteOutcome::Committed:
            return WriteOutcome::Committed;
        case ExtensionEnablementLedgerSecureStore::WriteOutcome::OutcomeUnknown:
            return WriteOutcome::OutcomeUnknown;
        case ExtensionEnablementLedgerSecureStore::WriteOutcome::DefiniteFailure:
            break;
        }
        return WriteOutcome::DefiniteFailure;
    }

private:
    ExtensionEnablementLedgerSecureStore *m_target = nullptr;
};

} // namespace

ExtensionEnablementLedgerStore::ExtensionEnablementLedgerStore(
    ExtensionEnablementLedgerSecureStore *secureStore, QSettings *settings)
    : m_secureStore(secureStore)
    , m_settings(settings)
{
}

QString ExtensionEnablementLedgerStore::recordSettingsKey()
{
    return kRecordKey;
}

ExtensionEnablementLedgerStoreResult ExtensionEnablementLedgerStore::load()
{
    if (!m_secureStore) {
        ExtensionEnablementLedgerStoreResult result;
        result.state = ExtensionEnablementLedgerStoreState::Unavailable;
        result.errorCode =
            QStringLiteral("extension-enablement-store-unavailable");
        return result;
    }
    SecureStoreAdapter adapter(m_secureStore);
    ExtensionEvidenceLedgerStore store(domain(), &adapter, m_settings);
    return toResult(store.load());
}

bool ExtensionEnablementLedgerStore::discard(
    ExtensionEnablementLedgerStoreResult *updated, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    if (updated) *updated = ExtensionEnablementLedgerStoreResult{};
    if (!m_secureStore) {
        // 没有后端就是读不出来。这里绝不能退化成"从未授权过"：那会把一次读取失败表述成
        // 用户从未要求启用过任何东西。
        if (errorCode) {
            *errorCode = QStringLiteral("extension-enablement-store-unavailable");
        }
        return false;
    }
    SecureStoreAdapter adapter(m_secureStore);
    ExtensionEvidenceLedgerStore store(domain(), &adapter, m_settings);
    ExtensionEvidenceLedgerStoreResult shared;
    const bool discarded = store.discard(&shared, errorCode);
    if (updated) *updated = toResult(shared);
    return discarded;
}

bool ExtensionEnablementLedgerStore::replace(
    const QList<ExtensionEnablementGrant> &grants, qint64 expectedGeneration,
    ExtensionEnablementLedgerStoreResult *updated, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    if (updated) *updated = ExtensionEnablementLedgerStoreResult{};
    if (!m_secureStore) {
        if (errorCode) {
            *errorCode =
                QStringLiteral("extension-enablement-store-unavailable");
        }
        return false;
    }
    QList<ExtensionEvidenceEntry> entries;
    entries.reserve(grants.size());
    for (const ExtensionEnablementGrant &grant : grants) {
        entries.append(toEntry(grant));
    }

    SecureStoreAdapter adapter(m_secureStore);
    ExtensionEvidenceLedgerStore store(domain(), &adapter, m_settings);
    ExtensionEvidenceLedgerStoreResult result;
    const bool committed =
        store.replace(entries, expectedGeneration, &result, errorCode);
    if (updated) *updated = toResult(result);
    return committed;
}
