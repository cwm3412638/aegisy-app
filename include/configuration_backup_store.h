#ifndef CONFIGURATION_BACKUP_STORE_H
#define CONFIGURATION_BACKUP_STORE_H

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

class ConfigurationBackupKeyProvider
{
public:
    virtual ~ConfigurationBackupKeyProvider() = default;

    // Implementations must return exactly 32 bytes. Production may create the
    // SecureStorage-backed key only when allowCreate is true.
    virtual bool keyForScope(const QString &scope, bool allowCreate,
                             QByteArray *key, QString *error) = 0;
};

struct ConfigurationBackupFile {
    int slot = -1;
    bool existed = false;
    QByteArray content;
};

struct ConfigurationBackupSnapshot {
    QString backupId;
    QString tool;
    QDateTime createdAt;
    QList<ConfigurationBackupFile> files;
};

enum class ConfigurationBackupInventoryState {
    Empty,
    Ready,
    Unavailable,
    Invalid,
};

struct ConfigurationBackupInventoryEntry {
    QString backupId;
    QString tool;
    QDateTime createdAt;
    int fileCount = 0;
    QString identity;
};

struct ConfigurationBackupInventoryResult {
    ConfigurationBackupInventoryState state =
        ConfigurationBackupInventoryState::Invalid;
    QList<ConfigurationBackupInventoryEntry> entries;
    QString issue;
};

class ConfigurationBackupStore
{
public:
    static constexpr qint64 MaxFileBytes = 4 * 1024 * 1024;
    static constexpr qint64 MaxPayloadBytes = 8 * 1024 * 1024;
    static constexpr qint64 MaxManifestBytes = 16 * 1024 * 1024;
    static constexpr int MaxFiles = 16;
    static constexpr int MaxBackups = 64;

    ConfigurationBackupStore(const QString &rootPath,
                             ConfigurationBackupKeyProvider *keyProvider);

    bool create(const ConfigurationBackupSnapshot &snapshot, QString *error);
    bool read(const QString &tool, const QString &backupId,
              ConfigurationBackupSnapshot *snapshot, QString *error);

    // Migrates the exact legacy ToolManager v1 directory in place. A valid
    // manifest.v2.pending file is the crash-recovery authority once published.
    bool migrateLegacy(const QString &tool, int legacyToolValue,
                       const QString &backupId,
                       const QStringList &managedPaths, QString *error);

    ConfigurationBackupInventoryResult inventory(
        const QString &tool, int legacyToolValue,
        const QStringList &managedPaths);
    bool removeVerified(const QString &tool, const QString &backupId,
                        const QString &expectedIdentity, QString *error);

    static bool isValidBackupId(const QString &backupId);
    static bool isValidTool(const QString &tool);

private:
    bool migrateLegacyLocked(const QString &tool, int legacyToolValue,
                             const QString &backupId,
                             const QStringList &managedPaths, QString *error);

    QString m_rootPath;
    ConfigurationBackupKeyProvider *m_keyProvider = nullptr;
};

#endif // CONFIGURATION_BACKUP_STORE_H
