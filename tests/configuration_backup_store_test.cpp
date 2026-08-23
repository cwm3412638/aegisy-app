#include "configuration_backup_store.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <openssl/evp.h>

#include <iostream>

namespace {

const QString kId = QStringLiteral("20260823_120000_123_abcdef12");
const QString kId2 = QStringLiteral("20260823_120001_456_1234abcd");
const QString kId3 = QStringLiteral("20260823_120002_789_deadbeef");

class FakeKeyProvider final : public ConfigurationBackupKeyProvider
{
public:
    bool keyForScope(const QString &scope, bool allowCreate,
                     QByteArray *key, QString *error) override
    {
        if (fail) {
            if (error) *error = QStringLiteral("fake-key-unavailable");
            return false;
        }
        if (!keys.contains(scope)) {
            if (!allowCreate) {
                if (error) *error = QStringLiteral("fake-key-missing");
                return false;
            }
            keys.insert(scope, QByteArray(32, static_cast<char>(0x41 + keys.size())));
        }
        *key = keys.value(scope);
        return true;
    }

    QByteArray key(const QString &tool) const
    {
        return keys.value(QStringLiteral("tool-manager/config-backup-master/v1/") + tool);
    }

    QHash<QString, QByteArray> keys;
    bool fail = false;
};

bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray recursiveBytes(const QString &root)
{
    QByteArray result;
    const QDir directory(root);
    const QFileInfoList entries = directory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (entry.isDir()) result += recursiveBytes(entry.filePath());
        else result += readFile(entry.filePath());
    }
    return result;
}

ConfigurationBackupSnapshot snapshot(const QString &id)
{
    ConfigurationBackupSnapshot value;
    value.backupId = id;
    value.tool = QStringLiteral("codex");
    value.createdAt = QDateTime::fromString(
        QStringLiteral("2026-08-23T12:00:00.123Z"), Qt::ISODateWithMs);
    value.files = {
        { 0, true, QByteArray("OPENAI_API_KEY=sentinel-credential-value-000001") },
        { 1, false, QByteArray() },
    };
    return value;
}

void appendLengthFramed(QByteArray *output, const QByteArray &value)
{
    const quint64 size = qToBigEndian<quint64>(static_cast<quint64>(value.size()));
    output->append(reinterpret_cast<const char *>(&size), sizeof(size));
    output->append(value);
}

QByteArray aad(const QString &tool, const QString &id,
               const QString &createdAt, int fileCount)
{
    static constexpr char prefix[] = "aegisy-tool-config-backup-manifest/0.2\0";
    QByteArray output(prefix, sizeof(prefix) - 1);
    appendLengthFramed(&output, tool.toUtf8());
    appendLengthFramed(&output, id.toUtf8());
    appendLengthFramed(&output, createdAt.toUtf8());
    const quint64 count = qToBigEndian<quint64>(static_cast<quint64>(fileCount));
    output.append(reinterpret_cast<const char *>(&count), sizeof(count));
    return output;
}

bool decrypt(const QByteArray &cipherText, const QByteArray &associated,
             const QByteArray &key, const QByteArray &nonce,
             const QByteArray &tag, QByteArray *plainText)
{
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
            reinterpret_cast<const unsigned char *>(associated.constData()),
            associated.size()) == 1
        && EVP_DecryptUpdate(
            context, reinterpret_cast<unsigned char *>(plainText->data()), &written,
            reinterpret_cast<const unsigned char *>(cipherText.constData()),
            cipherText.size()) == 1
        && EVP_CIPHER_CTX_ctrl(
            context, EVP_CTRL_GCM_SET_TAG, tag.size(),
            const_cast<char *>(tag.constData())) == 1;
    const bool ok = initialized && EVP_DecryptFinal_ex(
        context, reinterpret_cast<unsigned char *>(plainText->data()) + written,
        &finalWritten) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!ok) return false;
    plainText->resize(written + finalWritten);
    return true;
}

bool encrypt(const QByteArray &plainText, const QByteArray &associated,
             const QByteArray &key, const QByteArray &nonce,
             QByteArray *cipherText, QByteArray *tag)
{
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
            reinterpret_cast<const unsigned char *>(associated.constData()),
            associated.size()) == 1
        && EVP_EncryptUpdate(
            context, reinterpret_cast<unsigned char *>(cipherText->data()), &written,
            reinterpret_cast<const unsigned char *>(plainText.constData()),
            plainText.size()) == 1
        && EVP_EncryptFinal_ex(
            context, reinterpret_cast<unsigned char *>(cipherText->data()) + written,
            &finalWritten) == 1;
    tag->resize(16);
    const bool tagOk = ok && EVP_CIPHER_CTX_ctrl(
        context, EVP_CTRL_GCM_GET_TAG, tag->size(), tag->data()) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!tagOk) return false;
    cipherText->resize(written + finalWritten);
    return true;
}

bool mutateAuthenticatedPayload(const QString &manifestPath,
                                const QByteArray &key,
                                const std::function<void(QJsonObject *)> &mutation)
{
    QJsonDocument manifestDocument = QJsonDocument::fromJson(readFile(manifestPath));
    QJsonObject manifest = manifestDocument.object();
    const QByteArray nonce = QByteArray::fromBase64(
        manifest.value(QStringLiteral("nonce")).toString().toLatin1());
    const QByteArray oldTag = QByteArray::fromBase64(
        manifest.value(QStringLiteral("tag")).toString().toLatin1());
    const QByteArray cipherText = QByteArray::fromBase64(
        manifest.value(QStringLiteral("ciphertext")).toString().toLatin1());
    const QByteArray associated = aad(
        manifest.value(QStringLiteral("tool")).toString(),
        manifest.value(QStringLiteral("backup_id")).toString(),
        manifest.value(QStringLiteral("created_at")).toString(),
        manifest.value(QStringLiteral("file_count")).toInt());
    QByteArray plainText;
    if (!decrypt(cipherText, associated, key, nonce, oldTag, &plainText)) return false;
    QJsonObject payload = QJsonDocument::fromJson(plainText).object();
    mutation(&payload);
    plainText = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    QByteArray changedCipherText;
    QByteArray changedTag;
    if (!encrypt(plainText, associated, key, nonce, &changedCipherText, &changedTag)) {
        return false;
    }
    manifest.insert(QStringLiteral("ciphertext"),
                    QString::fromLatin1(changedCipherText.toBase64()));
    manifest.insert(QStringLiteral("tag"), QString::fromLatin1(changedTag.toBase64()));
    return writeFile(manifestPath,
                     QJsonDocument(manifest).toJson(QJsonDocument::Compact));
}

bool writeLegacy(const QString &root, const QString &id,
                 const QStringList &paths, const QList<QByteArray> &contents,
                 bool unknownEntry = false)
{
    const QString directoryPath = QDir(root).filePath(id);
    if (!QDir().mkpath(directoryPath)) return false;
    QJsonArray files;
    for (int i = 0; i < paths.size(); ++i) {
        const bool existed = i < contents.size() && !contents.at(i).isNull();
        const QString payload = QStringLiteral("file_%1.bin").arg(i);
        QJsonObject entry;
        entry.insert(QStringLiteral("path"), paths.at(i));
        entry.insert(QStringLiteral("existed"), existed);
        entry.insert(QStringLiteral("payload"), payload);
        files.append(entry);
        if (existed && !writeFile(QDir(directoryPath).filePath(payload), contents.at(i))) {
            return false;
        }
    }
    QJsonObject manifest;
    manifest.insert(QStringLiteral("version"), 1);
    manifest.insert(QStringLiteral("tool"), 1);
    manifest.insert(QStringLiteral("created_at"),
                    QStringLiteral("2026-08-23T12:00:00.123Z"));
    manifest.insert(QStringLiteral("files"), files);
    if (!writeFile(QDir(directoryPath).filePath(QStringLiteral("manifest.json")),
                   QJsonDocument(manifest).toJson(QJsonDocument::Indented))) {
        return false;
    }
    return !unknownEntry
        || writeFile(QDir(directoryPath).filePath(QStringLiteral("unknown.bin")),
                     QByteArray("unknown"));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary;
    if (!require(temporary.isValid(), "temporary root creation failed")) return 1;

    FakeKeyProvider keys;
    const QString root = temporary.filePath(QStringLiteral("backups/codex"));
    ConfigurationBackupStore store(root, &keys);
    QString error;
    const ConfigurationBackupSnapshot source = snapshot(kId);
    if (!require(store.create(source, &error), "encrypted backup creation failed")) {
        std::cerr << error.toStdString() << '\n';
        return 1;
    }
    const QString manifestPath = QDir(root).filePath(kId + QStringLiteral("/manifest.json"));
    const QByteArray diskBytes = recursiveBytes(root);
    if (!require(!diskBytes.contains(source.files.at(0).content),
                 "credential plaintext entered backup storage")
            || !require(!diskBytes.contains(temporary.path().toUtf8()),
                        "absolute root entered backup storage")
            || !require(QDir(QFileInfo(manifestPath).dir()).entryList(
                    QDir::Files | QDir::NoDotAndDotDot) == QStringList{QStringLiteral("manifest.json")},
                        "v2 backup contained unexpected files")) {
        return 1;
    }

    ConfigurationBackupSnapshot restored;
    if (!require(store.read(QStringLiteral("codex"), kId, &restored, &error),
                 "encrypted backup read failed")
            || !require(restored.files.size() == 2
                        && restored.files.at(0).content == source.files.at(0).content
                        && !restored.files.at(1).existed,
                        "encrypted backup round trip changed payload")) {
        std::cerr << error.toStdString() << '\n';
        return 1;
    }

    ConfigurationBackupSnapshot rejected = restored;
    if (!require(!store.read(QStringLiteral("claude"), kId, &rejected, &error)
                    && rejected.files.isEmpty(),
                 "wrong tool produced output")
            || !require(!store.read(QStringLiteral("codex"), QStringLiteral("../escape"),
                                    &rejected, &error)
                    && rejected.files.isEmpty(),
                        "invalid backup ID produced output")) {
        return 1;
    }

    const QByteArray originalManifest = readFile(manifestPath);
    QJsonObject tamperedOuter = QJsonDocument::fromJson(originalManifest).object();
    tamperedOuter.insert(QStringLiteral("file_count"), 1);
    if (!require(writeFile(manifestPath,
                           QJsonDocument(tamperedOuter).toJson(QJsonDocument::Compact)),
                 "failed to write outer tamper")
            || !require(!store.read(QStringLiteral("codex"), kId, &rejected, &error)
                        && rejected.files.isEmpty(),
                        "AAD metadata tamper produced output")
            || !require(writeFile(manifestPath, originalManifest),
                        "failed to restore manifest")) {
        return 1;
    }

    if (!require(mutateAuthenticatedPayload(
                     manifestPath, keys.key(QStringLiteral("codex")),
                     [](QJsonObject *payload) {
                         QJsonArray files = payload->value(QStringLiteral("files")).toArray();
                         QJsonObject first = files.at(0).toObject();
                         first.insert(QStringLiteral("slot"), 1);
                         files.replace(0, first);
                         payload->insert(QStringLiteral("files"), files);
                     }),
                 "failed to create authenticated slot tamper")
            || !require(!store.read(QStringLiteral("codex"), kId, &rejected, &error)
                        && rejected.files.isEmpty(),
                        "authenticated slot tamper produced output")
            || !require(writeFile(manifestPath, originalManifest),
                        "failed to restore manifest after slot tamper")) {
        return 1;
    }

    if (!require(mutateAuthenticatedPayload(
                     manifestPath, keys.key(QStringLiteral("codex")),
                     [](QJsonObject *payload) {
                         QJsonArray files = payload->value(QStringLiteral("files")).toArray();
                         QJsonObject first = files.at(0).toObject();
                         first.insert(QStringLiteral("byte_count"), 1.5);
                         files.replace(0, first);
                         payload->insert(QStringLiteral("files"), files);
                     }),
                 "failed to create authenticated numeric tamper")
            || !require(!store.read(QStringLiteral("codex"), kId, &rejected, &error)
                        && rejected.files.isEmpty(),
                        "fractional payload count produced output")
            || !require(writeFile(manifestPath, originalManifest),
                        "failed to restore manifest after numeric tamper")) {
        return 1;
    }

    if (!require(mutateAuthenticatedPayload(
                     manifestPath, keys.key(QStringLiteral("codex")),
                     [](QJsonObject *payload) {
                         QJsonArray files = payload->value(QStringLiteral("files")).toArray();
                         QJsonObject first = files.at(0).toObject();
                         first.insert(QStringLiteral("sha256"), QString(64, QLatin1Char('0')));
                         files.replace(0, first);
                         payload->insert(QStringLiteral("files"), files);
                     }),
                 "failed to create authenticated hash tamper")
            || !require(!store.read(QStringLiteral("codex"), kId, &rejected, &error)
                        && rejected.files.isEmpty(),
                        "authenticated hash tamper produced output")
            || !require(writeFile(manifestPath, originalManifest),
                        "failed to restore manifest after hash tamper")) {
        return 1;
    }

    FakeKeyProvider wrongKeys;
    wrongKeys.keys.insert(
        QStringLiteral("tool-manager/config-backup-master/v1/codex"),
        QByteArray(32, 'Z'));
    ConfigurationBackupStore wrongKeyStore(root, &wrongKeys);
    if (!require(!wrongKeyStore.read(QStringLiteral("codex"), kId, &rejected, &error)
                    && rejected.files.isEmpty(),
                 "wrong key produced output")) {
        return 1;
    }

    ConfigurationBackupSnapshot oversized = snapshot(kId2);
    oversized.files[0].content = QByteArray(
        ConfigurationBackupStore::MaxFileBytes + 1, 'x');
    if (!require(!store.create(oversized, &error)
                    && !QFileInfo::exists(QDir(root).filePath(kId2)),
                 "oversized file created a backup")) {
        return 1;
    }

    const QStringList legacyPaths = {
        temporary.filePath(QStringLiteral("home/.codex/auth.json")),
        temporary.filePath(QStringLiteral("home/.codex/config.toml")),
    };
    const QByteArray legacyCredential("legacy-secret-sentinel-credential-000002");
    const QList<QByteArray> legacyContents = { legacyCredential, QByteArray() };
    if (!require(writeLegacy(root, kId2, legacyPaths, legacyContents),
                 "failed to create legacy fixture")
            || !require(store.migrateLegacy(
                            QStringLiteral("codex"), 1, kId2, legacyPaths, &error),
                        "legacy migration failed")) {
        std::cerr << error.toStdString() << '\n';
        return 1;
    }
    ConfigurationBackupSnapshot migrated;
    if (!require(store.read(QStringLiteral("codex"), kId2, &migrated, &error),
                 "migrated backup was unreadable")
            || !require(migrated.files.at(0).content == legacyCredential,
                        "legacy migration changed payload")
            || !require(!recursiveBytes(QDir(root).filePath(kId2)).contains(legacyCredential),
                        "legacy plaintext remained after migration")) {
        return 1;
    }

    // Build a valid pending manifest elsewhere, then simulate a crash after one
    // legacy payload was removed but before v2 publication.
    const QString stagingRoot = temporary.filePath(QStringLiteral("staging/codex"));
    ConfigurationBackupStore staging(stagingRoot, &keys);
    ConfigurationBackupSnapshot pendingSource = snapshot(kId3);
    pendingSource.files[0].content = QByteArray("pending-secret-sentinel-000003");
    pendingSource.files[1] = { 1, true, QByteArray("second-pending-file") };
    if (!require(staging.create(pendingSource, &error),
                 "failed to create pending authority")
            || !require(writeLegacy(
                            root, kId3, legacyPaths,
                            { pendingSource.files.at(0).content,
                              pendingSource.files.at(1).content }),
                        "failed to create interrupted legacy fixture")) {
        return 1;
    }
    const QString interruptedDir = QDir(root).filePath(kId3);
    if (!require(writeFile(
                     QDir(interruptedDir).filePath(QStringLiteral("manifest.v2.pending")),
                     readFile(QDir(stagingRoot).filePath(
                         kId3 + QStringLiteral("/manifest.json")))),
                 "failed to publish pending fixture")
            || !require(QFile::remove(
                            QDir(interruptedDir).filePath(QStringLiteral("file_0.bin"))),
                        "failed to simulate partial cleanup")
            || !require(store.migrateLegacy(
                            QStringLiteral("codex"), 1, kId3, legacyPaths, &error),
                        "pending migration did not resume")) {
        std::cerr << error.toStdString() << '\n';
        return 1;
    }
    if (!require(store.read(QStringLiteral("codex"), kId3, &migrated, &error)
                    && migrated.files.at(0).content == pendingSource.files.at(0).content
                    && migrated.files.at(1).content == pendingSource.files.at(1).content,
                 "resumed pending migration changed payload")) {
        return 1;
    }
    const QString finalManifest = QDir(interruptedDir).filePath(
        QStringLiteral("manifest.json"));
    const QString finalPending = QDir(interruptedDir).filePath(
        QStringLiteral("manifest.v2.pending"));
    if (!require(writeFile(finalPending, readFile(finalManifest)),
                 "failed to simulate post-publication pending state")
            || !require(store.migrateLegacy(
                            QStringLiteral("codex"), 1, kId3, legacyPaths, &error),
                        "post-publication pending state did not recover")
            || !require(!QFileInfo::exists(finalPending),
                        "post-publication pending file remained")) {
        return 1;
    }

    const QString badId = QStringLiteral("20260823_120003_000_badc0ffe");
    if (!require(writeLegacy(root, badId, legacyPaths, legacyContents, true),
                 "failed to create unknown-entry fixture")
            || !require(!store.migrateLegacy(
                            QStringLiteral("codex"), 1, badId, legacyPaths, &error),
                        "unknown legacy entry did not fail closed")
            || !require(!QFileInfo::exists(QDir(root).filePath(
                            badId + QStringLiteral("/manifest.v2.pending"))),
                        "unknown legacy entry was mutated before rejection")
            || !require(recursiveBytes(QDir(root).filePath(badId)).contains(
                            legacyCredential),
                        "failed migration deleted uncertain plaintext")) {
        return 1;
    }

    const ConfigurationBackupInventoryResult invalidInventory = store.inventory(
        QStringLiteral("codex"), 1, legacyPaths);
    if (!require(invalidInventory.state == ConfigurationBackupInventoryState::Invalid
                    && invalidInventory.entries.isEmpty(),
                 "unknown legacy evidence did not make inventory invalid")) {
        return 1;
    }

    const QString emptyRoot = temporary.filePath(QStringLiteral("inventory-empty"));
    ConfigurationBackupStore emptyStore(emptyRoot, &keys);
    const ConfigurationBackupInventoryResult emptyInventory = emptyStore.inventory(
        QStringLiteral("codex"), 1, legacyPaths);
    if (!require(emptyInventory.state == ConfigurationBackupInventoryState::Empty
                    && emptyInventory.entries.isEmpty()
                    && !QFileInfo::exists(emptyRoot),
                 "absent backup root was not a zero-write empty inventory")) {
        return 1;
    }

    const QString readyRoot = temporary.filePath(QStringLiteral("inventory-ready"));
    ConfigurationBackupStore readyStore(readyRoot, &keys);
    ConfigurationBackupSnapshot oldest = snapshot(
        QStringLiteral("20260823_130000_000_00000001"));
    oldest.createdAt = QDateTime::fromString(
        QStringLiteral("2026-08-23T13:00:00.000Z"), Qt::ISODateWithMs);
    ConfigurationBackupSnapshot tiedSecond = snapshot(
        QStringLiteral("20260823_130001_000_00000002"));
    tiedSecond.createdAt = QDateTime::fromString(
        QStringLiteral("2026-08-23T13:00:01.000Z"), Qt::ISODateWithMs);
    ConfigurationBackupSnapshot tiedFirst = snapshot(
        QStringLiteral("20260823_130001_000_00000001"));
    tiedFirst.createdAt = tiedSecond.createdAt;
    if (!require(readyStore.create(oldest, &error)
                    && readyStore.create(tiedSecond, &error)
                    && readyStore.create(tiedFirst, &error),
                 "failed to create ready inventory fixtures")) {
        return 1;
    }
    ConfigurationBackupInventoryResult readyInventory = readyStore.inventory(
        QStringLiteral("codex"), 1, legacyPaths);
    if (!require(readyInventory.state == ConfigurationBackupInventoryState::Ready
                    && readyInventory.entries.size() == 3,
                 "valid backups were not ready")
            || !require(readyInventory.entries.at(0).backupId == tiedFirst.backupId
                        && readyInventory.entries.at(1).backupId == tiedSecond.backupId
                        && readyInventory.entries.at(2).backupId == oldest.backupId,
                        "inventory ordering was not deterministic")
            || !require(!readyInventory.entries.at(0).identity.isEmpty()
                        && readyInventory.entries.at(0).fileCount == 2,
                        "ready inventory omitted verified metadata")) {
        return 1;
    }

    FakeKeyProvider missingKeys;
    ConfigurationBackupStore missingKeyStore(readyRoot, &missingKeys);
    const ConfigurationBackupInventoryResult missingKeyInventory =
        missingKeyStore.inventory(QStringLiteral("codex"), 1, legacyPaths);
    if (!require(missingKeyInventory.state
                        == ConfigurationBackupInventoryState::Unavailable
                    && missingKeyInventory.entries.isEmpty(),
                 "missing backup key was not unavailable")) {
        return 1;
    }

    QLockFile heldLock(QDir(readyRoot).filePath(QStringLiteral(".backup.lock")));
    heldLock.setStaleLockTime(30000);
    if (!require(heldLock.tryLock(), "failed to hold inventory lock fixture")) return 1;
    const ConfigurationBackupInventoryResult lockedInventory = readyStore.inventory(
        QStringLiteral("codex"), 1, legacyPaths);
    heldLock.unlock();
    if (!require(lockedInventory.state == ConfigurationBackupInventoryState::Unavailable
                    && lockedInventory.entries.isEmpty(),
                 "lock contention was not unavailable")) {
        return 1;
    }

    const QString corruptRoot = temporary.filePath(QStringLiteral("inventory-corrupt"));
    ConfigurationBackupStore corruptStore(corruptRoot, &keys);
    ConfigurationBackupSnapshot corruptSource = snapshot(
        QStringLiteral("20260823_140000_000_c0ffee00"));
    if (!require(corruptStore.create(corruptSource, &error),
                 "failed to create corrupt inventory fixture")) {
        return 1;
    }
    const QString corruptManifest = QDir(corruptRoot).filePath(
        corruptSource.backupId + QStringLiteral("/manifest.json"));
    QByteArray corruptBytes = readFile(corruptManifest);
    corruptBytes[corruptBytes.size() / 2] ^= 1;
    if (!require(writeFile(corruptManifest, corruptBytes),
                 "failed to corrupt inventory fixture")) {
        return 1;
    }
    const ConfigurationBackupInventoryResult corruptInventory = corruptStore.inventory(
        QStringLiteral("codex"), 1, legacyPaths);
    if (!require(corruptInventory.state == ConfigurationBackupInventoryState::Invalid
                    && corruptInventory.entries.isEmpty()
                    && readFile(corruptManifest) == corruptBytes,
                 "corrupt inventory was accepted or modified")) {
        return 1;
    }

    const QString unknownRoot = temporary.filePath(QStringLiteral("inventory-unknown"));
    if (!require(QDir().mkpath(unknownRoot)
                    && writeFile(QDir(unknownRoot).filePath(QStringLiteral("unknown.bin")),
                                 QByteArray("preserve-unknown-evidence")),
                 "failed to create root unknown-entry fixture")) {
        return 1;
    }
    ConfigurationBackupStore unknownStore(unknownRoot, &keys);
    const ConfigurationBackupInventoryResult unknownInventory = unknownStore.inventory(
        QStringLiteral("codex"), 1, legacyPaths);
    if (!require(unknownInventory.state == ConfigurationBackupInventoryState::Invalid
                    && readFile(QDir(unknownRoot).filePath(QStringLiteral("unknown.bin")))
                        == QByteArray("preserve-unknown-evidence"),
                 "root unknown entry was accepted or modified")) {
        return 1;
    }

#ifndef Q_OS_WIN
    const QString symlinkRoot = temporary.filePath(QStringLiteral("inventory-symlink"));
    const QString symlinkTarget = temporary.filePath(QStringLiteral("symlink-target"));
    const QString symlinkId = QStringLiteral("20260823_150000_000_5a5a5a5a");
    if (!require(QDir().mkpath(symlinkRoot) && QDir().mkpath(symlinkTarget)
                    && QFile::link(symlinkTarget, QDir(symlinkRoot).filePath(symlinkId)),
                 "failed to create symlink inventory fixture")) {
        return 1;
    }
    ConfigurationBackupStore symlinkStore(symlinkRoot, &keys);
    const ConfigurationBackupInventoryResult symlinkInventory = symlinkStore.inventory(
        QStringLiteral("codex"), 1, legacyPaths);
    if (!require(symlinkInventory.state == ConfigurationBackupInventoryState::Invalid,
                 "symlink backup directory was not invalid")) {
        return 1;
    }
#endif

    const QString migrationRoot = temporary.filePath(QStringLiteral("inventory-migration"));
    const QString migrationId = QStringLiteral("20260823_160000_000_1e6ac000");
    FakeKeyProvider migrationKeys;
    ConfigurationBackupStore migrationStore(migrationRoot, &migrationKeys);
    if (!require(writeLegacy(
                     migrationRoot, migrationId, legacyPaths, legacyContents),
                 "failed to create inventory legacy fixture")) {
        return 1;
    }
    const ConfigurationBackupInventoryResult migratedInventory =
        migrationStore.inventory(QStringLiteral("codex"), 1, legacyPaths);
    if (!require(migratedInventory.state == ConfigurationBackupInventoryState::Ready
                    && migratedInventory.entries.size() == 1,
                 "inventory did not migrate exact legacy evidence")
            || !require(!recursiveBytes(migrationRoot).contains(legacyCredential)
                        && !QFileInfo::exists(QDir(migrationRoot).filePath(
                            migrationId + QStringLiteral("/file_0.bin"))),
                        "inventory migration retained legacy plaintext")) {
        return 1;
    }

    const QString removeRoot = temporary.filePath(QStringLiteral("remove-verified"));
    const QString removeStagingRoot = temporary.filePath(
        QStringLiteral("remove-verified-staging"));
    const QString removeId = QStringLiteral("20260823_170000_000_de1e7e00");
    ConfigurationBackupStore removeStore(removeRoot, &keys);
    ConfigurationBackupStore removeStaging(removeStagingRoot, &keys);
    ConfigurationBackupSnapshot removeSource = snapshot(removeId);
    if (!require(removeStore.create(removeSource, &error),
                 "failed to create removal fixture")) {
        return 1;
    }
    ConfigurationBackupInventoryResult removeInventory = removeStore.inventory(
        QStringLiteral("codex"), 1, legacyPaths);
    if (!require(removeInventory.state == ConfigurationBackupInventoryState::Ready
                    && removeInventory.entries.size() == 1,
                 "removal fixture did not inventory")) {
        return 1;
    }
    const QString removeDirectory = QDir(removeRoot).filePath(removeId);
    const QString removeManifest = QDir(removeDirectory).filePath(
        QStringLiteral("manifest.json"));
    const QByteArray removalOriginal = readFile(removeManifest);
    if (!require(!removeStore.removeVerified(
                     QStringLiteral("codex"), removeId,
                     QStringLiteral("configuration-backup-manifest:sha256:")
                         + QString(64, QLatin1Char('0')), &error)
                    && readFile(removeManifest) == removalOriginal,
                 "wrong removal identity changed evidence")) {
        return 1;
    }

    ConfigurationBackupSnapshot replacement = removeSource;
    replacement.files[0].content = QByteArray("valid-replacement-content");
    if (!require(removeStaging.create(replacement, &error),
                 "failed to create valid replacement fixture")) {
        return 1;
    }
    const QByteArray replacementManifest = readFile(QDir(removeStagingRoot).filePath(
        removeId + QStringLiteral("/manifest.json")));
    if (!require(writeFile(removeManifest, replacementManifest)
                    && !removeStore.removeVerified(
                        QStringLiteral("codex"), removeId,
                        removeInventory.entries.first().identity, &error)
                    && readFile(removeManifest) == replacementManifest,
                 "valid identity replacement was deleted or modified")) {
        return 1;
    }

    if (!require(writeFile(removeManifest, removalOriginal),
                 "failed to restore removal fixture")) {
        return 1;
    }
    QByteArray removalTamper = removalOriginal;
    removalTamper[removalTamper.size() / 2] ^= 1;
    if (!require(writeFile(removeManifest, removalTamper)
                    && !removeStore.removeVerified(
                        QStringLiteral("codex"), removeId,
                        removeInventory.entries.first().identity, &error)
                    && readFile(removeManifest) == removalTamper,
                 "tampered removal evidence was deleted or modified")) {
        return 1;
    }

    if (!require(writeFile(removeManifest, removalOriginal),
                 "failed to restore verified removal fixture")) {
        return 1;
    }
    removeInventory = removeStore.inventory(
        QStringLiteral("codex"), 1, legacyPaths);
    if (!require(removeInventory.state == ConfigurationBackupInventoryState::Ready
                    && removeStore.removeVerified(
                        QStringLiteral("codex"), removeId,
                        removeInventory.entries.first().identity, &error)
                    && !QFileInfo::exists(removeDirectory),
                 "verified removal did not remove the exact directory")) {
        std::cerr << error.toStdString() << '\n';
        return 1;
    }
    const ConfigurationBackupInventoryResult removedInventory = removeStore.inventory(
        QStringLiteral("codex"), 1, legacyPaths);
    if (!require(removedInventory.state == ConfigurationBackupInventoryState::Empty,
                 "removed store was not empty")) {
        return 1;
    }

    keys.fail = true;
    if (!require(!store.read(QStringLiteral("codex"), kId, &rejected, &error)
                    && rejected.files.isEmpty(),
                 "key backend failure produced output")) {
        return 1;
    }
    return 0;
}
