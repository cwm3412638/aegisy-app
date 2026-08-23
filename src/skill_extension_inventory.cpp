#include "skill_extension_inventory.h"
#include "strict_json_validator.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QVector>

#include <algorithm>

namespace {

constexpr char kManifestName[] = "aegisy-skill.json";
constexpr char kSkillDocumentName[] = "SKILL.md";

struct TreeEntry {
    QString relativePath;
    bool directory = false;
    QByteArray bytes;
};

struct ScanBudget {
    int entries = 0;
    qint64 bytes = 0;
};

void appendLength(QCryptographicHash *hash, quint64 size)
{
    char encoded[8];
    for (int index = 0; index < 8; ++index) {
        encoded[index] = static_cast<char>((size >> (56 - index * 8)) & 0xff);
    }
    hash->addData(QByteArray(encoded, 8));
}

void appendFramed(QCryptographicHash *hash, const QByteArray &value)
{
    appendLength(hash, static_cast<quint64>(value.size()));
    hash->addData(value);
}

QString digestIdentity(const QByteArray &domain,
                       const QList<QByteArray> &parts,
                       const QString &prefix)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(domain);
    for (const QByteArray &part : parts) appendFramed(&hash, part);
    return prefix + QString::fromLatin1(hash.result().toHex());
}

SkillExtensionInventoryResult failure(SkillExtensionInventoryState state,
                                      const QString &code)
{
    SkillExtensionInventoryResult result;
    result.state = state;
    result.errorCode = code;
    return result;
}

bool safeScalar(const QString &value, int maximum, bool allowEmpty = false)
{
    if ((!allowEmpty && value.isEmpty()) || value.size() > maximum) return false;
    for (const QChar character : value) {
        if (character.unicode() < 0x20 || character == QChar(0x7f)) return false;
    }
    return true;
}

bool registrySafeText(const QString &value, int maximum, bool allowEmpty = false)
{
    if (!safeScalar(value, maximum, allowEmpty)) return false;
    const QString lowered = value.toLower();
    return !lowered.contains(QStringLiteral("authorization"))
        && !lowered.contains(QStringLiteral("api_key"))
        && !lowered.contains(QStringLiteral("api-key"))
        && !lowered.contains(QStringLiteral("password"))
        && !lowered.contains(QStringLiteral("secret="))
        && !lowered.contains(QStringLiteral("token="))
        && !QRegularExpression(QStringLiteral("(^|[^a-z0-9])sk-[a-z0-9_-]{8,}"),
                               QRegularExpression::CaseInsensitiveOption)
                .match(value).hasMatch();
}

bool safeEntryName(const QString &name)
{
    if (!safeScalar(name, 255) || name.toUtf8().size() > 1024
            || name == QStringLiteral(".") || name == QStringLiteral("..")) {
        return false;
    }
    return !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'))
        && !name.contains(QLatin1Char(':'));
}

bool containedBy(const QString &root, const QString &candidate)
{
    const QString normalizedRoot = QDir::cleanPath(QDir::fromNativeSeparators(root));
    const QString normalizedCandidate = QDir::cleanPath(
        QDir::fromNativeSeparators(candidate));
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    return normalizedCandidate.startsWith(normalizedRoot + QLatin1Char('/'), sensitivity);
}

bool readStableFile(const QFileInfo &initial,
                    const QString &root,
                    QByteArray *bytes,
                    SkillExtensionInventoryResult *error)
{
    if (initial.isSymLink() || !initial.isFile() || initial.size() < 0) {
        *error = failure(SkillExtensionInventoryState::Invalid,
                         QStringLiteral("skill-file-invalid"));
        return false;
    }
    if (initial.size() > SkillExtensionInventory::MaxFileBytes) {
        *error = failure(SkillExtensionInventoryState::Invalid,
                         QStringLiteral("skill-file-oversized"));
        return false;
    }
    const QString canonical = initial.canonicalFilePath();
    if (canonical.isEmpty() || !containedBy(root, canonical)) {
        *error = failure(SkillExtensionInventoryState::Invalid,
                         QStringLiteral("skill-path-outside-root"));
        return false;
    }
    QFile file(initial.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        *error = failure(SkillExtensionInventoryState::Unavailable,
                         QStringLiteral("skill-file-unavailable"));
        return false;
    }
    const QByteArray content = file.read(SkillExtensionInventory::MaxFileBytes + 1);
    const bool readFailed = file.error() != QFileDevice::NoError;
    file.close();
    if (readFailed) {
        *error = failure(SkillExtensionInventoryState::Unavailable,
                         QStringLiteral("skill-file-unavailable"));
        return false;
    }
    if (content.size() > SkillExtensionInventory::MaxFileBytes) {
        *error = failure(SkillExtensionInventoryState::Invalid,
                         QStringLiteral("skill-file-oversized"));
        return false;
    }
    QFileInfo final(initial.absoluteFilePath());
    if (final.isSymLink() || !final.isFile() || final.size() != content.size()
            || final.canonicalFilePath() != canonical
            || initial.size() != content.size()) {
        *error = failure(SkillExtensionInventoryState::Invalid,
                         QStringLiteral("skill-file-drift"));
        return false;
    }
    *bytes = content;
    return true;
}

bool scanDirectory(const QString &root,
                   const QString &directory,
                   const QString &relativeDirectory,
                   int depth,
                   ScanBudget *budget,
                   QVector<TreeEntry> *tree,
                   SkillExtensionInventoryResult *error)
{
    if (depth > SkillExtensionInventory::MaxDepth) {
        *error = failure(SkillExtensionInventoryState::Invalid,
                         QStringLiteral("skill-depth-limit"));
        return false;
    }
    const QFileInfo directoryInfo(directory);
    if (directoryInfo.isSymLink() || !directoryInfo.isDir()) {
        *error = failure(SkillExtensionInventoryState::Invalid,
                         QStringLiteral("skill-directory-invalid"));
        return false;
    }
    const QString canonical = directoryInfo.canonicalFilePath();
    if (canonical.isEmpty() || (canonical != root && !containedBy(root, canonical))) {
        *error = failure(SkillExtensionInventoryState::Invalid,
                         QStringLiteral("skill-path-outside-root"));
        return false;
    }

    QDir dir(canonical);
    QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::NoSort);
    std::sort(entries.begin(), entries.end(), [](const QFileInfo &left,
                                                  const QFileInfo &right) {
        return left.fileName().toUtf8() < right.fileName().toUtf8();
    });
    QSet<QString> foldedNames;
    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        if (!safeEntryName(name) || foldedNames.contains(name.toCaseFolded())) {
            *error = failure(SkillExtensionInventoryState::Invalid,
                             QStringLiteral("skill-entry-invalid"));
            return false;
        }
        foldedNames.insert(name.toCaseFolded());
        ++budget->entries;
        if (budget->entries > SkillExtensionInventory::MaxEntries) {
            *error = failure(SkillExtensionInventoryState::Invalid,
                             QStringLiteral("skill-entry-limit"));
            return false;
        }
        if (entry.isSymLink()) {
            *error = failure(SkillExtensionInventoryState::Invalid,
                             QStringLiteral("skill-symlink-invalid"));
            return false;
        }
        const QString relative = relativeDirectory.isEmpty()
            ? name : relativeDirectory + QLatin1Char('/') + name;
        if (relative.toUtf8().size() > 4096) {
            *error = failure(SkillExtensionInventoryState::Invalid,
                             QStringLiteral("skill-path-limit"));
            return false;
        }
        if (entry.isDir()) {
            tree->append(TreeEntry{relative, true, {}});
            if (!scanDirectory(root, entry.absoluteFilePath(), relative, depth + 1,
                               budget, tree, error)) {
                return false;
            }
            continue;
        }
        if (!entry.isFile()) {
            *error = failure(SkillExtensionInventoryState::Invalid,
                             QStringLiteral("skill-entry-invalid"));
            return false;
        }
        QByteArray bytes;
        if (!readStableFile(entry, root, &bytes, error)) return false;
        if (budget->bytes > SkillExtensionInventory::MaxTotalBytes - bytes.size()) {
            *error = failure(SkillExtensionInventoryState::Invalid,
                             QStringLiteral("skill-total-bytes-limit"));
            return false;
        }
        budget->bytes += bytes.size();
        tree->append(TreeEntry{relative, false, bytes});
    }
    return true;
}

const TreeEntry *findFile(const QVector<TreeEntry> &tree, const QString &path)
{
    const auto found = std::find_if(tree.cbegin(), tree.cend(), [&](const TreeEntry &entry) {
        return !entry.directory && entry.relativePath == path;
    });
    return found == tree.cend() ? nullptr : &*found;
}

bool stringArray(const QJsonValue &value,
                 int maximumItems,
                 int maximumLength,
                 QStringList *output)
{
    if (!value.isArray()) return false;
    const QJsonArray array = value.toArray();
    if (array.size() > maximumItems) return false;
    QSet<QString> seen;
    for (const QJsonValue &item : array) {
        if (!item.isString() || !safeScalar(item.toString(), maximumLength)
                || seen.contains(item.toString())) {
            return false;
        }
        seen.insert(item.toString());
        output->append(item.toString());
    }
    return true;
}

bool parseManifest(const QByteArray &bytes,
                   QString *id,
                   QString *name,
                   QString *version,
                   QStringList *capabilities)
{
    if (bytes.isEmpty() || bytes.size() > SkillExtensionInventory::MaxManifestBytes
            || bytes.startsWith("\xef\xbb\xbf")) {
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
            || !StrictJsonValidator::accepts(bytes)) {
        return false;
    }
    const QJsonObject manifest = document.object();
    const QSet<QString> required{
        QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("version"),
        QStringLiteral("executor"), QStringLiteral("enabled"),
        QStringLiteral("trusted"), QStringLiteral("builtin")};
    const QSet<QString> allowed{
        QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("version"),
        QStringLiteral("description"), QStringLiteral("executor"),
        QStringLiteral("enabled"), QStringLiteral("trusted"),
        QStringLiteral("builtin"), QStringLiteral("source"),
        QStringLiteral("required_group"), QStringLiteral("permissions"),
        QStringLiteral("triggers")};
    const QStringList manifestKeys = manifest.keys();
    const QSet<QString> actual(manifestKeys.cbegin(), manifestKeys.cend());
    QSet<QString> missing = required;
    missing.subtract(actual);
    QSet<QString> unknown = actual;
    unknown.subtract(allowed);
    if (!missing.isEmpty() || !unknown.isEmpty()) {
        return false;
    }
    if (!manifest.value(QStringLiteral("id")).isString()
            || !manifest.value(QStringLiteral("name")).isString()
            || !manifest.value(QStringLiteral("version")).isString()
            || !manifest.value(QStringLiteral("executor")).isString()
            || !manifest.value(QStringLiteral("enabled")).isBool()
            || !manifest.value(QStringLiteral("trusted")).isBool()
            || !manifest.value(QStringLiteral("builtin")).isBool()) {
        return false;
    }
    *id = manifest.value(QStringLiteral("id")).toString();
    *name = manifest.value(QStringLiteral("name")).toString();
    *version = manifest.value(QStringLiteral("version")).toString();
    if (!QRegularExpression(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,127}$"))
                .match(*id).hasMatch()
            || !registrySafeText(*name, 128)
            || !registrySafeText(*version, 64)
            || !QSet<QString>{QStringLiteral("instruction"), QStringLiteral("image"),
                              QStringLiteral("presentation")}
                    .contains(manifest.value(QStringLiteral("executor")).toString())) {
        return false;
    }
    for (const auto &field : {
             qMakePair(QStringLiteral("description"), 2048),
             qMakePair(QStringLiteral("source"), 2048),
             qMakePair(QStringLiteral("required_group"), 128)}) {
        if (manifest.contains(field.first)
                && (!manifest.value(field.first).isString()
                    || !safeScalar(manifest.value(field.first).toString(),
                                   field.second, true))) {
            return false;
        }
    }

    QStringList permissions;
    if (manifest.contains(QStringLiteral("permissions"))
            && !stringArray(manifest.value(QStringLiteral("permissions")),
                            16, 64, &permissions)) {
        return false;
    }
    QStringList triggers;
    if (manifest.contains(QStringLiteral("triggers"))
            && !stringArray(manifest.value(QStringLiteral("triggers")),
                            64, 256, &triggers)) {
        return false;
    }
    QSet<QString> requested{QStringLiteral("skill-content")};
    const QSet<QString> allowedPermissions{
        QStringLiteral("files-read"), QStringLiteral("files-write"),
        QStringLiteral("local-process"), QStringLiteral("aegisy-network"),
        QStringLiteral("model-request")};
    for (const QString &permission : permissions) {
        if (!allowedPermissions.contains(permission)) return false;
        if (permission == QStringLiteral("files-read")
                || permission == QStringLiteral("files-write")) {
            requested.insert(QStringLiteral("filesystem-read"));
        } else if (permission == QStringLiteral("local-process")) {
            requested.insert(QStringLiteral("process"));
        } else {
            requested.insert(QStringLiteral("network"));
        }
    }
    *capabilities = requested.values();
    std::sort(capabilities->begin(), capabilities->end());
    return true;
}

QString contentIdentity(const QVector<TreeEntry> &tree)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayLiteral("aegisy-skill-extension-content/0.1\0"));
    for (const TreeEntry &entry : tree) {
        appendFramed(&hash, entry.directory ? QByteArrayLiteral("directory")
                                             : QByteArrayLiteral("file"));
        appendFramed(&hash, entry.relativePath.toUtf8());
        if (!entry.directory) appendFramed(&hash, entry.bytes);
    }
    return QStringLiteral("extension-content:sha256:")
        + QString::fromLatin1(hash.result().toHex());
}

} // namespace

SkillExtensionInventoryResult SkillExtensionInventory::inspectRoot(
    const QString &rootPath)
{
    if (!safeScalar(rootPath, 4096) || rootPath.toUtf8().size() > 16384) {
        return failure(SkillExtensionInventoryState::Invalid,
                       QStringLiteral("skill-root-path-invalid"));
    }
    const QFileInfo suppliedRoot(rootPath);
    if (suppliedRoot.isSymLink()) {
        return failure(SkillExtensionInventoryState::Invalid,
                       QStringLiteral("skill-root-symlink-invalid"));
    }
    if (!suppliedRoot.exists()) {
        SkillExtensionInventoryResult result;
        result.state = SkillExtensionInventoryState::Empty;
        result.sourceIdentity = digestIdentity(
            QByteArrayLiteral("aegisy-skill-inventory-source/0.1\0"),
            {QDir::cleanPath(suppliedRoot.absoluteFilePath()).toUtf8(),
             QByteArrayLiteral("missing")},
            QStringLiteral("skill-inventory-source:sha256:"));
        return result;
    }
    if (!suppliedRoot.isDir()) {
        return failure(SkillExtensionInventoryState::Invalid,
                       QStringLiteral("skill-root-invalid"));
    }
    const QString root = suppliedRoot.canonicalFilePath();
    if (root.isEmpty()) {
        return failure(SkillExtensionInventoryState::Unavailable,
                       QStringLiteral("skill-root-unavailable"));
    }
    QDir rootDirectory(root);
    QFileInfoList skillDirectories = rootDirectory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::NoSort);
    std::sort(skillDirectories.begin(), skillDirectories.end(),
              [](const QFileInfo &left, const QFileInfo &right) {
        return left.fileName().toUtf8() < right.fileName().toUtf8();
    });
    if (skillDirectories.size() > MaxSkills) {
        return failure(SkillExtensionInventoryState::Invalid,
                       QStringLiteral("skill-count-limit"));
    }
    if (skillDirectories.isEmpty()) {
        SkillExtensionInventoryResult result;
        result.state = SkillExtensionInventoryState::Empty;
        result.sourceIdentity = digestIdentity(
            QByteArrayLiteral("aegisy-skill-inventory-source/0.1\0"),
            {root.toUtf8(), QByteArrayLiteral("empty")},
            QStringLiteral("skill-inventory-source:sha256:"));
        return result;
    }

    SkillExtensionInventoryResult result;
    ScanBudget budget;
    QSet<QString> foldedDirectories;
    QSet<QString> ids;
    for (const QFileInfo &skillDirectory : skillDirectories) {
        if (!safeEntryName(skillDirectory.fileName())
                || foldedDirectories.contains(skillDirectory.fileName().toCaseFolded())
                || skillDirectory.isSymLink() || !skillDirectory.isDir()) {
            return failure(SkillExtensionInventoryState::Invalid,
                           QStringLiteral("skill-root-entry-invalid"));
        }
        foldedDirectories.insert(skillDirectory.fileName().toCaseFolded());
        ++budget.entries;
        QVector<TreeEntry> tree;
        SkillExtensionInventoryResult scanError;
        if (!scanDirectory(root, skillDirectory.absoluteFilePath(), QString(), 0,
                           &budget, &tree, &scanError)) {
            return scanError;
        }
        const TreeEntry *manifest = findFile(tree, QString::fromLatin1(kManifestName));
        const TreeEntry *skillDocument = findFile(
            tree, QString::fromLatin1(kSkillDocumentName));
        if (!manifest || !skillDocument || skillDocument->bytes.isEmpty()
                || QString::fromUtf8(skillDocument->bytes).toUtf8()
                    != skillDocument->bytes
                || skillDocument->bytes.contains('\0')) {
            return failure(SkillExtensionInventoryState::Invalid,
                           QStringLiteral("skill-required-content-invalid"));
        }
        QString id;
        QString name;
        QString version;
        QStringList capabilities;
        if (!parseManifest(manifest->bytes, &id, &name, &version, &capabilities)) {
            return failure(SkillExtensionInventoryState::Invalid,
                           QStringLiteral("skill-manifest-invalid"));
        }
        if (ids.contains(id)) {
            return failure(SkillExtensionInventoryState::Invalid,
                           QStringLiteral("skill-id-duplicate"));
        }
        ids.insert(id);

        ExtensionRegistryRecord record;
        record.kind = ExtensionKind::Skill;
        record.id = id;
        record.name = name;
        record.version = version;
        record.sourceKind = ExtensionSourceKind::LocalDirectory;
        record.sourceIdentity = digestIdentity(
            QByteArrayLiteral("aegisy-skill-extension-source/0.1\0"),
            {root.toUtf8(), skillDirectory.fileName().toUtf8()},
            QStringLiteral("extension-source:sha256:"));
        record.contentIdentity = contentIdentity(tree);
        record.trust = ExtensionTrustState::Unverified;
        record.compatibility = ExtensionCompatibilityState::Unknown;
        record.compatibilityReason = QStringLiteral("skill-compatibility-unverified");
        record.scope = QStringLiteral("user");
        record.requestedCapabilities = capabilities;
        record.installed = true;
        record.effectiveEnabled = false;
        record.updateAvailable = false;
        record.recoveryAvailable = false;
        result.records.append(record);
    }

    std::sort(result.records.begin(), result.records.end(),
              [](const ExtensionRegistryRecord &left,
                 const ExtensionRegistryRecord &right) {
        return left.id < right.id;
    });
    QList<QByteArray> rootParts{root.toUtf8()};
    for (const ExtensionRegistryRecord &record : result.records) {
        rootParts.append(record.sourceIdentity.toUtf8());
        rootParts.append(record.contentIdentity.toUtf8());
    }
    result.sourceIdentity = digestIdentity(
        QByteArrayLiteral("aegisy-skill-inventory-source/0.1\0"), rootParts,
        QStringLiteral("skill-inventory-source:sha256:"));
    result.state = SkillExtensionInventoryState::Ready;
    return result;
}
