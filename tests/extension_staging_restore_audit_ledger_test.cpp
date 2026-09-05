#include "extension_staging_restore_audit_ledger.h"

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

QDateTime decidedAt(int minute)
{
    return QDateTime::fromString(
        QStringLiteral("2026-09-05T12:%1:00.000Z")
            .arg(minute, 2, 10, QLatin1Char('0')),
        Qt::ISODateWithMs);
}

ExtensionStagingRestoreAuditEntry entry(const QString &subject,
                                        const QString &backupId,
                                        const QByteArray &seed,
                                        int minute)
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
    value.decidedAt = decidedAt(minute);
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
            declined,
            // 同一份备份可以再次被决定：审计条目允许重复，不做去重。
            entry(QStringLiteral("skill:fixture.skill"),
                  QStringLiteral("backup-a"), "a", 10)};
}

bool invalid(const ExtensionStagingRestoreAuditLedgerResult &result,
             const QString &code)
{
    // 反降级：被篡改的载荷永远得出 Invalid，绝不退化成 Empty。
    return result.state == ExtensionStagingRestoreAuditLedgerState::Invalid
        && result.errorCode == code && result.entries.isEmpty()
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

QJsonObject entryJson(const ExtensionStagingRestoreAuditEntry &value)
{
    QJsonArray warnings;
    for (const ExtensionStagingRestoreWarning warning : value.warnings) {
        switch (warning) {
        case ExtensionStagingRestoreWarning::DestinationNotEmpty:
            warnings.append(QStringLiteral("destination-not-empty"));
            break;
        case ExtensionStagingRestoreWarning::AlreadyInPlaceFiles:
            warnings.append(QStringLiteral("already-in-place-files"));
            break;
        case ExtensionStagingRestoreWarning::SharedSettingsFileRestore:
            warnings.append(QStringLiteral("shared-settings-file-restore"));
            break;
        case ExtensionStagingRestoreWarning::LargeRestore:
            warnings.append(QStringLiteral("large-restore"));
            break;
        case ExtensionStagingRestoreWarning::OldBackup:
            warnings.append(QStringLiteral("old-backup"));
            break;
        case ExtensionStagingRestoreWarning::RestoreDoesNotExecuteYet:
            warnings.append(QStringLiteral("restore-does-not-execute-yet"));
            break;
        }
    }
    return QJsonObject{
        {QStringLiteral("subject"), value.subject},
        {QStringLiteral("backup_id"), value.backupId},
        {QStringLiteral("destination_root"), value.destinationRoot},
        {QStringLiteral("plan_identity"), value.planIdentity},
        {QStringLiteral("tree_identity"), value.treeIdentity},
        {QStringLiteral("warnings"), warnings},
        {QStringLiteral("decision"),
         value.decision == ExtensionStagingRestoreAuditDecision::Approved
             ? QStringLiteral("approved")
             : QStringLiteral("declined")},
        {QStringLiteral("decided_at"),
         value.decidedAt.toString(Qt::ISODateWithMs)},
    };
}

bool sameEntry(const ExtensionStagingRestoreAuditEntry &left,
               const ExtensionStagingRestoreAuditEntry &right)
{
    return left.subject == right.subject && left.backupId == right.backupId
        && left.destinationRoot == right.destinationRoot
        && left.planIdentity == right.planIdentity
        && left.treeIdentity == right.treeIdentity
        && left.warnings == right.warnings
        && left.decision == right.decision
        && left.decidedAt == right.decidedAt;
}

void roundTripTests()
{
    const QByteArray key = keyOf('k');
    const QList<ExtensionStagingRestoreAuditEntry> entries = sampleEntries();
    const QByteArray bytes =
        ExtensionStagingRestoreAuditLedger::serialize(7, entries, key);
    if (!expect(!bytes.isEmpty(),
                "a valid restore audit ledger did not serialize")) return;

    const ExtensionStagingRestoreAuditLedgerResult parsed =
        ExtensionStagingRestoreAuditLedger::parse(bytes, key);
    expect(parsed.state == ExtensionStagingRestoreAuditLedgerState::Ready
               && parsed.generation == 7 && parsed.entries.size() == 3
               && parsed.errorCode.isEmpty()
               && parsed.identity.startsWith(
                   QStringLiteral("extension-restore-audit-ledger:sha256:")),
           "a valid restore audit ledger did not round trip");
    bool preserved = parsed.entries.size() == entries.size();
    for (int index = 0; index < parsed.entries.size() && preserved; ++index) {
        preserved = sameEntry(parsed.entries.at(index), entries.at(index));
    }
    expect(preserved,
           "restore audit entry fields did not survive the round trip");
    // 拒绝同样被记录：问题被问过并且被回答了。
    expect(parsed.entries.at(1).decision
               == ExtensionStagingRestoreAuditDecision::Declined,
           "a declined restore decision was not recorded");

    // 序列化必须是确定的，否则同一组记录会产生不同的身份摘要。
    expect(ExtensionStagingRestoreAuditLedger::serialize(7, entries, key)
               == bytes,
           "restore audit ledger serialization is not deterministic");

    // 身份摘要必须绑定代号与集合内容。
    const ExtensionStagingRestoreAuditLedgerResult laterGeneration =
        ExtensionStagingRestoreAuditLedger::parse(
            ExtensionStagingRestoreAuditLedger::serialize(8, entries, key),
            key);
    expect(laterGeneration.state
                   == ExtensionStagingRestoreAuditLedgerState::Ready
               && laterGeneration.identity != parsed.identity,
           "the audit ledger identity does not bind the generation");
    QList<ExtensionStagingRestoreAuditEntry> reordered{
        entries.at(1), entries.at(0), entries.at(2)};
    const ExtensionStagingRestoreAuditLedgerResult swapped =
        ExtensionStagingRestoreAuditLedger::parse(
            ExtensionStagingRestoreAuditLedger::serialize(7, reordered, key),
            key);
    expect(swapped.state == ExtensionStagingRestoreAuditLedgerState::Ready
               && swapped.identity != parsed.identity,
           "the audit ledger identity does not bind entry ordering");

    // 已认证的空日志是合法的"已审计、尚无记录"状态。
    const QByteArray emptySet =
        ExtensionStagingRestoreAuditLedger::serialize(1, {}, key);
    const ExtensionStagingRestoreAuditLedgerResult emptyParsed =
        ExtensionStagingRestoreAuditLedger::parse(emptySet, key);
    expect(!emptySet.isEmpty()
               && emptyParsed.state
                   == ExtensionStagingRestoreAuditLedgerState::Ready
               && emptyParsed.entries.isEmpty()
               && emptyParsed.generation == 1,
           "an authenticated empty audit log was not accepted");
    // 但它必须与"从未建立账本"区分开：前者有身份摘要与代号，后者两者皆无。
    const ExtensionStagingRestoreAuditLedgerResult absent =
        ExtensionStagingRestoreAuditLedger::parse({}, key);
    expect(!emptyParsed.identity.isEmpty() && absent.identity.isEmpty()
               && absent.generation == 0
               && absent.state != emptyParsed.state,
           "an empty audit log is indistinguishable from no ledger");
}

void emptyAndKeyTests()
{
    const QByteArray key = keyOf('k');
    const ExtensionStagingRestoreAuditLedgerResult absent =
        ExtensionStagingRestoreAuditLedger::parse({}, key);
    expect(absent.state == ExtensionStagingRestoreAuditLedgerState::Empty
               && absent.entries.isEmpty() && absent.errorCode.isEmpty(),
           "an absent audit ledger was not reported empty");

    // 密钥不可用时当前内容未知，这不是"没有记录"，也不是"载荷损坏"。
    const QByteArray bytes = ExtensionStagingRestoreAuditLedger::serialize(
        1, sampleEntries(), key);
    const ExtensionStagingRestoreAuditLedgerResult noKey =
        ExtensionStagingRestoreAuditLedger::parse(bytes, QByteArray(16, 'k'));
    expect(noKey.state
                   == ExtensionStagingRestoreAuditLedgerState::Unavailable
               && noKey.errorCode == QStringLiteral(
                   "extension-restore-audit-ledger-key-unavailable")
               && noKey.entries.isEmpty(),
           "an unusable key did not resolve to unavailable");
    // 密钥缺失时空载荷仍然是 Empty：确实没有东西可以认证。
    expect(ExtensionStagingRestoreAuditLedger::parse({}, QByteArray()).state
               == ExtensionStagingRestoreAuditLedgerState::Empty,
           "an absent audit ledger changed meaning without a key");

    // 错误的密钥不能得出"没有记录"，必须是 MAC 不匹配。
    expect(invalid(ExtensionStagingRestoreAuditLedger::parse(bytes, keyOf('x')),
                   QStringLiteral(
                       "extension-restore-audit-ledger-mac-mismatch")),
           "a wrong key did not fail authentication");
}

void tamperTests()
{
    const QByteArray key = keyOf('k');
    const QList<ExtensionStagingRestoreAuditEntry> entries = sampleEntries();
    const QByteArray bytes =
        ExtensionStagingRestoreAuditLedger::serialize(7, entries, key);

    // 代号被换到另一组记录上必须失败：MAC 联合覆盖两者。
    expect(invalid(ExtensionStagingRestoreAuditLedger::parse(
                       withField(bytes, QStringLiteral("generation"), 8), key),
                   QStringLiteral(
                       "extension-restore-audit-ledger-mac-mismatch")),
           "the generation could be substituted");

    // 追加一条记录必须失败，否则任何人都可以伪造一条"用户批准过"。
    QJsonObject object = QJsonDocument::fromJson(bytes).object();
    QJsonArray array = object.value(QStringLiteral("entries")).toArray();
    array.append(entryJson(entry(QStringLiteral("skill:attacker.skill"),
                                 QStringLiteral("backup-z"), "z", 20)));
    expect(invalid(ExtensionStagingRestoreAuditLedger::parse(
                       withField(bytes, QStringLiteral("entries"), array),
                       key),
                   QStringLiteral(
                       "extension-restore-audit-ledger-mac-mismatch")),
           "an audit entry could be appended");

    // 删除一条记录同样必须失败：否则一次拒绝可以被从历史里抹掉。
    QJsonArray removed = object.value(QStringLiteral("entries")).toArray();
    removed.removeLast();
    expect(invalid(ExtensionStagingRestoreAuditLedger::parse(
                       withField(bytes, QStringLiteral("entries"), removed),
                       key),
                   QStringLiteral(
                       "extension-restore-audit-ledger-mac-mismatch")),
           "an audit entry could be removed");

    // 重排记录必须失败：MAC 覆盖顺序。
    QJsonArray reordered;
    reordered.append(object.value(QStringLiteral("entries")).toArray().at(1));
    reordered.append(object.value(QStringLiteral("entries")).toArray().at(0));
    reordered.append(object.value(QStringLiteral("entries")).toArray().at(2));
    expect(invalid(ExtensionStagingRestoreAuditLedger::parse(
                       withField(bytes, QStringLiteral("entries"), reordered),
                       key),
                   QStringLiteral(
                       "extension-restore-audit-ledger-mac-mismatch")),
           "audit entries could be reordered");

    // 替换单条记录的计划身份必须失败——这正是把同意转移到另一份计划上的手法。
    QJsonArray rewritten =
        object.value(QStringLiteral("entries")).toArray();
    QJsonObject first = rewritten.at(0).toObject();
    first.insert(QStringLiteral("plan_identity"),
                 digest(QStringLiteral("extension-staging-restore-plan:sha256:"),
                        "swapped"));
    rewritten.replace(0, first);
    expect(invalid(ExtensionStagingRestoreAuditLedger::parse(
                       withField(bytes, QStringLiteral("entries"), rewritten),
                       key),
                   QStringLiteral(
                       "extension-restore-audit-ledger-mac-mismatch")),
           "a recorded plan identity could be swapped");

    // 把"拒绝"改写成"批准"同样必须失败。
    QJsonArray flipped = object.value(QStringLiteral("entries")).toArray();
    QJsonObject second = flipped.at(1).toObject();
    second.insert(QStringLiteral("decision"), QStringLiteral("approved"));
    flipped.replace(1, second);
    expect(invalid(ExtensionStagingRestoreAuditLedger::parse(
                       withField(bytes, QStringLiteral("entries"), flipped),
                       key),
                   QStringLiteral(
                       "extension-restore-audit-ledger-mac-mismatch")),
           "a declined decision could be flipped to approved");

    // 替换 MAC 本身必须失败。
    expect(invalid(ExtensionStagingRestoreAuditLedger::parse(
                       withField(bytes, QStringLiteral("mac"),
                                 QString(64, QLatin1Char('a'))), key),
                   QStringLiteral(
                       "extension-restore-audit-ledger-mac-mismatch")),
           "a substituted MAC was accepted");
}

void malformedTests()
{
    const QByteArray key = keyOf('k');
    const QByteArray bytes = ExtensionStagingRestoreAuditLedger::serialize(
        7, sampleEntries(), key);

    // 结构性缺陷一律 Invalid，绝不退化成 Empty。
    const QList<QPair<QByteArray, QString>> cases{
        {QByteArrayLiteral("not json"),
         QStringLiteral("extension-restore-audit-ledger-record-invalid")},
        {QByteArrayLiteral("[]"),
         QStringLiteral("extension-restore-audit-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("schema"), QJsonValue()),
         QStringLiteral("extension-restore-audit-ledger-record-invalid")},
        // 复核域与启用域的模式串在本域里都无法解析。
        {withField(bytes, QStringLiteral("schema"),
                   QStringLiteral("aegisy-extension-review-ledger/0.1")),
         QStringLiteral("extension-restore-audit-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("schema"),
                   QStringLiteral("aegisy-extension-enablement-ledger/0.1")),
         QStringLiteral("extension-restore-audit-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("extra"), 1),
         QStringLiteral("extension-restore-audit-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("mac"), QJsonValue()),
         QStringLiteral("extension-restore-audit-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("entries"), QJsonValue()),
         QStringLiteral("extension-restore-audit-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("entries"), QStringLiteral("[]")),
         QStringLiteral("extension-restore-audit-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("generation"), 0),
         QStringLiteral("extension-restore-audit-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("generation"), 1.5),
         QStringLiteral("extension-restore-audit-ledger-record-invalid")},
        {withField(bytes, QStringLiteral("generation"),
                   QStringLiteral("7")),
         QStringLiteral("extension-restore-audit-ledger-record-invalid")},
    };
    for (const auto &entryCase : cases) {
        expect(invalid(ExtensionStagingRestoreAuditLedger::parse(
                           entryCase.first, key),
                       entryCase.second),
               "a malformed restore audit ledger was not rejected exactly");
    }

    const ExtensionStagingRestoreAuditEntry valid = sampleEntries().first();
    QJsonObject broken;

    // 单条记录的格式缺陷：缺失键、额外键、畸形身份、未知警告、重复或乱序的警告
    // 集合、未知决定、非规范决定时间，各自整体拒绝。
    broken = entryJson(valid);
    broken.remove(QStringLiteral("tree_identity"));
    const QJsonObject missingKey = broken;

    broken = entryJson(valid);
    broken.insert(QStringLiteral("note"), QStringLiteral("extra"));
    const QJsonObject extraKey = broken;

    broken = entryJson(valid);
    broken.insert(QStringLiteral("subject"), QStringLiteral("other:fixture"));
    const QJsonObject unknownSubject = broken;

    broken = entryJson(valid);
    broken.insert(QStringLiteral("subject"), QStringLiteral("skill:"));
    const QJsonObject emptySubject = broken;

    broken = entryJson(valid);
    broken.insert(QStringLiteral("backup_id"), QStringLiteral("bad\nid"));
    const QJsonObject unsafeBackupId = broken;

    broken = entryJson(valid);
    broken.insert(QStringLiteral("plan_identity"),
                  QStringLiteral("extension-staging-restore-plan:sha256:zz"));
    const QJsonObject badPlanIdentity = broken;

    broken = entryJson(valid);
    // 树身份不属于任何已知身份域。
    broken.insert(QStringLiteral("tree_identity"),
                  digest(QStringLiteral("extension-source:sha256:"), "t"));
    const QJsonObject badTreeIdentity = broken;

    broken = entryJson(valid);
    broken.insert(QStringLiteral("warnings"),
                  QJsonArray{QStringLiteral("made-up-warning")});
    const QJsonObject unknownWarning = broken;

    broken = entryJson(valid);
    broken.insert(QStringLiteral("warnings"),
                  QJsonArray{QStringLiteral("destination-not-empty"),
                             QStringLiteral("destination-not-empty")});
    const QJsonObject duplicateWarning = broken;

    broken = entryJson(valid);
    // 乱序的警告集合：同一个逻辑集合只允许一个规范字节形。
    broken.insert(QStringLiteral("warnings"),
                  QJsonArray{QStringLiteral("restore-does-not-execute-yet"),
                             QStringLiteral("destination-not-empty")});
    const QJsonObject unorderedWarnings = broken;

    broken = entryJson(valid);
    broken.insert(QStringLiteral("decision"), QStringLiteral("maybe"));
    const QJsonObject unknownDecision = broken;

    broken = entryJson(valid);
    broken.insert(QStringLiteral("decided_at"),
                  QStringLiteral("2026-09-05T12:00:00Z"));
    const QJsonObject nonCanonicalTime = broken;

    broken = entryJson(valid);
    broken.insert(QStringLiteral("decided_at"),
                  QStringLiteral("not a time"));
    const QJsonObject invalidTime = broken;

    const QList<QJsonObject> entryCases{
        missingKey, extraKey, unknownSubject, emptySubject, unsafeBackupId,
        badPlanIdentity, badTreeIdentity, unknownWarning, duplicateWarning,
        unorderedWarnings, unknownDecision, nonCanonicalTime, invalidTime};
    for (const QJsonObject &entryCase : entryCases) {
        QJsonArray array;
        array.append(entryCase);
        expect(invalid(ExtensionStagingRestoreAuditLedger::parse(
                           withField(bytes, QStringLiteral("entries"), array),
                           key),
                       QStringLiteral(
                           "extension-restore-audit-ledger-entry-invalid")),
               "a malformed audit entry was not rejected exactly");
    }

    // 超出数量上限的载荷在校验 MAC 前就必须被拒绝。
    QJsonArray oversized;
    for (int index = 0;
         index <= ExtensionStagingRestoreAuditLedger::MaxEntries; ++index) {
        oversized.append(entryJson(valid));
    }
    expect(invalid(ExtensionStagingRestoreAuditLedger::parse(
                       withField(bytes, QStringLiteral("entries"), oversized),
                       key),
                   QStringLiteral(
                       "extension-restore-audit-ledger-entry-limit")),
           "an oversized audit ledger was not rejected");
}

void serializeGuardTests()
{
    const QByteArray key = keyOf('k');
    const ExtensionStagingRestoreAuditEntry valid = sampleEntries().first();

    expect(ExtensionStagingRestoreAuditLedger::serialize(0, {valid}, key)
               .isEmpty(),
           "generation zero was serialized");
    expect(ExtensionStagingRestoreAuditLedger::serialize(-1, {valid}, key)
               .isEmpty(),
           "a negative generation was serialized");
    expect(ExtensionStagingRestoreAuditLedger::serialize(
               ExtensionStagingRestoreAuditLedger::MaxGeneration + 1, {valid},
               key)
               .isEmpty(),
           "an exhausted generation was serialized");
    expect(!ExtensionStagingRestoreAuditLedger::serialize(
                ExtensionStagingRestoreAuditLedger::MaxGeneration, {valid},
                key)
                .isEmpty(),
           "the maximum generation was rejected");
    expect(ExtensionStagingRestoreAuditLedger::serialize(
               1, {valid}, QByteArray(31, 'k'))
               .isEmpty(),
           "a short key was accepted for serialization");
    expect(ExtensionStagingRestoreAuditLedger::serialize(
               1, {valid}, QByteArray())
               .isEmpty(),
           "an absent key was accepted for serialization");

    // 不合法的条目在写入任何东西之前被拒绝。
    ExtensionStagingRestoreAuditEntry broken = valid;
    broken.planIdentity = QStringLiteral("extension-content:sha256:nope");
    expect(ExtensionStagingRestoreAuditLedger::serialize(1, {broken}, key)
               .isEmpty(),
           "an entry with a malformed plan identity was serialized");
    broken = valid;
    broken.treeIdentity =
        digest(QStringLiteral("extension-source:sha256:"), "t");
    expect(ExtensionStagingRestoreAuditLedger::serialize(1, {broken}, key)
               .isEmpty(),
           "an entry with a foreign tree identity domain was serialized");
    broken = valid;
    broken.backupId = QStringLiteral("bad\tid");
    expect(ExtensionStagingRestoreAuditLedger::serialize(1, {broken}, key)
               .isEmpty(),
           "an entry with an unsafe backup id was serialized");
    broken = valid;
    broken.warnings = {ExtensionStagingRestoreWarning::RestoreDoesNotExecuteYet,
                       ExtensionStagingRestoreWarning::DestinationNotEmpty};
    expect(ExtensionStagingRestoreAuditLedger::serialize(1, {broken}, key)
               .isEmpty(),
           "an entry with an unordered warning set was serialized");
    broken = valid;
    broken.decidedAt = QDateTime();
    expect(ExtensionStagingRestoreAuditLedger::serialize(1, {broken}, key)
               .isEmpty(),
           "an entry without a decision time was serialized");
    broken = valid;
    // 本地墙钟时间不属于审计记录：决定时间必须是 UTC。
    broken.decidedAt = QDateTime::fromString(
        QStringLiteral("2026-09-05T12:00:00.000"), Qt::ISODateWithMs);
    expect(broken.decidedAt.isValid()
               && broken.decidedAt.timeSpec() != Qt::UTC
               && ExtensionStagingRestoreAuditLedger::serialize(
                      1, {broken}, key)
                      .isEmpty(),
           "an entry with a non-UTC decision time was serialized");

    // 重复条目是合法历史：同一份备份可以被多次决定。
    expect(!ExtensionStagingRestoreAuditLedger::serialize(
                1, {valid, valid}, key)
                .isEmpty(),
           "repeated audit entries were rejected as duplicates");

    QList<ExtensionStagingRestoreAuditEntry> oversized;
    for (int index = 0;
         index <= ExtensionStagingRestoreAuditLedger::MaxEntries; ++index) {
        oversized.append(valid);
    }
    expect(ExtensionStagingRestoreAuditLedger::serialize(1, oversized, key)
               .isEmpty(),
           "an oversized audit entry set was serialized");
}

void crossDomainTests()
{
    // 跨域搬迁拒绝：复核域签发的载荷字节在恢复审计域里必须无法解析，否则
    // "我看过这份内容"就能被搬成"用户同意过恢复"。
    const QByteArray key = keyOf('k');
    const ExtensionReviewPin pin{ExtensionKind::Skill,
                                 QStringLiteral("fixture.skill"),
                                 digest(QStringLiteral("extension-source:sha256:"),
                                        "s"),
                                 digest(QStringLiteral("extension-content:sha256:"),
                                        "c")};
    const QByteArray reviewBytes =
        ExtensionReviewLedger::serialize(3, {pin}, key);
    expect(invalid(ExtensionStagingRestoreAuditLedger::parse(reviewBytes, key),
                   QStringLiteral(
                       "extension-restore-audit-ledger-record-invalid")),
           "a review ledger payload parsed as a restore audit record");

    // 反向同样成立：恢复审计载荷在复核域里无法解析。
    const QByteArray auditBytes = ExtensionStagingRestoreAuditLedger::serialize(
        3, sampleEntries(), key);
    const ExtensionReviewLedgerResult reviewParse =
        ExtensionReviewLedger::parse(auditBytes, key);
    expect(reviewParse.state == ExtensionReviewLedgerState::Invalid
               && reviewParse.errorCode
                   == QStringLiteral("extension-review-ledger-record-invalid"),
           "a restore audit payload parsed as review evidence");
}

// 恢复审计域的字节形独立重算：从域字符串、8 字节大端长度前缀与"代号在前、集合在后"
// 的顺序重算 MAC 与身份摘要，而不是复用实现里的任何辅助函数，实现漂移会被发现而不是
// 被镜像。
void wireFormatTests()
{
    const QByteArray key = keyOf('\x31');
    const QList<ExtensionStagingRestoreAuditEntry> entries = sampleEntries();
    const QByteArray bytes =
        ExtensionStagingRestoreAuditLedger::serialize(7, entries, key);
    const QJsonObject object = QJsonDocument::fromJson(bytes).object();

    // 模式串与条目键名都进入被持久化的字节。
    expect(object.value(QStringLiteral("schema")).toString()
               == QStringLiteral("aegisy-extension-restore-audit-ledger/0.1"),
           "the persisted restore audit schema changed");
    expect(object.contains(QStringLiteral("entries")),
           "the persisted restore audit entry key changed");

    auto framed = [](QByteArray *target, const QByteArray &value) {
        const quint64 size = static_cast<quint64>(value.size());
        for (int shift = 56; shift >= 0; shift -= 8) {
            target->append(static_cast<char>((size >> shift) & 0xff));
        }
        target->append(value);
    };
    auto warningLabel = [](ExtensionStagingRestoreWarning warning) {
        switch (warning) {
        case ExtensionStagingRestoreWarning::DestinationNotEmpty:
            return QByteArrayLiteral("destination-not-empty");
        case ExtensionStagingRestoreWarning::AlreadyInPlaceFiles:
            return QByteArrayLiteral("already-in-place-files");
        case ExtensionStagingRestoreWarning::SharedSettingsFileRestore:
            return QByteArrayLiteral("shared-settings-file-restore");
        case ExtensionStagingRestoreWarning::LargeRestore:
            return QByteArrayLiteral("large-restore");
        case ExtensionStagingRestoreWarning::OldBackup:
            return QByteArrayLiteral("old-backup");
        case ExtensionStagingRestoreWarning::RestoreDoesNotExecuteYet:
            return QByteArrayLiteral("restore-does-not-execute-yet");
        }
        return QByteArray();
    };
    auto frameEntry = [&](QByteArray *input,
                          const ExtensionStagingRestoreAuditEntry &value) {
        framed(input, value.subject.toUtf8());
        framed(input, value.backupId.toUtf8());
        framed(input, value.destinationRoot.toUtf8());
        framed(input, value.planIdentity.toUtf8());
        framed(input, value.treeIdentity.toUtf8());
        framed(input,
               QByteArray::number(static_cast<qint64>(value.warnings.size())));
        for (const ExtensionStagingRestoreWarning warning : value.warnings) {
            framed(input, warningLabel(warning));
        }
        framed(input,
               value.decision == ExtensionStagingRestoreAuditDecision::Approved
                   ? QByteArrayLiteral("approved")
                   : QByteArrayLiteral("declined"));
        framed(input,
               value.decidedAt.toString(Qt::ISODateWithMs).toUtf8());
    };

    const char macDomain[] =
        "aegisy-extension-restore-audit-ledger-hmac/0.1\0";
    QByteArray macInput(macDomain, sizeof(macDomain) - 1);
    framed(&macInput, QByteArray::number(7));
    framed(&macInput, QByteArray::number(static_cast<qint64>(entries.size())));
    for (const ExtensionStagingRestoreAuditEntry &value : entries) {
        frameEntry(&macInput, value);
    }
    unsigned char digestBytes[EVP_MAX_MD_SIZE]{};
    unsigned int digestLength = 0;
    HMAC(EVP_sha256(), key.constData(), key.size(),
         reinterpret_cast<const unsigned char *>(macInput.constData()),
         static_cast<size_t>(macInput.size()), digestBytes, &digestLength);
    const QString expectedMac = QString::fromLatin1(
        QByteArray(reinterpret_cast<const char *>(digestBytes), 32).toHex());
    expect(object.value(QStringLiteral("mac")).toString() == expectedMac,
           "the restore audit MAC domain or framing changed");

    // 身份摘要有自己的域，且不覆盖条目数量，与 MAC 预映像不同。
    const char identityDomain[] =
        "aegisy-extension-restore-audit-ledger-identity/0.1\0";
    QByteArray identityInput(identityDomain, sizeof(identityDomain) - 1);
    framed(&identityInput, QByteArray::number(7));
    for (const ExtensionStagingRestoreAuditEntry &value : entries) {
        frameEntry(&identityInput, value);
    }
    const QString expectedIdentity =
        QStringLiteral("extension-restore-audit-ledger:sha256:")
        + QString::fromLatin1(QCryptographicHash::hash(
            identityInput, QCryptographicHash::Sha256).toHex());
    const ExtensionStagingRestoreAuditLedgerResult parsed =
        ExtensionStagingRestoreAuditLedger::parse(bytes, key);
    expect(parsed.state == ExtensionStagingRestoreAuditLedgerState::Ready
               && parsed.identity == expectedIdentity,
           "the restore audit identity domain or framing changed");

    // 两个域必须彼此不同，否则身份摘要会退化成一个用同一预映像算出的值。
    expect(QByteArray(macDomain, sizeof(macDomain) - 1)
               != QByteArray(identityDomain, sizeof(identityDomain) - 1),
           "the restore audit MAC and identity domains collapsed");

    // 本域的常量与复核、启用两个域的全部对应常量都必须不同。
    expect(QByteArray(macDomain, sizeof(macDomain) - 1)
                   != QByteArrayLiteral(
                       "aegisy-extension-review-ledger-hmac/0.1")
               && QByteArray(macDomain, sizeof(macDomain) - 1)
                   != QByteArrayLiteral(
                       "aegisy-extension-enablement-ledger-hmac/0.1"),
           "the restore audit MAC domain collides with another evidence "
           "domain");
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
    crossDomainTests();
    wireFormatTests();
    if (failures == 0) {
        QTextStream(stdout)
            << "extension staging restore audit ledger tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
