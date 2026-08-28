#include "extension_review_ledger.h"

#include "extension_evidence_ledger.h"

namespace {

// 复核证据自己的域常量。这些字节进入被持久化的载荷与 MAC 预映像，因此必须与抽取
// 之前完全一致，否则现有安装会读不出自己的复核记录。
const char kMacDomain[] = "aegisy-extension-review-ledger-hmac/0.1\0";
const char kIdentityDomain[] = "aegisy-extension-review-ledger-identity/0.1\0";

ExtensionEvidenceLedgerDomain domain()
{
    ExtensionEvidenceLedgerDomain value;
    value.schema = QStringLiteral("aegisy-extension-review-ledger/0.1");
    value.macDomain = QByteArray(kMacDomain, sizeof(kMacDomain) - 1);
    value.identityDomain = QByteArray(kIdentityDomain, sizeof(kIdentityDomain) - 1);
    value.entriesKey = QStringLiteral("pins");
    value.entryCodeNoun = QStringLiteral("pin");
    value.identityPrefix = QStringLiteral("extension-review-ledger:sha256:");
    value.errorPrefix = QStringLiteral("extension-review-ledger");
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

ExtensionReviewLedgerState toState(ExtensionEvidenceLedgerState state)
{
    switch (state) {
    case ExtensionEvidenceLedgerState::Empty:
        return ExtensionReviewLedgerState::Empty;
    case ExtensionEvidenceLedgerState::Ready:
        return ExtensionReviewLedgerState::Ready;
    case ExtensionEvidenceLedgerState::Unavailable:
        return ExtensionReviewLedgerState::Unavailable;
    case ExtensionEvidenceLedgerState::Invalid:
        break;
    }
    return ExtensionReviewLedgerState::Invalid;
}

} // namespace

QByteArray ExtensionReviewLedger::serialize(qint64 generation,
                                            const QList<ExtensionReviewPin> &pins,
                                            const QByteArray &key)
{
    QList<ExtensionEvidenceEntry> entries;
    entries.reserve(pins.size());
    for (const ExtensionReviewPin &pin : pins) entries.append(toEntry(pin));
    return ExtensionEvidenceLedger::serialize(domain(), generation, entries, key);
}

ExtensionReviewLedgerResult ExtensionReviewLedger::parse(const QByteArray &bytes,
                                                        const QByteArray &key)
{
    const ExtensionEvidenceLedgerResult source =
        ExtensionEvidenceLedger::parse(domain(), bytes, key);
    ExtensionReviewLedgerResult result;
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
