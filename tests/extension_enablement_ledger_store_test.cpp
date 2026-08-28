#include "extension_enablement_ledger_store.h"

#include "extension_review_ledger_store.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
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

// 可注入的安全存储：写入可以在任意一次调用上被打断，用来驱动三阶段发布的每一个
// 中断点。
class FakeSecureStore final : public ExtensionEnablementLedgerSecureStore
{
public:
    ReadState readFresh(QByteArray *value, QString *errorCode) override
    {
        ++reads;
        if (errorCode) errorCode->clear();
        if (readState != ReadState::Found) {
            if (errorCode && readState != ReadState::Missing) {
                *errorCode = QStringLiteral("fake-read-%1")
                    .arg(readState == ReadState::Unavailable
                         ? QStringLiteral("unavailable") : QStringLiteral("invalid"));
            }
            return readState;
        }
        if (stored.isEmpty()) return ReadState::Missing;
        if (value) *value = stored;
        return ReadState::Found;
    }

    WriteOutcome write(const QByteArray &value, QString *errorCode) override
    {
        ++writes;
        if (errorCode) errorCode->clear();
        if (failAllWrites || writes == failWriteAt) {
            if (errorCode) *errorCode = QStringLiteral("fake-write-failed");
            // 被打断的写入不改变已存内容。
            return failWriteOutcome;
        }
        stored = value;
        return WriteOutcome::Committed;
    }

    QByteArray stored;
    ReadState readState = ReadState::Found;
    bool failAllWrites = false;
    int failWriteAt = -1;
    WriteOutcome failWriteOutcome = WriteOutcome::DefiniteFailure;
    int reads = 0;
    int writes = 0;
};

// 复核证据侧的安全存储，仅用于跨域检验。
class FakeReviewSecureStore final : public ExtensionReviewLedgerSecureStore
{
public:
    ReadState readFresh(QByteArray *value, QString *errorCode) override
    {
        if (errorCode) errorCode->clear();
        if (stored.isEmpty()) return ReadState::Missing;
        if (value) *value = stored;
        return ReadState::Found;
    }

    WriteOutcome write(const QByteArray &value, QString *errorCode) override
    {
        if (errorCode) errorCode->clear();
        stored = value;
        return WriteOutcome::Committed;
    }

    QByteArray stored;
};

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
            grant(ExtensionKind::Mcp, QStringLiteral("fixture.mcp"), "b")};
}

struct Fixture {
    explicit Fixture(const QString &path)
        : settings(path, QSettings::IniFormat)
    {
    }

    FakeSecureStore secure;
    QSettings settings;

    ExtensionEnablementLedgerStore store()
    {
        return ExtensionEnablementLedgerStore(&secure, &settings);
    }
};

QString settingsPath(QTemporaryDir &dir, const QString &name)
{
    return dir.filePath(name + QStringLiteral(".ini"));
}

void emptyAndFirstWriteTests(QTemporaryDir &dir)
{
    Fixture fixture(settingsPath(dir, QStringLiteral("first")));

    // 两半都不存在才是"从未授权过"。
    const ExtensionEnablementLedgerStoreResult initial = fixture.store().load();
    expect(initial.state == ExtensionEnablementLedgerStoreState::Empty
               && initial.grants.isEmpty() && initial.generation == 0
               && initial.errorCode.isEmpty(),
           "a fresh install was not reported as empty");

    ExtensionEnablementLedgerStoreResult updated;
    QString errorCode;
    ExtensionEnablementLedgerStore store = fixture.store();
    expect(store.replace(sampleGrants(), 0, &updated, &errorCode)
               && updated.state == ExtensionEnablementLedgerStoreState::Ready
               && updated.generation == 1
               && updated.grants.size() == 2 && errorCode.isEmpty(),
           "the first grant commit failed");

    // 密钥只存在于安全存储里，绝不进入普通设置。
    expect(!fixture.secure.stored.isEmpty(),
           "the authority envelope was not persisted");
    const QByteArray recordBytes = fixture.settings
        .value(ExtensionEnablementLedgerStore::recordSettingsKey()).toByteArray();
    expect(!recordBytes.isEmpty(), "the grant payload was not persisted");
    expect(!recordBytes.contains(QByteArrayLiteral("hmac_key_base64")),
           "the payload carries the authority key");

    const ExtensionEnablementLedgerStoreResult reloaded = fixture.store().load();
    expect(reloaded.state == ExtensionEnablementLedgerStoreState::Ready
               && reloaded.generation == 1 && reloaded.grants.size() == 2
               && reloaded.identity == updated.identity,
           "a committed grant set did not reload");

    // 载荷键必须与复核证据的键不同，否则两类证据会互相覆盖。
    expect(ExtensionEnablementLedgerStore::recordSettingsKey()
               != ExtensionReviewLedgerStore::recordSettingsKey(),
           "the grant payload shares its settings key with review evidence");
}

void degradationTests(QTemporaryDir &dir)
{
    // 删除任意一半都不是"从未授权过"：那会把一次篡改表述成用户从未要求启用。
    {
        Fixture fixture(settingsPath(dir, QStringLiteral("orphan-payload")));
        ExtensionEnablementLedgerStoreResult updated;
        QString errorCode;
        ExtensionEnablementLedgerStore store = fixture.store();
        expect(store.replace(sampleGrants(), 0, &updated, &errorCode),
               "the degradation fixture failed to commit");
        fixture.secure.stored.clear();
        const ExtensionEnablementLedgerStoreResult result = fixture.store().load();
        expect(result.state == ExtensionEnablementLedgerStoreState::Invalid
                   && result.errorCode == QStringLiteral(
                       "extension-enablement-store-record-without-authority")
                   && result.grants.isEmpty(),
               "an orphaned grant payload degraded to empty");
    }
    {
        Fixture fixture(settingsPath(dir, QStringLiteral("orphan-authority")));
        ExtensionEnablementLedgerStoreResult updated;
        QString errorCode;
        ExtensionEnablementLedgerStore store = fixture.store();
        expect(store.replace(sampleGrants(), 0, &updated, &errorCode),
               "the degradation fixture failed to commit");
        fixture.settings.remove(
            ExtensionEnablementLedgerStore::recordSettingsKey());
        fixture.settings.sync();
        const ExtensionEnablementLedgerStoreResult result = fixture.store().load();
        expect(result.state == ExtensionEnablementLedgerStoreState::Invalid
                   && result.errorCode
                       == QStringLiteral("extension-enablement-store-record-deleted")
                   && result.grants.isEmpty(),
               "a deleted grant payload degraded to empty");
    }
    {
        // 载荷损坏时报告载荷层自己的代码，而不是"没有授权"。
        Fixture fixture(settingsPath(dir, QStringLiteral("corrupt-payload")));
        ExtensionEnablementLedgerStoreResult updated;
        QString errorCode;
        ExtensionEnablementLedgerStore store = fixture.store();
        expect(store.replace(sampleGrants(), 0, &updated, &errorCode),
               "the degradation fixture failed to commit");
        fixture.settings.setValue(
            ExtensionEnablementLedgerStore::recordSettingsKey(),
            QByteArrayLiteral("{\"schema\":\"x\"}"));
        fixture.settings.sync();
        const ExtensionEnablementLedgerStoreResult result = fixture.store().load();
        expect(result.state == ExtensionEnablementLedgerStoreState::Invalid
                   && result.errorCode.startsWith(
                       QStringLiteral("extension-enablement-ledger-"))
                   && result.grants.isEmpty(),
               "a corrupt grant payload degraded to empty");
    }
    {
        // 后端被锁定时当前内容未知，同样不是"没有授权"。
        Fixture fixture(settingsPath(dir, QStringLiteral("locked")));
        fixture.secure.readState = FakeSecureStore::ReadState::Unavailable;
        const ExtensionEnablementLedgerStoreResult result = fixture.store().load();
        expect(result.state == ExtensionEnablementLedgerStoreState::Unavailable
                   && result.grants.isEmpty(),
               "a locked backend degraded to empty");
    }
}

void replayTests(QTemporaryDir &dir)
{
    // 一份旧载荷当时是被合法签发过的，因此授权必须锚定已提交的代号与身份，否则把旧
    // 载荷放回原处就能让已经撤销的启用授权重新生效。
    Fixture fixture(settingsPath(dir, QStringLiteral("replay")));
    ExtensionEnablementLedgerStoreResult first;
    QString errorCode;
    ExtensionEnablementLedgerStore store = fixture.store();
    expect(store.replace(sampleGrants(), 0, &first, &errorCode),
           "the replay fixture failed its first commit");
    const QByteArray oldPayload = fixture.settings
        .value(ExtensionEnablementLedgerStore::recordSettingsKey()).toByteArray();

    // 撤销全部授权。
    ExtensionEnablementLedgerStoreResult revoked;
    ExtensionEnablementLedgerStore store2 = fixture.store();
    expect(store2.replace({}, 1, &revoked, &errorCode)
               && revoked.state == ExtensionEnablementLedgerStoreState::Ready
               && revoked.grants.isEmpty() && revoked.generation == 2,
           "revoking every grant failed");

    // 把旧载荷放回去：它自身仍然可认证，但不是授权提交的那一份。
    fixture.settings.setValue(
        ExtensionEnablementLedgerStore::recordSettingsKey(), oldPayload);
    fixture.settings.sync();
    const ExtensionEnablementLedgerStoreResult result = fixture.store().load();
    expect(result.state == ExtensionEnablementLedgerStoreState::Invalid
               && result.errorCode
                   == QStringLiteral("extension-enablement-store-record-superseded")
               && result.grants.isEmpty(),
           "a replayed grant payload revived revoked enablement");
}

void interruptionTests(QTemporaryDir &dir)
{
    // 阶段一（预留）失败：载荷字节还没有被改动，因此确定性回滚。
    {
        Fixture fixture(settingsPath(dir, QStringLiteral("reserve-failed")));
        fixture.secure.failWriteAt = 1;
        ExtensionEnablementLedgerStoreResult updated;
        QString errorCode;
        ExtensionEnablementLedgerStore store = fixture.store();
        expect(!store.replace(sampleGrants(), 0, &updated, &errorCode),
               "a failed reservation reported success");
        expect(!fixture.settings.contains(
                   ExtensionEnablementLedgerStore::recordSettingsKey()),
               "a failed reservation still wrote the grant payload");
        expect(fixture.store().load().state
                   == ExtensionEnablementLedgerStoreState::Empty,
               "a failed reservation left residue behind");
    }
    // 阶段三（完成提交）失败：载荷已经落盘，下一次读取应当依据落盘字节完成提交，
    // 而不是丢弃已经写下的内容。
    {
        Fixture fixture(settingsPath(dir, QStringLiteral("commit-failed")));
        fixture.secure.failWriteAt = 2;
        ExtensionEnablementLedgerStoreResult updated;
        QString errorCode;
        ExtensionEnablementLedgerStore store = fixture.store();
        expect(!store.replace(sampleGrants(), 0, &updated, &errorCode),
               "an unresolved commit reported success");
        // 恢复后端后，落盘的载荷与预留身份一致，因此提交被完成。
        fixture.secure.failWriteAt = -1;
        const ExtensionEnablementLedgerStoreResult recovered =
            fixture.store().load();
        expect(recovered.state == ExtensionEnablementLedgerStoreState::Ready
                   && recovered.generation == 1 && recovered.grants.size() == 2,
               "a landed payload was not promoted after an unresolved commit");
        // 恢复被持久化，而不是每次读取重新推断。
        const ExtensionEnablementLedgerStoreResult again = fixture.store().load();
        expect(again.state == ExtensionEnablementLedgerStoreState::Ready
                   && again.generation == 1,
               "the resolved reservation was not persisted");
    }
    // 预留解决本身也失败时结果未知，且不返回任何授权。
    {
        Fixture fixture(settingsPath(dir, QStringLiteral("unresolvable")));
        fixture.secure.failWriteAt = 2;
        ExtensionEnablementLedgerStoreResult updated;
        QString errorCode;
        ExtensionEnablementLedgerStore store = fixture.store();
        expect(!store.replace(sampleGrants(), 0, &updated, &errorCode),
               "an unresolved commit reported success");
        fixture.secure.failAllWrites = true;
        fixture.secure.failWriteOutcome =
            FakeSecureStore::WriteOutcome::OutcomeUnknown;
        const ExtensionEnablementLedgerStoreResult result = fixture.store().load();
        expect(result.state == ExtensionEnablementLedgerStoreState::OutcomeUnknown
                   && result.grants.isEmpty(),
               "an unresolvable reservation returned grants");
    }
}

void casTests(QTemporaryDir &dir)
{
    // 并发修改由比较并交换裁决，而不是最后写入者获胜：否则两次并发的启用/撤销会
    // 互相静默覆盖。
    Fixture fixture(settingsPath(dir, QStringLiteral("cas")));
    ExtensionEnablementLedgerStoreResult updated;
    QString errorCode;
    ExtensionEnablementLedgerStore store = fixture.store();
    expect(store.replace(sampleGrants(), 0, &updated, &errorCode),
           "the CAS fixture failed to commit");

    ExtensionEnablementLedgerStore stale = fixture.store();
    expect(!stale.replace({}, 0, &updated, &errorCode)
               && errorCode == QStringLiteral(
                   "extension-enablement-store-generation-conflict"),
           "a stale generation overwrote a newer grant set");

    // 不合法或重复的授权在写入任何东西之前被拒绝。
    const QByteArray before = fixture.secure.stored;
    const ExtensionEnablementGrant duplicated =
        grant(ExtensionKind::Skill, QStringLiteral("dup"), "d");
    ExtensionEnablementLedgerStore store2 = fixture.store();
    expect(!store2.replace({duplicated, duplicated}, 1, &updated, &errorCode)
               && errorCode == QStringLiteral(
                   "extension-enablement-store-grants-invalid"),
           "a duplicated grant was committed");
    expect(fixture.secure.stored == before,
           "a rejected grant set left a reservation behind");

    ExtensionEnablementGrant malformed = duplicated;
    malformed.contentIdentity = QStringLiteral("extension-content:sha256:zz");
    ExtensionEnablementLedgerStore store3 = fixture.store();
    expect(!store3.replace({malformed}, 1, &updated, &errorCode)
               && errorCode == QStringLiteral(
                   "extension-enablement-store-grants-invalid"),
           "a malformed grant was committed");

    ExtensionEnablementLedgerStore store4 = fixture.store();
    expect(!store4.replace({}, -1, &updated, &errorCode)
               && errorCode == QStringLiteral(
                   "extension-enablement-store-generation-invalid"),
           "a negative expected generation was accepted");
}

void domainSeparationTests(QTemporaryDir &dir)
{
    // 这是持久化层最关键的性质：一份复核授权信封与载荷不能被当作启用授权采用。否则
    // "我看过这份内容"就能被搬成"我要求运行这份内容"。
    const QString path = settingsPath(dir, QStringLiteral("cross-domain"));
    QSettings settings(path, QSettings::IniFormat);

    // 先建立一份完整的复核证据。
    FakeReviewSecureStore reviewSecure;
    ExtensionReviewLedgerStore reviewStore(&reviewSecure, &settings);
    QList<ExtensionReviewPin> pins;
    for (const ExtensionEnablementGrant &value : sampleGrants()) {
        ExtensionReviewPin pin;
        pin.kind = value.kind;
        pin.id = value.id;
        pin.sourceIdentity = value.sourceIdentity;
        pin.contentIdentity = value.contentIdentity;
        pins.append(pin);
    }
    ExtensionReviewLedgerStoreResult reviewResult;
    QString errorCode;
    expect(reviewStore.replace(pins, 0, &reviewResult, &errorCode),
           "the cross-domain review fixture failed to commit");

    // 复核证据存在时，启用授权仍然是"从未授权过"：两者的键与授权互不相干。
    FakeSecureStore enablementSecure;
    ExtensionEnablementLedgerStore enablementStore(&enablementSecure, &settings);
    expect(enablementStore.load().state
               == ExtensionEnablementLedgerStoreState::Empty,
           "review evidence was read as enablement grants");

    // 把复核的授权信封与载荷整体搬到启用授权的位置。
    enablementSecure.stored = reviewSecure.stored;
    settings.setValue(ExtensionEnablementLedgerStore::recordSettingsKey(),
                      settings.value(
                          ExtensionReviewLedgerStore::recordSettingsKey()));
    settings.sync();
    ExtensionEnablementLedgerStore adopted(&enablementSecure, &settings);
    const ExtensionEnablementLedgerStoreResult result = adopted.load();
    expect(result.state == ExtensionEnablementLedgerStoreState::Invalid
               && result.errorCode == QStringLiteral(
                   "extension-enablement-store-authority-invalid")
               && result.grants.isEmpty(),
           "a review authority envelope was adopted as enablement authority");

    // 复核证据自身没有被这次尝试破坏。
    ExtensionReviewLedgerStore reviewAgain(&reviewSecure, &settings);
    expect(reviewAgain.load().state == ExtensionReviewLedgerStoreState::Ready,
           "the cross-domain attempt damaged the review evidence");
}

void policyAgreementTests(QTemporaryDir &dir)
{
    // 持久化只搬字节：读出的授权仍然要经过启用判定，而判定还要求已复核、兼容且已
    // 安装。因此一份已提交的授权本身不启用任何东西。
    Fixture fixture(settingsPath(dir, QStringLiteral("policy")));
    const ExtensionEnablementGrant value =
        grant(ExtensionKind::Skill, QStringLiteral("fixture.skill"), "a");
    ExtensionEnablementLedgerStoreResult updated;
    QString errorCode;
    ExtensionEnablementLedgerStore store = fixture.store();
    expect(store.replace({value}, 0, &updated, &errorCode),
           "the policy fixture failed to commit");

    ExtensionRegistryRecord record;
    record.kind = value.kind;
    record.id = value.id;
    record.name = QStringLiteral("Sample");
    record.version = QStringLiteral("1.0.0");
    record.sourceIdentity = value.sourceIdentity;
    record.contentIdentity = value.contentIdentity;
    record.scope = QStringLiteral("user");
    record.installed = true;

    expect(!ExtensionEnablementPolicy::evaluate(record, updated.grants).enabled,
           "a persisted grant enabled an unreviewed record");

    record.trust = ExtensionTrustState::Verified;
    record.compatibility = ExtensionCompatibilityState::Compatible;
    expect(ExtensionEnablementPolicy::evaluate(record, updated.grants).enabled,
           "a persisted grant did not enable an eligible record");

    // 撤销之后同一条记录不再启用。
    ExtensionEnablementLedgerStoreResult revoked;
    ExtensionEnablementLedgerStore store2 = fixture.store();
    expect(store2.replace({}, updated.generation, &revoked, &errorCode),
           "revoking the persisted grant failed");
    expect(!ExtensionEnablementPolicy::evaluate(record, revoked.grants).enabled,
           "enablement survived grant revocation");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!dir.isValid()) {
        QTextStream(stderr) << "FAIL: temporary directory unavailable\n";
        return 1;
    }
    emptyAndFirstWriteTests(dir);
    degradationTests(dir);
    replayTests(dir);
    interruptionTests(dir);
    casTests(dir);
    domainSeparationTests(dir);
    policyAgreementTests(dir);
    if (failures == 0) {
        QTextStream(stdout) << "extension enablement ledger store tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
