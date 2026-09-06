#include "extension_staging_restore_audit_ledger.h"

#include "extension_display_safety.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>

#include <cmath>

namespace {

using Safety = ExtensionDisplaySafety;

// 恢复审计自己的域常量。它们与复核记录、启用授权的域完全不同，因此那两个域的载荷字节
// 无法被搬到恢复审计的位置——那等于把"我看过这份内容"或"我要求运行这份内容"伪造成
// "用户同意把这份备份写回目标"。
const char kMacDomain[] = "aegisy-extension-restore-audit-ledger-hmac/0.1\0";
const char kIdentityDomain[] =
    "aegisy-extension-restore-audit-ledger-identity/0.1\0";
const QString kSchema =
    QStringLiteral("aegisy-extension-restore-audit-ledger/0.1");
const QString kEntriesKey = QStringLiteral("entries");
// 执行结果分节键。为空时整个键被省略：只含决定的载荷因此与旧构建写出的字节逐字节
// 一致（字节兼容是形状决定，见头文件）。
const QString kOutcomesKey = QStringLiteral("outcomes");
const QString kIdentityPrefix =
    QStringLiteral("extension-restore-audit-ledger:sha256:");
const QString kErrorPrefix =
    QStringLiteral("extension-restore-audit-ledger");

// 字段展示上界与呈现层一致：主体与目标根按路径界，备份 id 按标签界。
constexpr int kMaxSubjectCharacters = 256;
constexpr int kMaxDestinationCharacters = 256;
constexpr int kMaxBackupIdCharacters = 128;

QString code(const char *suffix)
{
    return kErrorPrefix + QLatin1Char('-') + QLatin1String(suffix);
}

// 长度前缀分帧，使拼接后的字节序列无法通过移动字段边界产生歧义。
void append(QByteArray *target, const QByteArray &value)
{
    const quint64 size = static_cast<quint64>(value.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        target->append(static_cast<char>((size >> shift) & 0xff));
    }
    target->append(value);
}

// 警告名称是审计域自己的持久化词汇，与任何呈现文案无关。
QString warningName(ExtensionStagingRestoreWarning warning)
{
    switch (warning) {
    case ExtensionStagingRestoreWarning::DestinationNotEmpty:
        return QStringLiteral("destination-not-empty");
    case ExtensionStagingRestoreWarning::AlreadyInPlaceFiles:
        return QStringLiteral("already-in-place-files");
    case ExtensionStagingRestoreWarning::SharedSettingsFileRestore:
        return QStringLiteral("shared-settings-file-restore");
    case ExtensionStagingRestoreWarning::LargeRestore:
        return QStringLiteral("large-restore");
    case ExtensionStagingRestoreWarning::OldBackup:
        return QStringLiteral("old-backup");
    case ExtensionStagingRestoreWarning::RestoreDoesNotExecuteYet:
        return QStringLiteral("restore-does-not-execute-yet");
    }
    return QString();
}

bool warningFromName(const QString &value,
                     ExtensionStagingRestoreWarning *warning)
{
    if (!warning) return false;
    if (value == QStringLiteral("destination-not-empty")) {
        *warning = ExtensionStagingRestoreWarning::DestinationNotEmpty;
    } else if (value == QStringLiteral("already-in-place-files")) {
        *warning = ExtensionStagingRestoreWarning::AlreadyInPlaceFiles;
    } else if (value == QStringLiteral("shared-settings-file-restore")) {
        *warning = ExtensionStagingRestoreWarning::SharedSettingsFileRestore;
    } else if (value == QStringLiteral("large-restore")) {
        *warning = ExtensionStagingRestoreWarning::LargeRestore;
    } else if (value == QStringLiteral("old-backup")) {
        *warning = ExtensionStagingRestoreWarning::OldBackup;
    } else if (value == QStringLiteral("restore-does-not-execute-yet")) {
        *warning = ExtensionStagingRestoreWarning::RestoreDoesNotExecuteYet;
    } else {
        return false;
    }
    return true;
}

QString decisionName(ExtensionStagingRestoreAuditDecision decision)
{
    switch (decision) {
    case ExtensionStagingRestoreAuditDecision::Approved:
        return QStringLiteral("approved");
    case ExtensionStagingRestoreAuditDecision::Declined:
        return QStringLiteral("declined");
    }
    return QString();
}

// 执行结果的持久化词汇：与执行器共用同一枚举，本层只做命名映射。
QString outcomeName(ExtensionStagingRestoreExecutionState outcome)
{
    switch (outcome) {
    case ExtensionStagingRestoreExecutionState::Complete:
        return QStringLiteral("complete");
    case ExtensionStagingRestoreExecutionState::Partial:
        return QStringLiteral("partial");
    case ExtensionStagingRestoreExecutionState::Refused:
        return QStringLiteral("refused");
    case ExtensionStagingRestoreExecutionState::NotStarted:
        return QStringLiteral("not-started");
    }
    return QString();
}

bool outcomeFromName(const QString &value,
                     ExtensionStagingRestoreExecutionState *outcome)
{
    if (!outcome) return false;
    if (value == QStringLiteral("complete")) {
        *outcome = ExtensionStagingRestoreExecutionState::Complete;
    } else if (value == QStringLiteral("partial")) {
        *outcome = ExtensionStagingRestoreExecutionState::Partial;
    } else if (value == QStringLiteral("refused")) {
        *outcome = ExtensionStagingRestoreExecutionState::Refused;
    } else if (value == QStringLiteral("not-started")) {
        *outcome = ExtensionStagingRestoreExecutionState::NotStarted;
    } else {
        return false;
    }
    return true;
}

// 计数与下标的上界：计划操作数受捕获预算约束，2^20 已经宽松到不可能误伤；上界的意义
// 是让每个字段只有一个规范字节形。
constexpr int kMaxOutcomeCount = 1 << 20;

// JSON 整数必须是有限、无小数、在界内的值。
bool safeCount(const QJsonValue &value, int minimum, int *result)
{
    if (!result || !value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
            || number < static_cast<double>(minimum)
            || number > static_cast<double>(kMaxOutcomeCount)) {
        return false;
    }
    *result = static_cast<int>(number);
    return true;
}

// 树身份的形状检查枚举两个已知身份域（与审批层同一先例），而不是重新推导
// "主体种类 → 身份域" 的映射。
bool knownTreeIdentity(const QString &identity)
{
    return Safety::hashIdentity(identity,
                                QStringLiteral("extension-content:sha256:"))
        || Safety::hashIdentity(identity,
                                QStringLiteral("mcp-backup-content:sha256:"));
}

bool validEntry(const ExtensionStagingRestoreAuditEntry &entry)
{
    // 主体必须属于已知的两个主体种类且名指非空；展示安全规则只有一份，本层不本地
    // 重新实现任何字符类别或码位检查。
    const bool knownSubject =
        (entry.subject.startsWith(QStringLiteral("skill:"))
         && entry.subject.size() > 6)
        || (entry.subject.startsWith(QStringLiteral("mcp:"))
            && entry.subject.size() > 4);
    if (!knownSubject
            || !Safety::safeDisplayText(entry.subject, kMaxSubjectCharacters)) {
        return false;
    }
    if (!Safety::safeDisplayText(entry.backupId, kMaxBackupIdCharacters)
            || !Safety::safeDisplayText(entry.destinationRoot,
                                        kMaxDestinationCharacters)) {
        return false;
    }
    // 截断或畸形的身份无法与任何内容对齐。
    if (!Safety::hashIdentity(
            entry.planIdentity,
            QStringLiteral("extension-staging-restore-plan:sha256:"))
            || !knownTreeIdentity(entry.treeIdentity)) {
        return false;
    }
    // 警告集合按枚举序严格递增：重复或乱序都让同一个集合出现第二个字节形。
    if (entry.warnings.size()
            > ExtensionStagingRestoreAuditLedger::MaxWarnings) {
        return false;
    }
    int previous = -1;
    for (const ExtensionStagingRestoreWarning warning : entry.warnings) {
        if (warningName(warning).isEmpty()) return false;
        const int ordinal = static_cast<int>(warning);
        if (ordinal <= previous) return false;
        previous = ordinal;
    }
    if (decisionName(entry.decision).isEmpty()) return false;
    // 决定时间必须是一个有效的 UTC 时刻：歧义的本地墙钟时间不属于审计记录。
    return entry.decidedAt.isValid()
        && entry.decidedAt.timeSpec() == Qt::UTC;
}

// 结果条目的形状校验：身份字段与决定条目同一套规则（它绑定的是同一个被批准的对象），
// 外加结果分类与计数的一致性——计数与失败点必须能互相印证，否则同一份结果有第二个字节形。
bool validOutcome(const ExtensionStagingRestoreOutcomeEntry &outcome)
{
    const bool knownSubject =
        (outcome.subject.startsWith(QStringLiteral("skill:"))
         && outcome.subject.size() > 6)
        || (outcome.subject.startsWith(QStringLiteral("mcp:"))
            && outcome.subject.size() > 4);
    if (!knownSubject
            || !Safety::safeDisplayText(outcome.subject, kMaxSubjectCharacters)) {
        return false;
    }
    if (!Safety::safeDisplayText(outcome.backupId, kMaxBackupIdCharacters)
            || !Safety::safeDisplayText(outcome.destinationRoot,
                                        kMaxDestinationCharacters)) {
        return false;
    }
    if (!Safety::hashIdentity(
            outcome.planIdentity,
            QStringLiteral("extension-staging-restore-plan:sha256:"))
            || !knownTreeIdentity(outcome.treeIdentity)) {
        return false;
    }
    if (outcomeName(outcome.outcome).isEmpty()) return false;
    // 恢复前备份 id 允许为空（目标原本不存在时没有回退路径），非空时按备份 id 界。
    if (!outcome.preRestoreBackupId.isEmpty()
            && !Safety::safeDisplayText(outcome.preRestoreBackupId,
                                        kMaxBackupIdCharacters)) {
        return false;
    }
    if (outcome.doneCount < 0 || outcome.doneCount > kMaxOutcomeCount
            || outcome.skippedVerifiedCount < 0
            || outcome.skippedVerifiedCount > kMaxOutcomeCount
            || outcome.failedCount < 0
            || outcome.failedCount > kMaxOutcomeCount
            || outcome.failureIndex < -1
            || outcome.failureIndex > kMaxOutcomeCount) {
        return false;
    }
    // 分类与计数的一致性：Complete 无失败；Refused 零写入零失败；Partial 至少一条完成
    // 后恰好一条失败；NotStarted 第一条即失败、无任何完成。
    switch (outcome.outcome) {
    case ExtensionStagingRestoreExecutionState::Complete:
        if (outcome.failureIndex != -1 || outcome.failedCount != 0) return false;
        break;
    case ExtensionStagingRestoreExecutionState::Refused:
        if (outcome.failureIndex != -1 || outcome.doneCount != 0
                || outcome.skippedVerifiedCount != 0
                || outcome.failedCount != 0) {
            return false;
        }
        break;
    case ExtensionStagingRestoreExecutionState::Partial:
        if (outcome.failureIndex < 0 || outcome.failedCount != 1
                || outcome.doneCount + outcome.skippedVerifiedCount < 1) {
            return false;
        }
        break;
    case ExtensionStagingRestoreExecutionState::NotStarted:
        if (outcome.failureIndex != 0 || outcome.doneCount != 0
                || outcome.skippedVerifiedCount != 0
                || outcome.failedCount != 1) {
            return false;
        }
        break;
    }
    // 记录时间必须是一个有效的 UTC 时刻，与决定时间同一规则。
    return outcome.recordedAt.isValid()
        && outcome.recordedAt.timeSpec() == Qt::UTC;
}

QString decidedAtLabel(const QDateTime &decidedAt)
{
    return decidedAt.toString(Qt::ISODateWithMs);
}

// MAC 覆盖代号与整个集合的规范化预映像，因此代号不能被换到另一组条目上，单条条目也
// 不能被替换、删除或重排。域常量进入预映像，因此其他证据域的 MAC 在本域不成立。
// 结果分节只在非空时追加：空结果集的预映像与旧格式逐字节一致，旧载荷的 MAC 因此在
// 新构建下原样成立。
QByteArray macPreimage(
    qint64 generation,
    const QList<ExtensionStagingRestoreAuditEntry> &entries,
    const QList<ExtensionStagingRestoreOutcomeEntry> &outcomes)
{
    QByteArray input(kMacDomain, sizeof(kMacDomain) - 1);
    append(&input, QByteArray::number(generation));
    append(&input, QByteArray::number(static_cast<qint64>(entries.size())));
    for (const ExtensionStagingRestoreAuditEntry &entry : entries) {
        append(&input, entry.subject.toUtf8());
        append(&input, entry.backupId.toUtf8());
        append(&input, entry.destinationRoot.toUtf8());
        append(&input, entry.planIdentity.toUtf8());
        append(&input, entry.treeIdentity.toUtf8());
        append(&input,
               QByteArray::number(static_cast<qint64>(entry.warnings.size())));
        for (const ExtensionStagingRestoreWarning warning : entry.warnings) {
            append(&input, warningName(warning).toUtf8());
        }
        append(&input, decisionName(entry.decision).toUtf8());
        append(&input, decidedAtLabel(entry.decidedAt).toUtf8());
    }
    if (!outcomes.isEmpty()) {
        // 分节标记使结果分节无法与更长的决定列表发生边界歧义。
        append(&input, kOutcomesKey.toUtf8());
        append(&input, QByteArray::number(static_cast<qint64>(outcomes.size())));
        for (const ExtensionStagingRestoreOutcomeEntry &outcome : outcomes) {
            append(&input, outcome.subject.toUtf8());
            append(&input, outcome.backupId.toUtf8());
            append(&input, outcome.destinationRoot.toUtf8());
            append(&input, outcome.planIdentity.toUtf8());
            append(&input, outcome.treeIdentity.toUtf8());
            append(&input, outcomeName(outcome.outcome).toUtf8());
            append(&input, QByteArray::number(outcome.failureIndex));
            append(&input, QByteArray::number(outcome.doneCount));
            append(&input, QByteArray::number(outcome.skippedVerifiedCount));
            append(&input, QByteArray::number(outcome.failedCount));
            append(&input, outcome.preRestoreBackupId.toUtf8());
            append(&input, decidedAtLabel(outcome.recordedAt).toUtf8());
        }
    }
    return input;
}

QString mac(const QByteArray &key, const QByteArray &preimage)
{
    unsigned char result[EVP_MAX_MD_SIZE]{};
    unsigned int length = 0;
    if (key.size() != 32
            || !HMAC(EVP_sha256(), key.constData(), key.size(),
                     reinterpret_cast<const unsigned char *>(preimage.constData()),
                     static_cast<size_t>(preimage.size()), result, &length)
            || length != 32) {
        OPENSSL_cleanse(result, sizeof(result));
        return {};
    }
    const QString value = QString::fromLatin1(
        QByteArray(reinterpret_cast<const char *>(result), 32).toHex());
    OPENSSL_cleanse(result, sizeof(result));
    return value;
}

bool equalMac(const QString &left, const QString &right)
{
    const QByteArray leftBytes = left.toLatin1();
    const QByteArray rightBytes = right.toLatin1();
    return leftBytes.size() == 64 && rightBytes.size() == 64
        && CRYPTO_memcmp(leftBytes.constData(), rightBytes.constData(), 64) == 0;
}

QString contentIdentity(
    qint64 generation,
    const QList<ExtensionStagingRestoreAuditEntry> &entries,
    const QList<ExtensionStagingRestoreOutcomeEntry> &outcomes)
{
    QByteArray input(kIdentityDomain, sizeof(kIdentityDomain) - 1);
    append(&input, QByteArray::number(generation));
    for (const ExtensionStagingRestoreAuditEntry &entry : entries) {
        append(&input, entry.subject.toUtf8());
        append(&input, entry.backupId.toUtf8());
        append(&input, entry.destinationRoot.toUtf8());
        append(&input, entry.planIdentity.toUtf8());
        append(&input, entry.treeIdentity.toUtf8());
        append(&input,
               QByteArray::number(static_cast<qint64>(entry.warnings.size())));
        for (const ExtensionStagingRestoreWarning warning : entry.warnings) {
            append(&input, warningName(warning).toUtf8());
        }
        append(&input, decisionName(entry.decision).toUtf8());
        append(&input, decidedAtLabel(entry.decidedAt).toUtf8());
    }
    // 与 MAC 预映像同一规则：空结果集不追加任何字节，旧载荷的身份逐字节不变。
    if (!outcomes.isEmpty()) {
        append(&input, kOutcomesKey.toUtf8());
        append(&input, QByteArray::number(static_cast<qint64>(outcomes.size())));
        for (const ExtensionStagingRestoreOutcomeEntry &outcome : outcomes) {
            append(&input, outcome.subject.toUtf8());
            append(&input, outcome.backupId.toUtf8());
            append(&input, outcome.destinationRoot.toUtf8());
            append(&input, outcome.planIdentity.toUtf8());
            append(&input, outcome.treeIdentity.toUtf8());
            append(&input, outcomeName(outcome.outcome).toUtf8());
            append(&input, QByteArray::number(outcome.failureIndex));
            append(&input, QByteArray::number(outcome.doneCount));
            append(&input, QByteArray::number(outcome.skippedVerifiedCount));
            append(&input, QByteArray::number(outcome.failedCount));
            append(&input, outcome.preRestoreBackupId.toUtf8());
            append(&input, decidedAtLabel(outcome.recordedAt).toUtf8());
        }
    }
    return kIdentityPrefix
        + QString::fromLatin1(
            QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex());
}

bool safeGeneration(const QJsonValue &value, qint64 *result)
{
    if (!result || !value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number || number < 1
            || number > static_cast<double>(
                   ExtensionStagingRestoreAuditLedger::MaxGeneration)) {
        return false;
    }
    *result = static_cast<qint64>(number);
    return true;
}

ExtensionStagingRestoreAuditLedgerResult failure(
    ExtensionStagingRestoreAuditLedgerState state, const QString &errorCode)
{
    ExtensionStagingRestoreAuditLedgerResult result;
    result.state = state;
    result.errorCode = errorCode;
    return result;
}

} // namespace

QByteArray ExtensionStagingRestoreAuditLedger::serialize(
    qint64 generation,
    const QList<ExtensionStagingRestoreAuditEntry> &entries,
    const QByteArray &key,
    const QList<ExtensionStagingRestoreOutcomeEntry> &outcomes)
{
    if (generation < 1 || generation > MaxGeneration
            || entries.size() > MaxEntries
            || outcomes.size() > MaxOutcomeEntries || key.size() != 32) {
        return {};
    }
    QJsonArray array;
    for (const ExtensionStagingRestoreAuditEntry &entry : entries) {
        // 不合法的身份、警告集合或决定时间在写入任何东西之前被拒绝。
        if (!validEntry(entry)) return {};
        QJsonArray warnings;
        for (const ExtensionStagingRestoreWarning warning : entry.warnings) {
            warnings.append(warningName(warning));
        }
        array.append(QJsonObject{
            {QStringLiteral("subject"), entry.subject},
            {QStringLiteral("backup_id"), entry.backupId},
            {QStringLiteral("destination_root"), entry.destinationRoot},
            {QStringLiteral("plan_identity"), entry.planIdentity},
            {QStringLiteral("tree_identity"), entry.treeIdentity},
            {QStringLiteral("warnings"), warnings},
            {QStringLiteral("decision"), decisionName(entry.decision)},
            {QStringLiteral("decided_at"), decidedAtLabel(entry.decidedAt)},
        });
    }
    QJsonArray outcomeArray;
    for (const ExtensionStagingRestoreOutcomeEntry &outcome : outcomes) {
        if (!validOutcome(outcome)) return {};
        outcomeArray.append(QJsonObject{
            {QStringLiteral("subject"), outcome.subject},
            {QStringLiteral("backup_id"), outcome.backupId},
            {QStringLiteral("destination_root"), outcome.destinationRoot},
            {QStringLiteral("plan_identity"), outcome.planIdentity},
            {QStringLiteral("tree_identity"), outcome.treeIdentity},
            {QStringLiteral("outcome"), outcomeName(outcome.outcome)},
            {QStringLiteral("failure_index"), outcome.failureIndex},
            {QStringLiteral("done_count"), outcome.doneCount},
            {QStringLiteral("skipped_verified_count"),
             outcome.skippedVerifiedCount},
            {QStringLiteral("failed_count"), outcome.failedCount},
            {QStringLiteral("pre_restore_backup_id"),
             outcome.preRestoreBackupId},
            {QStringLiteral("recorded_at"),
             decidedAtLabel(outcome.recordedAt)},
        });
    }
    const QString authenticator = mac(key, macPreimage(generation, entries,
                                                       outcomes));
    if (authenticator.isEmpty()) return {};
    QJsonObject object{
        {QStringLiteral("schema"), kSchema},
        {QStringLiteral("generation"), generation},
        {kEntriesKey, array},
        {QStringLiteral("mac"), authenticator},
    };
    // 空结果集不写入 outcomes 键：只含决定的载荷与旧构建的字节逐字节一致。
    if (!outcomes.isEmpty()) {
        object.insert(kOutcomesKey, outcomeArray);
    }
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return bytes.size() <= MaxRecordBytes ? bytes : QByteArray();
}

ExtensionStagingRestoreAuditLedgerResult
ExtensionStagingRestoreAuditLedger::parse(const QByteArray &bytes,
                                          const QByteArray &key)
{
    // 只有确实没有载荷时才是 Empty。任何存在但不可信的载荷都是 Invalid。
    if (bytes.isEmpty()) {
        ExtensionStagingRestoreAuditLedgerResult result;
        result.state = ExtensionStagingRestoreAuditLedgerState::Empty;
        return result;
    }
    if (bytes.size() > MaxRecordBytes) {
        return failure(ExtensionStagingRestoreAuditLedgerState::Invalid,
                       code("oversized"));
    }
    if (key.size() != 32) {
        // 密钥不可用时当前内容无法判断，这不是"没有记录"。
        return failure(ExtensionStagingRestoreAuditLedgerState::Unavailable,
                       code("key-unavailable"));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return failure(ExtensionStagingRestoreAuditLedgerState::Invalid,
                       code("record-invalid"));
    }
    const QJsonObject object = document.object();
    // 顶层键集合允许两种形状：旧格式（无 outcomes 键——bdaf49c 起产品写出的真实载荷
    // 就是它）与携带执行结果分节的新格式。两种形状之外的一切都是 Invalid。
    const QSet<QString> legacyExpected{
        QStringLiteral("schema"), QStringLiteral("generation"),
        kEntriesKey, QStringLiteral("mac")};
    QSet<QString> withOutcomes = legacyExpected;
    withOutcomes.insert(kOutcomesKey);
    const QStringList keys = object.keys();
    const QSet<QString> keySet(keys.cbegin(), keys.cend());
    qint64 generation = 0;
    // 模式串必须与本域完全一致：复核或启用载荷因此在恢复审计域里无法解析。
    if ((keySet != legacyExpected && keySet != withOutcomes)
            || object.value(QStringLiteral("schema")).toString() != kSchema
            || !safeGeneration(object.value(QStringLiteral("generation")),
                               &generation)
            || !object.value(kEntriesKey).isArray()
            || (keySet == withOutcomes
                && !object.value(kOutcomesKey).isArray())
            || !object.value(QStringLiteral("mac")).isString()) {
        return failure(ExtensionStagingRestoreAuditLedgerState::Invalid,
                       code("record-invalid"));
    }
    const QJsonArray array = object.value(kEntriesKey).toArray();
    if (array.size() > MaxEntries) {
        return failure(ExtensionStagingRestoreAuditLedgerState::Invalid,
                       code("entry-limit"));
    }
    static const QSet<QString> entryKeys{
        QStringLiteral("subject"), QStringLiteral("backup_id"),
        QStringLiteral("destination_root"), QStringLiteral("plan_identity"),
        QStringLiteral("tree_identity"), QStringLiteral("warnings"),
        QStringLiteral("decision"), QStringLiteral("decided_at")};
    QList<ExtensionStagingRestoreAuditEntry> entries;
    for (const QJsonValue &value : array) {
        if (!value.isObject()) {
            return failure(ExtensionStagingRestoreAuditLedgerState::Invalid,
                           code("entry-invalid"));
        }
        const QJsonObject item = value.toObject();
        const QStringList itemKeys = item.keys();
        ExtensionStagingRestoreAuditEntry entry;
        if (QSet<QString>(itemKeys.cbegin(), itemKeys.cend()) != entryKeys
                || !item.value(QStringLiteral("subject")).isString()
                || !item.value(QStringLiteral("backup_id")).isString()
                || !item.value(QStringLiteral("destination_root")).isString()
                || !item.value(QStringLiteral("plan_identity")).isString()
                || !item.value(QStringLiteral("tree_identity")).isString()
                || !item.value(QStringLiteral("warnings")).isArray()
                || !item.value(QStringLiteral("decision")).isString()
                || !item.value(QStringLiteral("decided_at")).isString()) {
            return failure(ExtensionStagingRestoreAuditLedgerState::Invalid,
                           code("entry-invalid"));
        }
        entry.subject = item.value(QStringLiteral("subject")).toString();
        entry.backupId = item.value(QStringLiteral("backup_id")).toString();
        entry.destinationRoot =
            item.value(QStringLiteral("destination_root")).toString();
        entry.planIdentity =
            item.value(QStringLiteral("plan_identity")).toString();
        entry.treeIdentity =
            item.value(QStringLiteral("tree_identity")).toString();
        const QJsonArray warnings =
            item.value(QStringLiteral("warnings")).toArray();
        for (const QJsonValue &warningValue : warnings) {
            if (!warningValue.isString()) {
                return failure(
                    ExtensionStagingRestoreAuditLedgerState::Invalid,
                    code("entry-invalid"));
            }
            ExtensionStagingRestoreWarning warning;
            if (!warningFromName(warningValue.toString(), &warning)) {
                return failure(
                    ExtensionStagingRestoreAuditLedgerState::Invalid,
                    code("entry-invalid"));
            }
            entry.warnings.append(warning);
        }
        const QString decision =
            item.value(QStringLiteral("decision")).toString();
        if (decision == QStringLiteral("approved")) {
            entry.decision = ExtensionStagingRestoreAuditDecision::Approved;
        } else if (decision == QStringLiteral("declined")) {
            entry.decision = ExtensionStagingRestoreAuditDecision::Declined;
        } else {
            return failure(ExtensionStagingRestoreAuditLedgerState::Invalid,
                           code("entry-invalid"));
        }
        const QString decidedAtText =
            item.value(QStringLiteral("decided_at")).toString();
        const QDateTime decidedAt =
            QDateTime::fromString(decidedAtText, Qt::ISODateWithMs);
        // 决定时间必须是规范的 UTC 形式：重新序列化不一致说明字段被改动过或不是
        // 规范形。
        if (!decidedAt.isValid() || decidedAt.timeSpec() != Qt::UTC
                || decidedAtLabel(decidedAt) != decidedAtText) {
            return failure(ExtensionStagingRestoreAuditLedgerState::Invalid,
                           code("entry-invalid"));
        }
        entry.decidedAt = decidedAt;
        if (!validEntry(entry)) {
            return failure(ExtensionStagingRestoreAuditLedgerState::Invalid,
                           code("entry-invalid"));
        }
        entries.append(entry);
    }
    // 执行结果分节：缺失即空列表（旧格式），存在时逐条严格校验——键集合精确、计数字段
    // 是无小数的有界整数、记录时间是规范 UTC 形式、分类与计数互相印证。
    QList<ExtensionStagingRestoreOutcomeEntry> outcomes;
    if (keySet == withOutcomes) {
        const QJsonArray outcomeArray = object.value(kOutcomesKey).toArray();
        if (outcomeArray.size() > MaxOutcomeEntries) {
            return failure(ExtensionStagingRestoreAuditLedgerState::Invalid,
                           code("entry-limit"));
        }
        static const QSet<QString> outcomeKeys{
            QStringLiteral("subject"), QStringLiteral("backup_id"),
            QStringLiteral("destination_root"), QStringLiteral("plan_identity"),
            QStringLiteral("tree_identity"), QStringLiteral("outcome"),
            QStringLiteral("failure_index"), QStringLiteral("done_count"),
            QStringLiteral("skipped_verified_count"),
            QStringLiteral("failed_count"),
            QStringLiteral("pre_restore_backup_id"),
            QStringLiteral("recorded_at")};
        for (const QJsonValue &value : outcomeArray) {
            if (!value.isObject()) {
                return failure(ExtensionStagingRestoreAuditLedgerState::Invalid,
                               code("entry-invalid"));
            }
            const QJsonObject item = value.toObject();
            const QStringList itemKeys = item.keys();
            ExtensionStagingRestoreOutcomeEntry outcome;
            if (QSet<QString>(itemKeys.cbegin(), itemKeys.cend())
                        != outcomeKeys
                    || !item.value(QStringLiteral("subject")).isString()
                    || !item.value(QStringLiteral("backup_id")).isString()
                    || !item.value(QStringLiteral("destination_root"))
                            .isString()
                    || !item.value(QStringLiteral("plan_identity")).isString()
                    || !item.value(QStringLiteral("tree_identity")).isString()
                    || !item.value(QStringLiteral("outcome")).isString()
                    || !item.value(QStringLiteral("pre_restore_backup_id"))
                            .isString()
                    || !item.value(QStringLiteral("recorded_at")).isString()
                    || !safeCount(item.value(QStringLiteral("failure_index")),
                                  -1, &outcome.failureIndex)
                    || !safeCount(item.value(QStringLiteral("done_count")), 0,
                                  &outcome.doneCount)
                    || !safeCount(
                           item.value(
                               QStringLiteral("skipped_verified_count")), 0,
                           &outcome.skippedVerifiedCount)
                    || !safeCount(item.value(QStringLiteral("failed_count")),
                                  0, &outcome.failedCount)) {
                return failure(ExtensionStagingRestoreAuditLedgerState::Invalid,
                               code("entry-invalid"));
            }
            outcome.subject = item.value(QStringLiteral("subject")).toString();
            outcome.backupId =
                item.value(QStringLiteral("backup_id")).toString();
            outcome.destinationRoot =
                item.value(QStringLiteral("destination_root")).toString();
            outcome.planIdentity =
                item.value(QStringLiteral("plan_identity")).toString();
            outcome.treeIdentity =
                item.value(QStringLiteral("tree_identity")).toString();
            if (!outcomeFromName(
                    item.value(QStringLiteral("outcome")).toString(),
                    &outcome.outcome)) {
                return failure(ExtensionStagingRestoreAuditLedgerState::Invalid,
                               code("entry-invalid"));
            }
            outcome.preRestoreBackupId =
                item.value(QStringLiteral("pre_restore_backup_id")).toString();
            const QString recordedAtText =
                item.value(QStringLiteral("recorded_at")).toString();
            const QDateTime recordedAt =
                QDateTime::fromString(recordedAtText, Qt::ISODateWithMs);
            if (!recordedAt.isValid() || recordedAt.timeSpec() != Qt::UTC
                    || decidedAtLabel(recordedAt) != recordedAtText) {
                return failure(ExtensionStagingRestoreAuditLedgerState::Invalid,
                               code("entry-invalid"));
            }
            outcome.recordedAt = recordedAt;
            if (!validOutcome(outcome)) {
                return failure(ExtensionStagingRestoreAuditLedgerState::Invalid,
                               code("entry-invalid"));
            }
            outcomes.append(outcome);
        }
    }
    // MAC 在最后校验，且覆盖代号与整个集合，因此任何字段替换都会被发现。
    if (!equalMac(object.value(QStringLiteral("mac")).toString(),
                  mac(key, macPreimage(generation, entries, outcomes)))) {
        return failure(ExtensionStagingRestoreAuditLedgerState::Invalid,
                       code("mac-mismatch"));
    }
    ExtensionStagingRestoreAuditLedgerResult result;
    result.state = ExtensionStagingRestoreAuditLedgerState::Ready;
    result.entries = entries;
    result.outcomes = outcomes;
    result.generation = generation;
    result.identity = contentIdentity(generation, entries, outcomes);
    return result;
}
