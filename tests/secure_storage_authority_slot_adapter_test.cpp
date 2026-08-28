#include "companion_activation_journal_secure_storage_adapter.h"
#include "extension_enablement_ledger_secure_storage_adapter.h"
#include "extension_review_ledger_secure_storage_adapter.h"

#include "authority_slot_publication.h"
#include "secure_storage_authority_slot_adapter.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QList>
#include <QSet>
#include <QString>
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

// 独立重算槽位帧，而不是通过 frame()/parseFrame() 往返：往返测试无法发现被改掉的
// 域字节，而域字节参与已持久化的内容，改掉它会让现有安装读不出自己的授权。
QByteArray framed(const QByteArray &value)
{
    QByteArray out;
    const quint64 size = static_cast<quint64>(value.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.append(static_cast<char>((size >> shift) & 0xff));
    }
    out.append(value);
    return out;
}

QString digestOf(const QByteArray &digestDomain, qint64 generation,
                 const QByteArray &payload)
{
    QByteArray input = digestDomain;
    input.append(framed(QByteArray::number(generation)));
    input.append(framed(payload));
    return QString::fromLatin1(
        QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex());
}

// 各子系统实际持久化的作用域与域串，在测试里独立重写一份。下面的断言把这些字面量与
// 适配器返回的真实值逐字节比对，因此任何一侧被改动都会失败——不能从适配器构造期望
// 值，那样等于用被测对象证明自己。
struct Expectation {
    const char *label;
    SecureStorageAuthoritySlotScopes actual;
    const char *slotA;
    const char *slotB;
    const char *legacy; // 新增子系统必须为空串。
    const char *schema;
    const char *digest;
    const char *domainErrorPrefix;
    const char *errorPrefix;
};

QList<Expectation> expectations()
{
    return {
        {"review",
         SecureStorageExtensionReviewLedgerAdapter::authoritySlotScopes(),
         "extensions/review-ledger-authority/slot-a/v1",
         "extensions/review-ledger-authority/slot-b/v1",
         "",
         "aegisy-extension-review-ledger-authority-slot/0.1",
         "aegisy-extension-review-ledger-authority-slot-digest/0.1",
         "extension-review-authority-slot-",
         "extension-review-secure"},
        {"enablement",
         SecureStorageExtensionEnablementLedgerAdapter::authoritySlotScopes(),
         "extensions/enablement-ledger-authority/slot-a/v1",
         "extensions/enablement-ledger-authority/slot-b/v1",
         "",
         "aegisy-extension-enablement-ledger-authority-slot/0.1",
         "aegisy-extension-enablement-ledger-authority-slot-digest/0.1",
         "extension-enablement-authority-slot-",
         "extension-enablement-secure"},
        {"activation",
         SecureStorageCompanionActivationJournalAdapter::authoritySlotScopes(),
         "companion/activation-journal-authority/slot-a/v1",
         "companion/activation-journal-authority/slot-b/v1",
         "companion/activation-journal-authority/v1",
         "aegisy-companion-activation-journal-authority-slot/0.1",
         "aegisy-companion-activation-journal-authority-slot-digest/0.1",
         "activation-authority-slot-",
         "activation-journal-secure"},
    };
}

// 摘要域串以一个分隔用的 NUL 字节结尾，这一点也是格式的一部分。
QByteArray digestDomainOf(const char *digest)
{
    return QByteArray(digest) + QByteArray(1, '\0');
}

void persistedDomainTests()
{
    // 适配器实际使用的作用域与域串必须与上面独立重写的字面量逐字节一致。这些值参与
    // 已持久化的字节：改掉任何一个都会让现有安装读不出自己的授权，而那份授权里的
    // HMAC 密钥没有第二份副本。
    for (const Expectation &item : expectations()) {
        expect(item.actual.isValid(),
               "an adapter published an invalid scope set");
        expect(item.actual.slotAScope == QString::fromLatin1(item.slotA),
               "an adapter's slot A scope drifted from its persisted value");
        expect(item.actual.slotBScope == QString::fromLatin1(item.slotB),
               "an adapter's slot B scope drifted from its persisted value");
        expect(item.actual.legacyScope == QString::fromLatin1(item.legacy),
               "an adapter's legacy scope drifted from its persisted value");
        expect(item.actual.domain.frameSchema == QByteArray(item.schema),
               "an adapter's frame schema drifted from its persisted value");
        expect(item.actual.domain.digestDomain == digestDomainOf(item.digest),
               "an adapter's digest domain drifted from its persisted value");
        expect(item.actual.domain.errorPrefix
                   == QString::fromLatin1(item.domainErrorPrefix),
               "an adapter's slot error prefix changed");
        expect(item.actual.errorPrefix == QString::fromLatin1(item.errorPrefix),
               "an adapter's secure storage error prefix changed");
    }

    // 复核记录与启用授权都是新增子系统，没有迁移前的单槽授权可以采纳。非空的旧作用域
    // 会让它们去读别人写下的授权信封。
    expect(SecureStorageExtensionReviewLedgerAdapter::authoritySlotScopes()
               .legacyScope.isEmpty(),
           "the review authority adopts a legacy scope it never wrote");
    expect(SecureStorageExtensionEnablementLedgerAdapter::authoritySlotScopes()
               .legacyScope.isEmpty(),
           "the enablement authority adopts a legacy scope it never wrote");
}

void scopeSeparationTests()
{
    // 三个子系统的槽位作用域必须两两不同。作用域决定字节落在哪里，共用任何一个都会
    // 让一份授权覆盖另一份。
    QStringList scopes;
    for (const Expectation &item : expectations()) {
        scopes << item.actual.slotAScope << item.actual.slotBScope;
        if (!item.actual.legacyScope.isEmpty()) {
            scopes << item.actual.legacyScope;
        }
    }
    for (const QString &scope : scopes) {
        expect(!scope.isEmpty(), "an authority slot scope is empty");
    }
    expect(QSet<QString>(scopes.cbegin(), scopes.cend()).size() == scopes.size(),
           "two authority slot scopes collide");

    // 访问器与真实作用域必须一致，否则测试与产品会看到两套值。
    expect(SecureStorageExtensionReviewLedgerAdapter::authoritySlotAScope()
               == SecureStorageExtensionReviewLedgerAdapter::authoritySlotScopes()
                      .slotAScope,
           "the review scope accessor disagrees with the scope set");
    expect(SecureStorageExtensionEnablementLedgerAdapter::authoritySlotAScope()
               == SecureStorageExtensionEnablementLedgerAdapter::
                      authoritySlotScopes().slotAScope,
           "the enablement scope accessor disagrees with the scope set");
    expect(SecureStorageCompanionActivationJournalAdapter::authorityScope()
               == SecureStorageCompanionActivationJournalAdapter::
                      authoritySlotScopes().legacyScope,
           "the activation legacy scope accessor disagrees with the scope set");

    // 启用授权的作用域不得与复核记录的作用域重叠：把一份复核授权搬进启用授权的位置
    // 等于把"我看过这份内容"变成"我要求运行这份内容"。
    expect(!SecureStorageExtensionEnablementLedgerAdapter::authoritySlotAScope()
               .contains(QStringLiteral("review-ledger")),
           "the enablement authority reuses the review scope namespace");
    expect(SecureStorageExtensionEnablementLedgerAdapter::authoritySlotAScope()
               .contains(QStringLiteral("enablement-ledger-authority")),
           "the enablement authority lost its own scope namespace");
}

void wireCompatibilityTests()
{
    // 抽取必须是字节兼容的，否则现有安装会读不出自己的复核授权与激活授权。这里用适配器
    // 真实的域串产出帧，再与独立重算的字节比较。
    const QByteArray payload = QByteArrayLiteral("{\"schema_version\":\"x\"}");
    const qint64 generation = 7;

    const QList<Expectation> cases = expectations();
    for (const Expectation &item : cases) {
        const AuthoritySlotDomain domain = item.actual.domain;
        const QByteArray frame =
            AuthoritySlotPublication::frame(domain, generation, payload);
        expect(!frame.isEmpty(), "a slot frame was not produced");
        // 摘要必须同时覆盖代号与载荷：只覆盖载荷会让旧摘要与新代号混用。这里用独立
        // 重写的域串重算，因此适配器的摘要域一旦改动就对不上。
        const QString expected =
            digestOf(digestDomainOf(item.digest), generation, payload);
        expect(frame.contains(expected.toLatin1()),
               "the slot digest does not match an independent recomputation");
        expect(frame.contains(QByteArray(item.schema)),
               "the slot frame does not carry its own schema");
        qint64 parsedGeneration = 0;
        QByteArray parsedPayload;
        expect(AuthoritySlotPublication::parseFrame(
                   domain, frame, &parsedGeneration, &parsedPayload)
                   && parsedGeneration == generation && parsedPayload == payload,
               "a slot frame did not round trip");
    }

    // 三个域必须两两不同，并且一个域的帧在另一个域下必须解析失败。相同载荷与代号在
    // 不同域下得到不同字节，因此搬运槽位内容无法冒充有效授权。
    for (int i = 0; i < cases.size(); ++i) {
        for (int j = 0; j < cases.size(); ++j) {
            if (i == j) continue;
            const AuthoritySlotDomain a = cases.at(i).actual.domain;
            const AuthoritySlotDomain b = cases.at(j).actual.domain;
            expect(a.frameSchema != b.frameSchema
                       && a.digestDomain != b.digestDomain,
                   "two authority slot domains collide");
            const QByteArray frame =
                AuthoritySlotPublication::frame(a, generation, payload);
            expect(AuthoritySlotPublication::frame(b, generation, payload) != frame,
                   "two domains produced identical slot bytes");
            qint64 parsedGeneration = 0;
            QByteArray parsedPayload;
            expect(!AuthoritySlotPublication::parseFrame(
                       b, frame, &parsedGeneration, &parsedPayload),
                   "a slot frame parsed under a foreign domain");
        }
    }
}

// 与任何子系统都无关的夹具域，用于检验共享层自身的入参校验。
AuthoritySlotDomain fixtureDomain()
{
    AuthoritySlotDomain value;
    value.frameSchema = QByteArrayLiteral("aegisy-fixture-authority-slot/0.1");
    value.digestDomain =
        QByteArrayLiteral("aegisy-fixture-authority-slot-digest/0.1")
        + QByteArray(1, '\0');
    value.errorPrefix = QStringLiteral("fixture-");
    return value;
}

void unconfiguredScopeTests()
{
    // 未配置的作用域被拒绝，而不是退回某个默认位置：一个漏填的门面会静默共用别人的
    // 授权。读取与写入都必须拒绝。
    SecureStorageAuthoritySlotScopes empty;
    QByteArray value;
    QString errorCode;
    expect(SecureStorageAuthoritySlotAdapter::readFresh(empty, &value, &errorCode)
               == SecureStorageAuthoritySlotReadState::Invalid
               && errorCode == QStringLiteral(
                   "secure-authority-slot-scopes-unconfigured"),
           "an unconfigured scope set was readable");
    expect(SecureStorageAuthoritySlotAdapter::write(
               empty, QByteArrayLiteral("x"), &errorCode)
               == SecureStorageAuthoritySlotWriteOutcome::DefiniteFailure
               && errorCode == QStringLiteral(
                   "secure-authority-slot-scopes-unconfigured"),
           "an unconfigured scope set was writable");

    // 两个槽位共用同一作用域时同样被拒绝：那会让 A/B 发布退化成单槽，一次被打断的
    // 写入就能销毁唯一的密钥副本。
    SecureStorageAuthoritySlotScopes collapsed;
    collapsed.domain = fixtureDomain();
    collapsed.slotAScope = QStringLiteral("fixture/slot/v1");
    collapsed.slotBScope = collapsed.slotAScope;
    collapsed.errorPrefix = QStringLiteral("fixture-secure");
    expect(!collapsed.isValid(),
           "collapsing both slots onto one scope was accepted");
    expect(SecureStorageAuthoritySlotAdapter::write(
               collapsed, QByteArrayLiteral("x"), &errorCode)
               == SecureStorageAuthoritySlotWriteOutcome::DefiniteFailure,
           "a collapsed slot pair was writable");

    // 空载荷与超限载荷是确定性失败，不是"结果未知"：不能让调用者以为可能已经写入。
    SecureStorageAuthoritySlotScopes valid;
    valid.domain = fixtureDomain();
    valid.slotAScope = QStringLiteral("fixture/slot-a/v1");
    valid.slotBScope = QStringLiteral("fixture/slot-b/v1");
    valid.errorPrefix = QStringLiteral("fixture-secure");
    expect(SecureStorageAuthoritySlotAdapter::write(valid, QByteArray(), &errorCode)
               == SecureStorageAuthoritySlotWriteOutcome::DefiniteFailure
               && errorCode == QStringLiteral("fixture-secure-write-invalid"),
           "an empty authority payload was not a definite failure");
    const QByteArray oversized(
        SecureStorageAuthoritySlotAdapter::MaxAuthorityBytes + 1, 'a');
    expect(SecureStorageAuthoritySlotAdapter::write(valid, oversized, &errorCode)
               == SecureStorageAuthoritySlotWriteOutcome::DefiniteFailure
               && errorCode == QStringLiteral("fixture-secure-write-invalid"),
           "an oversized authority payload was not a definite failure");
    // 读取目标为空同样是确定性失败，且带各自的前缀。
    expect(SecureStorageAuthoritySlotAdapter::readFresh(valid, nullptr, &errorCode)
               == SecureStorageAuthoritySlotReadState::Invalid
               && errorCode == QStringLiteral("fixture-secure-target-invalid"),
           "a null read target was not rejected");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    persistedDomainTests();
    scopeSeparationTests();
    wireCompatibilityTests();
    unconfiguredScopeTests();
    if (failures == 0) {
        QTextStream(stdout)
            << "secure storage authority slot adapter tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
