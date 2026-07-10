#include "profile_archive.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace {

constexpr int kArchiveVersion = 1;
constexpr int kIterations = 200000;
constexpr int kSaltSize = 16;
constexpr int kIvSize = 12;
constexpr int kTagSize = 16;
constexpr int kKeySize = 32;
constexpr qint64 kMaxArchiveSize = 16 * 1024 * 1024;

bool deriveKey(const QString &password, const QByteArray &salt,
               int iterations, QByteArray *key)
{
    const QByteArray passwordBytes = password.toUtf8();
    key->resize(kKeySize);
    return PKCS5_PBKDF2_HMAC(
        passwordBytes.constData(), passwordBytes.size(),
        reinterpret_cast<const unsigned char *>(salt.constData()), salt.size(),
        iterations, EVP_sha256(), key->size(),
        reinterpret_cast<unsigned char *>(key->data())) == 1;
}

bool encrypt(const QByteArray &plainText, const QByteArray &key,
             const QByteArray &iv, QByteArray *cipherText, QByteArray *tag)
{
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    if (!context) {
        return false;
    }

    cipherText->resize(plainText.size() + EVP_MAX_BLOCK_LENGTH);
    int written = 0;
    int finalWritten = 0;
    const bool ok = EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr) == 1
        && EVP_EncryptInit_ex(
            context, nullptr, nullptr,
            reinterpret_cast<const unsigned char *>(key.constData()),
            reinterpret_cast<const unsigned char *>(iv.constData())) == 1
        && EVP_EncryptUpdate(
            context,
            reinterpret_cast<unsigned char *>(cipherText->data()), &written,
            reinterpret_cast<const unsigned char *>(plainText.constData()),
            plainText.size()) == 1
        && EVP_EncryptFinal_ex(
            context,
            reinterpret_cast<unsigned char *>(cipherText->data()) + written,
            &finalWritten) == 1;

    tag->resize(kTagSize);
    const bool tagOk = ok
        && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, tag->size(), tag->data()) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!tagOk) {
        cipherText->clear();
        tag->clear();
        return false;
    }
    cipherText->resize(written + finalWritten);
    return true;
}

bool decrypt(const QByteArray &cipherText, const QByteArray &key,
             const QByteArray &iv, const QByteArray &tag, QByteArray *plainText)
{
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    if (!context) {
        return false;
    }

    plainText->resize(cipherText.size() + EVP_MAX_BLOCK_LENGTH);
    int written = 0;
    int finalWritten = 0;
    const bool initialized = EVP_DecryptInit_ex(
            context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr) == 1
        && EVP_DecryptInit_ex(
            context, nullptr, nullptr,
            reinterpret_cast<const unsigned char *>(key.constData()),
            reinterpret_cast<const unsigned char *>(iv.constData())) == 1
        && EVP_DecryptUpdate(
            context,
            reinterpret_cast<unsigned char *>(plainText->data()), &written,
            reinterpret_cast<const unsigned char *>(cipherText.constData()),
            cipherText.size()) == 1
        && EVP_CIPHER_CTX_ctrl(
            context, EVP_CTRL_GCM_SET_TAG, tag.size(),
            const_cast<char *>(tag.constData())) == 1;
    const bool authenticated = initialized
        && EVP_DecryptFinal_ex(
            context,
            reinterpret_cast<unsigned char *>(plainText->data()) + written,
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

void setError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
}

} // namespace

bool ProfileArchive::writeEncrypted(const QString &filePath,
                                    const QList<ArchiveProfile> &profiles,
                                    const QString &password,
                                    QString *error)
{
    if (password.size() < 8) {
        setError(error, QStringLiteral("导出密码至少需要 8 个字符。"));
        return false;
    }

    QJsonArray items;
    for (const ArchiveProfile &profile : profiles) {
        QJsonObject item;
        item.insert(QStringLiteral("name"), profile.name);
        item.insert(QStringLiteral("type"), profile.type);
        item.insert(QStringLiteral("key"), profile.key);
        item.insert(QStringLiteral("model"), profile.model);
        items.append(item);
    }
    QJsonObject payloadObject;
    payloadObject.insert(QStringLiteral("profiles"), items);
    QByteArray plainText = QJsonDocument(payloadObject).toJson(QJsonDocument::Compact);

    QByteArray salt(kSaltSize, '\0');
    QByteArray iv(kIvSize, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(salt.data()), salt.size()) != 1
            || RAND_bytes(reinterpret_cast<unsigned char *>(iv.data()), iv.size()) != 1) {
        plainText.fill('\0');
        setError(error, QStringLiteral("无法生成安全随机数。"));
        return false;
    }

    QByteArray key;
    if (!deriveKey(password, salt, kIterations, &key)) {
        plainText.fill('\0');
        setError(error, QStringLiteral("无法派生导出加密密钥。"));
        return false;
    }

    QByteArray cipherText;
    QByteArray tag;
    const bool encrypted = encrypt(plainText, key, iv, &cipherText, &tag);
    OPENSSL_cleanse(key.data(), key.size());
    plainText.fill('\0');
    if (!encrypted) {
        setError(error, QStringLiteral("档案加密失败。"));
        return false;
    }

    QJsonObject archive;
    archive.insert(QStringLiteral("format"), QStringLiteral("aegisy-profile-archive"));
    archive.insert(QStringLiteral("version"), kArchiveVersion);
    archive.insert(QStringLiteral("cipher"), QStringLiteral("AES-256-GCM"));
    archive.insert(QStringLiteral("kdf"), QStringLiteral("PBKDF2-HMAC-SHA256"));
    archive.insert(QStringLiteral("iterations"), kIterations);
    archive.insert(QStringLiteral("salt"), QString::fromLatin1(salt.toBase64()));
    archive.insert(QStringLiteral("iv"), QString::fromLatin1(iv.toBase64()));
    archive.insert(QStringLiteral("tag"), QString::fromLatin1(tag.toBase64()));
    archive.insert(QStringLiteral("ciphertext"), QString::fromLatin1(cipherText.toBase64()));

    const QByteArray data = QJsonDocument(archive).toJson(QJsonDocument::Indented);
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)
            || file.write(data) != data.size()
            || !file.commit()) {
        setError(error, QStringLiteral("无法写入导出文件：%1").arg(filePath));
        return false;
    }
    QFile::setPermissions(filePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool ProfileArchive::readEncrypted(const QString &filePath,
                                   const QString &password,
                                   QList<ArchiveProfile> *profiles,
                                   QString *error)
{
    if (!profiles) {
        setError(error, QStringLiteral("导入目标无效。"));
        return false;
    }
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > kMaxArchiveSize) {
        setError(error, QStringLiteral("无法读取导入文件或文件过大。"));
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject archive = document.object();
    if (parseError.error != QJsonParseError::NoError
            || archive.value(QStringLiteral("format")).toString()
                != QStringLiteral("aegisy-profile-archive")
            || archive.value(QStringLiteral("version")).toInt() != kArchiveVersion) {
        setError(error, QStringLiteral("这不是受支持的 Aegisy 档案导出文件。"));
        return false;
    }

    const int iterations = archive.value(QStringLiteral("iterations")).toInt();
    const QByteArray salt = QByteArray::fromBase64(
        archive.value(QStringLiteral("salt")).toString().toLatin1());
    const QByteArray iv = QByteArray::fromBase64(
        archive.value(QStringLiteral("iv")).toString().toLatin1());
    const QByteArray tag = QByteArray::fromBase64(
        archive.value(QStringLiteral("tag")).toString().toLatin1());
    const QByteArray cipherText = QByteArray::fromBase64(
        archive.value(QStringLiteral("ciphertext")).toString().toLatin1());
    if (iterations < 100000 || iterations > 1000000
            || salt.size() != kSaltSize || iv.size() != kIvSize
            || tag.size() != kTagSize || cipherText.isEmpty()) {
        setError(error, QStringLiteral("导出文件的加密参数无效。"));
        return false;
    }

    QByteArray key;
    if (!deriveKey(password, salt, iterations, &key)) {
        setError(error, QStringLiteral("无法派生导入解密密钥。"));
        return false;
    }
    QByteArray plainText;
    const bool decrypted = decrypt(cipherText, key, iv, tag, &plainText);
    OPENSSL_cleanse(key.data(), key.size());
    if (!decrypted) {
        setError(error, QStringLiteral("密码错误或导出文件已损坏。"));
        return false;
    }

    const QJsonDocument payload = QJsonDocument::fromJson(plainText, &parseError);
    plainText.fill('\0');
    if (parseError.error != QJsonParseError::NoError || !payload.isObject()) {
        setError(error, QStringLiteral("解密后的档案数据无效。"));
        return false;
    }

    const QJsonArray items = payload.object().value(QStringLiteral("profiles")).toArray();
    if (items.size() > 500) {
        setError(error, QStringLiteral("导入文件包含过多档案。"));
        return false;
    }

    QList<ArchiveProfile> parsed;
    parsed.reserve(items.size());
    for (const QJsonValue &value : items) {
        if (!value.isObject()) {
            setError(error, QStringLiteral("导入文件包含无效档案。"));
            return false;
        }
        const QJsonObject item = value.toObject();
        ArchiveProfile profile;
        profile.name = item.value(QStringLiteral("name")).toString().trimmed();
        profile.type = item.value(QStringLiteral("type")).toInt();
        profile.key = item.value(QStringLiteral("key")).toString();
        profile.model = item.value(QStringLiteral("model")).toString().trimmed();
        if (profile.name.isEmpty() || profile.name.size() > 128
                || profile.key.size() > 8192 || profile.model.size() > 256) {
            setError(error, QStringLiteral("导入文件中的档案字段无效。"));
            return false;
        }
        parsed.append(profile);
    }
    *profiles = parsed;
    return true;
}
