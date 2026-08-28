#include "extension_enablement_ledger.h"

#include "extension_review_ledger.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <openssl/evp.h>
#include <openssl/hmac.h>

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

ExtensionEnablementGrant grant(ExtensionKind kind, const QString &id,
                               const QByteArray &seed)
{
    ExtensionEnablementGrant value;
    value.kind = kind;
    value.id = id;
    value.sourceIdentity =
        digest(QStringLiteral("extension-source:sha256:"), seed + "-source");
    value.contentIdentity =
        digest(QStringLiteral("extension-content:sha256:"), seed + "-content");
    return value;
}

QList<ExtensionEnablementGrant> sampleGrants()
{
    return {grant(ExtensionKind::Skill, QStringLiteral("fixture.skill"), "a"),
            grant(ExtensionKind::Mcp, QStringLiteral("fixture.mcp"), "b"),
            grant(ExtensionKind::CodexPlugin, QStringLiteral("fixture.plugin"), "c")};
}

bool sameGrant(const ExtensionEnablementGrant &left,
               const ExtensionEnablementGrant &right)
{
    return left.kind == right.kind && left.id == right.id
        && left.sourceIdentity == right.sourceIdentity
        && left.contentIdentity == right.contentIdentity;
}

bool invalid(const ExtensionEnablementLedgerResult &result, const QString &code)
{
    // 反降级：被篡改的载荷永远得出 Invalid，绝不退化成 Empty。退化会把"授权记录被
    // 改坏了"表述成"从未授权过"。
    return result.state == ExtensionEnablementLedgerState::Invalid
        && result.errorCode == code && result.grants.isEmpty()
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

void roundTripTests()
{
    const QByteArray key = keyOf('\x11');
    const QList<ExtensionEnablementGrant> grants = sampleGrants();
    const QByteArray bytes = ExtensionEnablementLedger::serialize(4, grants, key);
    expect(!bytes.isEmpty(), "a valid grant set failed to serialize");

    const ExtensionEnablementLedgerResult parsed =
        ExtensionEnablementLedger::parse(bytes, key);
    expect(parsed.state == ExtensionEnablementLedgerState::Ready
               && parsed.generation == 4
               && parsed.grants.size() == grants.size(),
           "a valid grant set did not round trip");
    for (int i = 0; i < grants.size() && i < parsed.grants.size(); ++i) {
        expect(sameGrant(parsed.grants.at(i), grants.at(i)),
               "a grant changed across a round trip");
    }
    expect(parsed.identity.startsWith(
               QStringLiteral("extension-enablement-ledger:sha256:")),
           "the grant identity uses an unexpected prefix");

    // 一个被认证过的空集合是"授权过，但当前没有任何扩展被启用"，与"从未授权过"
    // 不同：前者代号非零，且必须能与撤销后的状态区分开。
    const QByteArray emptyBytes = ExtensionEnablementLedger::serialize(9, {}, key);
    const ExtensionEnablementLedgerResult emptyParsed =
        ExtensionEnablementLedger::parse(emptyBytes, key);
    expect(emptyParsed.state == ExtensionEnablementLedgerState::Ready
               && emptyParsed.grants.isEmpty() && emptyParsed.generation == 9,
           "an authenticated empty grant set was not readable");
    expect(emptyParsed.identity != parsed.identity,
           "an empty grant set shares its identity with a populated one");

    // 完全没有载荷才是 Empty。
    const ExtensionEnablementLedgerResult absent =
        ExtensionEnablementLedger::parse({}, key);
    expect(absent.state == ExtensionEnablementLedgerState::Empty
               && absent.grants.isEmpty() && absent.generation == 0
               && absent.errorCode.isEmpty(),
           "an absent payload was not reported as empty");

    // 代号进入 MAC 与身份摘要，因此同一集合换一个代号得到不同的身份。
    const ExtensionEnablementLedgerResult other = ExtensionEnablementLedger::parse(
        ExtensionEnablementLedger::serialize(5, grants, key), key);
    expect(other.state == ExtensionEnablementLedgerState::Ready
               && other.identity != parsed.identity,
           "the grant identity ignores the generation");
}

void keyTests()
{
    const QByteArray key = keyOf('\x11');
    const QByteArray bytes =
        ExtensionEnablementLedger::serialize(3, sampleGrants(), key);

    // 换一把密钥不能得出"没有授权"，也不能得出一个可用集合。
    expect(invalid(ExtensionEnablementLedger::parse(bytes, keyOf('\x12')),
                   QStringLiteral("extension-enablement-ledger-mac-mismatch")),
           "a substituted key produced a usable grant set");

    // 密钥不可用时当前内容未知，这不是"没有授权"。
    const ExtensionEnablementLedgerResult unavailable =
        ExtensionEnablementLedger::parse(bytes, QByteArray(16, '\x11'));
    expect(unavailable.state == ExtensionEnablementLedgerState::Unavailable
               && unavailable.errorCode
                   == QStringLiteral("extension-enablement-ledger-key-unavailable")
               && unavailable.grants.isEmpty(),
           "an unusable key was not reported as unavailable");

    expect(ExtensionEnablementLedger::serialize(3, sampleGrants(),
                                               QByteArray(31, '\x11')).isEmpty(),
           "a short key produced a signed payload");
}

void tamperTests()
{
    const QByteArray key = keyOf('\x21');
    const QList<ExtensionEnablementGrant> grants = sampleGrants();
    const QByteArray bytes = ExtensionEnablementLedger::serialize(2, grants, key);

    // MAC 覆盖代号与整个集合，因此追加、删除、重排与单字段替换全部会被发现。
    QJsonObject object = QJsonDocument::fromJson(bytes).object();
    QJsonArray array = object.value(QStringLiteral("grants")).toArray();

    QJsonArray appended = array;
    appended.append(QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("skill")},
        {QStringLiteral("id"), QStringLiteral("extra")},
        {QStringLiteral("source_identity"),
         digest(QStringLiteral("extension-source:sha256:"), "extra")},
        {QStringLiteral("content_identity"),
         digest(QStringLiteral("extension-content:sha256:"), "extra")},
    });
    expect(invalid(ExtensionEnablementLedger::parse(
                       withField(bytes, QStringLiteral("grants"), appended), key),
                   QStringLiteral("extension-enablement-ledger-mac-mismatch")),
           "an appended grant was accepted");

    QJsonArray removed = array;
    removed.removeLast();
    expect(invalid(ExtensionEnablementLedger::parse(
                       withField(bytes, QStringLiteral("grants"), removed), key),
                   QStringLiteral("extension-enablement-ledger-mac-mismatch")),
           "a removed grant was accepted");

    QJsonArray reordered;
    reordered.append(array.at(1));
    reordered.append(array.at(0));
    reordered.append(array.at(2));
    expect(invalid(ExtensionEnablementLedger::parse(
                       withField(bytes, QStringLiteral("grants"), reordered), key),
                   QStringLiteral("extension-enablement-ledger-mac-mismatch")),
           "reordered grants were accepted");

    // 替换内容摘要是最关键的一项：它等于把一份授权套用到另一份内容上。
    QJsonArray substituted = array;
    QJsonObject first = substituted.at(0).toObject();
    first.insert(QStringLiteral("content_identity"),
                 digest(QStringLiteral("extension-content:sha256:"), "swapped"));
    substituted.replace(0, first);
    expect(invalid(ExtensionEnablementLedger::parse(
                       withField(bytes, QStringLiteral("grants"), substituted), key),
                   QStringLiteral("extension-enablement-ledger-mac-mismatch")),
           "a substituted grant content identity was accepted");

    // 换代号同样会被发现，因此一个旧代号不能被贴到新集合上。
    expect(invalid(ExtensionEnablementLedger::parse(
                       withField(bytes, QStringLiteral("generation"), 3), key),
                   QStringLiteral("extension-enablement-ledger-mac-mismatch")),
           "a substituted generation was accepted");
}

void malformedTests()
{
    const QByteArray key = keyOf('\x31');
    const QByteArray bytes =
        ExtensionEnablementLedger::serialize(1, sampleGrants(), key);

    expect(invalid(ExtensionEnablementLedger::parse(QByteArrayLiteral("{"), key),
                   QStringLiteral("extension-enablement-ledger-record-invalid")),
           "unparseable bytes were accepted");
    expect(invalid(ExtensionEnablementLedger::parse(
                       withField(bytes, QStringLiteral("extra"), 1), key),
                   QStringLiteral("extension-enablement-ledger-record-invalid")),
           "an unknown top-level field was accepted");
    expect(invalid(ExtensionEnablementLedger::parse(
                       withField(bytes, QStringLiteral("mac"), QJsonValue()), key),
                   QStringLiteral("extension-enablement-ledger-record-invalid")),
           "a missing MAC was accepted");
    expect(invalid(ExtensionEnablementLedger::parse(
                       withField(bytes, QStringLiteral("generation"), 0), key),
                   QStringLiteral("extension-enablement-ledger-record-invalid")),
           "a zero generation was accepted");
    expect(invalid(ExtensionEnablementLedger::parse(
                       withField(bytes, QStringLiteral("generation"), 1.5), key),
                   QStringLiteral("extension-enablement-ledger-record-invalid")),
           "a fractional generation was accepted");

    // 授权本身不合法时报告条目代码，而不是被跳过。
    QJsonArray badArray;
    badArray.append(QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("skill")},
        {QStringLiteral("id"), QStringLiteral("Bad-Id")},
        {QStringLiteral("source_identity"),
         digest(QStringLiteral("extension-source:sha256:"), "x")},
        {QStringLiteral("content_identity"),
         digest(QStringLiteral("extension-content:sha256:"), "x")},
    });
    expect(invalid(ExtensionEnablementLedger::parse(
                       withField(bytes, QStringLiteral("grants"), badArray), key),
                   QStringLiteral("extension-enablement-ledger-grant-invalid")),
           "a grant with an invalid identifier was accepted");

    // 同一 (kind, id) 出现两次会让启用判定得出冲突，因此载荷层直接拒绝。
    QJsonArray duplicated;
    const QJsonObject entry{
        {QStringLiteral("kind"), QStringLiteral("skill")},
        {QStringLiteral("id"), QStringLiteral("dup")},
        {QStringLiteral("source_identity"),
         digest(QStringLiteral("extension-source:sha256:"), "d")},
        {QStringLiteral("content_identity"),
         digest(QStringLiteral("extension-content:sha256:"), "d")},
    };
    duplicated.append(entry);
    duplicated.append(entry);
    expect(invalid(ExtensionEnablementLedger::parse(
                       withField(bytes, QStringLiteral("grants"), duplicated), key),
                   QStringLiteral("extension-enablement-ledger-grant-duplicate")),
           "a duplicated grant was accepted");

    // 序列化同样拒绝重复与不合法的授权，因此不合法的集合根本无法被写出。
    const ExtensionEnablementGrant one =
        grant(ExtensionKind::Skill, QStringLiteral("dup"), "d");
    expect(ExtensionEnablementLedger::serialize(1, {one, one}, key).isEmpty(),
           "a duplicated grant was serialized");
    ExtensionEnablementGrant broken = one;
    broken.contentIdentity = QStringLiteral("extension-content:sha256:zz");
    expect(ExtensionEnablementLedger::serialize(1, {broken}, key).isEmpty(),
           "a malformed grant was serialized");
    expect(ExtensionEnablementLedger::serialize(0, {one}, key).isEmpty(),
           "a zero generation was serialized");
}

void domainSeparationTests()
{
    // 这是本层最关键的性质：复核证据与启用授权是两类不同的授权。如果它们共用格式，
    // 一份复核记录的字节就能被移动到启用授权的位置，把"我看过这份内容"变成"我要求
    // 运行这份内容"。
    const QByteArray key = keyOf('\x41');
    const QList<ExtensionEnablementGrant> grants = sampleGrants();

    QList<ExtensionReviewPin> pins;
    for (const ExtensionEnablementGrant &value : grants) {
        ExtensionReviewPin pin;
        pin.kind = value.kind;
        pin.id = value.id;
        pin.sourceIdentity = value.sourceIdentity;
        pin.contentIdentity = value.contentIdentity;
        pins.append(pin);
    }

    const QByteArray grantBytes =
        ExtensionEnablementLedger::serialize(6, grants, key);
    const QByteArray reviewBytes = ExtensionReviewLedger::serialize(6, pins, key);
    expect(!grantBytes.isEmpty() && !reviewBytes.isEmpty(),
           "a domain fixture failed to serialize");
    // 相同代号、相同条目、相同密钥仍然产出不同的字节。
    expect(grantBytes != reviewBytes,
           "the review and enablement payloads are byte-identical");

    // 一份复核载荷在启用授权层无法解析，反之亦然。
    expect(invalid(ExtensionEnablementLedger::parse(reviewBytes, key),
                   QStringLiteral("extension-enablement-ledger-record-invalid")),
           "a review payload parsed as an enablement grant set");
    const ExtensionReviewLedgerResult crossed =
        ExtensionReviewLedger::parse(grantBytes, key);
    expect(crossed.state == ExtensionReviewLedgerState::Invalid
               && crossed.pins.isEmpty(),
           "an enablement payload parsed as a review set");

    // 仅仅改写 schema 字段也没有用：MAC 域也参与，因此重贴标签不能跨越域。
    QJsonObject relabelled = QJsonDocument::fromJson(reviewBytes).object();
    QJsonArray entries = relabelled.value(QStringLiteral("pins")).toArray();
    relabelled.remove(QStringLiteral("pins"));
    relabelled.insert(QStringLiteral("grants"), entries);
    relabelled.insert(QStringLiteral("schema"),
                      QStringLiteral("aegisy-extension-enablement-ledger/0.1"));
    expect(invalid(ExtensionEnablementLedger::parse(
                       QJsonDocument(relabelled).toJson(QJsonDocument::Compact), key),
                   QStringLiteral("extension-enablement-ledger-mac-mismatch")),
           "relabelling a review payload made it an enablement grant set");

    // 身份摘要也必须分域，否则两类证据会在诊断里互相混淆。
    const ExtensionEnablementLedgerResult grantParsed =
        ExtensionEnablementLedger::parse(grantBytes, key);
    const ExtensionReviewLedgerResult reviewParsed =
        ExtensionReviewLedger::parse(reviewBytes, key);
    expect(grantParsed.state == ExtensionEnablementLedgerState::Ready
               && reviewParsed.state == ExtensionReviewLedgerState::Ready
               && grantParsed.identity != reviewParsed.identity,
           "the review and enablement identities collapsed");
}

void wireCompatibilityTests()
{
    // 从域字符串、8 字节大端长度前缀与"代号在前、集合在后"的顺序独立重算 MAC 与
    // 身份摘要，而不是复用实现里的辅助函数：实现漂移会被发现，而不是被镜像。
    const QByteArray key = keyOf('\x51');
    const QList<ExtensionEnablementGrant> grants = sampleGrants();
    const QByteArray bytes = ExtensionEnablementLedger::serialize(8, grants, key);
    const QJsonObject object = QJsonDocument::fromJson(bytes).object();

    expect(object.value(QStringLiteral("schema")).toString()
               == QStringLiteral("aegisy-extension-enablement-ledger/0.1"),
           "the persisted enablement ledger schema changed");
    expect(object.contains(QStringLiteral("grants")),
           "the persisted enablement ledger entry key changed");

    auto framed = [](QByteArray *target, const QByteArray &value) {
        const quint64 size = static_cast<quint64>(value.size());
        for (int shift = 56; shift >= 0; shift -= 8) {
            target->append(static_cast<char>((size >> shift) & 0xff));
        }
        target->append(value);
    };
    auto kindLabel = [](ExtensionKind kind) {
        switch (kind) {
        case ExtensionKind::CodexPlugin: return QByteArrayLiteral("codex-plugin");
        case ExtensionKind::Skill:       return QByteArrayLiteral("skill");
        case ExtensionKind::Mcp:         return QByteArrayLiteral("mcp");
        }
        return QByteArray();
    };

    const char macDomain[] = "aegisy-extension-enablement-ledger-hmac/0.1\0";
    QByteArray macInput(macDomain, sizeof(macDomain) - 1);
    framed(&macInput, QByteArray::number(8));
    framed(&macInput, QByteArray::number(static_cast<qint64>(grants.size())));
    for (const ExtensionEnablementGrant &value : grants) {
        framed(&macInput, kindLabel(value.kind));
        framed(&macInput, value.id.toUtf8());
        framed(&macInput, value.sourceIdentity.toUtf8());
        framed(&macInput, value.contentIdentity.toUtf8());
    }
    unsigned char digestBytes[EVP_MAX_MD_SIZE]{};
    unsigned int digestLength = 0;
    HMAC(EVP_sha256(), key.constData(), key.size(),
         reinterpret_cast<const unsigned char *>(macInput.constData()),
         static_cast<size_t>(macInput.size()), digestBytes, &digestLength);
    expect(object.value(QStringLiteral("mac")).toString()
               == QString::fromLatin1(QByteArray(
                   reinterpret_cast<const char *>(digestBytes), 32).toHex()),
           "the enablement ledger MAC domain or framing changed");

    const char identityDomain[] =
        "aegisy-extension-enablement-ledger-identity/0.1\0";
    QByteArray identityInput(identityDomain, sizeof(identityDomain) - 1);
    framed(&identityInput, QByteArray::number(8));
    for (const ExtensionEnablementGrant &value : grants) {
        framed(&identityInput, kindLabel(value.kind));
        framed(&identityInput, value.id.toUtf8());
        framed(&identityInput, value.sourceIdentity.toUtf8());
        framed(&identityInput, value.contentIdentity.toUtf8());
    }
    const ExtensionEnablementLedgerResult parsed =
        ExtensionEnablementLedger::parse(bytes, key);
    expect(parsed.state == ExtensionEnablementLedgerState::Ready
               && parsed.identity
                   == QStringLiteral("extension-enablement-ledger:sha256:")
                       + QString::fromLatin1(QCryptographicHash::hash(
                           identityInput, QCryptographicHash::Sha256).toHex()),
           "the enablement ledger identity domain or framing changed");

    expect(QByteArray(macDomain, sizeof(macDomain) - 1)
               != QByteArray(identityDomain, sizeof(identityDomain) - 1),
           "the enablement ledger MAC and identity domains collapsed");
}

void policyAgreementTests()
{
    // 解析出的授权仍然要经过启用判定，而判定还要求已复核、兼容且已安装。因此一份
    // 认证过的授权本身不足以启用任何东西。
    const QByteArray key = keyOf('\x61');
    const ExtensionEnablementGrant value =
        grant(ExtensionKind::Skill, QStringLiteral("fixture.skill"), "a");
    const ExtensionEnablementLedgerResult parsed = ExtensionEnablementLedger::parse(
        ExtensionEnablementLedger::serialize(1, {value}, key), key);
    expect(parsed.state == ExtensionEnablementLedgerState::Ready,
           "the policy fixture failed to parse");

    ExtensionRegistryRecord record;
    record.kind = value.kind;
    record.id = value.id;
    record.name = QStringLiteral("Sample");
    record.version = QStringLiteral("1.0.0");
    record.sourceIdentity = value.sourceIdentity;
    record.contentIdentity = value.contentIdentity;
    record.scope = QStringLiteral("user");
    record.installed = true;

    // 未复核、未兼容时授权不生效。
    expect(!ExtensionEnablementPolicy::evaluate(record, parsed.grants).enabled,
           "an authenticated grant enabled an unreviewed record");

    record.trust = ExtensionTrustState::Verified;
    record.compatibility = ExtensionCompatibilityState::Compatible;
    expect(ExtensionEnablementPolicy::evaluate(record, parsed.grants).enabled,
           "an authenticated grant did not enable an eligible record");

    // 内容变化后同一份授权失效。
    record.contentIdentity =
        digest(QStringLiteral("extension-content:sha256:"), "moved");
    expect(!ExtensionEnablementPolicy::evaluate(record, parsed.grants).enabled,
           "an authenticated grant survived content drift");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    roundTripTests();
    keyTests();
    tamperTests();
    malformedTests();
    domainSeparationTests();
    wireCompatibilityTests();
    policyAgreementTests();
    if (failures == 0) {
        QTextStream(stdout) << "extension enablement ledger tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
