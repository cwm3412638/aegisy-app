#include "extension_staging_backup_key_provider.h"

#include "secure_storage.h"

#include <QDir>
#include <QRegularExpression>
#include <QStandardPaths>

#include <openssl/rand.h>

bool SecureStorageExtensionStagingBackupKeyProvider::keyForScope(
        const QString &scope, bool allowCreate, QByteArray *key, QString *error)
{
    // 只接受暂存域自己的密钥作用域：前缀加合法主体形状，别的字符串一律拒绝——
    // 密钥存取不会成为跨域取钥的旁路。
    static const QRegularExpression scopePattern(QStringLiteral(
        "^aegisy/extension-staging-backup-master/v1/"
        "(codex-plugin|skill|mcp):[a-z0-9][a-z0-9._-]{0,127}$"));
    if (key) key->clear();
    if (!key || !scopePattern.match(scope).hasMatch()
            || !SecureStorage::isAvailable()) {
        if (error) *error = QStringLiteral("extension-staging-backup-key-unavailable");
        return false;
    }

    const auto decode = [](const QString &encoded, QByteArray *decoded) {
        const QByteArray latin = encoded.toLatin1();
        if (QString::fromLatin1(latin) != encoded) return false;
        const QByteArray value = QByteArray::fromBase64(latin);
        if (value.size() != 32 || value.toBase64() != latin) return false;
        *decoded = value;
        return true;
    };

    const QString stored = SecureStorage::loadEncrypted(scope);
    if (!stored.isEmpty()) {
        if (!decode(stored, key)) {
            if (error) *error = QStringLiteral("extension-staging-backup-key-invalid");
            return false;
        }
        return true;
    }
    if (!allowCreate) {
        if (error) *error = QStringLiteral("extension-staging-backup-key-unavailable");
        return false;
    }

    QByteArray generated(32, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(generated.data()),
                   generated.size()) != 1) {
        OPENSSL_cleanse(generated.data(), static_cast<size_t>(generated.size()));
        if (error) *error = QStringLiteral("extension-staging-backup-random-failed");
        return false;
    }
    const QString encoded = QString::fromLatin1(generated.toBase64());
    if (!SecureStorage::saveEncrypted(scope, encoded)
            || SecureStorage::loadEncrypted(scope) != encoded
            || !decode(encoded, key)) {
        OPENSSL_cleanse(generated.data(), static_cast<size_t>(generated.size()));
        if (error) *error = QStringLiteral("extension-staging-backup-key-write-failed");
        return false;
    }
    OPENSSL_cleanse(generated.data(), static_cast<size_t>(generated.size()));
    return true;
}

QString extensionStagingBackupRootPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                + QStringLiteral("/backups"))
        .filePath(QStringLiteral("extensions-staging"));
}
