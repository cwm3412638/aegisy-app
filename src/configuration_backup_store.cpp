#include "configuration_backup_store.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QtEndian>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr int kManifestVersion = 2;
constexpr int kKeyBytes = 32;
constexpr int kNonceBytes = 12;
constexpr int kTagBytes = 16;
const QString kManifestFormat = QStringLiteral("aegisy-tool-config-backup");
const QString kPayloadFormat = QStringLiteral("aegisy-tool-config-backup-payload/0.1");
const QString kCipher = QStringLiteral("AES-256-GCM");
const QString kManifestName = QStringLiteral("manifest.json");
const QString kPendingName = QStringLiteral("manifest.v2.pending");

void setError(QString *error, const QString &value)
{
    if (error) *error = value;
}

bool exactKeys(const QJsonObject &object, const QStringList &expected)
{
    QStringList actual = object.keys();
    QStringList wanted = expected;
    actual.sort();
    wanted.sort();
    return actual == wanted;
}

bool exactInteger(const QJsonValue &value, int minimum, int maximum, int *result)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
            || number < minimum || number > maximum) {
        return false;
    }
    if (result) *result = static_cast<int>(number);
    return true;
}

bool canonicalBase64(const QString &encoded, QByteArray *decoded)
{
    if (!decoded || encoded.size() > ConfigurationBackupStore::MaxManifestBytes * 2) {
        return false;
    }
    const QByteArray latin = encoded.toLatin1();
    if (QString::fromLatin1(latin) != encoded) return false;
    const QByteArray value = QByteArray::fromBase64(latin);
    if (value.toBase64() != latin) return false;
    *decoded = value;
    return true;
}

QString canonicalUtc(const QDateTime &value)
{
    return value.toUTC().toString(Qt::ISODateWithMs);
}

bool parseCanonicalUtc(const QString &text, QDateTime *value)
{
    const QDateTime parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!parsed.isValid() || parsed.offsetFromUtc() != 0
            || canonicalUtc(parsed) != text) {
        return false;
    }
    if (value) *value = parsed.toUTC();
    return true;
}

void appendLengthFramed(QByteArray *output, const QByteArray &value)
{
    const quint64 size = qToBigEndian<quint64>(static_cast<quint64>(value.size()));
    output->append(reinterpret_cast<const char *>(&size), sizeof(size));
    output->append(value);
}

QByteArray associatedData(const QString &tool, const QString &backupId,
                          const QString &createdAt, int fileCount)
{
    static constexpr char prefix[] =
        "aegisy-tool-config-backup-manifest/0.2\0";
    QByteArray output(prefix, sizeof(prefix) - 1);
    appendLengthFramed(&output, tool.toUtf8());
    appendLengthFramed(&output, backupId.toUtf8());
    appendLengthFramed(&output, createdAt.toUtf8());
    const quint64 count = qToBigEndian<quint64>(static_cast<quint64>(fileCount));
    output.append(reinterpret_cast<const char *>(&count), sizeof(count));
    return output;
}

bool encryptGcm(const QByteArray &plainText, const QByteArray &aad,
                const QByteArray &key, const QByteArray &nonce,
                QByteArray *cipherText, QByteArray *tag)
{
    if (!cipherText || !tag || key.size() != kKeyBytes || nonce.size() != kNonceBytes
            || plainText.size() > std::numeric_limits<int>::max()
            || aad.size() > std::numeric_limits<int>::max()) {
        return false;
    }
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    if (!context) return false;

    cipherText->resize(plainText.size() + EVP_MAX_BLOCK_LENGTH);
    int ignored = 0;
    int written = 0;
    int finalWritten = 0;
    const bool ok = EVP_EncryptInit_ex(
            context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(
            context, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) == 1
        && EVP_EncryptInit_ex(
            context, nullptr, nullptr,
            reinterpret_cast<const unsigned char *>(key.constData()),
            reinterpret_cast<const unsigned char *>(nonce.constData())) == 1
        && EVP_EncryptUpdate(
            context, nullptr, &ignored,
            reinterpret_cast<const unsigned char *>(aad.constData()), aad.size()) == 1
        && EVP_EncryptUpdate(
            context, reinterpret_cast<unsigned char *>(cipherText->data()), &written,
            reinterpret_cast<const unsigned char *>(plainText.constData()),
            plainText.size()) == 1
        && EVP_EncryptFinal_ex(
            context,
            reinterpret_cast<unsigned char *>(cipherText->data()) + written,
            &finalWritten) == 1;

    tag->resize(kTagBytes);
    const bool tagOk = ok && EVP_CIPHER_CTX_ctrl(
        context, EVP_CTRL_GCM_GET_TAG, tag->size(), tag->data()) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!tagOk) {
        cipherText->fill('\0');
        cipherText->clear();
        tag->clear();
        return false;
    }
    cipherText->resize(written + finalWritten);
    return true;
}

bool decryptGcm(const QByteArray &cipherText, const QByteArray &aad,
                const QByteArray &key, const QByteArray &nonce,
                const QByteArray &tag, QByteArray *plainText)
{
    if (!plainText || key.size() != kKeyBytes || nonce.size() != kNonceBytes
            || tag.size() != kTagBytes
            || cipherText.size() > std::numeric_limits<int>::max()
            || aad.size() > std::numeric_limits<int>::max()) {
        return false;
    }
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    if (!context) return false;

    plainText->resize(cipherText.size() + EVP_MAX_BLOCK_LENGTH);
    int ignored = 0;
    int written = 0;
    int finalWritten = 0;
    const bool initialized = EVP_DecryptInit_ex(
            context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(
            context, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) == 1
        && EVP_DecryptInit_ex(
            context, nullptr, nullptr,
            reinterpret_cast<const unsigned char *>(key.constData()),
            reinterpret_cast<const unsigned char *>(nonce.constData())) == 1
        && EVP_DecryptUpdate(
            context, nullptr, &ignored,
            reinterpret_cast<const unsigned char *>(aad.constData()), aad.size()) == 1
        && EVP_DecryptUpdate(
            context, reinterpret_cast<unsigned char *>(plainText->data()), &written,
            reinterpret_cast<const unsigned char *>(cipherText.constData()),
            cipherText.size()) == 1
        && EVP_CIPHER_CTX_ctrl(
            context, EVP_CTRL_GCM_SET_TAG, tag.size(),
            const_cast<char *>(tag.constData())) == 1;
    const bool authenticated = initialized && EVP_DecryptFinal_ex(
        context, reinterpret_cast<unsigned char *>(plainText->data()) + written,
        &finalWritten) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!authenticated) {
        plainText->fill('\0');
        plainText->clear();
        return false;
    }
    plainText->resize(written + finalWritten);
    return true;
}

QByteArray sha256Hex(const QByteArray &value)
{
    return QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex();
}

bool validateSnapshot(const ConfigurationBackupSnapshot &snapshot, QString *error)
{
    if (!ConfigurationBackupStore::isValidBackupId(snapshot.backupId)
            || !ConfigurationBackupStore::isValidTool(snapshot.tool)
            || !snapshot.createdAt.isValid()
            || snapshot.files.isEmpty()
            || snapshot.files.size() > ConfigurationBackupStore::MaxFiles) {
        setError(error, QStringLiteral("configuration-backup-metadata-invalid"));
        return false;
    }
    qint64 aggregate = 0;
    for (int i = 0; i < snapshot.files.size(); ++i) {
        const ConfigurationBackupFile &file = snapshot.files.at(i);
        if (file.slot != i || (!file.existed && !file.content.isEmpty())
                || file.content.size() > ConfigurationBackupStore::MaxFileBytes
                || aggregate > ConfigurationBackupStore::MaxPayloadBytes
                    - file.content.size()) {
            setError(error, QStringLiteral("configuration-backup-payload-invalid"));
            return false;
        }
        aggregate += file.content.size();
    }
    return true;
}

QByteArray serializePayload(const ConfigurationBackupSnapshot &snapshot)
{
    QJsonArray files;
    for (const ConfigurationBackupFile &file : snapshot.files) {
        QJsonObject item;
        item.insert(QStringLiteral("byte_count"), file.content.size());
        item.insert(QStringLiteral("content"),
                    QString::fromLatin1(file.content.toBase64()));
        item.insert(QStringLiteral("existed"), file.existed);
        item.insert(QStringLiteral("sha256"),
                    QString::fromLatin1(sha256Hex(file.content)));
        item.insert(QStringLiteral("slot"), file.slot);
        files.append(item);
    }
    QJsonObject payload;
    payload.insert(QStringLiteral("files"), files);
    payload.insert(QStringLiteral("format"), kPayloadFormat);
    return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

void cleanseFiles(QList<ConfigurationBackupFile> *files)
{
    if (!files) return;
    for (ConfigurationBackupFile &file : *files) {
        if (!file.content.isEmpty()) {
            OPENSSL_cleanse(file.content.data(), static_cast<size_t>(file.content.size()));
            file.content.clear();
        }
    }
    files->clear();
}

class ScopedFilesCleanser
{
public:
    explicit ScopedFilesCleanser(QList<ConfigurationBackupFile> *files)
        : m_files(files)
    {
    }
    ~ScopedFilesCleanser() { cleanseFiles(m_files); }

    ScopedFilesCleanser(const ScopedFilesCleanser &) = delete;
    ScopedFilesCleanser &operator=(const ScopedFilesCleanser &) = delete;

private:
    QList<ConfigurationBackupFile> *m_files;
};

bool parsePayload(const QByteArray &bytes, int expectedFileCount,
                  QList<ConfigurationBackupFile> *files, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
            || document.toJson(QJsonDocument::Compact) != bytes) {
        setError(error, QStringLiteral("configuration-backup-payload-invalid"));
        return false;
    }
    const QJsonObject payload = document.object();
    if (!exactKeys(payload, { QStringLiteral("files"), QStringLiteral("format") })
            || payload.value(QStringLiteral("format")).toString() != kPayloadFormat
            || !payload.value(QStringLiteral("files")).isArray()) {
        setError(error, QStringLiteral("configuration-backup-payload-invalid"));
        return false;
    }
    const QJsonArray items = payload.value(QStringLiteral("files")).toArray();
    if (items.size() != expectedFileCount || items.isEmpty()
            || items.size() > ConfigurationBackupStore::MaxFiles) {
        setError(error, QStringLiteral("configuration-backup-payload-invalid"));
        return false;
    }

    QList<ConfigurationBackupFile> parsed;
    const auto reject = [&parsed, error]() {
        cleanseFiles(&parsed);
        setError(error, QStringLiteral("configuration-backup-payload-invalid"));
        return false;
    };
    qint64 aggregate = 0;
    for (int i = 0; i < items.size(); ++i) {
        if (!items.at(i).isObject()) {
            return reject();
        }
        const QJsonObject item = items.at(i).toObject();
        if (!exactKeys(item, {
                QStringLiteral("byte_count"), QStringLiteral("content"),
                QStringLiteral("existed"), QStringLiteral("sha256"),
                QStringLiteral("slot") })
                || !item.value(QStringLiteral("byte_count")).isDouble()
                || !item.value(QStringLiteral("content")).isString()
                || !item.value(QStringLiteral("existed")).isBool()
                || !item.value(QStringLiteral("sha256")).isString()
                || !exactInteger(item.value(QStringLiteral("slot")), i, i, nullptr)) {
            return reject();
        }
        int byteCount = -1;
        QByteArray content;
        if (!exactInteger(item.value(QStringLiteral("byte_count")), 0,
                          static_cast<int>(ConfigurationBackupStore::MaxFileBytes),
                          &byteCount)
                || !canonicalBase64(item.value(QStringLiteral("content")).toString(),
                                    &content)
                || content.size() != byteCount
                || aggregate > ConfigurationBackupStore::MaxPayloadBytes - byteCount
                || item.value(QStringLiteral("sha256")).toString().toLatin1()
                    != sha256Hex(content)) {
            content.fill('\0');
            return reject();
        }
        const bool existed = item.value(QStringLiteral("existed")).toBool();
        if (!existed && !content.isEmpty()) {
            content.fill('\0');
            return reject();
        }
        aggregate += byteCount;
        parsed.append({ i, existed, content });
    }
    *files = parsed;
    return true;
}

QString keyScope(const QString &tool)
{
    return QStringLiteral("tool-manager/config-backup-master/v1/") + tool;
}

bool loadKey(ConfigurationBackupKeyProvider *provider, const QString &tool,
             bool allowCreate, QByteArray *key, QString *error)
{
    if (!provider || !key
            || !provider->keyForScope(keyScope(tool), allowCreate, key, error)
            || key->size() != kKeyBytes) {
        if (key) {
            key->fill('\0');
            key->clear();
        }
        setError(error, QStringLiteral("configuration-backup-key-unavailable"));
        return false;
    }
    return true;
}

bool buildManifest(const ConfigurationBackupSnapshot &snapshot,
                   ConfigurationBackupKeyProvider *provider,
                   bool allowCreateKey, QByteArray *manifestBytes, QString *error)
{
    if (!validateSnapshot(snapshot, error)) return false;
    QByteArray key;
    if (!loadKey(provider, snapshot.tool, allowCreateKey, &key, error)) return false;

    QByteArray nonce(kNonceBytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), nonce.size()) != 1) {
        OPENSSL_cleanse(key.data(), static_cast<size_t>(key.size()));
        setError(error, QStringLiteral("configuration-backup-random-failed"));
        return false;
    }
    QByteArray plainText = serializePayload(snapshot);
    if (plainText.size() > ConfigurationBackupStore::MaxManifestBytes) {
        plainText.fill('\0');
        OPENSSL_cleanse(key.data(), static_cast<size_t>(key.size()));
        setError(error, QStringLiteral("configuration-backup-payload-invalid"));
        return false;
    }
    const QString createdAt = canonicalUtc(snapshot.createdAt);
    QByteArray cipherText;
    QByteArray tag;
    const bool encrypted = encryptGcm(
        plainText,
        associatedData(snapshot.tool, snapshot.backupId, createdAt,
                       snapshot.files.size()),
        key, nonce, &cipherText, &tag);
    plainText.fill('\0');
    OPENSSL_cleanse(key.data(), static_cast<size_t>(key.size()));
    if (!encrypted) {
        setError(error, QStringLiteral("configuration-backup-encryption-failed"));
        return false;
    }

    QJsonObject manifest;
    manifest.insert(QStringLiteral("backup_id"), snapshot.backupId);
    manifest.insert(QStringLiteral("cipher"), kCipher);
    manifest.insert(QStringLiteral("ciphertext"),
                    QString::fromLatin1(cipherText.toBase64()));
    manifest.insert(QStringLiteral("created_at"), createdAt);
    manifest.insert(QStringLiteral("file_count"), snapshot.files.size());
    manifest.insert(QStringLiteral("format"), kManifestFormat);
    manifest.insert(QStringLiteral("nonce"), QString::fromLatin1(nonce.toBase64()));
    manifest.insert(QStringLiteral("tag"), QString::fromLatin1(tag.toBase64()));
    manifest.insert(QStringLiteral("tool"), snapshot.tool);
    manifest.insert(QStringLiteral("version"), kManifestVersion);
    *manifestBytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    if (manifestBytes->isEmpty()
            || manifestBytes->size() > ConfigurationBackupStore::MaxManifestBytes) {
        manifestBytes->clear();
        setError(error, QStringLiteral("configuration-backup-manifest-too-large"));
        return false;
    }
    return true;
}

bool parseManifest(const QByteArray &bytes, const QString &expectedTool,
                   const QString &expectedBackupId,
                   ConfigurationBackupKeyProvider *provider,
                   ConfigurationBackupSnapshot *snapshot, QString *error)
{
    if (!snapshot || bytes.isEmpty()
            || bytes.size() > ConfigurationBackupStore::MaxManifestBytes) {
        setError(error, QStringLiteral("configuration-backup-manifest-invalid"));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
            || document.toJson(QJsonDocument::Compact) != bytes) {
        setError(error, QStringLiteral("configuration-backup-manifest-invalid"));
        return false;
    }
    const QJsonObject manifest = document.object();
    if (!exactKeys(manifest, {
            QStringLiteral("backup_id"), QStringLiteral("cipher"),
            QStringLiteral("ciphertext"), QStringLiteral("created_at"),
            QStringLiteral("file_count"), QStringLiteral("format"),
            QStringLiteral("nonce"), QStringLiteral("tag"),
            QStringLiteral("tool"), QStringLiteral("version") })
            || manifest.value(QStringLiteral("format")).toString() != kManifestFormat
            || !exactInteger(manifest.value(QStringLiteral("version")),
                             kManifestVersion, kManifestVersion, nullptr)
            || manifest.value(QStringLiteral("cipher")).toString() != kCipher
            || manifest.value(QStringLiteral("tool")).toString() != expectedTool
            || manifest.value(QStringLiteral("backup_id")).toString() != expectedBackupId
            || !ConfigurationBackupStore::isValidTool(expectedTool)
            || !ConfigurationBackupStore::isValidBackupId(expectedBackupId)
            || !manifest.value(QStringLiteral("file_count")).isDouble()) {
        setError(error, QStringLiteral("configuration-backup-manifest-invalid"));
        return false;
    }
    int fileCount = -1;
    QDateTime createdAt;
    QByteArray nonce;
    QByteArray tag;
    QByteArray cipherText;
    if (!exactInteger(manifest.value(QStringLiteral("file_count")), 1,
                      ConfigurationBackupStore::MaxFiles, &fileCount)
            || !parseCanonicalUtc(
                manifest.value(QStringLiteral("created_at")).toString(), &createdAt)
            || !canonicalBase64(manifest.value(QStringLiteral("nonce")).toString(),
                                &nonce)
            || !canonicalBase64(manifest.value(QStringLiteral("tag")).toString(),
                                &tag)
            || !canonicalBase64(
                manifest.value(QStringLiteral("ciphertext")).toString(), &cipherText)
            || nonce.size() != kNonceBytes || tag.size() != kTagBytes
            || cipherText.isEmpty()
            || cipherText.size() > ConfigurationBackupStore::MaxManifestBytes) {
        setError(error, QStringLiteral("configuration-backup-manifest-invalid"));
        return false;
    }

    QByteArray key;
    if (!loadKey(provider, expectedTool, false, &key, error)) return false;
    QByteArray plainText;
    const bool decrypted = decryptGcm(
        cipherText,
        associatedData(expectedTool, expectedBackupId, canonicalUtc(createdAt), fileCount),
        key, nonce, tag, &plainText);
    OPENSSL_cleanse(key.data(), static_cast<size_t>(key.size()));
    if (!decrypted) {
        setError(error, QStringLiteral("configuration-backup-authentication-failed"));
        return false;
    }
    QList<ConfigurationBackupFile> files;
    const bool validPayload = parsePayload(plainText, fileCount, &files, error);
    plainText.fill('\0');
    if (!validPayload) return false;

    ConfigurationBackupSnapshot parsed;
    parsed.backupId = expectedBackupId;
    parsed.tool = expectedTool;
    parsed.createdAt = createdAt;
    parsed.files = files;
    *snapshot = parsed;
    return true;
}

bool writeAtomic(const QString &path, const QByteArray &bytes, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
            || file.write(bytes) != bytes.size()
            || !file.commit()) {
        setError(error, QStringLiteral("configuration-backup-write-failed"));
        return false;
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(
            path, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        QFile::remove(path);
        setError(error, QStringLiteral("configuration-backup-permissions-failed"));
        return false;
    }
#endif
    return true;
}

bool readBoundedFile(const QString &path, qint64 maximum,
                     QByteArray *bytes, QString *error, bool allowEmpty = false)
{
    const QFileInfo info(path);
    if (!info.exists() || info.isSymLink() || !info.isFile()
            || (!allowEmpty && info.size() <= 0) || info.size() > maximum) {
        setError(error, QStringLiteral("configuration-backup-file-invalid"));
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("configuration-backup-read-failed"));
        return false;
    }
    const QByteArray value = file.read(maximum + 1);
    if (value.size() != info.size() || value.size() > maximum) {
        setError(error, QStringLiteral("configuration-backup-read-failed"));
        return false;
    }
    *bytes = value;
    return true;
}

bool prepareRoot(const QString &rootPath, QString *error)
{
    if (rootPath.isEmpty() || !QDir::isAbsolutePath(rootPath)) {
        setError(error, QStringLiteral("configuration-backup-root-invalid"));
        return false;
    }
    if (!QDir().mkpath(rootPath)) {
        setError(error, QStringLiteral("configuration-backup-root-unavailable"));
        return false;
    }
    const QFileInfo rootInfo(rootPath);
    if (!rootInfo.isDir() || rootInfo.isSymLink()) {
        setError(error, QStringLiteral("configuration-backup-root-invalid"));
        return false;
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(
            rootPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                | QFileDevice::ExeOwner)) {
        setError(error, QStringLiteral("configuration-backup-permissions-failed"));
        return false;
    }
#endif
    return true;
}

bool lockRoot(const QString &rootPath, QLockFile *lock, QString *error)
{
    lock->setStaleLockTime(30000);
    if (!lock->tryLock(2000)) {
        setError(error, QStringLiteral("configuration-backup-busy"));
        return false;
    }
    Q_UNUSED(rootPath);
    return true;
}

bool exactDirectoryInventory(const QString &directoryPath,
                             const QStringList &allowed, QString *error)
{
    const QDir directory(directoryPath);
    const QFileInfoList entries = directory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    QStringList actual;
    for (const QFileInfo &entry : entries) {
        if (entry.isSymLink() || !entry.isFile()) {
            setError(error, QStringLiteral("configuration-backup-inventory-invalid"));
            return false;
        }
        actual.append(entry.fileName());
    }
    QStringList expected = allowed;
    actual.sort();
    expected.sort();
    if (actual != expected) {
        setError(error, QStringLiteral("configuration-backup-inventory-invalid"));
        return false;
    }
    return true;
}

QString manifestIdentity(const QByteArray &manifestBytes)
{
    static constexpr char prefix[] =
        "aegisy-tool-config-backup-manifest-identity/0.1\0";
    QByteArray material(prefix, sizeof(prefix) - 1);
    material.append(manifestBytes);
    return QStringLiteral("configuration-backup-manifest:sha256:")
        + QString::fromLatin1(
            QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
}

bool validManifestIdentity(const QString &identity)
{
    static const QRegularExpression pattern(QStringLiteral(
        "^configuration-backup-manifest:sha256:[0-9a-f]{64}$"));
    return pattern.match(identity).hasMatch();
}

ConfigurationBackupInventoryState stateForIssue(const QString &issue)
{
    static const QSet<QString> unavailable = {
        QStringLiteral("configuration-backup-busy"),
        QStringLiteral("configuration-backup-key-unavailable"),
        QStringLiteral("configuration-backup-read-failed"),
        QStringLiteral("configuration-backup-write-failed"),
        QStringLiteral("configuration-backup-root-unavailable"),
        QStringLiteral("configuration-backup-permissions-failed"),
        QStringLiteral("configuration-backup-legacy-cleanup-failed"),
        QStringLiteral("configuration-backup-pending-cleanup-failed"),
    };
    return unavailable.contains(issue)
        ? ConfigurationBackupInventoryState::Unavailable
        : ConfigurationBackupInventoryState::Invalid;
}

bool scanRootShape(const QString &rootPath, QStringList *backupIds, QString *error)
{
    const QDir root(rootPath);
    const QFileInfoList entries = root.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    QStringList ids;
    for (const QFileInfo &entry : entries) {
        if (entry.fileName() == QStringLiteral(".backup.lock")) {
            if (entry.isSymLink() || !entry.isFile()) {
                setError(error, QStringLiteral("configuration-backup-inventory-invalid"));
                return false;
            }
            continue;
        }
        if (!ConfigurationBackupStore::isValidBackupId(entry.fileName())
                || entry.isSymLink() || !entry.isDir()) {
            setError(error, QStringLiteral("configuration-backup-inventory-invalid"));
            return false;
        }
        ids.append(entry.fileName());
        if (ids.size() > ConfigurationBackupStore::MaxBackups) {
            setError(error, QStringLiteral("configuration-backup-inventory-invalid"));
            return false;
        }
    }
    if (backupIds) *backupIds = ids;
    return true;
}

struct LegacyManifest {
    QDateTime createdAt;
    QList<ConfigurationBackupFile> files;
    QStringList payloadNames;
};

bool parseLegacyManifest(const QString &directoryPath, int expectedTool,
                         const QStringList &managedPaths, bool allowMissingPayload,
                         LegacyManifest *legacy, QString *error)
{
    QByteArray manifestBytes;
    if (!readBoundedFile(QDir(directoryPath).filePath(kManifestName),
                         ConfigurationBackupStore::MaxManifestBytes,
                         &manifestBytes, error)) {
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(manifestBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("configuration-backup-legacy-invalid"));
        return false;
    }
    const QJsonObject object = document.object();
    if (!exactKeys(object, {
            QStringLiteral("created_at"), QStringLiteral("files"),
            QStringLiteral("tool"), QStringLiteral("version") })
            || !exactInteger(object.value(QStringLiteral("version")), 1, 1, nullptr)
            || !exactInteger(object.value(QStringLiteral("tool")),
                             expectedTool, expectedTool, nullptr)
            || !object.value(QStringLiteral("files")).isArray()) {
        setError(error, QStringLiteral("configuration-backup-legacy-invalid"));
        return false;
    }
    QDateTime createdAt;
    if (!parseCanonicalUtc(object.value(QStringLiteral("created_at")).toString(),
                           &createdAt)) {
        setError(error, QStringLiteral("configuration-backup-legacy-invalid"));
        return false;
    }
    const QJsonArray entries = object.value(QStringLiteral("files")).toArray();
    if (entries.size() != managedPaths.size() || entries.isEmpty()
            || entries.size() > ConfigurationBackupStore::MaxFiles) {
        setError(error, QStringLiteral("configuration-backup-legacy-invalid"));
        return false;
    }
    QList<ConfigurationBackupFile> files;
    QStringList payloadNames;
    qint64 aggregate = 0;
    for (int i = 0; i < entries.size(); ++i) {
        if (!entries.at(i).isObject()) {
            setError(error, QStringLiteral("configuration-backup-legacy-invalid"));
            return false;
        }
        const QJsonObject entry = entries.at(i).toObject();
        const QString payloadName = QStringLiteral("file_%1.bin").arg(i);
        if (!exactKeys(entry, {
                QStringLiteral("existed"), QStringLiteral("path"),
                QStringLiteral("payload") })
                || !entry.value(QStringLiteral("existed")).isBool()
                || entry.value(QStringLiteral("path")).toString() != managedPaths.at(i)
                || entry.value(QStringLiteral("payload")).toString() != payloadName) {
            setError(error, QStringLiteral("configuration-backup-legacy-invalid"));
            return false;
        }
        const bool existed = entry.value(QStringLiteral("existed")).toBool();
        QByteArray content;
        const QString payloadPath = QDir(directoryPath).filePath(payloadName);
        if (existed && QFileInfo::exists(payloadPath)) {
            if (!readBoundedFile(payloadPath, ConfigurationBackupStore::MaxFileBytes,
                                 &content, error, true)) {
                return false;
            }
            if (aggregate > ConfigurationBackupStore::MaxPayloadBytes - content.size()) {
                content.fill('\0');
                setError(error, QStringLiteral("configuration-backup-legacy-invalid"));
                return false;
            }
            aggregate += content.size();
            payloadNames.append(payloadName);
        } else if (existed && !allowMissingPayload) {
            setError(error, QStringLiteral("configuration-backup-legacy-invalid"));
            return false;
        } else if (!existed && QFileInfo::exists(payloadPath)) {
            setError(error, QStringLiteral("configuration-backup-legacy-invalid"));
            return false;
        }
        files.append({ i, existed, content });
    }
    legacy->createdAt = createdAt;
    legacy->files = files;
    legacy->payloadNames = payloadNames;
    return true;
}

} // namespace

ConfigurationBackupStore::ConfigurationBackupStore(
        const QString &rootPath, ConfigurationBackupKeyProvider *keyProvider)
    : m_rootPath(QDir::cleanPath(rootPath)), m_keyProvider(keyProvider)
{
}

bool ConfigurationBackupStore::isValidBackupId(const QString &backupId)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[0-9]{8}_[0-9]{6}_[0-9]{3}_[0-9a-f]{8}$"));
    return pattern.match(backupId).hasMatch();
}

bool ConfigurationBackupStore::isValidTool(const QString &tool)
{
    return tool == QStringLiteral("claude") || tool == QStringLiteral("codex")
        || tool == QStringLiteral("gemini") || tool == QStringLiteral("opencode");
}

bool ConfigurationBackupStore::create(
        const ConfigurationBackupSnapshot &snapshot, QString *error)
{
    if (error) error->clear();
    if (!validateSnapshot(snapshot, error) || !prepareRoot(m_rootPath, error)) {
        return false;
    }
    QLockFile lock(QDir(m_rootPath).filePath(QStringLiteral(".backup.lock")));
    if (!lockRoot(m_rootPath, &lock, error)) return false;

    const QString directoryPath = QDir(m_rootPath).filePath(snapshot.backupId);
    if (QFileInfo::exists(directoryPath) || !QDir().mkdir(directoryPath)) {
        setError(error, QStringLiteral("configuration-backup-already-exists"));
        return false;
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(
            directoryPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                | QFileDevice::ExeOwner)) {
        QDir(directoryPath).removeRecursively();
        setError(error, QStringLiteral("configuration-backup-permissions-failed"));
        return false;
    }
#endif

    QByteArray manifestBytes;
    if (!buildManifest(snapshot, m_keyProvider, true, &manifestBytes, error)
            || !writeAtomic(QDir(directoryPath).filePath(kManifestName),
                            manifestBytes, error)) {
        QDir(directoryPath).removeRecursively();
        return false;
    }
    QByteArray publishedBytes;
    ConfigurationBackupSnapshot verified;
    const bool published = readBoundedFile(
            QDir(directoryPath).filePath(kManifestName),
            MaxManifestBytes, &publishedBytes, error)
        && publishedBytes == manifestBytes
        && parseManifest(publishedBytes, snapshot.tool, snapshot.backupId,
                         m_keyProvider, &verified, error)
        && verified.files.size() == snapshot.files.size();
    cleanseFiles(&verified.files);
    if (!published) {
        QDir(directoryPath).removeRecursively();
        return false;
    }
    return true;
}

bool ConfigurationBackupStore::read(
        const QString &tool, const QString &backupId,
        ConfigurationBackupSnapshot *snapshot, QString *error)
{
    if (error) error->clear();
    if (snapshot) *snapshot = ConfigurationBackupSnapshot();
    if (!snapshot || !isValidTool(tool) || !isValidBackupId(backupId)
            || !prepareRoot(m_rootPath, error)) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("configuration-backup-request-invalid");
        }
        return false;
    }
    QLockFile lock(QDir(m_rootPath).filePath(QStringLiteral(".backup.lock")));
    if (!lockRoot(m_rootPath, &lock, error)) return false;
    const QString directoryPath = QDir(m_rootPath).filePath(backupId);
    const QFileInfo directoryInfo(directoryPath);
    if (!directoryInfo.isDir() || directoryInfo.isSymLink()
            || !exactDirectoryInventory(directoryPath, { kManifestName }, error)) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("configuration-backup-inventory-invalid");
        }
        return false;
    }
    QByteArray bytes;
    if (!readBoundedFile(QDir(directoryPath).filePath(kManifestName),
                         MaxManifestBytes, &bytes, error)) {
        return false;
    }
    ConfigurationBackupSnapshot parsed;
    if (!parseManifest(bytes, tool, backupId, m_keyProvider, &parsed, error)) {
        return false;
    }
    *snapshot = parsed;
    return true;
}

bool ConfigurationBackupStore::migrateLegacy(
        const QString &tool, int legacyToolValue, const QString &backupId,
        const QStringList &managedPaths, QString *error)
{
    if (error) error->clear();
    QSet<QString> uniqueManagedPaths;
    for (const QString &path : managedPaths) uniqueManagedPaths.insert(path);
    if (!isValidTool(tool) || !isValidBackupId(backupId)
            || managedPaths.isEmpty() || managedPaths.size() > MaxFiles
            || uniqueManagedPaths.size() != managedPaths.size()
            || !prepareRoot(m_rootPath, error)) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("configuration-backup-migration-invalid");
        }
        return false;
    }
    QLockFile lock(QDir(m_rootPath).filePath(QStringLiteral(".backup.lock")));
    if (!lockRoot(m_rootPath, &lock, error)) return false;

    return migrateLegacyLocked(
        tool, legacyToolValue, backupId, managedPaths, error);
}

bool ConfigurationBackupStore::migrateLegacyLocked(
        const QString &tool, int legacyToolValue, const QString &backupId,
        const QStringList &managedPaths, QString *error)
{
    QSet<QString> uniqueManagedPaths;
    for (const QString &path : managedPaths) uniqueManagedPaths.insert(path);
    if (!isValidTool(tool) || !isValidBackupId(backupId)
            || managedPaths.isEmpty() || managedPaths.size() > MaxFiles
            || uniqueManagedPaths.size() != managedPaths.size()) {
        setError(error, QStringLiteral("configuration-backup-migration-invalid"));
        return false;
    }

    const QString directoryPath = QDir(m_rootPath).filePath(backupId);
    const QFileInfo directoryInfo(directoryPath);
    if (!directoryInfo.isDir() || directoryInfo.isSymLink()) {
        setError(error, QStringLiteral("configuration-backup-migration-invalid"));
        return false;
    }
    const QString manifestPath = QDir(directoryPath).filePath(kManifestName);
    const QString pendingPath = QDir(directoryPath).filePath(kPendingName);
    QByteArray manifestBytes;
    if (!readBoundedFile(manifestPath, MaxManifestBytes, &manifestBytes, error)) {
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument current = QJsonDocument::fromJson(manifestBytes, &parseError);
    const bool currentV2 = parseError.error == QJsonParseError::NoError
        && current.isObject()
        && current.object().value(QStringLiteral("format")).toString() == kManifestFormat;
    const bool pendingExists = QFileInfo::exists(pendingPath);
    if (currentV2) {
        ConfigurationBackupSnapshot finalSnapshot;
        ScopedFilesCleanser finalFiles(&finalSnapshot.files);
        if (!parseManifest(manifestBytes, tool, backupId, m_keyProvider,
                           &finalSnapshot, error)) {
            return false;
        }
        if (!pendingExists) {
            return exactDirectoryInventory(directoryPath, { kManifestName }, error);
        }
        QByteArray pendingBytes;
        ConfigurationBackupSnapshot pendingSnapshot;
        ScopedFilesCleanser pendingFiles(&pendingSnapshot.files);
        if (!readBoundedFile(pendingPath, MaxManifestBytes, &pendingBytes, error)
                || pendingBytes != manifestBytes
                || !parseManifest(pendingBytes, tool, backupId, m_keyProvider,
                                  &pendingSnapshot, error)
                || !exactDirectoryInventory(
                    directoryPath, { kManifestName, kPendingName }, error)
                || !QFile::remove(pendingPath)) {
            if (error && error->isEmpty()) {
                *error = QStringLiteral("configuration-backup-migration-ambiguous");
            }
            return false;
        }
        return true;
    }

    LegacyManifest legacy;
    ScopedFilesCleanser legacyFiles(&legacy.files);
    if (!parseLegacyManifest(directoryPath, legacyToolValue, managedPaths,
                             pendingExists, &legacy, error)) {
        return false;
    }

    QByteArray pendingBytes;
    ConfigurationBackupSnapshot pendingSnapshot;
    ScopedFilesCleanser pendingFiles(&pendingSnapshot.files);
    QStringList preflightInventory = { kManifestName };
    if (pendingExists) preflightInventory.append(kPendingName);
    for (int i = 0; i < legacy.files.size(); ++i) {
        if (QFileInfo::exists(QDir(directoryPath).filePath(
                QStringLiteral("file_%1.bin").arg(i)))) {
            preflightInventory.append(QStringLiteral("file_%1.bin").arg(i));
        }
    }
    if (!exactDirectoryInventory(directoryPath, preflightInventory, error)) return false;
    if (pendingExists) {
        if (!readBoundedFile(pendingPath, MaxManifestBytes, &pendingBytes, error)
                || !parseManifest(pendingBytes, tool, backupId, m_keyProvider,
                                  &pendingSnapshot, error)
                || pendingSnapshot.createdAt != legacy.createdAt
                || pendingSnapshot.files.size() != managedPaths.size()) {
            if (error && error->isEmpty()) {
                *error = QStringLiteral("configuration-backup-migration-ambiguous");
            }
            return false;
        }
        for (int i = 0; i < legacy.files.size(); ++i) {
            const ConfigurationBackupFile &legacyFile = legacy.files.at(i);
            const ConfigurationBackupFile &pendingFile = pendingSnapshot.files.at(i);
            const QString payloadName = QStringLiteral("file_%1.bin").arg(i);
            if (legacyFile.existed != pendingFile.existed
                    || (legacy.payloadNames.contains(payloadName)
                        && legacyFile.content != pendingFile.content)) {
                setError(error, QStringLiteral("configuration-backup-migration-ambiguous"));
                return false;
            }
        }
    } else {
        ConfigurationBackupSnapshot candidate;
        ScopedFilesCleanser candidateFiles(&candidate.files);
        candidate.backupId = backupId;
        candidate.tool = tool;
        candidate.createdAt = legacy.createdAt;
        candidate.files = legacy.files;
        if (!buildManifest(candidate, m_keyProvider, true, &pendingBytes, error)
                || !writeAtomic(pendingPath, pendingBytes, error)
                || !parseManifest(pendingBytes, tool, backupId, m_keyProvider,
                                  &pendingSnapshot, error)) {
            return false;
        }
    }

    QStringList allowed = { kManifestName, kPendingName };
    for (int i = 0; i < pendingSnapshot.files.size(); ++i) {
        if (QFileInfo::exists(QDir(directoryPath).filePath(
                QStringLiteral("file_%1.bin").arg(i)))) {
            allowed.append(QStringLiteral("file_%1.bin").arg(i));
        }
    }
    if (!exactDirectoryInventory(directoryPath, allowed, error)) return false;
    for (int i = 0; i < pendingSnapshot.files.size(); ++i) {
        const QString payloadPath = QDir(directoryPath).filePath(
            QStringLiteral("file_%1.bin").arg(i));
        if (QFileInfo::exists(payloadPath) && !QFile::remove(payloadPath)) {
            setError(error, QStringLiteral("configuration-backup-legacy-cleanup-failed"));
            return false;
        }
    }
    if (!writeAtomic(manifestPath, pendingBytes, error)) return false;
    if (!QFile::remove(pendingPath)) {
        setError(error, QStringLiteral("configuration-backup-pending-cleanup-failed"));
        return false;
    }
    return exactDirectoryInventory(directoryPath, { kManifestName }, error);
}

ConfigurationBackupInventoryResult ConfigurationBackupStore::inventory(
        const QString &tool, int legacyToolValue,
        const QStringList &managedPaths)
{
    ConfigurationBackupInventoryResult result;
    if (!isValidTool(tool) || m_rootPath.isEmpty()
            || !QDir::isAbsolutePath(m_rootPath)) {
        result.issue = QStringLiteral("configuration-backup-inventory-invalid");
        return result;
    }

    const QFileInfo rootInfo(m_rootPath);
    if (!rootInfo.exists()) {
        result.state = ConfigurationBackupInventoryState::Empty;
        return result;
    }
    if (rootInfo.isSymLink() || !rootInfo.isDir()) {
        result.issue = QStringLiteral("configuration-backup-inventory-invalid");
        return result;
    }
    if (!rootInfo.isReadable()) {
        result.state = ConfigurationBackupInventoryState::Unavailable;
        result.issue = QStringLiteral("configuration-backup-root-unavailable");
        return result;
    }

    QString error;
    QLockFile lock(QDir(m_rootPath).filePath(QStringLiteral(".backup.lock")));
    if (!lockRoot(m_rootPath, &lock, &error)) {
        result.state = ConfigurationBackupInventoryState::Unavailable;
        result.issue = error;
        return result;
    }

    QStringList backupIds;
    if (!scanRootShape(m_rootPath, &backupIds, &error)) {
        result.issue = error;
        return result;
    }
    if (backupIds.isEmpty()) {
        result.state = ConfigurationBackupInventoryState::Empty;
        return result;
    }

    QList<ConfigurationBackupInventoryEntry> entries;
    for (const QString &backupId : backupIds) {
        const QString directoryPath = QDir(m_rootPath).filePath(backupId);
        const QString manifestPath = QDir(directoryPath).filePath(kManifestName);
        QByteArray manifestBytes;
        if (!readBoundedFile(
                manifestPath, MaxManifestBytes, &manifestBytes, &error)) {
            result.state = stateForIssue(error);
            result.issue = error;
            return result;
        }

        QJsonParseError parseError;
        const QJsonDocument current = QJsonDocument::fromJson(
            manifestBytes, &parseError);
        const bool currentV2 = parseError.error == QJsonParseError::NoError
            && current.isObject()
            && current.object().value(QStringLiteral("format")).toString()
                == kManifestFormat;
        if (!currentV2) {
            if (!migrateLegacyLocked(
                    tool, legacyToolValue, backupId, managedPaths, &error)) {
                result.state = stateForIssue(error);
                result.issue = error;
                return result;
            }
            manifestBytes.clear();
            if (!readBoundedFile(
                    manifestPath, MaxManifestBytes, &manifestBytes, &error)) {
                result.state = stateForIssue(error);
                result.issue = error;
                return result;
            }
        }

        if (!exactDirectoryInventory(directoryPath, { kManifestName }, &error)) {
            result.issue = error;
            return result;
        }
        ConfigurationBackupSnapshot snapshot;
        if (!parseManifest(
                manifestBytes, tool, backupId, m_keyProvider, &snapshot, &error)) {
            result.state = stateForIssue(error);
            result.issue = error;
            return result;
        }
        ConfigurationBackupInventoryEntry entry;
        entry.backupId = backupId;
        entry.tool = tool;
        entry.createdAt = snapshot.createdAt;
        entry.fileCount = snapshot.files.size();
        entry.identity = manifestIdentity(manifestBytes);
        cleanseFiles(&snapshot.files);
        entries.append(entry);
    }

    std::sort(entries.begin(), entries.end(),
              [](const ConfigurationBackupInventoryEntry &left,
                 const ConfigurationBackupInventoryEntry &right) {
        if (left.createdAt != right.createdAt) return left.createdAt > right.createdAt;
        return left.backupId < right.backupId;
    });
    result.state = ConfigurationBackupInventoryState::Ready;
    result.entries = entries;
    return result;
}

bool ConfigurationBackupStore::removeVerified(
        const QString &tool, const QString &backupId,
        const QString &expectedIdentity, QString *error)
{
    if (error) error->clear();
    if (!isValidTool(tool) || !isValidBackupId(backupId)
            || !validManifestIdentity(expectedIdentity)) {
        setError(error, QStringLiteral("configuration-backup-remove-invalid"));
        return false;
    }
    const QFileInfo rootInfo(m_rootPath);
    if (!rootInfo.exists() || rootInfo.isSymLink() || !rootInfo.isDir()) {
        setError(error, QStringLiteral("configuration-backup-remove-invalid"));
        return false;
    }

    QLockFile lock(QDir(m_rootPath).filePath(QStringLiteral(".backup.lock")));
    if (!lockRoot(m_rootPath, &lock, error)) return false;
    QStringList backupIds;
    if (!scanRootShape(m_rootPath, &backupIds, error)
            || !backupIds.contains(backupId)) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("configuration-backup-remove-invalid");
        }
        return false;
    }

    const QString directoryPath = QDir(m_rootPath).filePath(backupId);
    const QString manifestPath = QDir(directoryPath).filePath(kManifestName);
    if (!exactDirectoryInventory(directoryPath, { kManifestName }, error)) return false;
    QByteArray manifestBytes;
    ConfigurationBackupSnapshot snapshot;
    if (!readBoundedFile(manifestPath, MaxManifestBytes, &manifestBytes, error)
            || manifestIdentity(manifestBytes) != expectedIdentity
            || !parseManifest(
                manifestBytes, tool, backupId, m_keyProvider, &snapshot, error)) {
        cleanseFiles(&snapshot.files);
        if (error && error->isEmpty()) {
            *error = QStringLiteral("configuration-backup-remove-identity-drift");
        }
        return false;
    }
    cleanseFiles(&snapshot.files);

    QByteArray recheckedBytes;
    if (!readBoundedFile(manifestPath, MaxManifestBytes, &recheckedBytes, error)
            || recheckedBytes != manifestBytes
            || manifestIdentity(recheckedBytes) != expectedIdentity) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("configuration-backup-remove-identity-drift");
        }
        return false;
    }

    const QString quarantineName = QStringLiteral(".removing-") + backupId
        + QLatin1Char('-') + expectedIdentity.right(16);
    const QString quarantinePath = QDir(m_rootPath).filePath(quarantineName);
    QDir root(m_rootPath);
    if (QFileInfo::exists(quarantinePath)
            || !root.rename(backupId, quarantineName)) {
        setError(error, QStringLiteral("configuration-backup-remove-failed"));
        return false;
    }

    const auto restoreDirectoryName = [&]() {
        return root.rename(quarantineName, backupId);
    };

    QByteArray quarantinedBytes;
    ConfigurationBackupSnapshot quarantinedSnapshot;
    const QString quarantinedManifest = QDir(quarantinePath).filePath(kManifestName);
    const bool quarantineVerified = exactDirectoryInventory(
            quarantinePath, { kManifestName }, error)
        && readBoundedFile(
            quarantinedManifest, MaxManifestBytes, &quarantinedBytes, error)
        && quarantinedBytes == manifestBytes
        && manifestIdentity(quarantinedBytes) == expectedIdentity
        && parseManifest(quarantinedBytes, tool, backupId, m_keyProvider,
                         &quarantinedSnapshot, error);
    cleanseFiles(&quarantinedSnapshot.files);
    if (!quarantineVerified) {
        const bool restored = restoreDirectoryName();
        if (!restored) {
            setError(error, QStringLiteral("configuration-backup-remove-recovery-failed"));
        }
        return false;
    }

    const QString preservedName = QStringLiteral(".removed-manifest-") + backupId
        + QLatin1Char('-') + expectedIdentity.right(16);
    const QString preservedPath = QDir(m_rootPath).filePath(preservedName);
    if (QFileInfo::exists(preservedPath)
            || !QFile::rename(quarantinedManifest, preservedPath)) {
        const bool restored = restoreDirectoryName();
        setError(error, restored
            ? QStringLiteral("configuration-backup-remove-failed")
            : QStringLiteral("configuration-backup-remove-recovery-failed"));
        return false;
    }
    if (!root.rmdir(quarantineName)) {
        const bool restoredManifest = QFile::rename(preservedPath, quarantinedManifest);
        const bool restoredDirectory = restoredManifest && restoreDirectoryName();
        setError(error, restoredDirectory
            ? QStringLiteral("configuration-backup-remove-failed")
            : QStringLiteral("configuration-backup-remove-recovery-failed"));
        return false;
    }
    if (!QFile::remove(preservedPath)) {
        setError(error, QStringLiteral("configuration-backup-remove-finalize-failed"));
        return false;
    }
    return true;
}
