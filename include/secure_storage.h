#ifndef SECURE_STORAGE_H
#define SECURE_STORAGE_H

#include <QString>
#include <QByteArray>

class SecureStorage
{
public:
    // 保存加密数据
    static bool saveEncrypted(const QString &key, const QString &data);

    // 读取加密数据
    static QString loadEncrypted(const QString &key);

    // 删除数据
    static bool remove(const QString &key);

    // 保存 Token
    static bool saveToken(const QString &token);

    // 读取 Token
    static QString loadToken();

    // 清除 Token
    static bool clearToken();

private:
    // Windows DPAPI 加密/解密
    static QByteArray encryptWindows(const QByteArray &data);
    static QByteArray decryptWindows(const QByteArray &data);

    // macOS Keychain 加密/解密
    static bool saveToKeychain(const QString &service, const QString &account, const QString &data);
    static QString loadFromKeychain(const QString &service, const QString &account);
    static bool deleteFromKeychain(const QString &service, const QString &account);

    // 简单的 XOR 加密（回退方案）
    static QByteArray xorEncrypt(const QByteArray &data, const QByteArray &key);
};

#endif // SECURE_STORAGE_H
