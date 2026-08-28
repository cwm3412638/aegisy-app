#include "extension_enablement_ledger.h"

#include "extension_evidence_ledger.h"

namespace {

// 启用授权自己的域常量。它们与复核证据的域完全不同，因此一份复核记录的字节无法被
// 移动到启用授权的位置——那等于把"我看过这份内容"变成"我要求运行这份内容"。
const char kMacDomain[] = "aegisy-extension-enablement-ledger-hmac/0.1\0";
const char kIdentityDomain[] = "aegisy-extension-enablement-ledger-identity/0.1\0";

ExtensionEvidenceLedgerDomain domain()
{
    ExtensionEvidenceLedgerDomain value;
    value.schema = QStringLiteral("aegisy-extension-enablement-ledger/0.1");
    value.macDomain = QByteArray(kMacDomain, sizeof(kMacDomain) - 1);
    value.identityDomain = QByteArray(kIdentityDomain, sizeof(kIdentityDomain) - 1);
    value.entriesKey = QStringLiteral("grants");
    value.entryCodeNoun = QStringLiteral("grant");
    value.identityPrefix = QStringLiteral("extension-enablement-ledger:sha256:");
    value.errorPrefix = QStringLiteral("extension-enablement-ledger");
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

ExtensionEnablementLedgerState toState(ExtensionEvidenceLedgerState state)
{
    switch (state) {
    case ExtensionEvidenceLedgerState::Empty:
        return ExtensionEnablementLedgerState::Empty;
    case ExtensionEvidenceLedgerState::Ready:
        return ExtensionEnablementLedgerState::Ready;
    case ExtensionEvidenceLedgerState::Unavailable:
        return ExtensionEnablementLedgerState::Unavailable;
    case ExtensionEvidenceLedgerState::Invalid:
        break;
    }
    return ExtensionEnablementLedgerState::Invalid;
}

} // namespace

QByteArray ExtensionEnablementLedger::serialize(
    qint64 generation,
    const QList<ExtensionEnablementGrant> &grants,
    const QByteArray &key)
{
    QList<ExtensionEvidenceEntry> entries;
    entries.reserve(grants.size());
    for (const ExtensionEnablementGrant &grant : grants) {
        entries.append(toEntry(grant));
    }
    return ExtensionEvidenceLedger::serialize(domain(), generation, entries, key);
}

ExtensionEnablementLedgerResult ExtensionEnablementLedger::parse(
    const QByteArray &bytes, const QByteArray &key)
{
    const ExtensionEvidenceLedgerResult source =
        ExtensionEvidenceLedger::parse(domain(), bytes, key);
    ExtensionEnablementLedgerResult result;
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
