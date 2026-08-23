#include "secure_storage.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>

#include <initializer_list>

namespace {

QString readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.readAll());
}

bool require(bool condition, const char *message)
{
    if (condition) return true;
    QTextStream(stderr) << message << Qt::endl;
    return false;
}

bool requireContains(const QString &text, const QString &value, const char *message)
{
    return require(text.contains(value), message);
}

bool requireAbsent(const QString &text, const QString &value, const char *message)
{
    return require(!text.contains(value), message);
}

QString sourceRange(const QString &text, const QString &start, const QString &end)
{
    const qsizetype startIndex = text.indexOf(start);
    if (startIndex < 0) return {};
    const qsizetype endIndex = text.indexOf(end, startIndex + start.size());
    if (endIndex < 0) return {};
    return text.mid(startIndex, endIndex - startIndex);
}

bool requireOrdered(const QString &text, std::initializer_list<QString> values,
                    const char *message)
{
    qsizetype offset = 0;
    for (const QString &value : values) {
        const qsizetype index = text.indexOf(value, offset);
        if (index < 0) return require(false, message);
        offset = index + value.size();
    }
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() != 2) return 1;
    const QDir root(application.arguments().at(1));
    const QString header = readFile(root.filePath(QStringLiteral("include/secure_storage.h")));
    const QString source = readFile(root.filePath(QStringLiteral("src/secure_storage.cpp")));
    const QString cmake = readFile(root.filePath(QStringLiteral("CMakeLists.txt")));
    if (header.isEmpty() || source.isEmpty() || cmake.isEmpty()) return 1;

    bool valid = true;
    const SecureStorageReadResult defaultResult;
    valid &= require(defaultResult.state == SecureStorageReadState::Unavailable
                         && defaultResult.value.isEmpty()
                         && defaultResult.errorCode.isEmpty(),
                     "typed secure-storage read result has unsafe defaults");
    valid &= requireContains(header, QStringLiteral("enum class SecureStorageReadState"),
                             "typed secure-storage read state is missing");
    for (const QString &state : {QStringLiteral("Found"), QStringLiteral("Missing"),
                                 QStringLiteral("Unavailable"), QStringLiteral("Invalid")}) {
        valid &= requireContains(header, state, "secure-storage read state is incomplete");
    }
    valid &= requireContains(header, QStringLiteral("loadEncryptedFresh(const QString &key)"),
                             "fresh secure-storage read API is missing");

    const QString fresh = sourceRange(
        source, QStringLiteral("SecureStorageReadResult SecureStorage::loadEncryptedFresh("),
        QStringLiteral("QString SecureStorage::loadEncrypted("));
    const QString cached = sourceRange(
        source, QStringLiteral("QString SecureStorage::loadEncrypted("),
        QStringLiteral("bool SecureStorage::contains("));
    valid &= require(!fresh.isEmpty() && !cached.isEmpty(),
                     "secure-storage read implementations could not be isolated");
    valid &= requireAbsent(fresh, QStringLiteral("cachedCredential("),
                           "fresh secure-storage read consults the memory cache");
    valid &= requireAbsent(fresh, QStringLiteral("cacheCredential("),
                           "fresh secure-storage read mutates the memory cache");
    valid &= requireOrdered(
        cached,
        {QStringLiteral("cachedCredential(key, &foundInCache)"),
         QStringLiteral("loadEncryptedFresh(key)"),
         QStringLiteral("result.state != SecureStorageReadState::Found"),
         QStringLiteral("cacheCredential(key, result.value)")},
        "legacy secure-storage read no longer caches only successful fresh reads");

    valid &= requireOrdered(
        fresh,
        {QStringLiteral("settings.sync();"),
         QStringLiteral("settings.status() != QSettings::NoError"),
         QStringLiteral("storedKeyExists = settings.contains(key)"),
         QStringLiteral("settings.status() != QSettings::NoError"),
         QStringLiteral("!storedKeyExists"),
         QStringLiteral("const QVariant stored = settings.value(key)"),
         QStringLiteral("settings.status() != QSettings::NoError"),
         QStringLiteral("!stored.isValid() || stored.isNull()"),
         QStringLiteral("SecureStorageReadState::Invalid"),
         QStringLiteral("QByteArray::AbortOnBase64DecodingErrors"),
         QStringLiteral("encrypted.toBase64() != encoded"),
         QStringLiteral("decryptWindows(encrypted, &decrypted)"),
         QStringLiteral("decodeUtf8Strict(decrypted, &value)")},
        "Windows fresh read does not fail closed through persistence and decoding");
    valid &= requireOrdered(
        fresh,
        {QStringLiteral("SecItemCopyMatching(query, &rawResult)"),
         QStringLiteral("status == errSecItemNotFound"),
         QStringLiteral("status != errSecSuccess"),
         QStringLiteral("CFGetTypeID(rawResult) != CFDataGetTypeID()"),
         QStringLiteral("decodeUtf8Strict(bytes, &value)")},
        "macOS fresh read does not distinguish Keychain result classes");
    valid &= requireOrdered(
        fresh,
        {QStringLiteral("QStandardPaths::findExecutable(QStringLiteral(\"secret-tool\"))"),
         QStringLiteral("process.waitForStarted(3000)"),
         QStringLiteral("process.waitForFinished(5000)"),
         QStringLiteral("process.exitStatus() != QProcess::NormalExit"),
         QStringLiteral("process.exitCode() == 1 && standardOutput.isEmpty()"),
         QStringLiteral("standardError.trimmed().isEmpty()"),
         QStringLiteral("decodeUtf8Strict(bytes, &value)")},
        "Linux fresh read can confuse backend failure with a missing secret");
    valid &= requireContains(source, QStringLiteral("decoded.toUtf8() != bytes"),
                             "secure-storage fresh reads lack strict UTF-8 round-trip validation");
    valid &= requireAbsent(header + source + cmake,
                           QStringLiteral("AEGISY_SECURE_STORAGE_FRESH_READ_TESTING"),
                           "fresh read introduced a production test hook");
    return valid ? 0 : 1;
}
