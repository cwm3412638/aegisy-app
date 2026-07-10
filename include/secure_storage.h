#ifndef SECURE_STORAGE_H
#define SECURE_STORAGE_H

#include <QString>
#include <QByteArray>

class SecureStorage
{
public:
    // 当前平台是否具备可用的系统凭据存储。
    static bool isAvailable();

    // 保存加密数据
    static bool saveEncrypted(const QString &key, const QString &data);

    // 读取加密数据
    static QString loadEncrypted(const QString &key);

    // 仅检查凭据是否存在，不读取明文。macOS 上不会触发解密授权弹窗。
    static bool contains(const QString &key);

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

    // Linux Secret Service（通过 libsecret 提供的 secret-tool）。
    static bool saveToSecretService(const QString &service, const QString &account,
                                    const QString &data);
    static QString loadFromSecretService(const QString &service, const QString &account);
    static bool deleteFromSecretService(const QString &service, const QString &account);
};

#endif // SECURE_STORAGE_H
