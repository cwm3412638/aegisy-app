#include "extension_staging_restore_audit_ledger_store.h"

#include "extension_enablement_ledger_store.h"
#include "extension_review_ledger_store.h"

#include <QCoreApplication>
#include <QCryptographicHash>
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
class FakeSecureStore final : public ExtensionStagingRestoreAuditSecureStore
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
                         ? QStringLiteral("unavailable")
                         : QStringLiteral("invalid"));
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

// 复核证据侧与启用授权侧的安全存储，仅用于跨域检验。
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

class FakeEnablementSecureStore final
    : public ExtensionEnablementLedgerSecureStore
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

ExtensionStagingRestoreAuditEntry entry(const QString &subject,
                                        const QString &backupId,
                                        const QByteArray &seed, int minute)
{
    ExtensionStagingRestoreAuditEntry value;
    value.subject = subject;
    value.backupId = backupId;
    value.destinationRoot = QStringLiteral("/tmp/restore-target");
    value.planIdentity =
        digest(QStringLiteral("extension-staging-restore-plan:sha256:"),
               seed + "-plan");
    value.treeIdentity = subject.startsWith(QStringLiteral("mcp:"))
        ? digest(QStringLiteral("mcp-backup-content:sha256:"), seed + "-tree")
        : digest(QStringLiteral("extension-content:sha256:"), seed + "-tree");
    value.warnings = {ExtensionStagingRestoreWarning::DestinationNotEmpty,
                      ExtensionStagingRestoreWarning::RestoreDoesNotExecuteYet};
    value.decision = ExtensionStagingRestoreAuditDecision::Approved;
    value.decidedAt = QDateTime::fromString(
        QStringLiteral("2026-09-05T12:%1:00.000Z")
            .arg(minute, 2, 10, QLatin1Char('0')),
        Qt::ISODateWithMs);
    return value;
}

QList<ExtensionStagingRestoreAuditEntry> sampleEntries()
{
    ExtensionStagingRestoreAuditEntry declined =
        entry(QStringLiteral("mcp:fixture.server"), QStringLiteral("backup-b"),
              "b", 5);
    declined.decision = ExtensionStagingRestoreAuditDecision::Declined;
    declined.warnings = {
        ExtensionStagingRestoreWarning::SharedSettingsFileRestore,
        ExtensionStagingRestoreWarning::RestoreDoesNotExecuteYet};
    return {entry(QStringLiteral("skill:fixture.skill"),
                  QStringLiteral("backup-a"), "a", 0),
            declined};
}

struct Fixture {
    explicit Fixture(const QString &path)
        : settings(path, QSettings::IniFormat)
    {
    }

    FakeSecureStore secure;
    QSettings settings;

    ExtensionStagingRestoreAuditLedgerStore store()
    {
        return ExtensionStagingRestoreAuditLedgerStore(&secure, &settings);
    }
};

QString settingsPath(QTemporaryDir &dir, const QString &name)
{
    return dir.filePath(name + QStringLiteral(".ini"));
}

void emptyAndFirstWriteTests(QTemporaryDir &dir)
{
    Fixture fixture(settingsPath(dir, QStringLiteral("first")));

    // 两半都不存在才是"从未记录过任何决定"。
    const ExtensionStagingRestoreAuditStoreResult initial =
        fixture.store().load();
    expect(initial.state == ExtensionStagingRestoreAuditStoreState::Empty
               && initial.entries.isEmpty() && initial.generation == 0
               && initial.errorCode.isEmpty(),
           "a fresh install was not reported as empty");

    ExtensionStagingRestoreAuditStoreResult updated;
    QString errorCode;
    ExtensionStagingRestoreAuditLedgerStore store = fixture.store();
    expect(store.replace(sampleEntries(), 0, &updated, &errorCode)
               && updated.state == ExtensionStagingRestoreAuditStoreState::Ready
               && updated.generation == 1
               && updated.entries.size() == 2 && errorCode.isEmpty(),
           "the first audit commit failed");
    // 批准与拒绝都被记录。
    expect(updated.entries.at(0).decision
                   == ExtensionStagingRestoreAuditDecision::Approved
               && updated.entries.at(1).decision
                   == ExtensionStagingRestoreAuditDecision::Declined,
           "the audit log did not record both decisions");

    // 密钥只存在于安全存储里，绝不进入普通设置。
    expect(!fixture.secure.stored.isEmpty(),
           "the authority envelope was not persisted");
    const QByteArray recordBytes = fixture.settings
        .value(ExtensionStagingRestoreAuditLedgerStore::recordSettingsKey())
        .toByteArray();
    expect(!recordBytes.isEmpty(), "the audit payload was not persisted");
    expect(!recordBytes.contains(QByteArrayLiteral("hmac_key_base64")),
           "the payload carries the authority key");

    const ExtensionStagingRestoreAuditStoreResult reloaded =
        fixture.store().load();
    expect(reloaded.state == ExtensionStagingRestoreAuditStoreState::Ready
               && reloaded.generation == 1 && reloaded.entries.size() == 2
               && reloaded.identity == updated.identity,
           "a committed audit log did not reload");

    // 载荷键必须与复核证据、启用授权的键都不同，否则各类记录会互相覆盖。
    expect(ExtensionStagingRestoreAuditLedgerStore::recordSettingsKey()
                   != ExtensionReviewLedgerStore::recordSettingsKey()
               && ExtensionStagingRestoreAuditLedgerStore::recordSettingsKey()
                   != ExtensionEnablementLedgerStore::recordSettingsKey(),
           "the audit payload shares its settings key with another ledger");
}

void appendSemanticsTests(QTemporaryDir &dir)
{
    // 追加即"读出当前集合、末尾追加、连同读到的代号整体提交"：代号严格单调，
    // 历史完整保留。
    Fixture fixture(settingsPath(dir, QStringLiteral("append")));
    ExtensionStagingRestoreAuditStoreResult updated;
    QString errorCode;
    ExtensionStagingRestoreAuditLedgerStore store = fixture.store();
    expect(store.replace({sampleEntries().first()}, 0, &updated, &errorCode)
               && updated.generation == 1,
           "the append fixture failed its first commit");

    const ExtensionStagingRestoreAuditStoreResult current =
        fixture.store().load();
    QList<ExtensionStagingRestoreAuditEntry> next = current.entries;
    next.append(entry(QStringLiteral("mcp:fixture.server"),
                      QStringLiteral("backup-b"), "b", 5));
    ExtensionStagingRestoreAuditLedgerStore store2 = fixture.store();
    expect(store2.replace(next, current.generation, &updated, &errorCode)
               && updated.generation == 2 && updated.entries.size() == 2,
           "an append did not advance the generation monotonically");

    // 已认证的空日志是"已审计、尚无记录"，与"从未建立账本"严格区分。
    Fixture emptyFixture(settingsPath(dir, QStringLiteral("audited-empty")));
    ExtensionStagingRestoreAuditLedgerStore emptyStore = emptyFixture.store();
    expect(emptyStore.replace({}, 0, &updated, &errorCode)
               && updated.state == ExtensionStagingRestoreAuditStoreState::Ready
               && updated.entries.isEmpty() && updated.generation == 1
               && !updated.identity.isEmpty(),
           "an authenticated empty audit log was not committed");
    const ExtensionStagingRestoreAuditStoreResult reloaded =
        emptyFixture.store().load();
    expect(reloaded.state == ExtensionStagingRestoreAuditStoreState::Ready
               && reloaded.entries.isEmpty() && reloaded.generation == 1,
           "an authenticated empty audit log did not survive a reload");
}

void capTests(QTemporaryDir &dir)
{
    // 集合有界：写满后以独立代号拒绝新条目，绝不静默驱逐历史。
    Fixture fixture(settingsPath(dir, QStringLiteral("cap")));
    QList<ExtensionStagingRestoreAuditEntry> full;
    for (int index = 0;
         index < ExtensionStagingRestoreAuditLedgerStore::MaxEntries;
         ++index) {
        full.append(entry(QStringLiteral("skill:fixture.skill"),
                          QStringLiteral("backup-%1").arg(index), "a",
                          index % 60));
    }
    ExtensionStagingRestoreAuditStoreResult updated;
    QString errorCode;
    ExtensionStagingRestoreAuditLedgerStore store = fixture.store();
    expect(store.replace(full, 0, &updated, &errorCode)
               && updated.entries.size()
                   == ExtensionStagingRestoreAuditLedgerStore::MaxEntries,
           "a full audit log was rejected");
    QList<ExtensionStagingRestoreAuditEntry> over = full;
    over.append(sampleEntries().first());
    ExtensionStagingRestoreAuditLedgerStore store2 = fixture.store();
    expect(!store2.replace(over, 1, &updated, &errorCode)
               && errorCode == QStringLiteral(
                   "extension-restore-audit-store-entries-cap"),
           "an over-cap audit log did not fail with the cap code");
    // 历史一字节未动。
    const ExtensionStagingRestoreAuditStoreResult reloaded =
        fixture.store().load();
    expect(reloaded.state == ExtensionStagingRestoreAuditStoreState::Ready
               && reloaded.entries.size()
                   == ExtensionStagingRestoreAuditLedgerStore::MaxEntries,
           "a rejected over-cap write disturbed the recorded history");
}

void degradationTests(QTemporaryDir &dir)
{
    // 删除任意一半都不是"从未记录过"：那会把一次篡改表述成这个问题从未被问过。
    {
        Fixture fixture(settingsPath(dir, QStringLiteral("orphan-payload")));
        ExtensionStagingRestoreAuditStoreResult updated;
        QString errorCode;
        ExtensionStagingRestoreAuditLedgerStore store = fixture.store();
        expect(store.replace(sampleEntries(), 0, &updated, &errorCode),
               "the degradation fixture failed to commit");
        fixture.secure.stored.clear();
        const ExtensionStagingRestoreAuditStoreResult result =
            fixture.store().load();
        expect(result.state == ExtensionStagingRestoreAuditStoreState::Invalid
                   && result.errorCode == QStringLiteral(
                       "extension-restore-audit-store-record-without-authority")
                   && result.entries.isEmpty(),
               "an orphaned audit payload degraded to empty");
    }
    {
        Fixture fixture(settingsPath(dir, QStringLiteral("orphan-authority")));
        ExtensionStagingRestoreAuditStoreResult updated;
        QString errorCode;
        ExtensionStagingRestoreAuditLedgerStore store = fixture.store();
        expect(store.replace(sampleEntries(), 0, &updated, &errorCode),
               "the degradation fixture failed to commit");
        fixture.settings.remove(
            ExtensionStagingRestoreAuditLedgerStore::recordSettingsKey());
        fixture.settings.sync();
        const ExtensionStagingRestoreAuditStoreResult result =
            fixture.store().load();
        expect(result.state == ExtensionStagingRestoreAuditStoreState::Invalid
                   && result.errorCode == QStringLiteral(
                       "extension-restore-audit-store-record-deleted")
                   && result.entries.isEmpty(),
               "a deleted audit payload degraded to empty");
    }
    {
        // 载荷损坏时报告载荷层自己的代码，而不是"没有记录"。
        Fixture fixture(settingsPath(dir, QStringLiteral("corrupt-payload")));
        ExtensionStagingRestoreAuditStoreResult updated;
        QString errorCode;
        ExtensionStagingRestoreAuditLedgerStore store = fixture.store();
        expect(store.replace(sampleEntries(), 0, &updated, &errorCode),
               "the degradation fixture failed to commit");
        fixture.settings.setValue(
            ExtensionStagingRestoreAuditLedgerStore::recordSettingsKey(),
            QByteArrayLiteral("{\"schema\":\"x\"}"));
        fixture.settings.sync();
        const ExtensionStagingRestoreAuditStoreResult result =
            fixture.store().load();
        expect(result.state == ExtensionStagingRestoreAuditStoreState::Invalid
                   && result.errorCode.startsWith(
                       QStringLiteral("extension-restore-audit-ledger-"))
                   && result.entries.isEmpty(),
               "a corrupt audit payload degraded to empty");
    }
    {
        // 后端被锁定时当前内容未知，同样不是"没有记录"。
        Fixture fixture(settingsPath(dir, QStringLiteral("locked")));
        fixture.secure.readState = FakeSecureStore::ReadState::Unavailable;
        const ExtensionStagingRestoreAuditStoreResult result =
            fixture.store().load();
        expect(result.state
                   == ExtensionStagingRestoreAuditStoreState::Unavailable
                   && result.entries.isEmpty(),
               "a locked backend degraded to empty");
    }
}

void replayTests(QTemporaryDir &dir)
{
    // 一份旧载荷当时是被合法签发过的，因此授权必须锚定已提交的代号与身份，否则把旧
    // 载荷放回原处就能让后来的记录从历史里消失。
    Fixture fixture(settingsPath(dir, QStringLiteral("replay")));
    ExtensionStagingRestoreAuditStoreResult first;
    QString errorCode;
    ExtensionStagingRestoreAuditLedgerStore store = fixture.store();
    expect(store.replace({sampleEntries().first()}, 0, &first, &errorCode),
           "the replay fixture failed its first commit");
    const QByteArray oldPayload = fixture.settings
        .value(ExtensionStagingRestoreAuditLedgerStore::recordSettingsKey())
        .toByteArray();

    // 追加第二条记录。
    ExtensionStagingRestoreAuditStoreResult second;
    ExtensionStagingRestoreAuditLedgerStore store2 = fixture.store();
    expect(store2.replace(sampleEntries(), 1, &second, &errorCode)
               && second.generation == 2,
           "the replay fixture failed its second commit");

    // 把旧载荷放回去：它自身仍然可认证，但不是授权提交的那一份。
    fixture.settings.setValue(
        ExtensionStagingRestoreAuditLedgerStore::recordSettingsKey(),
        oldPayload);
    fixture.settings.sync();
    const ExtensionStagingRestoreAuditStoreResult result =
        fixture.store().load();
    expect(result.state == ExtensionStagingRestoreAuditStoreState::Invalid
               && result.errorCode == QStringLiteral(
                   "extension-restore-audit-store-record-superseded")
               && result.entries.isEmpty(),
           "a replayed audit payload erased recorded history");
}

void interruptionTests(QTemporaryDir &dir)
{
    // 阶段一（预留）失败：载荷字节还没有被改动，因此确定性回滚。
    {
        Fixture fixture(settingsPath(dir, QStringLiteral("reserve-failed")));
        fixture.secure.failWriteAt = 1;
        ExtensionStagingRestoreAuditStoreResult updated;
        QString errorCode;
        ExtensionStagingRestoreAuditLedgerStore store = fixture.store();
        expect(!store.replace(sampleEntries(), 0, &updated, &errorCode),
               "a failed reservation reported success");
        expect(!fixture.settings.contains(
                   ExtensionStagingRestoreAuditLedgerStore::
                       recordSettingsKey()),
               "a failed reservation still wrote the audit payload");
        expect(fixture.store().load().state
                   == ExtensionStagingRestoreAuditStoreState::Empty,
               "a failed reservation left residue behind");
    }
    // 阶段三（完成提交）失败：载荷已经落盘，下一次读取应当依据落盘字节完成提交，
    // 而不是丢弃已经写下的内容。
    {
        Fixture fixture(settingsPath(dir, QStringLiteral("commit-failed")));
        fixture.secure.failWriteAt = 2;
        ExtensionStagingRestoreAuditStoreResult updated;
        QString errorCode;
        ExtensionStagingRestoreAuditLedgerStore store = fixture.store();
        expect(!store.replace(sampleEntries(), 0, &updated, &errorCode),
               "an unresolved commit reported success");
        fixture.secure.failWriteAt = -1;
        const ExtensionStagingRestoreAuditStoreResult recovered =
            fixture.store().load();
        expect(recovered.state == ExtensionStagingRestoreAuditStoreState::Ready
                   && recovered.generation == 1
                   && recovered.entries.size() == 2,
               "a landed payload was not promoted after an unresolved commit");
        const ExtensionStagingRestoreAuditStoreResult again =
            fixture.store().load();
        expect(again.state == ExtensionStagingRestoreAuditStoreState::Ready
                   && again.generation == 1,
               "the resolved reservation was not persisted");
    }
    // 预留解决本身也失败时结果未知，且不返回任何记录。
    {
        Fixture fixture(settingsPath(dir, QStringLiteral("unresolvable")));
        fixture.secure.failWriteAt = 2;
        ExtensionStagingRestoreAuditStoreResult updated;
        QString errorCode;
        ExtensionStagingRestoreAuditLedgerStore store = fixture.store();
        expect(!store.replace(sampleEntries(), 0, &updated, &errorCode),
               "an unresolved commit reported success");
        fixture.secure.failAllWrites = true;
        fixture.secure.failWriteOutcome =
            FakeSecureStore::WriteOutcome::OutcomeUnknown;
        const ExtensionStagingRestoreAuditStoreResult result =
            fixture.store().load();
        expect(result.state
                   == ExtensionStagingRestoreAuditStoreState::OutcomeUnknown
                   && result.entries.isEmpty(),
               "an unresolvable reservation returned audit entries");
    }
}

void casTests(QTemporaryDir &dir)
{
    // 并发修改由比较并交换裁决，而不是最后写入者获胜：否则两次并发的记录会互相
    // 静默覆盖。
    Fixture fixture(settingsPath(dir, QStringLiteral("cas")));
    ExtensionStagingRestoreAuditStoreResult updated;
    QString errorCode;
    ExtensionStagingRestoreAuditLedgerStore store = fixture.store();
    expect(store.replace(sampleEntries(), 0, &updated, &errorCode),
           "the CAS fixture failed to commit");

    ExtensionStagingRestoreAuditLedgerStore stale = fixture.store();
    expect(!stale.replace({}, 0, &updated, &errorCode)
               && errorCode == QStringLiteral(
                   "extension-restore-audit-store-generation-conflict"),
           "a stale generation overwrote a newer audit log");

    // 不合法的条目在写入任何东西之前被拒绝。
    const QByteArray before = fixture.secure.stored;
    ExtensionStagingRestoreAuditEntry malformed = sampleEntries().first();
    malformed.planIdentity =
        QStringLiteral("extension-staging-restore-plan:sha256:zz");
    ExtensionStagingRestoreAuditLedgerStore store2 = fixture.store();
    expect(!store2.replace({malformed}, 1, &updated, &errorCode)
               && errorCode == QStringLiteral(
                   "extension-restore-audit-store-entries-invalid"),
           "a malformed audit entry was committed");
    expect(fixture.secure.stored == before,
           "a rejected audit entry left a reservation behind");

    ExtensionStagingRestoreAuditLedgerStore store3 = fixture.store();
    expect(!store3.replace({}, -1, &updated, &errorCode)
               && errorCode == QStringLiteral(
                   "extension-restore-audit-store-generation-invalid"),
           "a negative expected generation was accepted");
}

void discardTests(QTemporaryDir &dir)
{
    // 健康账本不能被丢弃：那是一条不经授权就销毁审计历史的路径。
    Fixture fixture(settingsPath(dir, QStringLiteral("discard-healthy")));
    ExtensionStagingRestoreAuditStoreResult updated;
    QString errorCode;
    ExtensionStagingRestoreAuditLedgerStore store = fixture.store();
    expect(store.replace(sampleEntries(), 0, &updated, &errorCode),
           "the discard fixture failed to commit");
    expect(!store.discard(&updated, &errorCode)
               && errorCode == QStringLiteral(
                   "extension-restore-audit-store-discard-not-required"),
           "a healthy audit log could be discarded");

    // 自相矛盾的账本只能被重建为"从未记录过"，而不是被修复成某个历史。
    fixture.secure.stored.clear();
    expect(fixture.store().load().state
               == ExtensionStagingRestoreAuditStoreState::Invalid,
           "the discard fixture was not contradictory");
    ExtensionStagingRestoreAuditLedgerStore store2 = fixture.store();
    expect(store2.discard(&updated, &errorCode)
               && updated.state == ExtensionStagingRestoreAuditStoreState::Empty
               && updated.entries.isEmpty(),
           "a contradictory audit log was not rebuilt as empty");
    // 重建之后可以记录新的决定。
    ExtensionStagingRestoreAuditLedgerStore store3 = fixture.store();
    expect(store3.replace(sampleEntries(), 0, &updated, &errorCode)
               && updated.generation == 1,
           "a rebuilt audit log did not accept new records");
}

void domainSeparationTests(QTemporaryDir &dir)
{
    // 持久化层最关键的性质：复核或启用的授权信封与载荷不能被当作恢复审计记录
    // 采用，否则"我看过这份内容"或"我要求运行这份内容"就能被搬成"用户同意过恢复"。
    const QString path = settingsPath(dir, QStringLiteral("cross-domain"));
    QSettings settings(path, QSettings::IniFormat);

    const ExtensionReviewPin pin{ExtensionKind::Skill,
                                 QStringLiteral("fixture.skill"),
                                 digest(QStringLiteral("extension-source:sha256:"),
                                        "s"),
                                 digest(QStringLiteral("extension-content:sha256:"),
                                        "c")};
    const ExtensionEnablementGrant grant{ExtensionKind::Skill,
                                         QStringLiteral("fixture.skill"),
                                         pin.sourceIdentity,
                                         pin.contentIdentity};

    // 复核授权整体搬迁。
    {
        FakeReviewSecureStore reviewSecure;
        ExtensionReviewLedgerStore reviewStore(&reviewSecure, &settings);
        ExtensionReviewLedgerStoreResult reviewResult;
        QString errorCode;
        expect(reviewStore.replace({pin}, 0, &reviewResult, &errorCode),
               "the cross-domain review fixture failed to commit");

        FakeSecureStore auditSecure;
        ExtensionStagingRestoreAuditLedgerStore auditStore(&auditSecure,
                                                           &settings);
        expect(auditStore.load().state
                   == ExtensionStagingRestoreAuditStoreState::Empty,
               "review evidence was read as restore audit records");

        auditSecure.stored = reviewSecure.stored;
        settings.setValue(
            ExtensionStagingRestoreAuditLedgerStore::recordSettingsKey(),
            settings.value(ExtensionReviewLedgerStore::recordSettingsKey()));
        settings.sync();
        ExtensionStagingRestoreAuditLedgerStore adopted(&auditSecure,
                                                        &settings);
        const ExtensionStagingRestoreAuditStoreResult result = adopted.load();
        expect(result.state == ExtensionStagingRestoreAuditStoreState::Invalid
                   && result.errorCode == QStringLiteral(
                       "extension-restore-audit-store-authority-invalid")
                   && result.entries.isEmpty(),
               "a review authority envelope was adopted as audit authority");

        ExtensionReviewLedgerStore reviewAgain(&reviewSecure, &settings);
        expect(reviewAgain.load().state
                   == ExtensionReviewLedgerStoreState::Ready,
               "the cross-domain attempt damaged the review evidence");
        settings.remove(
            ExtensionStagingRestoreAuditLedgerStore::recordSettingsKey());
        settings.sync();
    }

    // 启用授权整体搬迁。
    {
        FakeEnablementSecureStore enablementSecure;
        ExtensionEnablementLedgerStore enablementStore(&enablementSecure,
                                                       &settings);
        ExtensionEnablementLedgerStoreResult enablementResult;
        QString errorCode;
        expect(enablementStore.replace({grant}, 0, &enablementResult,
                                       &errorCode),
               "the cross-domain enablement fixture failed to commit");

        FakeSecureStore auditSecure;
        auditSecure.stored = enablementSecure.stored;
        settings.setValue(
            ExtensionStagingRestoreAuditLedgerStore::recordSettingsKey(),
            settings.value(
                ExtensionEnablementLedgerStore::recordSettingsKey()));
        settings.sync();
        ExtensionStagingRestoreAuditLedgerStore adopted(&auditSecure,
                                                        &settings);
        const ExtensionStagingRestoreAuditStoreResult result = adopted.load();
        expect(result.state == ExtensionStagingRestoreAuditStoreState::Invalid
                   && result.errorCode == QStringLiteral(
                       "extension-restore-audit-store-authority-invalid")
                   && result.entries.isEmpty(),
               "an enablement authority envelope was adopted as audit "
               "authority");

        ExtensionEnablementLedgerStore enablementAgain(&enablementSecure,
                                                       &settings);
        expect(enablementAgain.load().state
                   == ExtensionEnablementLedgerStoreState::Ready,
               "the cross-domain attempt damaged the enablement grants");
    }
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
    appendSemanticsTests(dir);
    capTests(dir);
    degradationTests(dir);
    replayTests(dir);
    interruptionTests(dir);
    casTests(dir);
    discardTests(dir);
    domainSeparationTests(dir);
    if (failures == 0) {
        QTextStream(stdout)
            << "extension staging restore audit ledger store tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
