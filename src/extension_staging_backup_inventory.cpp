#include "extension_staging_backup_inventory.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLockFile>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace {

const QString kErrorPrefix = QStringLiteral("extension-staging-inventory");
const QString kCipher = QStringLiteral("AES-256-GCM");
constexpr int kManifestVersion = 2;
constexpr int kNonceBytes = 12;
constexpr int kTagBytes = 16;

QString code(const char *suffix)
{
    return kErrorPrefix + QLatin1Char('-') + QLatin1String(suffix);
}

void setError(QString *error, const QString &value)
{
    if (error) *error = value;
}

bool validSubject(const ConfigurationBackupStoreDomain &domain,
                  const QString &subject)
{
    const QRegularExpression pattern(domain.subjectPattern);
    return pattern.match(subject).hasMatch();
}

bool validBackupId(const ConfigurationBackupStoreDomain &domain,
                   const QString &backupId)
{
    const QRegularExpression pattern(domain.backupIdPattern);
    return pattern.match(backupId).hasMatch();
}

bool exactKeys(const QJsonObject &object, const QStringList &expected)
{
    QStringList actual = object.keys();
    QStringList wanted = expected;
    actual.sort();
    wanted.sort();
    return actual == wanted;
}

bool exactInteger(const QJsonValue &value, int minimum, int maximum)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return std::isfinite(number) && std::floor(number) == number
        && number >= minimum && number <= maximum;
}

// 与存储相同的规范化 base64 纪律,上界跟随域的清单上限。
bool canonicalBase64(const ConfigurationBackupStoreDomain &domain,
                     const QString &encoded, QByteArray *decoded)
{
    if (!decoded || encoded.size() > domain.maxManifestBytes * 2) {
        return false;
    }
    const QByteArray latin = encoded.toLatin1();
    if (QString::fromLatin1(latin) != encoded) return false;
    const QByteArray value = QByteArray::fromBase64(latin);
    if (value.toBase64() != latin) return false;
    *decoded = value;
    return true;
}

bool canonicalUtcText(const QString &text, QDateTime *value)
{
    const QDateTime parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!parsed.isValid() || parsed.offsetFromUtc() != 0
            || parsed.toUTC().toString(Qt::ISODateWithMs) != text) {
        return false;
    }
    if (value) *value = parsed.toUTC();
    return true;
}

// 清单身份从读到的字节重算:它只是域身份材料加清单字节的散列,因此不需要密钥,损坏条目
// 的身份同样可以重算并报告。
QString recomputeManifestIdentity(const ConfigurationBackupStoreDomain &domain,
                                  const QByteArray &manifestBytes)
{
    QByteArray material = domain.identityDomain;
    material.append(manifestBytes);
    return domain.identityPrefix
        + QString::fromLatin1(
            QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
}

// 清单身份级结构校验:除 GCM 解密之外的全部清单契约。claimedSubject 在 JSON 可解析且主体
// 字段语法合法时填回——它未经认证,仅供把损坏条目归类到主体,绝不用作授权依据。
bool validateManifestStructure(const ConfigurationBackupStoreDomain &domain,
                               const QByteArray &manifestBytes,
                               const QString &directoryBackupId,
                               QString *claimedSubject,
                               QDateTime *createdAt)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        manifestBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
            || document.toJson(QJsonDocument::Compact) != manifestBytes) {
        return false;
    }
    const QJsonObject manifest = document.object();
    const QJsonValue subjectValue = manifest.value(domain.subjectJsonKey);
    if (subjectValue.isString()
            && validSubject(domain, subjectValue.toString())) {
        *claimedSubject = subjectValue.toString();
    }
    if (!exactKeys(manifest, {
            QStringLiteral("backup_id"), QStringLiteral("cipher"),
            QStringLiteral("ciphertext"), QStringLiteral("created_at"),
            QStringLiteral("file_count"), QStringLiteral("format"),
            QStringLiteral("nonce"), QStringLiteral("tag"),
            domain.subjectJsonKey, QStringLiteral("version") })
            || manifest.value(QStringLiteral("format")).toString()
                != domain.manifestFormat
            || !exactInteger(manifest.value(QStringLiteral("version")),
                             kManifestVersion, kManifestVersion)
            || manifest.value(QStringLiteral("cipher")).toString() != kCipher
            || claimedSubject->isEmpty()
            || manifest.value(QStringLiteral("backup_id")).toString()
                != directoryBackupId
            || !canonicalUtcText(
                manifest.value(QStringLiteral("created_at")).toString(),
                createdAt)
            || !exactInteger(manifest.value(QStringLiteral("file_count")),
                             1, domain.maxFiles)) {
        return false;
    }
    QByteArray nonce;
    QByteArray tag;
    QByteArray cipherText;
    if (!canonicalBase64(domain,
                         manifest.value(QStringLiteral("nonce")).toString(),
                         &nonce)
            || !canonicalBase64(domain,
                                manifest.value(QStringLiteral("tag")).toString(),
                                &tag)
            || !canonicalBase64(domain,
                                manifest.value(QStringLiteral("ciphertext"))
                                    .toString(),
                                &cipherText)
            || nonce.size() != kNonceBytes || tag.size() != kTagBytes
            || cipherText.isEmpty()
            || cipherText.size() > domain.maxManifestBytes) {
        return false;
    }
    return true;
}

// 未经认证的主体归类:仅从可解析的清单 JSON 里取语法合法的主体字段,仅供把损坏条目归到
// 主体的视野里,绝不用作授权依据。
QString claimedSubjectOf(const ConfigurationBackupStoreDomain &domain,
                         const QByteArray &manifestBytes)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        manifestBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    const QJsonValue value = document.object().value(domain.subjectJsonKey);
    if (!value.isString() || !validSubject(domain, value.toString())) {
        return {};
    }
    return value.toString();
}

// 单份备份目录的清点:目录形状违例、清单不可读、结构校验失败各自成为独立的损坏诊断,
// 条目一律保留在清单里。身份在字节可读时一律重算,即使条目最终判定为损坏。
ExtensionStagingBackupListEntry scanBackupDirectory(
    const ConfigurationBackupStoreDomain &domain, const QString &rootPath,
    const QString &backupId)
{
    ExtensionStagingBackupListEntry entry;
    entry.backupId = backupId;
    const QString directoryPath = QDir(rootPath).filePath(backupId);
    const QString manifestPath =
        QDir(directoryPath).filePath(domain.manifestName);

    bool directoryShapeValid = true;
    const QFileInfoList children = QDir(directoryPath).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    if (children.size() != 1
            || children.first().fileName() != domain.manifestName
            || children.first().isSymLink() || !children.first().isFile()) {
        directoryShapeValid = false;
    }

    const QFileInfo manifestInfo(manifestPath);
    QByteArray manifestBytes;
    bool manifestReadable = false;
    if (manifestInfo.exists() && !manifestInfo.isSymLink()
            && manifestInfo.isFile() && manifestInfo.size() > 0
            && manifestInfo.size() <= domain.maxManifestBytes) {
        QFile file(manifestPath);
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray bytes =
                file.read(domain.maxManifestBytes + 1);
            if (bytes.size() == manifestInfo.size()) {
                manifestBytes = bytes;
                manifestReadable = true;
            }
        }
    }
    if (manifestReadable) {
        entry.manifestIdentity =
            recomputeManifestIdentity(domain, manifestBytes);
    }

    if (!directoryShapeValid) {
        entry.subject = manifestReadable
            ? claimedSubjectOf(domain, manifestBytes) : QString();
        entry.verificationIssue = code("entry-directory-invalid");
        return entry;
    }
    if (!manifestReadable) {
        entry.verificationIssue = code("entry-manifest-unreadable");
        return entry;
    }
    QString claimedSubject;
    QDateTime createdAt;
    if (!validateManifestStructure(domain, manifestBytes, backupId,
                                   &claimedSubject, &createdAt)) {
        entry.subject = claimedSubject;
        entry.verificationIssue = code("entry-manifest-invalid");
        return entry;
    }
    entry.subject = claimedSubject;
    entry.createdAt = createdAt;
    entry.verification = ExtensionStagingBackupEntryVerification::ListedIntact;
    return entry;
}

} // namespace

bool ExtensionStagingBackupInventory::list(
        const QString &backupRoot, const QString &subject,
        ExtensionStagingBackupListResult *result, QString *error)
{
    if (error) error->clear();
    if (result) *result = ExtensionStagingBackupListResult();
    if (!result || backupRoot.isEmpty()) {
        setError(error, code("request-invalid"));
        return false;
    }
    const ConfigurationBackupStoreDomain domain =
        ConfigurationBackupStore::extensionStagingDomain();
    // 主体语法先于一切存储工作:一个畸形主体连根目录都不该被触碰。
    if (!subject.isEmpty() && !validSubject(domain, subject)) {
        setError(error, code("subject-invalid"));
        return false;
    }
    const QString rootPath = QDir::cleanPath(backupRoot);
    if (!QDir::isAbsolutePath(rootPath)) {
        setError(error, code("request-invalid"));
        return false;
    }

    const QFileInfo rootInfo(rootPath);
    if (!rootInfo.exists()) {
        result->state = ExtensionStagingBackupListState::Empty;
        return true;
    }
    if (rootInfo.isSymLink() || !rootInfo.isDir()) {
        result->state = ExtensionStagingBackupListState::Invalid;
        result->issue = code("root-invalid");
        return true;
    }
    if (!rootInfo.isReadable()) {
        result->state = ExtensionStagingBackupListState::Unavailable;
        result->issue = code("root-unavailable");
        return true;
    }

    QLockFile lock(QDir(rootPath).filePath(domain.lockFileName));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(2000)) {
        result->state = ExtensionStagingBackupListState::Unavailable;
        result->issue = code("busy");
        return true;
    }

    // 根形状纪律与存储逐字一致,只有一处刻意的分歧:备份目录数超过 maxBackups 不再判
    // Invalid——超限正是保留期规划要修复的现实,看不到它就永远规划不了裁剪。本层以
    // maxBackups 的 4 倍为扫描上限,保持单次清点读的总量有界。
    const int maxListingEntries = domain.maxBackups * 4;
    QStringList backupIds;
    const QFileInfoList rootEntries = QDir(rootPath).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo &entryInfo : rootEntries) {
        if (entryInfo.fileName() == domain.lockFileName) {
            if (entryInfo.isSymLink() || !entryInfo.isFile()) {
                result->state = ExtensionStagingBackupListState::Invalid;
                result->issue = code("store-shape-invalid");
                return true;
            }
            continue;
        }
        if (!validBackupId(domain, entryInfo.fileName())
                || entryInfo.isSymLink() || !entryInfo.isDir()) {
            result->state = ExtensionStagingBackupListState::Invalid;
            result->issue = code("store-shape-invalid");
            return true;
        }
        backupIds.append(entryInfo.fileName());
        if (backupIds.size() > maxListingEntries) {
            result->state = ExtensionStagingBackupListState::Invalid;
            result->issue = code("store-shape-invalid");
            return true;
        }
    }
    if (backupIds.isEmpty()) {
        result->state = ExtensionStagingBackupListState::Empty;
        return true;
    }

    QList<ExtensionStagingBackupListEntry> entries;
    for (const QString &backupId : backupIds) {
        const ExtensionStagingBackupListEntry entry =
            scanBackupDirectory(domain, rootPath, backupId);
        // 损坏条目永远留在清单里。作用域清点按可归类的声称主体过滤;主体无法归类的损坏
        // 条目只出现在全主体清点里,绝不被悄悄丢掉。
        if (!subject.isEmpty() && entry.subject != subject) continue;
        entries.append(entry);
    }
    std::sort(entries.begin(), entries.end(),
              [](const ExtensionStagingBackupListEntry &left,
                 const ExtensionStagingBackupListEntry &right) {
        if (left.createdAt.isValid() != right.createdAt.isValid()) {
            return left.createdAt.isValid();
        }
        if (left.createdAt != right.createdAt) {
            return left.createdAt > right.createdAt;
        }
        return left.backupId < right.backupId;
    });
    result->state = ExtensionStagingBackupListState::Ready;
    result->entries = entries;
    return true;
}

ExtensionStagingBackupRemovalResult ExtensionStagingBackupInventory::removeVerified(
        const QString &backupRoot, ConfigurationBackupKeyProvider *keyProvider,
        const QString &backupId)
{
    ExtensionStagingBackupRemovalResult result;
    result.backupId = backupId;
    const ConfigurationBackupStoreDomain domain =
        ConfigurationBackupStore::extensionStagingDomain();
    if (backupRoot.isEmpty() || !keyProvider) {
        result.outcome = ExtensionStagingBackupRemovalOutcome::RequestInvalid;
        result.diagnostic = code("request-invalid");
        return result;
    }
    // id 语法先于任何存储工作:畸形 id 连根目录都不该被触碰。
    if (!validBackupId(domain, backupId)) {
        result.outcome = ExtensionStagingBackupRemovalOutcome::IdMalformed;
        result.diagnostic = code("backup-id-invalid");
        return result;
    }

    ExtensionStagingBackupListResult listing;
    QString listError;
    if (!list(backupRoot, QString(), &listing, &listError)) {
        result.outcome = ExtensionStagingBackupRemovalOutcome::RequestInvalid;
        result.diagnostic = listError;
        return result;
    }
    if (listing.state == ExtensionStagingBackupListState::Unavailable
            || listing.state == ExtensionStagingBackupListState::Invalid) {
        // 清点退化时无法安全确认"是哪一份":拒绝删除,清点的原诊断逐字透传。
        result.outcome = ExtensionStagingBackupRemovalOutcome::ListingDegraded;
        result.diagnostic = listing.issue;
        return result;
    }
    const ExtensionStagingBackupListEntry *found = nullptr;
    for (const ExtensionStagingBackupListEntry &entry : listing.entries) {
        if (entry.backupId == backupId) {
            found = &entry;
            break;
        }
    }
    if (!found) {
        result.outcome = ExtensionStagingBackupRemovalOutcome::NotFound;
        result.diagnostic = code("backup-absent");
        return result;
    }
    result.subject = found->subject;
    if (found->verification
            != ExtensionStagingBackupEntryVerification::ListedIntact) {
        // 验证删除路径无法认证一份结构级损坏的清单:拒绝删除,证据原地保留。
        result.outcome = ExtensionStagingBackupRemovalOutcome::CorruptRefused;
        result.diagnostic = code("backup-corrupt");
        return result;
    }

    ConfigurationBackupStore store(domain, backupRoot, keyProvider);
    QString storeError;
    if (!store.removeVerified(found->subject, backupId,
                              found->manifestIdentity, &storeError)) {
        // 存储层诊断逐字透传:调用方按那些代号理解失败,另造本地代号会让同一个失败在
        // 两条路径上有两个名字。
        result.outcome = ExtensionStagingBackupRemovalOutcome::StoreFailed;
        result.diagnostic = storeError;
        return result;
    }
    result.outcome = ExtensionStagingBackupRemovalOutcome::Removed;
    return result;
}

bool ExtensionStagingBackupInventory::planRetention(
        const QString &backupRoot, const QString &subject,
        ExtensionStagingRetentionPlan *plan, QString *error)
{
    if (error) error->clear();
    if (plan) *plan = ExtensionStagingRetentionPlan();
    if (!plan || backupRoot.isEmpty()) {
        setError(error, code("request-invalid"));
        return false;
    }
    const ConfigurationBackupStoreDomain domain =
        ConfigurationBackupStore::extensionStagingDomain();
    // 主体语法先于一切存储工作。
    if (!validSubject(domain, subject)) {
        setError(error, code("subject-invalid"));
        return false;
    }

    ExtensionStagingBackupListResult listing;
    if (!list(backupRoot, subject, &listing, error)) {
        return false;
    }
    if (listing.state == ExtensionStagingBackupListState::Unavailable
            || listing.state == ExtensionStagingBackupListState::Invalid) {
        // 绝不基于退化输入产出计划:清点的原诊断逐字透传。
        setError(error, listing.issue);
        return false;
    }

    ExtensionStagingRetentionPlan built;
    built.subject = subject;
    built.maxBackups = domain.maxBackups;
    // 清点顺序即 newest-first:完整备份都有有效时间戳,排在前且新到旧。
    QStringList intactIds;
    QList<ExtensionStagingBackupListEntry> corruptEntries;
    for (const ExtensionStagingBackupListEntry &entry : listing.entries) {
        if (entry.verification
                == ExtensionStagingBackupEntryVerification::ListedIntact) {
            intactIds.append(entry.backupId);
        } else {
            corruptEntries.append(entry);
        }
    }
    for (int i = 0; i < intactIds.size() && i < domain.maxBackups; ++i) {
        built.keepBackupIds.append(intactIds.at(i));
    }
    for (int i = domain.maxBackups; i < intactIds.size(); ++i) {
        ExtensionStagingRetentionPruneEntry prune;
        prune.backupId = intactIds.at(i);
        prune.reason = ExtensionStagingPruneReason::OverLimit;
        for (const ExtensionStagingBackupListEntry &entry : listing.entries) {
            if (entry.backupId == intactIds.at(i)) {
                prune.manifestIdentity = entry.manifestIdentity;
                break;
            }
        }
        built.prune.append(prune);
    }
    for (const ExtensionStagingBackupListEntry &entry : corruptEntries) {
        ExtensionStagingRetentionPruneEntry prune;
        prune.backupId = entry.backupId;
        prune.reason = ExtensionStagingPruneReason::Corrupt;
        prune.manifestIdentity = entry.manifestIdentity;
        built.prune.append(prune);
    }
    // 最近一份完整备份无条件保留:这不是隐含的名单成员资格,而是一条显式决策。若计算
    // 出的 keep 集竟不含它,失败关闭而不是产出一份会裁掉它的计划。
    if (!intactIds.isEmpty()) {
        built.newestVerifiedKept = intactIds.first();
        if (!built.keepBackupIds.contains(built.newestVerifiedKept)) {
            setError(error, code("plan-inconsistent"));
            return false;
        }
    }
    *plan = built;
    return true;
}

QList<ExtensionStagingRetentionApplyEntry>
ExtensionStagingBackupInventory::applyRetention(
        const QString &backupRoot, ConfigurationBackupKeyProvider *keyProvider,
        const ExtensionStagingRetentionPlan &plan)
{
    QList<ExtensionStagingRetentionApplyEntry> outcomes;
    for (const ExtensionStagingRetentionPruneEntry &prune : plan.prune) {
        // 逐条组合验证删除:每条独立报告,绝不整体静默成败。
        const ExtensionStagingBackupRemovalResult removal =
            removeVerified(backupRoot, keyProvider, prune.backupId);
        ExtensionStagingRetentionApplyEntry entry;
        entry.backupId = prune.backupId;
        entry.outcome = removal.outcome;
        entry.diagnostic = removal.diagnostic;
        outcomes.append(entry);
    }
    return outcomes;
}
