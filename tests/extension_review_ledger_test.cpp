#include "extension_review_ledger.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

namespace {

int failures = 0;

bool expect(bool condition, const char *message)
{
    if (!condition) {
        QTextStream(stderr) << "FAIL: " << message << '\n';
        ++failures;
    }
    return condition;
}

QByteArray keyOf(char filler)
{
    return QByteArray(32, filler);
}

QString digest(const QString &prefix, const QByteArray &seed)
{
    return prefix + QString::fromLatin1(
        QCryptographicHash::hash(seed, QCryptographicHash::Sha256).toHex());
}

ExtensionReviewPin pin(ExtensionKind kind, const QString &id,
                       const QByteArray &seed)
{
    ExtensionReviewPin value;
    value.kind = kind;
    value.id = id;
    value.sourceIdentity =
        digest(QStringLiteral("extension-source:sha256:"), seed + "-source");
    value.contentIdentity =
        digest(QStringLiteral("extension-content:sha256:"), seed + "-content");
    return value;
}

bool invalid(const ExtensionReviewLedgerResult &result, const QString &code)
{
    // 反降级：被篡改的载荷永远得出 Invalid，绝不退化成 Empty。
    return result.state == ExtensionReviewLedgerState::Invalid
        && result.errorCode == code && result.pins.isEmpty()
        && result.generation == 0 && result.identity.isEmpty();
}

QByteArray withField(const QByteArray &bytes, const QString &field,
                     const QJsonValue &value)
{
    QJsonObject object = QJsonDocument::fromJson(bytes).object();
    if (value.isUndefined()) {
        object.remove(field);
    } else {
        object.insert(field, value);
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QList<ExtensionReviewPin> samplePins()
{
    return {pin(ExtensionKind::Skill, QStringLiteral("fixture.skill"), "a"),
            pin(ExtensionKind::Mcp, QStringLiteral("fixture.mcp"), "b"),
            pin(ExtensionKind::CodexPlugin, QStringLiteral("fixture.plugin"), "c")};
}

void roundTripTests()
{
    const QByteArray key = keyOf('k');
    const QList<ExtensionReviewPin> pins = samplePins();
    const QByteArray bytes = ExtensionReviewLedger::serialize(7, pins, key);
    if (!expect(!bytes.isEmpty(), "a valid review ledger did not serialize")) return;

    const ExtensionReviewLedgerResult parsed =
        ExtensionReviewLedger::parse(bytes, key);
    expect(parsed.state == ExtensionReviewLedgerState::Ready
               && parsed.generation == 7 && parsed.pins.size() == 3
               && parsed.errorCode.isEmpty()
               && parsed.identity.startsWith(
                   QStringLiteral("extension-review-ledger:sha256:")),
           "a valid review ledger did not round trip");
    bool preserved = parsed.pins.size() == 3;
    for (int index = 0; index < parsed.pins.size() && preserved; ++index) {
        preserved = parsed.pins.at(index).kind == pins.at(index).kind
            && parsed.pins.at(index).id == pins.at(index).id
            && parsed.pins.at(index).sourceIdentity == pins.at(index).sourceIdentity
            && parsed.pins.at(index).contentIdentity
                == pins.at(index).contentIdentity;
    }
    expect(preserved, "review pin fields did not survive the round trip");

    // 序列化必须是确定的，否则同一组复核会产生不同的身份摘要。
    expect(ExtensionReviewLedger::serialize(7, pins, key) == bytes,
           "review ledger serialization is not deterministic");

    // 身份摘要必须绑定代号与集合内容。
    const ExtensionReviewLedgerResult laterGeneration =
        ExtensionReviewLedger::parse(
            ExtensionReviewLedger::serialize(8, pins, key), key);
    expect(laterGeneration.state == ExtensionReviewLedgerState::Ready
               && laterGeneration.identity != parsed.identity,
           "the ledger identity does not bind the generation");
    QList<ExtensionReviewPin> reordered{pins.at(1), pins.at(0), pins.at(2)};
    const ExtensionReviewLedgerResult swapped = ExtensionReviewLedger::parse(
        ExtensionReviewLedger::serialize(7, reordered, key), key);
    expect(swapped.state == ExtensionReviewLedgerState::Ready
               && swapped.identity != parsed.identity,
           "the ledger identity does not bind pin ordering");

    // 空集合是一个合法的已认证状态："复核过，但目前没有任何扩展被信任"。
    const QByteArray emptySet = ExtensionReviewLedger::serialize(1, {}, key);
    const ExtensionReviewLedgerResult emptyParsed =
        ExtensionReviewLedger::parse(emptySet, key);
    expect(!emptySet.isEmpty()
               && emptyParsed.state == ExtensionReviewLedgerState::Ready
               && emptyParsed.pins.isEmpty() && emptyParsed.generation == 1,
           "an authenticated empty review set was not accepted");
    // 但它必须与"从未复核过"区分开：前者有身份摘要与代号，后者两者皆无。
    const ExtensionReviewLedgerResult absent = ExtensionReviewLedger::parse({}, key);
    expect(!emptyParsed.identity.isEmpty() && absent.identity.isEmpty()
               && absent.generation == 0
               && absent.state != emptyParsed.state,
           "an empty review set is indistinguishable from no ledger");
}

void emptyAndKeyTests()
{
    const QByteArray key = keyOf('k');
    const ExtensionReviewLedgerResult absent =
        ExtensionReviewLedger::parse({}, key);
    expect(absent.state == ExtensionReviewLedgerState::Empty
               && absent.pins.isEmpty() && absent.errorCode.isEmpty(),
           "an absent ledger was not reported empty");

    // 密钥不可用时当前内容未知，这不是"没有复核"，也不是"载荷损坏"。
    const QByteArray bytes =
        ExtensionReviewLedger::serialize(1, samplePins(), key);
    const ExtensionReviewLedgerResult noKey =
        ExtensionReviewLedger::parse(bytes, QByteArray(16, 'k'));
    expect(noKey.state == ExtensionReviewLedgerState::Unavailable
               && noKey.errorCode
                   == QStringLiteral("extension-review-ledger-key-unavailable")
               && noKey.pins.isEmpty(),
           "an unusable key did not resolve to unavailable");
    // 密钥缺失时空载荷仍然是 Empty：确实没有东西可以认证。
    expect(ExtensionReviewLedger::parse({}, QByteArray()).state
               == ExtensionReviewLedgerState::Empty,
           "an absent ledger changed meaning without a key");

    // 错误的密钥不能得出"没有复核"，必须是 MAC 不匹配。
    expect(invalid(ExtensionReviewLedger::parse(bytes, keyOf('x')),
                   QStringLiteral("extension-review-ledger-mac-mismatch")),
           "a wrong key did not fail authentication");
}

void tamperTests()
{
    const QByteArray key = keyOf('k');
    const QList<ExtensionReviewPin> pins = samplePins();
    const QByteArray bytes = ExtensionReviewLedger::serialize(7, pins, key);

    // 代号被换到另一组复核上必须失败：MAC 联合覆盖两者。
    expect(invalid(ExtensionReviewLedger::parse(
                       withField(bytes, QStringLiteral("generation"), 8), key),
                   QStringLiteral("extension-review-ledger-mac-mismatch")),
           "the generation could be substituted");

    // 追加一条复核记录必须失败，否则任何人都可以把自己的扩展加进受信列表。
    QJsonObject object = QJsonDocument::fromJson(bytes).object();
    QJsonArray array = object.value(QStringLiteral("pins")).toArray();
    const ExtensionReviewPin extra =
        pin(ExtensionKind::Skill, QStringLiteral("attacker.skill"), "z");
    array.append(QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("skill")},
        {QStringLiteral("id"), extra.id},
        {QStringLiteral("source_identity"), extra.sourceIdentity},
        {QStringLiteral("content_identity"), extra.contentIdentity},
    });
    expect(invalid(ExtensionReviewLedger::parse(
                       withField(bytes, QStringLiteral("pins"), array), key),
                   QStringLiteral("extension-review-ledger-mac-mismatch")),
           "a review pin could be appended");

    // 删除一条复核记录同样必须失败。
    QJsonArray removed = object.value(QStringLiteral("pins")).toArray();
    removed.removeLast();
    expect(invalid(ExtensionReviewLedger::parse(
                       withField(bytes, QStringLiteral("pins"), removed), key),
                   QStringLiteral("extension-review-ledger-mac-mismatch")),
           "a review pin could be removed");

    // 重排复核记录必须失败：MAC 覆盖顺序。
    QJsonArray reordered;
    reordered.append(object.value(QStringLiteral("pins")).toArray().at(1));
    reordered.append(object.value(QStringLiteral("pins")).toArray().at(0));
    reordered.append(object.value(QStringLiteral("pins")).toArray().at(2));
    expect(invalid(ExtensionReviewLedger::parse(
                       withField(bytes, QStringLiteral("pins"), reordered), key),
                   QStringLiteral("extension-review-ledger-mac-mismatch")),
           "review pins could be reordered");

    // 替换单条复核记录的内容摘要必须失败——这正是把信任转移到别的内容上的手法。
    QJsonArray rewritten = object.value(QStringLiteral("pins")).toArray();
    QJsonObject first = rewritten.at(0).toObject();
    first.insert(QStringLiteral("content_identity"),
                 digest(QStringLiteral("extension-content:sha256:"), "swapped"));
    rewritten.replace(0, first);
    expect(invalid(ExtensionReviewLedger::parse(
                       withField(bytes, QStringLiteral("pins"), rewritten), key),
                   QStringLiteral("extension-review-ledger-mac-mismatch")),
           "a pinned content identity could be swapped");

    // 替换 MAC 本身必须失败。
    expect(invalid(ExtensionReviewLedger::parse(
                       withField(bytes, QStringLiteral("mac"),
                                 QString(64, QLatin1Char('a'))), key),
                   QStringLiteral("extension-review-ledger-mac-mismatch")),
           "a substituted MAC was accepted");
}

void malformedTests()
{
    const QByteArray key = keyOf('k');
    const QByteArray bytes =
        ExtensionReviewLedger::serialize(7, samplePins(), key);

    // 结构性缺陷一律 Invalid，绝不退化成 Empty。
    const QList<QPair<QByteArray, QString>> cases{
        {QByteArrayLiteral("not json"),
         QStringLiteral("extension-review-ledger-record-invalid")},
        {QByteArrayLiteral("[]"),
         QStringLiteral("extension-review-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("schema"), QJsonValue()),
         QStringLiteral("extension-review-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("schema"),
                   QStringLiteral("aegisy-extension-review-ledger/0.2")),
         QStringLiteral("extension-review-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("extra"), 1),
         QStringLiteral("extension-review-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("mac"), QJsonValue()),
         QStringLiteral("extension-review-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("pins"), QJsonValue()),
         QStringLiteral("extension-review-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("pins"), QStringLiteral("[]")),
         QStringLiteral("extension-review-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("generation"), 0),
         QStringLiteral("extension-review-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("generation"), -1),
         QStringLiteral("extension-review-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("generation"), 1.5),
         QStringLiteral("extension-review-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("generation"),
                   QStringLiteral("7")),
         QStringLiteral("extension-review-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("generation"),
                   static_cast<double>(ExtensionReviewLedger::MaxGeneration) + 2048),
         QStringLiteral("extension-review-ledger-record-invalid")},
    };
    for (const auto &entry : cases) {
        expect(invalid(ExtensionReviewLedger::parse(entry.first, key), entry.second),
               "a malformed review ledger was not rejected exactly");
    }

    // 单条复核记录的格式缺陷。
    const QList<QPair<QJsonObject, QString>> pinCases{
        {QJsonObject{{QStringLiteral("kind"), QStringLiteral("skill")},
                     {QStringLiteral("id"), QStringLiteral("ok.skill")},
                     {QStringLiteral("source_identity"),
                      QStringLiteral("extension-source:sha256:short")},
                     {QStringLiteral("content_identity"),
                      digest(QStringLiteral("extension-content:sha256:"), "c")}},
         QStringLiteral("extension-review-ledger-pin-invalid")},
        {QJsonObject{{QStringLiteral("kind"), QStringLiteral("unknown")},
                     {QStringLiteral("id"), QStringLiteral("ok.skill")},
                     {QStringLiteral("source_identity"),
                      digest(QStringLiteral("extension-source:sha256:"), "s")},
                     {QStringLiteral("content_identity"),
                      digest(QStringLiteral("extension-content:sha256:"), "c")}},
         QStringLiteral("extension-review-ledger-pin-invalid")},
        {QJsonObject{{QStringLiteral("kind"), QStringLiteral("skill")},
                     {QStringLiteral("id"), QStringLiteral("Bad.Skill")},
                     {QStringLiteral("source_identity"),
                      digest(QStringLiteral("extension-source:sha256:"), "s")},
                     {QStringLiteral("content_identity"),
                      digest(QStringLiteral("extension-content:sha256:"), "c")}},
         QStringLiteral("extension-review-ledger-pin-invalid")},
        {QJsonObject{{QStringLiteral("kind"), QStringLiteral("skill")},
                     {QStringLiteral("id"), QStringLiteral("ok.skill")},
                     {QStringLiteral("source_identity"),
                      digest(QStringLiteral("extension-source:sha256:"), "s")}},
         QStringLiteral("extension-review-ledger-pin-invalid")},
        {QJsonObject{{QStringLiteral("kind"), QStringLiteral("skill")},
                     {QStringLiteral("id"), QStringLiteral("ok.skill")},
                     {QStringLiteral("source_identity"),
                      digest(QStringLiteral("extension-source:sha256:"), "s")},
                     {QStringLiteral("content_identity"),
                      digest(QStringLiteral("extension-content:sha256:"), "c")},
                     {QStringLiteral("note"), QStringLiteral("extra")}},
         QStringLiteral("extension-review-ledger-pin-invalid")},
        // 来源与内容摘要前缀互换：两者必须各自使用正确的域。
        {QJsonObject{{QStringLiteral("kind"), QStringLiteral("skill")},
                     {QStringLiteral("id"), QStringLiteral("ok.skill")},
                     {QStringLiteral("source_identity"),
                      digest(QStringLiteral("extension-content:sha256:"), "s")},
                     {QStringLiteral("content_identity"),
                      digest(QStringLiteral("extension-source:sha256:"), "c")}},
         QStringLiteral("extension-review-ledger-pin-invalid")},
    };
    for (const auto &entry : pinCases) {
        QJsonArray array;
        array.append(entry.first);
        expect(invalid(ExtensionReviewLedger::parse(
                           withField(bytes, QStringLiteral("pins"), array), key),
                       entry.second),
               "a malformed review pin was not rejected exactly");
    }

    // 载荷中出现重复 (kind, id) 必须报告重复，而不是留给信任判定去发现冲突。
    QJsonArray duplicated;
    const QJsonObject entry{
        {QStringLiteral("kind"), QStringLiteral("skill")},
        {QStringLiteral("id"), QStringLiteral("ok.skill")},
        {QStringLiteral("source_identity"),
         digest(QStringLiteral("extension-source:sha256:"), "s")},
        {QStringLiteral("content_identity"),
         digest(QStringLiteral("extension-content:sha256:"), "c")}};
    duplicated.append(entry);
    duplicated.append(entry);
    expect(invalid(ExtensionReviewLedger::parse(
                       withField(bytes, QStringLiteral("pins"), duplicated), key),
                   QStringLiteral("extension-review-ledger-pin-duplicate")),
           "duplicate review pins were not rejected by the ledger");

    // 超出数量上限的载荷在校验 MAC 前就必须被拒绝。
    QJsonArray oversized;
    for (int index = 0; index <= ExtensionReviewLedger::MaxPins; ++index) {
        oversized.append(QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("skill")},
            {QStringLiteral("id"), QStringLiteral("filler.%1").arg(index)},
            {QStringLiteral("source_identity"),
             digest(QStringLiteral("extension-source:sha256:"), "s")},
            {QStringLiteral("content_identity"),
             digest(QStringLiteral("extension-content:sha256:"), "c")}});
    }
    expect(invalid(ExtensionReviewLedger::parse(
                       withField(bytes, QStringLiteral("pins"), oversized), key),
                   QStringLiteral("extension-review-ledger-pin-limit")),
           "an oversized review ledger was not rejected");
}

void serializeGuardTests()
{
    const QByteArray key = keyOf('k');
    const ExtensionReviewPin valid =
        pin(ExtensionKind::Skill, QStringLiteral("fixture.skill"), "a");

    expect(ExtensionReviewLedger::serialize(0, {valid}, key).isEmpty(),
           "generation zero was serialized");
    expect(ExtensionReviewLedger::serialize(-1, {valid}, key).isEmpty(),
           "a negative generation was serialized");
    expect(ExtensionReviewLedger::serialize(
               ExtensionReviewLedger::MaxGeneration + 1, {valid}, key).isEmpty(),
           "an exhausted generation was serialized");
    expect(!ExtensionReviewLedger::serialize(
               ExtensionReviewLedger::MaxGeneration, {valid}, key).isEmpty(),
           "the maximum generation was rejected");
    expect(ExtensionReviewLedger::serialize(1, {valid}, QByteArray(31, 'k'))
               .isEmpty(),
           "a short key was accepted for serialization");
    expect(ExtensionReviewLedger::serialize(1, {valid}, QByteArray()).isEmpty(),
           "an absent key was accepted for serialization");

    ExtensionReviewPin broken = valid;
    broken.contentIdentity = QStringLiteral("extension-content:sha256:nope");
    expect(ExtensionReviewLedger::serialize(1, {broken}, key).isEmpty(),
           "a malformed pin was serialized");
    ExtensionReviewPin badId = valid;
    badId.id = QStringLiteral("Bad.Skill");
    expect(ExtensionReviewLedger::serialize(1, {badId}, key).isEmpty(),
           "a pin with an invalid id was serialized");
    // 写入重复项会让信任判定得出冲突，因此不允许被写入。
    expect(ExtensionReviewLedger::serialize(1, {valid, valid}, key).isEmpty(),
           "duplicate pins were serialized");
    // 同一 ID 的不同种类是不同扩展，允许共存。
    ExtensionReviewPin otherKind = valid;
    otherKind.kind = ExtensionKind::Mcp;
    expect(!ExtensionReviewLedger::serialize(1, {valid, otherKind}, key).isEmpty(),
           "distinct kinds sharing an id were rejected");

    QList<ExtensionReviewPin> oversized;
    for (int index = 0; index <= ExtensionReviewLedger::MaxPins; ++index) {
        ExtensionReviewPin filler = valid;
        filler.id = QStringLiteral("filler.%1").arg(index);
        oversized.append(filler);
    }
    expect(ExtensionReviewLedger::serialize(1, oversized, key).isEmpty(),
           "an oversized pin set was serialized");
}

void trustAgreementTests()
{
    // 记录层解析出的复核记录必须能被信任判定直接采纳，否则两层的校验不一致。
    const QByteArray key = keyOf('k');
    const QList<ExtensionReviewPin> pins = samplePins();
    const ExtensionReviewLedgerResult parsed = ExtensionReviewLedger::parse(
        ExtensionReviewLedger::serialize(3, pins, key), key);
    if (!expect(parsed.state == ExtensionReviewLedgerState::Ready,
                "the trust agreement fixture did not parse")) return;

    ExtensionRegistryRecord record;
    record.kind = pins.at(0).kind;
    record.id = pins.at(0).id;
    record.name = QStringLiteral("Sample");
    record.version = QStringLiteral("1.0.0");
    record.sourceIdentity = pins.at(0).sourceIdentity;
    record.contentIdentity = pins.at(0).contentIdentity;
    record.scope = QStringLiteral("user");
    record.installed = true;

    const ExtensionTrustDecision decision =
        ExtensionTrustPolicy::evaluate(record, parsed.pins);
    expect(decision.state == ExtensionTrustState::Verified
               && decision.evidence == ExtensionTrustEvidence::ReviewMatched,
           "ledger pins were not accepted by the trust policy");

    // 认证过的空集合不授予任何信任：合法载荷不等于信任。
    const ExtensionReviewLedgerResult emptySet = ExtensionReviewLedger::parse(
        ExtensionReviewLedger::serialize(4, {}, key), key);
    expect(ExtensionTrustPolicy::evaluate(record, emptySet.pins).state
               == ExtensionTrustState::Unverified,
           "an authenticated empty ledger granted trust");

    // 载荷不可信时不能落回一组可用的复核记录。
    const ExtensionReviewLedgerResult broken = ExtensionReviewLedger::parse(
        ExtensionReviewLedger::serialize(3, pins, key), keyOf('x'));
    expect(broken.pins.isEmpty()
               && ExtensionTrustPolicy::evaluate(record, broken.pins).state
                   == ExtensionTrustState::Unverified,
           "an unauthenticated ledger still produced usable review pins");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    roundTripTests();
    emptyAndKeyTests();
    tamperTests();
    malformedTests();
    serializeGuardTests();
    trustAgreementTests();
    if (failures == 0) {
        QTextStream(stdout) << "extension review ledger tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
