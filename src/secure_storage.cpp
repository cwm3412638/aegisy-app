#include "secure_storage.h"
#include <QSettings>
#include <QStandardPaths>
#include <QDebug>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#endif

#ifdef Q_OS_MAC
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

// 常量定义
static const QString SERVICE_NAME = "AegisyClient";
static const QString TOKEN_KEY = "auth_token";

bool SecureStorage::saveToken(const QString &token)
{
    return saveEncrypted(TOKEN_KEY, token);
}

QString SecureStorage::loadToken()
{
    return loadEncrypted(TOKEN_KEY);
}

bool SecureStorage::clearToken()
{
    return remove(TOKEN_KEY);
}

bool SecureStorage::saveEncrypted(const QString &key, const QString &data)
{
#ifdef Q_OS_WIN
    // Windows: 使用 DPAPI
    QByteArray encrypted = encryptWindows(data.toUtf8());
    if (encrypted.isEmpty()) {
        qWarning() << "DPAPI encryption failed";
        return false;
    }

    QSettings settings(QSettings::NativeFormat, QSettings::UserScope, "Aegisy", "AegisyClient");
    settings.setValue(key, encrypted.toBase64());
    return true;

#elif defined(Q_OS_MAC)
    // macOS: 使用 Keychain
    return saveToKeychain(SERVICE_NAME, key, data);

#else
    // Linux: 使用简单的 XOR 加密 + 文件存储（回退方案）
    QByteArray xorKey = "AegisySecretKey2024";  // 实际应该从机器特征生成
    QByteArray encrypted = xorEncrypt(data.toUtf8(), xorKey);

    QSettings settings(QSettings::NativeFormat, QSettings::UserScope, "Aegisy", "AegisyClient");
    settings.setValue(key, encrypted.toBase64());
    return true;
#endif
}

QString SecureStorage::loadEncrypted(const QString &key)
{
#ifdef Q_OS_WIN
    QSettings settings(QSettings::NativeFormat, QSettings::UserScope, "Aegisy", "AegisyClient");
    QString encryptedBase64 = settings.value(key).toString();
    if (encryptedBase64.isEmpty()) {
        return QString();
    }

    QByteArray encrypted = QByteArray::fromBase64(encryptedBase64.toUtf8());
    QByteArray decrypted = decryptWindows(encrypted);
    return QString::fromUtf8(decrypted);

#elif defined(Q_OS_MAC)
    return loadFromKeychain(SERVICE_NAME, key);

#else
    QSettings settings(QSettings::NativeFormat, QSettings::UserScope, "Aegisy", "AegisyClient");
    QString encryptedBase64 = settings.value(key).toString();
    if (encryptedBase64.isEmpty()) {
        return QString();
    }

    QByteArray xorKey = "AegisySecretKey2024";
    QByteArray encrypted = QByteArray::fromBase64(encryptedBase64.toUtf8());
    QByteArray decrypted = xorEncrypt(encrypted, xorKey);
    return QString::fromUtf8(decrypted);
#endif
}

bool SecureStorage::remove(const QString &key)
{
#ifdef Q_OS_MAC
    return deleteFromKeychain(SERVICE_NAME, key);
#else
    QSettings settings(QSettings::NativeFormat, QSettings::UserScope, "Aegisy", "AegisyClient");
    settings.remove(key);
    return true;
#endif
}

#ifdef Q_OS_WIN
QByteArray SecureStorage::encryptWindows(const QByteArray &data)
{
    DATA_BLOB inputBlob;
    DATA_BLOB outputBlob;

    inputBlob.pbData = (BYTE*)data.data();
    inputBlob.cbData = data.size();

    if (CryptProtectData(&inputBlob, L"AegisyClient", NULL, NULL, NULL, 0, &outputBlob)) {
        QByteArray result((char*)outputBlob.pbData, outputBlob.cbData);
        LocalFree(outputBlob.pbData);
        return result;
    }

    return QByteArray();
}

QByteArray SecureStorage::decryptWindows(const QByteArray &data)
{
    DATA_BLOB inputBlob;
    DATA_BLOB outputBlob;

    inputBlob.pbData = (BYTE*)data.data();
    inputBlob.cbData = data.size();

    if (CryptUnprotectData(&inputBlob, NULL, NULL, NULL, NULL, 0, &outputBlob)) {
        QByteArray result((char*)outputBlob.pbData, outputBlob.cbData);
        LocalFree(outputBlob.pbData);
        return result;
    }

    return QByteArray();
}
#endif

#ifdef Q_OS_MAC
bool SecureStorage::saveToKeychain(const QString &service, const QString &account, const QString &data)
{
    QByteArray serviceUtf8 = service.toUtf8();
    QByteArray accountUtf8 = account.toUtf8();
    QByteArray dataUtf8 = data.toUtf8();

    // 先尝试删除旧的
    deleteFromKeychain(service, account);

    OSStatus status = SecKeychainAddGenericPassword(
        NULL,
        serviceUtf8.length(), serviceUtf8.data(),
        accountUtf8.length(), accountUtf8.data(),
        dataUtf8.length(), dataUtf8.data(),
        NULL
    );

    return status == errSecSuccess;
}

QString SecureStorage::loadFromKeychain(const QString &service, const QString &account)
{
    QByteArray serviceUtf8 = service.toUtf8();
    QByteArray accountUtf8 = account.toUtf8();

    UInt32 passwordLength = 0;
    void *passwordData = NULL;

    OSStatus status = SecKeychainFindGenericPassword(
        NULL,
        serviceUtf8.length(), serviceUtf8.data(),
        accountUtf8.length(), accountUtf8.data(),
        &passwordLength, &passwordData,
        NULL
    );

    if (status == errSecSuccess) {
        QString result = QString::fromUtf8((char*)passwordData, passwordLength);
        SecKeychainItemFreeContent(NULL, passwordData);
        return result;
    }

    return QString();
}

bool SecureStorage::deleteFromKeychain(const QString &service, const QString &account)
{
    QByteArray serviceUtf8 = service.toUtf8();
    QByteArray accountUtf8 = account.toUtf8();

    SecKeychainItemRef itemRef = NULL;
    OSStatus status = SecKeychainFindGenericPassword(
        NULL,
        serviceUtf8.length(), serviceUtf8.data(),
        accountUtf8.length(), accountUtf8.data(),
        NULL, NULL,
        &itemRef
    );

    if (status == errSecSuccess && itemRef) {
        SecKeychainItemDelete(itemRef);
        CFRelease(itemRef);
        return true;
    }

    return false;
}
#endif

QByteArray SecureStorage::xorEncrypt(const QByteArray &data, const QByteArray &key)
{
    QByteArray result = data;
    for (int i = 0; i < result.size(); ++i) {
        result[i] = result[i] ^ key[i % key.size()];
    }
    return result;
}
