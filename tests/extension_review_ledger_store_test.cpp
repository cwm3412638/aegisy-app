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

// 可注入的安全存储：写入可以在任意一次调用上被打断，用来驱动两阶段提交的
// 每一个中断点。
class FakeSecureStore final : public ExtensionReviewLedgerSecureStore
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

QString digest(const QString &prefix, const QByteArray &seed)
{
    return prefix + QString::fromLatin1(
        QCryptographicHash::hash(seed, QCryptographicHash::Sha256).toHex());
}

ExtensionReviewPin pin(ExtensionKind kind, const QString &id, const QByteArray &seed)
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

QList<ExtensionReviewPin> samplePins()
{
    return {pin(ExtensionKind::Skill, QStringLiteral("fixture.skill"), "a"),
            pin(ExtensionKind::Mcp, QStringLiteral("fixture.mcp"), "b")};
}

struct Fixture {
    explicit Fixture(const QString &path)
        : settings(path, QSettings::IniFormat)
    {
    }

    FakeSecureStore secure;
    QSettings settings;

    ExtensionReviewLedgerStore store()
    {
        return ExtensionReviewLedgerStore(&secure, &settings);
    }
};

QString settingsPath(QTemporaryDir &dir, const QString &name)
{
    return dir.filePath(name + QStringLiteral(".ini"));
}

void emptyAndFirstWriteTests(QTemporaryDir &dir)
{
    Fixture fixture(settingsPath(dir, QStringLiteral("first")));
    ExtensionReviewLedgerStore store = fixture.store();

    const ExtensionReviewLedgerStoreResult initial = store.load();
    expect(initial.state == ExtensionReviewLedgerStoreState::Empty
               && initial.generation == 0 && initial.pins.isEmpty()
               && initial.errorCode.isEmpty(),
           "a fresh store did not report empty");

    // 首次提交必须声明"我读到的是不存在"，也就是代号 0。
    ExtensionReviewLedgerStoreResult updated;
    QString errorCode;
    if (!expect(store.replace(samplePins(), 0, &updated, &errorCode),
                "the first review set could not be committed")) {
        QTextStream(stderr) << "  error: " << errorCode << '\n';
        return;
    }
    expect(updated.state == ExtensionReviewLedgerStoreState::Ready
               && updated.generation == 1 && updated.pins.size() == 2
               && !updated.identity.isEmpty(),
           "the first commit did not report the new set");

    const ExtensionReviewLedgerStoreResult reloaded = store.load();
    expect(reloaded.state == ExtensionReviewLedgerStoreState::Ready
               && reloaded.generation == 1 && reloaded.pins.size() == 2
               && reloaded.identity == updated.identity
               && reloaded.pins.at(0).contentIdentity
                   == samplePins().at(0).contentIdentity,
           "the committed review set did not survive a reload");

    // 密钥只存在于安全存储里，绝不能出现在 QSettings 中。
    fixture.settings.sync();
    const QByteArray recordBytes = fixture.settings
        .value(ExtensionReviewLedgerStore::recordSettingsKey()).toByteArray();
    const QJsonObject authority =
        QJsonDocument::fromJson(fixture.secure.stored).object();
    const QString keyEncoded =
        authority.value(QStringLiteral("hmac_key_base64")).toString();
    expect(!keyEncoded.isEmpty() && !recordBytes.contains(keyEncoded.toLatin1()),
           "the review ledger MAC key leaked into ordinary settings");
    expect(!authority.contains(QStringLiteral("pins"))
               && !fixture.secure.stored.contains("extension-content:sha256:"),
           "review pins were duplicated into the secure authority");

    // 后续提交必须携带当前代号，并且代号单调前进。
    QList<ExtensionReviewPin> extended = samplePins();
    extended.append(pin(ExtensionKind::CodexPlugin,
                        QStringLiteral("fixture.plugin"), "c"));
    ExtensionReviewLedgerStoreResult second;
    expect(store.replace(extended, 1, &second, &errorCode)
               && second.generation == 2 && second.pins.size() == 3
               && second.identity != updated.identity,
           "a second review commit did not advance the generation");

    // 用过期的代号提交必须失败，否则并发复核会静默覆盖彼此。
    expect(!store.replace(samplePins(), 1, nullptr, &errorCode)
               && errorCode
                   == QStringLiteral("extension-review-store-generation-conflict"),
           "a stale generation was allowed to overwrite a newer review set");
    expect(store.load().generation == 2,
           "a rejected commit still changed the stored review set");

    // 把复核集合清空是一次正常提交，不是删除。
    ExtensionReviewLedgerStoreResult cleared;
    expect(store.replace({}, 2, &cleared, &errorCode)
               && cleared.state == ExtensionReviewLedgerStoreState::Ready
               && cleared.pins.isEmpty() && cleared.generation == 3,
           "clearing the review set was not a normal commit");
    const ExtensionReviewLedgerStoreResult afterClear = store.load();
    expect(afterClear.state == ExtensionReviewLedgerStoreState::Ready
               && afterClear.pins.isEmpty() && afterClear.generation == 3,
           "a cleared review set degraded to empty");
}

void degradationTests(QTemporaryDir &dir)
{
    Fixture fixture(settingsPath(dir, QStringLiteral("degrade")));
    ExtensionReviewLedgerStore store = fixture.store();
    QString errorCode;
    if (!expect(store.replace(samplePins(), 0, nullptr, &errorCode),
                "the degradation fixture could not be committed")) {
        return;
    }
    const QByteArray authorityBackup = fixture.secure.stored;
    fixture.settings.sync();
    const QByteArray recordBackup = fixture.settings
        .value(ExtensionReviewLedgerStore::recordSettingsKey()).toByteArray();

    // 删除载荷字节不能变成"从未复核过"：授权仍然锚定一份载荷。
    fixture.settings.remove(ExtensionReviewLedgerStore::recordSettingsKey());
    fixture.settings.sync();
    ExtensionReviewLedgerStoreResult result = store.load();
    expect(result.state == ExtensionReviewLedgerStoreState::Invalid
               && result.errorCode
                   == QStringLiteral("extension-review-store-record-deleted")
               && result.pins.isEmpty(),
           "deleting the payload degraded the store to empty");

    // 删除授权同样不能变成空白：孤立载荷说明有人拿掉了认证依据。
    fixture.settings.setValue(ExtensionReviewLedgerStore::recordSettingsKey(),
                              recordBackup);
    fixture.settings.sync();
    fixture.secure.stored.clear();
    result = store.load();
    expect(result.state == ExtensionReviewLedgerStoreState::Invalid
               && result.errorCode
                   == QStringLiteral("extension-review-store-record-without-authority"),
           "deleting the authority degraded the store to empty");

    // 只有两半都不存在才是真正的空白。
    fixture.settings.remove(ExtensionReviewLedgerStore::recordSettingsKey());
    fixture.settings.sync();
    expect(store.load().state == ExtensionReviewLedgerStoreState::Empty,
           "an genuinely absent store did not report empty");

    // 损坏的载荷是 Invalid，不是 Empty，也不是缺失。
    fixture.secure.stored = authorityBackup;
    fixture.settings.setValue(ExtensionReviewLedgerStore::recordSettingsKey(),
                              QByteArrayLiteral("not json"));
    fixture.settings.sync();
    result = store.load();
    expect(result.state == ExtensionReviewLedgerStoreState::Invalid
               && result.errorCode.startsWith(
                   QStringLiteral("extension-review-ledger-")),
           "a corrupt payload did not report the ledger's own error code");

    // 类型不合法的载荷同样是"存在且损坏"。
    fixture.settings.setValue(ExtensionReviewLedgerStore::recordSettingsKey(),
                              QStringLiteral("payload"));
    fixture.settings.sync();
    expect(store.load().state == ExtensionReviewLedgerStoreState::Invalid,
           "a wrongly typed payload did not report invalid");

    // 后端被锁定时当前内容未知，不是"没有复核"。
    fixture.settings.setValue(ExtensionReviewLedgerStore::recordSettingsKey(),
                              recordBackup);
    fixture.settings.sync();
    fixture.secure.readState = ExtensionReviewLedgerSecureStore::ReadState::Unavailable;
    result = store.load();
    expect(result.state == ExtensionReviewLedgerStoreState::Unavailable
               && result.pins.isEmpty(),
           "a locked backend did not resolve to unavailable");
    fixture.secure.readState = ExtensionReviewLedgerSecureStore::ReadState::Invalid;
    expect(store.load().state == ExtensionReviewLedgerStoreState::Invalid,
           "an invalid backend did not resolve to invalid");
    fixture.secure.readState = ExtensionReviewLedgerSecureStore::ReadState::Found;

    // 授权本身被改坏时不能落回任何可用的复核记录。
    fixture.secure.stored = QByteArrayLiteral("{\"schema_version\":\"x\"}");
    result = store.load();
    expect(result.state == ExtensionReviewLedgerStoreState::Invalid
               && result.errorCode
                   == QStringLiteral("extension-review-store-authority-invalid"),
           "a corrupt authority did not report invalid");

    // 写入路径同样不能在不确定状态下继续。
    expect(!store.replace(samplePins(), 0, nullptr, &errorCode)
               && errorCode
                   == QStringLiteral("extension-review-store-authority-invalid"),
           "a commit proceeded against a corrupt authority");
}

void rollbackTests(QTemporaryDir &dir)
{
    // 一份旧载荷曾经被合法签发过。把它放回原处不能让已撤销的复核重新生效。
    Fixture fixture(settingsPath(dir, QStringLiteral("rollback")));
    ExtensionReviewLedgerStore store = fixture.store();
    QString errorCode;
    if (!expect(store.replace(samplePins(), 0, nullptr, &errorCode),
                "the rollback fixture could not be committed")) {
        return;
    }
    fixture.settings.sync();
    const QByteArray oldRecord = fixture.settings
        .value(ExtensionReviewLedgerStore::recordSettingsKey()).toByteArray();
    if (!expect(store.replace({}, 1, nullptr, &errorCode),
                "the rollback fixture could not revoke its reviews")) {
        return;
    }
    fixture.settings.setValue(ExtensionReviewLedgerStore::recordSettingsKey(),
                              oldRecord);
    fixture.settings.sync();
    const ExtensionReviewLedgerStoreResult result = store.load();
    expect(result.state == ExtensionReviewLedgerStoreState::Invalid
               && result.errorCode
                   == QStringLiteral("extension-review-store-record-superseded")
               && result.pins.isEmpty(),
           "a superseded payload was replayed as a valid review set");
}

void interruptionTests(QTemporaryDir &dir)
{
    const QList<ExtensionReviewPin> pins = samplePins();

    // 预留阶段失败：载荷字节还没有被改动，因此内容必须完全不变。
    {
        Fixture fixture(settingsPath(dir, QStringLiteral("reserve")));
        ExtensionReviewLedgerStore store = fixture.store();
        QString errorCode;
        if (!expect(store.replace(pins, 0, nullptr, &errorCode),
                    "the reserve fixture could not be committed")) {
            return;
        }
        const ExtensionReviewLedgerStoreResult before = store.load();
        fixture.secure.writes = 0;
        fixture.secure.failWriteAt = 1;
        QList<ExtensionReviewPin> replacement{
            pin(ExtensionKind::Skill, QStringLiteral("other.skill"), "z")};
        expect(!store.replace(replacement, 1, nullptr, &errorCode),
               "an interrupted reservation reported success");
        fixture.secure.failWriteAt = -1;
        const ExtensionReviewLedgerStoreResult after = store.load();
        expect(after.state == ExtensionReviewLedgerStoreState::Ready
                   && after.generation == before.generation
                   && after.identity == before.identity
                   && after.pins.size() == before.pins.size(),
               "an interrupted reservation changed the effective review set");
    }

    // 完成提交阶段失败：载荷已经落盘，因此下一次 load() 必须确定性地完成提交，
    // 既不丢内容也不需要人工推断。
    {
        Fixture fixture(settingsPath(dir, QStringLiteral("commit")));
        ExtensionReviewLedgerStore store = fixture.store();
        QString errorCode;
        if (!expect(store.replace(pins, 0, nullptr, &errorCode),
                    "the commit fixture could not be seeded")) {
            return;
        }
        QList<ExtensionReviewPin> replacement{
            pin(ExtensionKind::Skill, QStringLiteral("other.skill"), "z")};
        fixture.secure.writes = 0;
        // 第一次写入是预留，第二次是完成提交。
        fixture.secure.failWriteAt = 2;
        expect(!store.replace(replacement, 1, nullptr, &errorCode)
                   && errorCode == QStringLiteral("fake-write-failed"),
               "an interrupted commit reported success");
        fixture.secure.failWriteAt = -1;
        const ExtensionReviewLedgerStoreResult recovered = store.load();
        expect(recovered.state == ExtensionReviewLedgerStoreState::Ready
                   && recovered.generation == 2
                   && recovered.pins.size() == 1
                   && recovered.pins.at(0).id == QStringLiteral("other.skill"),
               "an interrupted commit did not finish deterministically on reload");
        // 恢复必须是持久的，而不是每次读取时重新推断。
        const ExtensionReviewLedgerStoreResult again = store.load();
        expect(again.state == ExtensionReviewLedgerStoreState::Ready
                   && again.generation == 2 && again.identity == recovered.identity,
               "the recovered commit was not persisted");
        // 恢复之后必须能继续正常提交。
        expect(store.replace(pins, 2, nullptr, &errorCode),
               "the store could not continue after recovering an interrupted commit");
    }

    // 载荷写入阶段失败（预留已生效，字节未落盘）：必须回滚到上一份，而不是
    // 停在预留状态或丢失内容。
    {
        Fixture fixture(settingsPath(dir, QStringLiteral("payload")));
        ExtensionReviewLedgerStore store = fixture.store();
        QString errorCode;
        if (!expect(store.replace(pins, 0, nullptr, &errorCode),
                    "the payload fixture could not be seeded")) {
            return;
        }
        const ExtensionReviewLedgerStoreResult before = store.load();
        fixture.settings.sync();
        const QByteArray recordBackup = fixture.settings
            .value(ExtensionReviewLedgerStore::recordSettingsKey()).toByteArray();
        // 手工构造"预留已写入但字节未更新"的中断点。
        QJsonObject authority =
            QJsonDocument::fromJson(fixture.secure.stored).object();
        authority.insert(QStringLiteral("reserved"), QJsonObject{
            {QStringLiteral("generation"), before.generation + 1},
            {QStringLiteral("identity"),
             QStringLiteral("extension-review-ledger:sha256:")
                 + QString(64, QLatin1Char('a'))},
        });
        fixture.secure.stored =
            QJsonDocument(authority).toJson(QJsonDocument::Compact);
        const ExtensionReviewLedgerStoreResult rolled = store.load();
        expect(rolled.state == ExtensionReviewLedgerStoreState::Ready
                   && rolled.generation == before.generation
                   && rolled.identity == before.identity,
               "an unlanded payload write did not roll back to the previous set");
        expect(!QJsonDocument::fromJson(fixture.secure.stored).object()
                    .value(QStringLiteral("reserved")).isObject(),
               "the rolled-back reservation was not cleared from the authority");
        expect(fixture.settings
                   .value(ExtensionReviewLedgerStore::recordSettingsKey())
                   .toByteArray() == recordBackup,
               "the rollback altered the payload bytes");
    }

    // 预留无法解决时结果未知，绝不能报告成一份可用的复核集合。
    {
        Fixture fixture(settingsPath(dir, QStringLiteral("unresolved")));
        ExtensionReviewLedgerStore store = fixture.store();
        QString errorCode;
        if (!expect(store.replace(pins, 0, nullptr, &errorCode),
                    "the unresolved fixture could not be seeded")) {
            return;
        }
        QJsonObject authority =
            QJsonDocument::fromJson(fixture.secure.stored).object();
        authority.insert(QStringLiteral("reserved"), QJsonObject{
            {QStringLiteral("generation"), 2},
            {QStringLiteral("identity"),
             QStringLiteral("extension-review-ledger:sha256:")
                 + QString(64, QLatin1Char('b'))},
        });
        fixture.secure.stored =
            QJsonDocument(authority).toJson(QJsonDocument::Compact);
        fixture.secure.failAllWrites = true;
        fixture.secure.failWriteOutcome =
            ExtensionReviewLedgerSecureStore::WriteOutcome::OutcomeUnknown;
        const ExtensionReviewLedgerStoreResult result = store.load();
        expect(result.state == ExtensionReviewLedgerStoreState::OutcomeUnknown
                   && result.pins.isEmpty(),
               "an unresolvable reservation reported a usable review set");
        expect(!store.replace(pins, 1, nullptr, &errorCode),
               "a commit proceeded while the reservation was unresolved");
        // 后端恢复之后同一次中断必须仍然确定性地解决。
        fixture.secure.failAllWrites = false;
        fixture.secure.failWriteOutcome =
            ExtensionReviewLedgerSecureStore::WriteOutcome::DefiniteFailure;
        const ExtensionReviewLedgerStoreResult healed = store.load();
        expect(healed.state == ExtensionReviewLedgerStoreState::Ready
                   && healed.generation == 1,
               "the reservation did not resolve once the backend recovered");
    }
}

void authorityShapeTests(QTemporaryDir &dir)
{
    Fixture fixture(settingsPath(dir, QStringLiteral("shape")));
    ExtensionReviewLedgerStore store = fixture.store();
    QString errorCode;
    if (!expect(store.replace(samplePins(), 0, nullptr, &errorCode),
                "the shape fixture could not be committed")) {
        return;
    }
    const QByteArray backup = fixture.secure.stored;

    const auto withAuthority = [&](const QString &field, const QJsonValue &value) {
        QJsonObject object = QJsonDocument::fromJson(backup).object();
        if (value.isUndefined()) {
            object.remove(field);
        } else {
            object.insert(field, value);
        }
        fixture.secure.stored = QJsonDocument(object).toJson(QJsonDocument::Compact);
        return store.load();
    };

    // 授权的键集合必须精确匹配，代号与身份必须成对出现。
    expect(withAuthority(QStringLiteral("extra"), 1).state
               == ExtensionReviewLedgerStoreState::Invalid,
           "an authority with unknown fields was accepted");
    expect(withAuthority(QStringLiteral("committed_identity"), QJsonValue()).state
               == ExtensionReviewLedgerStoreState::Invalid,
           "an authority missing its identity was accepted");
    expect(withAuthority(QStringLiteral("committed_identity"), QString()).state
               == ExtensionReviewLedgerStoreState::Invalid,
           "an authority with a generation but no identity was accepted");
    expect(withAuthority(QStringLiteral("committed_generation"), 0).state
               == ExtensionReviewLedgerStoreState::Invalid,
           "an authority with an identity but no generation was accepted");
    expect(withAuthority(QStringLiteral("committed_generation"), 1.5).state
               == ExtensionReviewLedgerStoreState::Invalid,
           "an authority with a non-integer generation was accepted");
    expect(withAuthority(QStringLiteral("committed_generation"), -1).state
               == ExtensionReviewLedgerStoreState::Invalid,
           "an authority with a negative generation was accepted");
    expect(withAuthority(QStringLiteral("schema_version"),
                         QStringLiteral("aegisy-extension-review-ledger-authority/0.2"))
               .state == ExtensionReviewLedgerStoreState::Invalid,
           "an authority with a drifted schema was accepted");
    // 非规范 base64 密钥必须被拒绝：它意味着授权字节被改动过。
    expect(withAuthority(QStringLiteral("hmac_key_base64"),
                         QStringLiteral("AAAA")).state
               == ExtensionReviewLedgerStoreState::Invalid,
           "an authority with a short key was accepted");
    // 预留代号必须严格前进。
    expect(withAuthority(QStringLiteral("reserved"), QJsonObject{
                             {QStringLiteral("generation"), 1},
                             {QStringLiteral("identity"), QStringLiteral("x")}}).state
               == ExtensionReviewLedgerStoreState::Invalid,
           "a reservation that does not advance the generation was accepted");
    expect(withAuthority(QStringLiteral("reserved"), QJsonObject{
                             {QStringLiteral("generation"), 2}}).state
               == ExtensionReviewLedgerStoreState::Invalid,
           "a reservation missing its identity was accepted");

    fixture.secure.stored = backup;
    expect(store.load().state == ExtensionReviewLedgerStoreState::Ready,
           "the shape fixture did not survive its own restoration");
}

void guardTests(QTemporaryDir &dir)
{
    Fixture fixture(settingsPath(dir, QStringLiteral("guards")));
    QString errorCode;

    ExtensionReviewLedgerStore missingStore(nullptr, &fixture.settings);
    expect(missingStore.load().state == ExtensionReviewLedgerStoreState::Unavailable,
           "a store without a secure backend did not report unavailable");
    expect(!missingStore.replace(samplePins(), 0, nullptr, &errorCode),
           "a store without a secure backend accepted a commit");

    ExtensionReviewLedgerStore store = fixture.store();
    expect(!store.replace(samplePins(), -1, nullptr, &errorCode)
               && errorCode
                   == QStringLiteral("extension-review-store-generation-invalid"),
           "a negative expected generation was accepted");
    // 不合法的复核记录不能被写入，也不能留下预留残余。
    ExtensionReviewPin broken =
        pin(ExtensionKind::Skill, QStringLiteral("fixture.skill"), "a");
    broken.contentIdentity = QStringLiteral("extension-content:sha256:nope");
    expect(!store.replace({broken}, 0, nullptr, &errorCode)
               && errorCode == QStringLiteral("extension-review-store-pins-invalid"),
           "a malformed review pin was persisted");
    expect(store.load().state == ExtensionReviewLedgerStoreState::Empty,
           "a rejected commit left the store non-empty");
    // 重复项同样被拒绝在写入之前。
    const ExtensionReviewPin duplicate =
        pin(ExtensionKind::Skill, QStringLiteral("fixture.skill"), "a");
    expect(!store.replace({duplicate, duplicate}, 0, nullptr, &errorCode)
               && errorCode == QStringLiteral("extension-review-store-pins-invalid"),
           "duplicate review pins were persisted");
}

void trustAgreementTests(QTemporaryDir &dir)
{
    // 持久化层交出的复核记录必须能被信任判定直接采纳。
    Fixture fixture(settingsPath(dir, QStringLiteral("trust")));
    ExtensionReviewLedgerStore store = fixture.store();
    const QList<ExtensionReviewPin> pins = samplePins();
    QString errorCode;
    if (!expect(store.replace(pins, 0, nullptr, &errorCode),
                "the trust fixture could not be committed")) {
        return;
    }
    const ExtensionReviewLedgerStoreResult loaded = store.load();
    if (!expect(loaded.state == ExtensionReviewLedgerStoreState::Ready,
                "the trust fixture did not reload")) {
        return;
    }

    ExtensionRegistryRecord record;
    record.kind = pins.at(0).kind;
    record.id = pins.at(0).id;
    record.name = QStringLiteral("Sample");
    record.version = QStringLiteral("1.0.0");
    record.sourceIdentity = pins.at(0).sourceIdentity;
    record.contentIdentity = pins.at(0).contentIdentity;
    record.scope = QStringLiteral("user");
    record.installed = true;

    expect(ExtensionTrustPolicy::evaluate(record, loaded.pins).state
               == ExtensionTrustState::Verified,
           "persisted review pins were not accepted by the trust policy");

    // 撤销之后同一条记录必须立刻失去信任。
    if (!expect(store.replace({}, loaded.generation, nullptr, &errorCode),
                "the trust fixture could not revoke its reviews")) {
        return;
    }
    const ExtensionReviewLedgerStoreResult revoked = store.load();
    expect(ExtensionTrustPolicy::evaluate(record, revoked.pins).state
               == ExtensionTrustState::Unverified,
           "a revoked review still verified the record");
    // 不可用状态同样不能交出可用的复核记录。
    fixture.secure.readState = ExtensionReviewLedgerSecureStore::ReadState::Unavailable;
    const ExtensionReviewLedgerStoreResult blocked = store.load();
    expect(blocked.pins.isEmpty()
               && ExtensionTrustPolicy::evaluate(record, blocked.pins).state
                   == ExtensionTrustState::Unverified,
           "an unavailable store produced usable review pins");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    if (!dir.isValid()) {
        QTextStream(stderr) << "FAIL: cannot create an isolated settings root\n";
        return 1;
    }
    emptyAndFirstWriteTests(dir);
    degradationTests(dir);
    rollbackTests(dir);
    interruptionTests(dir);
    authorityShapeTests(dir);
    guardTests(dir);
    trustAgreementTests(dir);
    if (failures == 0) {
        QTextStream(stdout) << "extension review ledger store tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
