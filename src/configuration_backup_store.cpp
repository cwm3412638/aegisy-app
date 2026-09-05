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

// 固定诊断代号由域的前缀构成,因此抽取不改变工具域已经被测试与文档固定的任何一个代号。
// `stateForIssue` 按代号后缀裁决清点状态,所以第二个域必须有自己的前缀:共用前缀会让两个
// 域的诊断在日志里无法区分,而按前缀匹配的那一层会把另一个域的失败读成自己的。
QString code(const ConfigurationBackupStoreDomain &domain, const char *suffix)
{
    return domain.errorPrefix + QLatin1Char('-') + QLatin1String(suffix);
}

bool validSubject(const ConfigurationBackupStoreDomain &domain, const QString &subject)
{
    const QRegularExpression pattern(domain.subjectPattern);
    return pattern.match(subject).hasMatch();
}

bool validBackupId(const ConfigurationBackupStoreDomain &domain, const QString &backupId)
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

// 上限跟随域的清单上限:base64 膨胀约 4/3,乘 2 是一个宽松但有界的上界。它必须与域一起变化,
// 否则一个提高了清单上限的域会在解码这一步被静默拒绝。
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

// 前缀由域提供。新域把域标识与载荷形状编进自己的前缀,而不是在这里追加分帧字段:给工具域
// 的前缀追加字段会让既有备份全部失效。
QByteArray associatedData(const ConfigurationBackupStoreDomain &domain,
                          const QString &tool, const QString &backupId,
                          const QString &createdAt, int fileCount)
{
    QByteArray output = domain.aadPrefix;
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

bool validateSnapshot(const ConfigurationBackupStoreDomain &domain, const ConfigurationBackupSnapshot &snapshot,
                      QString *error)
{
    if (!validBackupId(domain, snapshot.backupId)
            || !validSubject(domain, snapshot.tool)
            || !snapshot.createdAt.isValid()
            || snapshot.files.isEmpty()
            || snapshot.files.size() > domain.maxFiles) {
        setError(error, code(domain, "metadata-invalid"));
        return false;
    }
    qint64 aggregate = 0;
    for (int i = 0; i < snapshot.files.size(); ++i) {
        const ConfigurationBackupFile &file = snapshot.files.at(i);
        if (file.slot != i || (!file.existed && !file.content.isEmpty())
                || file.content.size() > domain.maxFileBytes
                || aggregate > domain.maxPayloadBytes
                    - file.content.size()) {
            setError(error, code(domain, "payload-invalid"));
            return false;
        }
        aggregate += file.content.size();
    }
    return true;
}

QByteArray serializePayload(const ConfigurationBackupStoreDomain &domain,
                            const ConfigurationBackupSnapshot &snapshot)
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
    payload.insert(QStringLiteral("format"), domain.payloadFormat);
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

bool parsePayload(const ConfigurationBackupStoreDomain &domain, const QByteArray &bytes, int expectedFileCount,
                  QList<ConfigurationBackupFile> *files, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
            || document.toJson(QJsonDocument::Compact) != bytes) {
        setError(error, code(domain, "payload-invalid"));
        return false;
    }
    const QJsonObject payload = document.object();
    if (!exactKeys(payload, { QStringLiteral("files"), QStringLiteral("format") })
            || payload.value(QStringLiteral("format")).toString() != domain.payloadFormat
            || !payload.value(QStringLiteral("files")).isArray()) {
        setError(error, code(domain, "payload-invalid"));
        return false;
    }
    const QJsonArray items = payload.value(QStringLiteral("files")).toArray();
    if (items.size() != expectedFileCount || items.isEmpty()
            || items.size() > domain.maxFiles) {
        setError(error, code(domain, "payload-invalid"));
        return false;
    }

    QList<ConfigurationBackupFile> parsed;
    const auto reject = [&parsed, &domain, error]() {
        cleanseFiles(&parsed);
        setError(error, code(domain, "payload-invalid"));
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
                          static_cast<int>(domain.maxFileBytes),
                          &byteCount)
                || !canonicalBase64(domain, item.value(QStringLiteral("content")).toString(),
                                    &content)
                || content.size() != byteCount
                || aggregate > domain.maxPayloadBytes - byteCount
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

QString keyScope(const ConfigurationBackupStoreDomain &domain, const QString &tool)
{
    return domain.keyScopePrefix + tool;
}

bool loadKey(const ConfigurationBackupStoreDomain &domain,
             ConfigurationBackupKeyProvider *provider, const QString &tool,
             bool allowCreate, QByteArray *key, QString *error)
{
    if (!provider || !key
            || !provider->keyForScope(keyScope(domain, tool), allowCreate, key, error)
            || key->size() != kKeyBytes) {
        if (key) {
            key->fill('\0');
            key->clear();
        }
        setError(error, code(domain, "key-unavailable"));
        return false;
    }
    return true;
}

bool buildManifest(const ConfigurationBackupStoreDomain &domain, const ConfigurationBackupSnapshot &snapshot,
                   ConfigurationBackupKeyProvider *provider,
                   bool allowCreateKey, QByteArray *manifestBytes, QString *error)
{
    if (!validateSnapshot(domain, snapshot, error)) return false;
    QByteArray key;
    if (!loadKey(domain, provider, snapshot.tool, allowCreateKey, &key, error)) return false;

    QByteArray nonce(kNonceBytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), nonce.size()) != 1) {
        OPENSSL_cleanse(key.data(), static_cast<size_t>(key.size()));
        setError(error, code(domain, "random-failed"));
        return false;
    }
    QByteArray plainText = serializePayload(domain, snapshot);
    if (plainText.size() > domain.maxManifestBytes) {
        plainText.fill('\0');
        OPENSSL_cleanse(key.data(), static_cast<size_t>(key.size()));
        setError(error, code(domain, "payload-invalid"));
        return false;
    }
    const QString createdAt = canonicalUtc(snapshot.createdAt);
    QByteArray cipherText;
    QByteArray tag;
    const bool encrypted = encryptGcm(
        plainText,
        associatedData(domain, snapshot.tool, snapshot.backupId, createdAt,
                       snapshot.files.size()),
        key, nonce, &cipherText, &tag);
    plainText.fill('\0');
    OPENSSL_cleanse(key.data(), static_cast<size_t>(key.size()));
    if (!encrypted) {
        setError(error, code(domain, "encryption-failed"));
        return false;
    }

    QJsonObject manifest;
    manifest.insert(QStringLiteral("backup_id"), snapshot.backupId);
    manifest.insert(QStringLiteral("cipher"), kCipher);
    manifest.insert(QStringLiteral("ciphertext"),
                    QString::fromLatin1(cipherText.toBase64()));
    manifest.insert(QStringLiteral("created_at"), createdAt);
    manifest.insert(QStringLiteral("file_count"), snapshot.files.size());
    manifest.insert(QStringLiteral("format"), domain.manifestFormat);
    manifest.insert(QStringLiteral("nonce"), QString::fromLatin1(nonce.toBase64()));
    manifest.insert(QStringLiteral("tag"), QString::fromLatin1(tag.toBase64()));
    manifest.insert(domain.subjectJsonKey, snapshot.tool);
    manifest.insert(QStringLiteral("version"), kManifestVersion);
    *manifestBytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    if (manifestBytes->isEmpty()
            || manifestBytes->size() > domain.maxManifestBytes) {
        manifestBytes->clear();
        setError(error, code(domain, "manifest-too-large"));
        return false;
    }
    return true;
}

bool parseManifest(const ConfigurationBackupStoreDomain &domain, const QByteArray &bytes, const QString &expectedTool,
                   const QString &expectedBackupId,
                   ConfigurationBackupKeyProvider *provider,
                   ConfigurationBackupSnapshot *snapshot, QString *error)
{
    if (!snapshot || bytes.isEmpty()
            || bytes.size() > domain.maxManifestBytes) {
        setError(error, code(domain, "manifest-invalid"));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
            || document.toJson(QJsonDocument::Compact) != bytes) {
        setError(error, code(domain, "manifest-invalid"));
        return false;
    }
    const QJsonObject manifest = document.object();
    if (!exactKeys(manifest, {
            QStringLiteral("backup_id"), QStringLiteral("cipher"),
            QStringLiteral("ciphertext"), QStringLiteral("created_at"),
            QStringLiteral("file_count"), QStringLiteral("format"),
            QStringLiteral("nonce"), QStringLiteral("tag"),
            domain.subjectJsonKey, QStringLiteral("version") })
            || manifest.value(QStringLiteral("format")).toString() != domain.manifestFormat
            || !exactInteger(manifest.value(QStringLiteral("version")),
                             kManifestVersion, kManifestVersion, nullptr)
            || manifest.value(QStringLiteral("cipher")).toString() != kCipher
            || manifest.value(domain.subjectJsonKey).toString() != expectedTool
            || manifest.value(QStringLiteral("backup_id")).toString() != expectedBackupId
            || !validSubject(domain, expectedTool)
            || !validBackupId(domain, expectedBackupId)
            || !manifest.value(QStringLiteral("file_count")).isDouble()) {
        setError(error, code(domain, "manifest-invalid"));
        return false;
    }
    int fileCount = -1;
    QDateTime createdAt;
    QByteArray nonce;
    QByteArray tag;
    QByteArray cipherText;
    if (!exactInteger(manifest.value(QStringLiteral("file_count")), 1,
                      domain.maxFiles, &fileCount)
            || !parseCanonicalUtc(
                manifest.value(QStringLiteral("created_at")).toString(), &createdAt)
            || !canonicalBase64(domain, manifest.value(QStringLiteral("nonce")).toString(),
                                &nonce)
            || !canonicalBase64(domain, manifest.value(QStringLiteral("tag")).toString(),
                                &tag)
            || !canonicalBase64(domain, 
                manifest.value(QStringLiteral("ciphertext")).toString(), &cipherText)
            || nonce.size() != kNonceBytes || tag.size() != kTagBytes
            || cipherText.isEmpty()
            || cipherText.size() > domain.maxManifestBytes) {
        setError(error, code(domain, "manifest-invalid"));
        return false;
    }

    QByteArray key;
    if (!loadKey(domain, provider, expectedTool, false, &key, error)) return false;
    QByteArray plainText;
    const bool decrypted = decryptGcm(
        cipherText,
        associatedData(domain, expectedTool, expectedBackupId, canonicalUtc(createdAt), fileCount),
        key, nonce, tag, &plainText);
    OPENSSL_cleanse(key.data(), static_cast<size_t>(key.size()));
    if (!decrypted) {
        setError(error, code(domain, "authentication-failed"));
        return false;
    }
    QList<ConfigurationBackupFile> files;
    const bool validPayload = parsePayload(domain, plainText, fileCount, &files, error);
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

bool writeAtomic(const ConfigurationBackupStoreDomain &domain, const QString &path, const QByteArray &bytes,
                 QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
            || file.write(bytes) != bytes.size()
            || !file.commit()) {
        setError(error, code(domain, "write-failed"));
        return false;
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(
            path, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        QFile::remove(path);
        setError(error, code(domain, "permissions-failed"));
        return false;
    }
#endif
    return true;
}

bool readBoundedFile(const ConfigurationBackupStoreDomain &domain, const QString &path, qint64 maximum,
                     QByteArray *bytes, QString *error, bool allowEmpty = false)
{
    const QFileInfo info(path);
    if (!info.exists() || info.isSymLink() || !info.isFile()
            || (!allowEmpty && info.size() <= 0) || info.size() > maximum) {
        setError(error, code(domain, "file-invalid"));
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, code(domain, "read-failed"));
        return false;
    }
    const QByteArray value = file.read(maximum + 1);
    if (value.size() != info.size() || value.size() > maximum) {
        setError(error, code(domain, "read-failed"));
        return false;
    }
    *bytes = value;
    return true;
}

bool prepareRoot(const ConfigurationBackupStoreDomain &domain, const QString &rootPath, QString *error)
{
    if (rootPath.isEmpty() || !QDir::isAbsolutePath(rootPath)) {
        setError(error, code(domain, "root-invalid"));
        return false;
    }
    if (!QDir().mkpath(rootPath)) {
        setError(error, code(domain, "root-unavailable"));
        return false;
    }
    const QFileInfo rootInfo(rootPath);
    if (!rootInfo.isDir() || rootInfo.isSymLink()) {
        setError(error, code(domain, "root-invalid"));
        return false;
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(
            rootPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                | QFileDevice::ExeOwner)) {
        setError(error, code(domain, "permissions-failed"));
        return false;
    }
#endif
    return true;
}

bool lockRoot(const ConfigurationBackupStoreDomain &domain, const QString &rootPath, QLockFile *lock, QString *error)
{
    lock->setStaleLockTime(30000);
    if (!lock->tryLock(2000)) {
        setError(error, code(domain, "busy"));
        return false;
    }
    Q_UNUSED(rootPath);
    return true;
}

bool exactDirectoryInventory(const ConfigurationBackupStoreDomain &domain, const QString &directoryPath,
                             const QStringList &allowed, QString *error)
{
    const QDir directory(directoryPath);
    const QFileInfoList entries = directory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    QStringList actual;
    for (const QFileInfo &entry : entries) {
        if (entry.isSymLink() || !entry.isFile()) {
            setError(error, code(domain, "inventory-invalid"));
            return false;
        }
        actual.append(entry.fileName());
    }
    QStringList expected = allowed;
    actual.sort();
    expected.sort();
    if (actual != expected) {
        setError(error, code(domain, "inventory-invalid"));
        return false;
    }
    return true;
}

QString manifestIdentity(const ConfigurationBackupStoreDomain &domain,
                         const QByteArray &manifestBytes)
{
    QByteArray material = domain.identityDomain;
    material.append(manifestBytes);
    return domain.identityPrefix
        + QString::fromLatin1(
            QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
}

bool validManifestIdentity(const ConfigurationBackupStoreDomain &domain,
                           const QString &identity)
{
    // 逐次构造:模式来自域,因此不能是函数静态量。
    const QRegularExpression pattern(domain.identityPattern);
    return pattern.match(identity).hasMatch();
}

ConfigurationBackupInventoryState stateForIssue(
    const ConfigurationBackupStoreDomain &domain, const QString &issue)
{
    // 逐次构造:代号来自域前缀,因此不能是函数静态量——静态量会把第一个被实例化的域的代号
    // 集合永久固定下来,于是第二个域的"存储不可用"会被读成"证据无效"。
    const QSet<QString> unavailable = {
        code(domain, "busy"),
        code(domain, "key-unavailable"),
        code(domain, "read-failed"),
        code(domain, "write-failed"),
        code(domain, "root-unavailable"),
        code(domain, "permissions-failed"),
        code(domain, "legacy-cleanup-failed"),
        code(domain, "pending-cleanup-failed"),
    };
    return unavailable.contains(issue)
        ? ConfigurationBackupInventoryState::Unavailable
        : ConfigurationBackupInventoryState::Invalid;
}

bool scanRootShape(const ConfigurationBackupStoreDomain &domain, const QString &rootPath, QStringList *backupIds,
                   int maxBackupCount, QString *error)
{
    const QDir root(rootPath);
    const QFileInfoList entries = root.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    QStringList ids;
    for (const QFileInfo &entry : entries) {
        if (entry.fileName() == domain.lockFileName) {
            if (entry.isSymLink() || !entry.isFile()) {
                setError(error, code(domain, "inventory-invalid"));
                return false;
            }
            continue;
        }
        if (!validBackupId(domain, entry.fileName())
                || entry.isSymLink() || !entry.isDir()) {
            setError(error, code(domain, "inventory-invalid"));
            return false;
        }
        ids.append(entry.fileName());
        if (ids.size() > maxBackupCount) {
            setError(error, code(domain, "inventory-invalid"));
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

bool parseLegacyManifest(const ConfigurationBackupStoreDomain &domain, const QString &directoryPath, int expectedTool,
                         const QStringList &managedPaths, bool allowMissingPayload,
                         LegacyManifest *legacy, QString *error)
{
    QByteArray manifestBytes;
    if (!readBoundedFile(domain, QDir(directoryPath).filePath(domain.manifestName),
                         domain.maxManifestBytes,
                         &manifestBytes, error)) {
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(manifestBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, code(domain, "legacy-invalid"));
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
        setError(error, code(domain, "legacy-invalid"));
        return false;
    }
    QDateTime createdAt;
    if (!parseCanonicalUtc(object.value(QStringLiteral("created_at")).toString(),
                           &createdAt)) {
        setError(error, code(domain, "legacy-invalid"));
        return false;
    }
    const QJsonArray entries = object.value(QStringLiteral("files")).toArray();
    if (entries.size() != managedPaths.size() || entries.isEmpty()
            || entries.size() > domain.maxFiles) {
        setError(error, code(domain, "legacy-invalid"));
        return false;
    }
    QList<ConfigurationBackupFile> files;
    QStringList payloadNames;
    qint64 aggregate = 0;
    for (int i = 0; i < entries.size(); ++i) {
        if (!entries.at(i).isObject()) {
            setError(error, code(domain, "legacy-invalid"));
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
            setError(error, code(domain, "legacy-invalid"));
            return false;
        }
        const bool existed = entry.value(QStringLiteral("existed")).toBool();
        QByteArray content;
        const QString payloadPath = QDir(directoryPath).filePath(payloadName);
        if (existed && QFileInfo::exists(payloadPath)) {
            if (!readBoundedFile(domain, payloadPath, domain.maxFileBytes,
                                 &content, error, true)) {
                return false;
            }
            if (aggregate > domain.maxPayloadBytes - content.size()) {
                content.fill('\0');
                setError(error, code(domain, "legacy-invalid"));
                return false;
            }
            aggregate += content.size();
            payloadNames.append(payloadName);
        } else if (existed && !allowMissingPayload) {
            setError(error, code(domain, "legacy-invalid"));
            return false;
        } else if (!existed && QFileInfo::exists(payloadPath)) {
            setError(error, code(domain, "legacy-invalid"));
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

bool ConfigurationBackupStoreDomain::configured() const
{
    // 每一个字段都必须被填写。一个半填的域不会报错——它只会让某一项检查退化成"没有约束",
    // 例如空的 AAD 前缀会让跨域互认的最后一道防线消失。
    return !aadPrefix.isEmpty() && !manifestFormat.isEmpty()
        && !payloadFormat.isEmpty() && !identityDomain.isEmpty()
        && !identityPrefix.isEmpty() && !identityPattern.isEmpty()
        && !keyScopePrefix.isEmpty() && !subjectJsonKey.isEmpty()
        && !subjectPattern.isEmpty()
        && !manifestName.isEmpty() && !pendingName.isEmpty()
        && !lockFileName.isEmpty() && !backupIdPattern.isEmpty()
        && !errorPrefix.isEmpty()
        // 上限为 0 意味着任何载荷都超限,而那会把存储变成永久拒绝——同样是一种沉默的失效。
        && maxFiles > 0 && maxFileBytes > 0 && maxPayloadBytes > 0
        && maxManifestBytes > 0 && maxBackups > 0
        // 清单名与 pending 名相同会让崩溃恢复权威覆盖正式清单;锁名与两者相同会让根目录
        // 扫描把锁当成清单。三者必须互不相同。
        && manifestName != pendingName && manifestName != lockFileName
        && pendingName != lockFileName;
}

ConfigurationBackupStoreDomain ConfigurationBackupStore::toolDomain()
{
    ConfigurationBackupStoreDomain value;
    // 以下每一个字面量都已经随既有备份发布,逐字节固定。两处带内嵌 NUL 的前缀尤其危险:
    // 写错会静默改变每一份 AAD 与每一个清单身份,于是既有备份在需要回滚的那一刻才被发现
    // 无法解密。`configuration_backup_store_domain` 用金字符串测试把它们钉住。
    static constexpr char kAadPrefix[] =
        "aegisy-tool-config-backup-manifest/0.2\0";
    static constexpr char kIdentityDomain[] =
        "aegisy-tool-config-backup-manifest-identity/0.1\0";
    value.aadPrefix = QByteArray(kAadPrefix, sizeof(kAadPrefix) - 1);
    value.manifestFormat = kManifestFormat;
    value.payloadFormat = kPayloadFormat;
    value.identityDomain = QByteArray(kIdentityDomain, sizeof(kIdentityDomain) - 1);
    value.identityPrefix = QStringLiteral("configuration-backup-manifest:sha256:");
    value.identityPattern =
        QStringLiteral("^configuration-backup-manifest:sha256:[0-9a-f]{64}$");
    value.keyScopePrefix = QStringLiteral("tool-manager/config-backup-master/v1/");
    value.subjectJsonKey = QStringLiteral("tool");
    value.manifestName = kManifestName;
    value.pendingName = kPendingName;
    value.lockFileName = QStringLiteral(".backup.lock");
    value.backupIdPattern =
        QStringLiteral("^[0-9]{8}_[0-9]{6}_[0-9]{3}_[0-9a-f]{8}$");
    value.subjectPattern = QStringLiteral("^(claude|codex|gemini|opencode)$");
    value.maxFiles = MaxFiles;
    value.maxFileBytes = MaxFileBytes;
    value.maxPayloadBytes = MaxPayloadBytes;
    value.maxManifestBytes = MaxManifestBytes;
    value.maxBackups = MaxBackups;
    value.errorPrefix = QStringLiteral("configuration-backup");
    // 工具域确实有 v1 历史需要搬运,因此只有它开启这条路径。
    value.legacyV1MigrationEnabled = true;
    return value;
}

ConfigurationBackupStoreDomain ConfigurationBackupStore::extensionStagingDomain()
{
    ConfigurationBackupStoreDomain value;
    // 扩展暂存域的字节一旦发布就不能再改。AAD 与身份域都带内嵌 NUL,保持与工具域
    // 相同的分帧习惯,但使用完全不同的域标识,即使两个域意外拿到同一把密钥也无法互认。
    static constexpr char kAadPrefix[] =
        "aegisy-extension-staging-backup-manifest/0.1\0";
    static constexpr char kIdentityDomain[] =
        "aegisy-extension-staging-backup-manifest-identity/0.1\0";
    value.aadPrefix = QByteArray(kAadPrefix, sizeof(kAadPrefix) - 1);
    value.manifestFormat = QStringLiteral("aegisy-extension-staging-backup");
    value.payloadFormat =
        QStringLiteral("aegisy-extension-staging-backup-payload/0.1");
    value.identityDomain =
        QByteArray(kIdentityDomain, sizeof(kIdentityDomain) - 1);
    value.identityPrefix =
        QStringLiteral("extension-staging-backup-manifest:sha256:");
    value.identityPattern =
        QStringLiteral("^extension-staging-backup-manifest:sha256:[0-9a-f]{64}$");
    value.keyScopePrefix =
        QStringLiteral("aegisy/extension-staging-backup-master/v1/");
    value.subjectJsonKey = QStringLiteral("extension");
    value.manifestName = QStringLiteral("manifest.json");
    value.pendingName = QStringLiteral("manifest.v2.pending");
    value.lockFileName = QStringLiteral(".backup.lock");
    value.backupIdPattern =
        QStringLiteral("^ext_[0-9]{8}_[0-9]{6}_[0-9a-f]{8}$");
    // 以 kind:id 作为主体,避免与工具域的 claude/codex/gemini/opencode 命名空间重叠。
    value.subjectPattern =
        QStringLiteral("^(codex-plugin|skill|mcp):[a-z0-9][a-z0-9._-]{0,127}$");
    value.maxFiles = 256;
    value.maxFileBytes = 4 * 1024 * 1024;
    value.maxPayloadBytes = 64 * 1024 * 1024;
    value.maxManifestBytes = 32 * 1024 * 1024;
    value.maxBackups = 32;
    value.errorPrefix = QStringLiteral("extension-staging-backup");
    // 扩展域没有历史 v1 格式,不得从未经认证的清单触发迁移写入。
    value.legacyV1MigrationEnabled = false;
    return value;
}

ConfigurationBackupStore::ConfigurationBackupStore(
        const QString &rootPath, ConfigurationBackupKeyProvider *keyProvider)
    : m_domain(toolDomain())
    , m_rootPath(QDir::cleanPath(rootPath))
    , m_keyProvider(keyProvider)
{
}

ConfigurationBackupStore::ConfigurationBackupStore(
        const ConfigurationBackupStoreDomain &domain, const QString &rootPath,
        ConfigurationBackupKeyProvider *keyProvider)
    : m_domain(domain)
    , m_rootPath(QDir::cleanPath(rootPath))
    , m_keyProvider(keyProvider)
{
}

// 两个静态包装保留并委托到工具域。它们有本存储之外的调用方——激活日志按备份代号与身份前缀
// 校验记录——因此改动它们会让激活日志开始拒绝合法记录。
bool ConfigurationBackupStore::isValidBackupId(const QString &backupId)
{
    return validBackupId(toolDomain(), backupId);
}

bool ConfigurationBackupStore::isValidTool(const QString &tool)
{
    return validSubject(toolDomain(), tool);
}

bool ConfigurationBackupStore::create(
        const ConfigurationBackupSnapshot &snapshot, QString *error)
{
    if (error) error->clear();
    if (!validateSnapshot(m_domain, snapshot, error) || !prepareRoot(m_domain, m_rootPath, error)) {
        return false;
    }
    QLockFile lock(QDir(m_rootPath).filePath(m_domain.lockFileName));
    if (!lockRoot(m_domain, m_rootPath, &lock, error)) return false;

    const QString directoryPath = QDir(m_rootPath).filePath(snapshot.backupId);
    if (QFileInfo::exists(directoryPath) || !QDir().mkdir(directoryPath)) {
        setError(error, code(m_domain, "already-exists"));
        return false;
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(
            directoryPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                | QFileDevice::ExeOwner)) {
        QDir(directoryPath).removeRecursively();
        setError(error, code(m_domain, "permissions-failed"));
        return false;
    }
#endif

    QByteArray manifestBytes;
    if (!buildManifest(m_domain, snapshot, m_keyProvider, true, &manifestBytes, error)
            || !writeAtomic(m_domain, QDir(directoryPath).filePath(m_domain.manifestName),
                            manifestBytes, error)) {
        QDir(directoryPath).removeRecursively();
        return false;
    }
    QByteArray publishedBytes;
    ConfigurationBackupSnapshot verified;
    const bool published = readBoundedFile(m_domain, 
            QDir(directoryPath).filePath(m_domain.manifestName),
            m_domain.maxManifestBytes, &publishedBytes, error)
        && publishedBytes == manifestBytes
        && parseManifest(m_domain, publishedBytes, snapshot.tool, snapshot.backupId,
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
    if (!snapshot || !validSubject(m_domain, tool) || !validBackupId(m_domain, backupId)
            || !prepareRoot(m_domain, m_rootPath, error)) {
        if (error && error->isEmpty()) {
            *error = code(m_domain, "request-invalid");
        }
        return false;
    }
    QLockFile lock(QDir(m_rootPath).filePath(m_domain.lockFileName));
    if (!lockRoot(m_domain, m_rootPath, &lock, error)) return false;
    const QString directoryPath = QDir(m_rootPath).filePath(backupId);
    const QFileInfo directoryInfo(directoryPath);
    if (!directoryInfo.isDir() || directoryInfo.isSymLink()
            || !exactDirectoryInventory(m_domain, directoryPath, { m_domain.manifestName }, error)) {
        if (error && error->isEmpty()) {
            *error = code(m_domain, "inventory-invalid");
        }
        return false;
    }
    QByteArray bytes;
    if (!readBoundedFile(m_domain, QDir(directoryPath).filePath(m_domain.manifestName),
                         m_domain.maxManifestBytes, &bytes, error)) {
        return false;
    }
    ConfigurationBackupSnapshot parsed;
    if (!parseManifest(m_domain, bytes, tool, backupId, m_keyProvider, &parsed, error)) {
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
    // 没有 v1 历史的域不得走这条路径。这是本存储唯一一处依据未经认证的输入清单去写盘的
    // 动作,继承它等于凭一份任何人都能放进目录的明文清单触发写入。因此它默认关闭,而且
    // 在做任何事情之前就拒绝——包括不建立根目录、不取锁。
    if (!m_domain.legacyV1MigrationEnabled) {
        if (error) *error = code(m_domain, "migration-unsupported");
        return false;
    }
    QSet<QString> uniqueManagedPaths;
    for (const QString &path : managedPaths) uniqueManagedPaths.insert(path);
    if (!validSubject(m_domain, tool) || !validBackupId(m_domain, backupId)
            || managedPaths.isEmpty() || managedPaths.size() > m_domain.maxFiles
            || uniqueManagedPaths.size() != managedPaths.size()
            || !prepareRoot(m_domain, m_rootPath, error)) {
        if (error && error->isEmpty()) {
            *error = code(m_domain, "migration-invalid");
        }
        return false;
    }
    QLockFile lock(QDir(m_rootPath).filePath(m_domain.lockFileName));
    if (!lockRoot(m_domain, m_rootPath, &lock, error)) return false;

    return migrateLegacyLocked(
        tool, legacyToolValue, backupId, managedPaths, error);
}

bool ConfigurationBackupStore::migrateLegacyLocked(
        const QString &tool, int legacyToolValue, const QString &backupId,
        const QStringList &managedPaths, QString *error)
{
    QSet<QString> uniqueManagedPaths;
    for (const QString &path : managedPaths) uniqueManagedPaths.insert(path);
    if (!validSubject(m_domain, tool) || !validBackupId(m_domain, backupId)
            || managedPaths.isEmpty() || managedPaths.size() > m_domain.maxFiles
            || uniqueManagedPaths.size() != managedPaths.size()) {
        setError(error, code(m_domain, "migration-invalid"));
        return false;
    }

    const QString directoryPath = QDir(m_rootPath).filePath(backupId);
    const QFileInfo directoryInfo(directoryPath);
    if (!directoryInfo.isDir() || directoryInfo.isSymLink()) {
        setError(error, code(m_domain, "migration-invalid"));
        return false;
    }
    const QString manifestPath = QDir(directoryPath).filePath(m_domain.manifestName);
    const QString pendingPath = QDir(directoryPath).filePath(m_domain.pendingName);
    QByteArray manifestBytes;
    if (!readBoundedFile(m_domain, manifestPath, m_domain.maxManifestBytes, &manifestBytes, error)) {
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument current = QJsonDocument::fromJson(manifestBytes, &parseError);
    const bool currentV2 = parseError.error == QJsonParseError::NoError
        && current.isObject()
        && current.object().value(QStringLiteral("format")).toString() == m_domain.manifestFormat;
    const bool pendingExists = QFileInfo::exists(pendingPath);
    if (currentV2) {
        ConfigurationBackupSnapshot finalSnapshot;
        ScopedFilesCleanser finalFiles(&finalSnapshot.files);
        if (!parseManifest(m_domain, manifestBytes, tool, backupId, m_keyProvider,
                           &finalSnapshot, error)) {
            return false;
        }
        if (!pendingExists) {
            return exactDirectoryInventory(m_domain, directoryPath, { m_domain.manifestName }, error);
        }
        QByteArray pendingBytes;
        ConfigurationBackupSnapshot pendingSnapshot;
        ScopedFilesCleanser pendingFiles(&pendingSnapshot.files);
        if (!readBoundedFile(m_domain, pendingPath, m_domain.maxManifestBytes, &pendingBytes, error)
                || pendingBytes != manifestBytes
                || !parseManifest(m_domain, pendingBytes, tool, backupId, m_keyProvider,
                                  &pendingSnapshot, error)
                || !exactDirectoryInventory(m_domain, 
                    directoryPath, { m_domain.manifestName, m_domain.pendingName }, error)
                || !QFile::remove(pendingPath)) {
            if (error && error->isEmpty()) {
                *error = code(m_domain, "migration-ambiguous");
            }
            return false;
        }
        return true;
    }

    LegacyManifest legacy;
    ScopedFilesCleanser legacyFiles(&legacy.files);
    if (!parseLegacyManifest(m_domain, directoryPath, legacyToolValue, managedPaths,
                             pendingExists, &legacy, error)) {
        return false;
    }

    QByteArray pendingBytes;
    ConfigurationBackupSnapshot pendingSnapshot;
    ScopedFilesCleanser pendingFiles(&pendingSnapshot.files);
    QStringList preflightInventory = { m_domain.manifestName };
    if (pendingExists) preflightInventory.append(m_domain.pendingName);
    for (int i = 0; i < legacy.files.size(); ++i) {
        if (QFileInfo::exists(QDir(directoryPath).filePath(
                QStringLiteral("file_%1.bin").arg(i)))) {
            preflightInventory.append(QStringLiteral("file_%1.bin").arg(i));
        }
    }
    if (!exactDirectoryInventory(m_domain, directoryPath, preflightInventory, error)) return false;
    if (pendingExists) {
        if (!readBoundedFile(m_domain, pendingPath, m_domain.maxManifestBytes, &pendingBytes, error)
                || !parseManifest(m_domain, pendingBytes, tool, backupId, m_keyProvider,
                                  &pendingSnapshot, error)
                || pendingSnapshot.createdAt != legacy.createdAt
                || pendingSnapshot.files.size() != managedPaths.size()) {
            if (error && error->isEmpty()) {
                *error = code(m_domain, "migration-ambiguous");
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
                setError(error, code(m_domain, "migration-ambiguous"));
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
        if (!buildManifest(m_domain, candidate, m_keyProvider, true, &pendingBytes, error)
                || !writeAtomic(m_domain, pendingPath, pendingBytes, error)
                || !parseManifest(m_domain, pendingBytes, tool, backupId, m_keyProvider,
                                  &pendingSnapshot, error)) {
            return false;
        }
    }

    QStringList allowed = { m_domain.manifestName, m_domain.pendingName };
    for (int i = 0; i < pendingSnapshot.files.size(); ++i) {
        if (QFileInfo::exists(QDir(directoryPath).filePath(
                QStringLiteral("file_%1.bin").arg(i)))) {
            allowed.append(QStringLiteral("file_%1.bin").arg(i));
        }
    }
    if (!exactDirectoryInventory(m_domain, directoryPath, allowed, error)) return false;
    for (int i = 0; i < pendingSnapshot.files.size(); ++i) {
        const QString payloadPath = QDir(directoryPath).filePath(
            QStringLiteral("file_%1.bin").arg(i));
        if (QFileInfo::exists(payloadPath) && !QFile::remove(payloadPath)) {
            setError(error, code(m_domain, "legacy-cleanup-failed"));
            return false;
        }
    }
    if (!writeAtomic(m_domain, manifestPath, pendingBytes, error)) return false;
    if (!QFile::remove(pendingPath)) {
        setError(error, code(m_domain, "pending-cleanup-failed"));
        return false;
    }
    return exactDirectoryInventory(m_domain, directoryPath, { m_domain.manifestName }, error);
}

ConfigurationBackupInventoryResult ConfigurationBackupStore::inventory(
        const QString &tool, int legacyToolValue,
        const QStringList &managedPaths)
{
    ConfigurationBackupInventoryResult result;
    if (!validSubject(m_domain, tool) || m_rootPath.isEmpty()
            || !QDir::isAbsolutePath(m_rootPath)) {
        result.issue = code(m_domain, "inventory-invalid");
        return result;
    }

    const QFileInfo rootInfo(m_rootPath);
    if (!rootInfo.exists()) {
        result.state = ConfigurationBackupInventoryState::Empty;
        return result;
    }
    if (rootInfo.isSymLink() || !rootInfo.isDir()) {
        result.issue = code(m_domain, "inventory-invalid");
        return result;
    }
    if (!rootInfo.isReadable()) {
        result.state = ConfigurationBackupInventoryState::Unavailable;
        result.issue = code(m_domain, "root-unavailable");
        return result;
    }

    QString error;
    QLockFile lock(QDir(m_rootPath).filePath(m_domain.lockFileName));
    if (!lockRoot(m_domain, m_rootPath, &lock, &error)) {
        result.state = ConfigurationBackupInventoryState::Unavailable;
        result.issue = error;
        return result;
    }

    // 扫描上限是 maxBackups 的 4 倍,与 `removeVerified` 及暂存清点层同宽:按主体的清点必须
    // 先看到每一个目录才能分辨"别人主体的完整备份"与"损坏证据",而分辨要求逐个读并验证清单,
    // 单次清点的总工作量仍由这个上限守住。超过它说明根已经大到一次诚实清点读不完,判 Invalid
    // 而不是截断出一份看似完整的清单。所查主体自己的份数上限在验证之后按作用域内份数单独判定。
    QStringList backupIds;
    if (!scanRootShape(m_domain, m_rootPath, &backupIds, m_domain.maxBackups * 4, &error)) {
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
        const QString manifestPath = QDir(directoryPath).filePath(m_domain.manifestName);
        QByteArray manifestBytes;
        if (!readBoundedFile(m_domain, 
                manifestPath, m_domain.maxManifestBytes, &manifestBytes, &error)) {
            result.state = stateForIssue(m_domain, error);
            result.issue = error;
            return result;
        }

        QJsonParseError parseError;
        const QJsonDocument current = QJsonDocument::fromJson(
            manifestBytes, &parseError);
        const bool currentV2 = parseError.error == QJsonParseError::NoError
            && current.isObject()
            && current.object().value(QStringLiteral("format")).toString()
                == m_domain.manifestFormat;
        if (!currentV2) {
            // 没有 v1 历史的域看到一份非 v2 清单,只能判定为无效证据,而不是去搬运它:
            // 那份清单不是本域写的,而把它迁移过来等于接受一份来源不明的载荷。
            if (!m_domain.legacyV1MigrationEnabled) {
                result.state = ConfigurationBackupInventoryState::Invalid;
                result.issue = code(m_domain, "manifest-invalid");
                return result;
            }
            if (!migrateLegacyLocked(
                    tool, legacyToolValue, backupId, managedPaths, &error)) {
                result.state = stateForIssue(m_domain, error);
                result.issue = error;
                return result;
            }
            manifestBytes.clear();
            if (!readBoundedFile(m_domain, 
                    manifestPath, m_domain.maxManifestBytes, &manifestBytes, &error)) {
                result.state = stateForIssue(m_domain, error);
                result.issue = error;
                return result;
            }
        }

        if (!exactDirectoryInventory(m_domain, directoryPath, { m_domain.manifestName }, &error)) {
            result.issue = error;
            return result;
        }
        // 主体作用域:清单声称的主体语法合法且与所查主体不同,这份备份才可能只是"别人主体
        // 的"。它必须先通过与作用域内条目完全相同的完整验证——目录形状已在上面查过,这里以它
        // 自己的主体做完整清单解析(含用它自己的密钥做 GCM 认证,密钥不可得则无法判定)——通过
        // 者才是 foreign-intact:越出本次查询的作用域,跳过,绝不退化结果;任何一步失败都说明
        // 它不是一份可辨认的别人备份,按原诊断如实退化。损坏条目绝不因为"可能属于别人"而被静默
        // 跳过:foreign 与 corrupt 的分界线就是这次完整验证,诚实优先于速度。声称主体取不出来
        // (字段缺失、非字符串或语法非法)的清单没有资格被当成别人的:落回下面的作用域内路径按
        // 所查主体判定,与既有行为逐字一致。
        QString claimedSubject;
        {
            QJsonParseError claimedParse;
            const QJsonDocument claimedDocument = QJsonDocument::fromJson(
                manifestBytes, &claimedParse);
            if (claimedParse.error == QJsonParseError::NoError
                    && claimedDocument.isObject()) {
                const QJsonValue subjectValue =
                    claimedDocument.object().value(m_domain.subjectJsonKey);
                if (subjectValue.isString()
                        && validSubject(m_domain, subjectValue.toString())) {
                    claimedSubject = subjectValue.toString();
                }
            }
        }
        if (!claimedSubject.isEmpty() && claimedSubject != tool) {
            ConfigurationBackupSnapshot foreign;
            if (parseManifest(m_domain, manifestBytes, claimedSubject, backupId,
                              m_keyProvider, &foreign, &error)) {
                cleanseFiles(&foreign.files);
                continue;
            }
            cleanseFiles(&foreign.files);
            result.state = stateForIssue(m_domain, error);
            result.issue = error;
            return result;
        }
        ConfigurationBackupSnapshot snapshot;
        if (!parseManifest(m_domain, 
                manifestBytes, tool, backupId, m_keyProvider, &snapshot, &error)) {
            result.state = stateForIssue(m_domain, error);
            result.issue = error;
            return result;
        }
        ConfigurationBackupInventoryEntry entry;
        entry.backupId = backupId;
        entry.tool = tool;
        entry.createdAt = snapshot.createdAt;
        entry.fileCount = snapshot.files.size();
        entry.identity = manifestIdentity(m_domain, manifestBytes);
        cleanseFiles(&snapshot.files);
        entries.append(entry);
    }

    // 作用域内的超限判定:所查主体自己的完整备份超过 maxBackups 时,这个主体的清点判
    // Invalid——与既有"超限根即 Invalid"的语义一致,只是分母换成作用域内的份数;别人主体的
    // 完整备份不占这个额度。代号与扫描上限路径同为 inventory-invalid:两种情形对调用方都是
    // 同一个真相——这个主体此刻没有一份可信的界内清单。
    if (entries.size() > m_domain.maxBackups) {
        result.state = ConfigurationBackupInventoryState::Invalid;
        result.issue = code(m_domain, "inventory-invalid");
        return result;
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
    if (!validSubject(m_domain, tool) || !validBackupId(m_domain, backupId)
            || !validManifestIdentity(m_domain, expectedIdentity)) {
        setError(error, code(m_domain, "remove-invalid"));
        return false;
    }
    const QFileInfo rootInfo(m_rootPath);
    if (!rootInfo.exists() || rootInfo.isSymLink() || !rootInfo.isDir()) {
        setError(error, code(m_domain, "remove-invalid"));
        return false;
    }

    QLockFile lock(QDir(m_rootPath).filePath(m_domain.lockFileName));
    if (!lockRoot(m_domain, m_rootPath, &lock, error)) return false;
    // 删除的扫描上限是 maxBackups 的 4 倍,而不是 maxBackups 本身:根一旦越过保留上限,
    // 清点判定 Invalid,若删除也以同一上限拒绝,证据就永远无法经验证路径裁回界内——删除
    // 恰恰是必须在超限根上工作的操作。上限仍保持有界,且与暂存清点层的扫描上限同宽。
    QStringList backupIds;
    if (!scanRootShape(m_domain, m_rootPath, &backupIds, m_domain.maxBackups * 4, error)
            || !backupIds.contains(backupId)) {
        if (error && error->isEmpty()) {
            *error = code(m_domain, "remove-invalid");
        }
        return false;
    }

    const QString directoryPath = QDir(m_rootPath).filePath(backupId);
    const QString manifestPath = QDir(directoryPath).filePath(m_domain.manifestName);
    if (!exactDirectoryInventory(m_domain, directoryPath, { m_domain.manifestName }, error)) return false;
    QByteArray manifestBytes;
    ConfigurationBackupSnapshot snapshot;
    if (!readBoundedFile(m_domain, manifestPath, m_domain.maxManifestBytes, &manifestBytes, error)
            || manifestIdentity(m_domain, manifestBytes) != expectedIdentity
            || !parseManifest(m_domain, 
                manifestBytes, tool, backupId, m_keyProvider, &snapshot, error)) {
        cleanseFiles(&snapshot.files);
        if (error && error->isEmpty()) {
            *error = code(m_domain, "remove-identity-drift");
        }
        return false;
    }
    cleanseFiles(&snapshot.files);

    QByteArray recheckedBytes;
    if (!readBoundedFile(m_domain, manifestPath, m_domain.maxManifestBytes, &recheckedBytes, error)
            || recheckedBytes != manifestBytes
            || manifestIdentity(m_domain, recheckedBytes) != expectedIdentity) {
        if (error && error->isEmpty()) {
            *error = code(m_domain, "remove-identity-drift");
        }
        return false;
    }

    const QString quarantineName = QStringLiteral(".removing-") + backupId
        + QLatin1Char('-') + expectedIdentity.right(16);
    const QString quarantinePath = QDir(m_rootPath).filePath(quarantineName);
    QDir root(m_rootPath);
    if (QFileInfo::exists(quarantinePath)
            || !root.rename(backupId, quarantineName)) {
        setError(error, code(m_domain, "remove-failed"));
        return false;
    }

    const auto restoreDirectoryName = [&]() {
        return root.rename(quarantineName, backupId);
    };

    QByteArray quarantinedBytes;
    ConfigurationBackupSnapshot quarantinedSnapshot;
    const QString quarantinedManifest = QDir(quarantinePath).filePath(m_domain.manifestName);
    const bool quarantineVerified = exactDirectoryInventory(m_domain, 
            quarantinePath, { m_domain.manifestName }, error)
        && readBoundedFile(m_domain, 
            quarantinedManifest, m_domain.maxManifestBytes, &quarantinedBytes, error)
        && quarantinedBytes == manifestBytes
        && manifestIdentity(m_domain, quarantinedBytes) == expectedIdentity
        && parseManifest(m_domain, quarantinedBytes, tool, backupId, m_keyProvider,
                         &quarantinedSnapshot, error);
    cleanseFiles(&quarantinedSnapshot.files);
    if (!quarantineVerified) {
        const bool restored = restoreDirectoryName();
        if (!restored) {
            setError(error, code(m_domain, "remove-recovery-failed"));
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
            ? code(m_domain, "remove-failed")
            : code(m_domain, "remove-recovery-failed"));
        return false;
    }
    if (!root.rmdir(quarantineName)) {
        const bool restoredManifest = QFile::rename(preservedPath, quarantinedManifest);
        const bool restoredDirectory = restoredManifest && restoreDirectoryName();
        setError(error, restoredDirectory
            ? code(m_domain, "remove-failed")
            : code(m_domain, "remove-recovery-failed"));
        return false;
    }
    if (!QFile::remove(preservedPath)) {
        setError(error, code(m_domain, "remove-finalize-failed"));
        return false;
    }
    return true;
}
