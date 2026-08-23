#include "profile_manager.h"
#include "secure_storage.h"
#include "profile_archive.h"
#include "credential_metadata.h"

#include <QSettings>
#include <QStringList>
#include <QUuid>

namespace {

const QString kProfilesPrefix = QStringLiteral("profiles");
constexpr int kSingleToolSchemaVersion = 2;
constexpr int kCredentialSchemaVersion = 3;
constexpr int kActiveSchemaVersion = 4;
constexpr int kCredentialPresenceSchemaVersion = 5;
constexpr int kCredentialBindingSchemaVersion = 6;
constexpr int kWebsiteBindingSchemaVersion = 7;
constexpr int kSchemaVersion = kWebsiteBindingSchemaVersion;

const QStringList kProfileKeys = {
    QStringLiteral("id"),
    QStringLiteral("name"),
    QStringLiteral("type"),
    QStringLiteral("credential_ref"),
    QStringLiteral("has_credential"),
    QStringLiteral("key"), // 仅用于安全存储迁移失败时保留旧档案。
    QStringLiteral("model"),
    QStringLiteral("key_hint"),
    QStringLiteral("website_account_identity"),
    QStringLiteral("website_key_identity"),
    QStringLiteral("website_projection_sha256"),
};

QString profilePath(int index, const QString &field)
{
    return QStringLiteral("%1/%2/%3")
        .arg(kProfilesPrefix)
        .arg(index)
        .arg(field);
}

QString activeProfileKey(ProfileType type)
{
    QString suffix;
    switch (type) {
    case ProfileType::Claude: suffix = QStringLiteral("claude"); break;
    case ProfileType::Codex:  suffix = QStringLiteral("codex"); break;
    case ProfileType::Gemini: suffix = QStringLiteral("gemini"); break;
    case ProfileType::OpenCode: suffix = QStringLiteral("opencode"); break;
    }
    return QStringLiteral("%1/active/%2").arg(kProfilesPrefix, suffix);
}

QString newProfileId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString credentialRefForId(const QString &id)
{
    return QStringLiteral("profile/%1/api-key").arg(id);
}

bool validProfileId(const QString &id)
{
    const QUuid uuid(id);
    return !uuid.isNull()
        && uuid.toString(QUuid::WithoutBraces) == id.toLower();
}

bool credentialBindingValid(QSettings &settings, int index, const QString &id)
{
    return validProfileId(id)
        && settings.value(profilePath(index, QStringLiteral("credential_ref"))).toString()
            == credentialRefForId(id);
}

bool validCredentialFingerprint(const QString &value)
{
    if (value.isEmpty()) return true;
    if (value.size() != 8) return false;
    for (const QChar character : value) {
        if (!character.isDigit()
                && !(character >= QLatin1Char('a') && character <= QLatin1Char('f'))) {
            return false;
        }
    }
    return true;
}

bool validLowerSha256(const QString &value)
{
    if (value.size() != 64) return false;
    for (const QChar character : value) {
        if (!character.isDigit()
                && !(character >= QLatin1Char('a') && character <= QLatin1Char('f'))) {
            return false;
        }
    }
    return true;
}

bool validPrefixedSha256(const QString &value, const QString &prefix)
{
    return value.startsWith(prefix) && validLowerSha256(value.mid(prefix.size()));
}

bool validWebsiteBinding(const ProfileWebsiteBinding &binding)
{
    const bool empty = binding.accountIdentity.isEmpty()
        && binding.keyIdentity.isEmpty() && binding.projectionSha256.isEmpty();
    return empty
        || (validPrefixedSha256(
                binding.accountIdentity, QStringLiteral("website-account-session:sha256:"))
            && validPrefixedSha256(
                binding.keyIdentity, QStringLiteral("website-key:sha256:"))
            && validLowerSha256(binding.projectionSha256));
}

// 生成域分离短指纹，避免将任何凭据子串持久化到普通设置。
QString maskedTail(const QString &key)
{
    return credentialFingerprint(key);
}

struct LegacyToolConfig {
    ProfileType type;
    QString key;
    QString model;
};

} // namespace

ProfileManager::ProfileManager(QObject *parent)
    : QObject(parent)
{
    migrateLegacyProfiles();
    migrateProfileCredentials();
    migrateActiveProfiles();
    migrateCredentialPresence();
    migrateCredentialBindings();
    migrateWebsiteBindings();
    ensureDefaultProfile();
}

void ProfileManager::migrateLegacyProfiles()
{
    QSettings settings;
    if (settings.value(kProfilesPrefix + QStringLiteral("/schema_version"), 0).toInt()
            >= kSingleToolSchemaVersion) {
        return;
    }

    const int oldCount = settings.value(kProfilesPrefix + QStringLiteral("/count"), 0).toInt();
    if (oldCount <= 0) {
        settings.setValue(
            kProfilesPrefix + QStringLiteral("/schema_version"),
            kSingleToolSchemaVersion);
        return;
    }

    const int oldActive = qBound(
        0,
        settings.value(kProfilesPrefix + QStringLiteral("/active"), 0).toInt(),
        oldCount - 1);

    QList<Profile> migrated;
    int newActive = -1;

    for (int i = 0; i < oldCount; ++i) {
        const QString base = QStringLiteral("%1/%2").arg(kProfilesPrefix).arg(i);
        const QString baseName = settings.value(
            base + QStringLiteral("/name"),
            QStringLiteral("档案 %1").arg(i + 1)).toString();
        const ProfileType storedType = static_cast<ProfileType>(
            settings.value(base + QStringLiteral("/type"), 0).toInt());

        QList<LegacyToolConfig> configs;

        // 兼容迁移过程中已经写入新版字段、但尚未记录版本号的情况。
        if (settings.contains(base + QStringLiteral("/key"))) {
            const ProfileType type = isValidProfileType(storedType)
                ? storedType
                : ProfileType::Codex;
            configs.append({
                type,
                settings.value(base + QStringLiteral("/key")).toString(),
                settings.value(base + QStringLiteral("/model")).toString(),
            });
        } else {
            const struct {
                ProfileType type;
                const char *keyField;
                const char *modelField;
            } legacyFields[] = {
                { ProfileType::Claude, "claude_key", "claude_model" },
                { ProfileType::Codex,  "codex_key",  "codex_model"  },
                { ProfileType::Gemini, "gemini_key", "gemini_model" },
            };

            for (const auto &fields : legacyFields) {
                const QString key = settings.value(
                    base + QLatin1Char('/') + QLatin1String(fields.keyField)).toString();
                if (key.isEmpty()) {
                    continue;
                }
                configs.append({
                    fields.type,
                    key,
                    settings.value(
                        base + QLatin1Char('/') + QLatin1String(fields.modelField)).toString(),
                });
            }
        }

        if (configs.isEmpty()) {
            configs.append({
                isValidProfileType(storedType) ? storedType : ProfileType::Codex,
                QString(),
                QString(),
            });
        }

        const int firstMigratedIndex = migrated.size();
        for (const LegacyToolConfig &config : configs) {
            Profile profile;
            profile.name = configs.size() == 1
                ? baseName
                : QStringLiteral("%1 · %2").arg(baseName, profileTypeName(config.type));
            profile.type = config.type;
            profile.key = config.key;
            profile.model = config.model;
            profile.hasCredential = !config.key.isEmpty();
            migrated.append(profile);
        }

        if (i == oldActive) {
            newActive = firstMigratedIndex;
            if (isValidProfileType(storedType)) {
                for (int offset = 0; offset < configs.size(); ++offset) {
                    if (configs[offset].type == storedType) {
                        newActive = firstMigratedIndex + offset;
                        break;
                    }
                }
            }
        }
    }

    QStringList stagedCredentialRefs;
    for (Profile &profile : migrated) {
        profile.id = newProfileId();
        const QString credentialRef = credentialRefForId(profile.id);
        if (!profile.key.isEmpty()
                && !SecureStorage::saveEncrypted(credentialRef, profile.key)) {
            for (const QString &stagedRef : stagedCredentialRefs) {
                SecureStorage::remove(stagedRef);
            }
            m_lastError = QStringLiteral(
                "旧档案无法迁移到系统安全存储，原数据已保留。Linux 请安装并启用 secret-tool/Secret Service。");
            return;
        }
        if (!profile.key.isEmpty()) {
            stagedCredentialRefs.append(credentialRef);
        }
    }

    settings.remove(kProfilesPrefix);
    settings.setValue(kProfilesPrefix + QStringLiteral("/schema_version"), kSchemaVersion);
    settings.setValue(kProfilesPrefix + QStringLiteral("/count"), migrated.size());
    for (ProfileType type : allProfileTypes()) {
        settings.setValue(activeProfileKey(type), -1);
    }
    if (newActive >= 0 && newActive < migrated.size()) {
        settings.setValue(activeProfileKey(migrated[newActive].type), newActive);
    }

    for (int i = 0; i < migrated.size(); ++i) {
        const Profile &profile = migrated[i];
        settings.setValue(profilePath(i, QStringLiteral("id")), profile.id);
        settings.setValue(profilePath(i, QStringLiteral("name")), profile.name);
        settings.setValue(
            profilePath(i, QStringLiteral("type")),
            static_cast<int>(profile.type));
        settings.setValue(
            profilePath(i, QStringLiteral("credential_ref")),
            credentialRefForId(profile.id));
        settings.setValue(
            profilePath(i, QStringLiteral("has_credential")),
            profile.hasCredential);
        settings.setValue(profilePath(i, QStringLiteral("model")), profile.model);
    }
    settings.sync();
}

void ProfileManager::migrateProfileCredentials()
{
    QSettings settings;
    const int storedVersion = settings.value(
        kProfilesPrefix + QStringLiteral("/schema_version"), 0).toInt();
    if (storedVersion >= kCredentialSchemaVersion
            || storedVersion < kSingleToolSchemaVersion) {
        return;
    }

    bool migrated = true;
    const int profileCount = settings.value(
        kProfilesPrefix + QStringLiteral("/count"), 0).toInt();
    for (int i = 0; i < profileCount; ++i) {
        QString id = settings.value(profilePath(i, QStringLiteral("id"))).toString();
        if (id.isEmpty()) {
            id = newProfileId();
        }
        const QString credentialRef = credentialRefForId(id);
        const QString legacyKey = settings.value(
            profilePath(i, QStringLiteral("key"))).toString();

        if (!legacyKey.isEmpty()
                && !SecureStorage::saveEncrypted(credentialRef, legacyKey)) {
            migrated = false;
            continue;
        }

        settings.setValue(profilePath(i, QStringLiteral("id")), id);
        settings.setValue(
            profilePath(i, QStringLiteral("credential_ref")), credentialRef);
        settings.setValue(
            profilePath(i, QStringLiteral("has_credential")),
            !legacyKey.isEmpty() || SecureStorage::contains(credentialRef));
        settings.remove(profilePath(i, QStringLiteral("key")));
    }

    if (migrated) {
        settings.setValue(
            kProfilesPrefix + QStringLiteral("/schema_version"),
            kCredentialSchemaVersion);
    } else {
        m_lastError = QStringLiteral(
            "部分档案凭据无法迁移到系统安全存储。Linux 请安装并启用 secret-tool/Secret Service。");
    }
    settings.sync();
}

void ProfileManager::migrateActiveProfiles()
{
    QSettings settings;
    const int storedVersion = settings.value(
        kProfilesPrefix + QStringLiteral("/schema_version"), 0).toInt();
    if (storedVersion >= kActiveSchemaVersion
            || storedVersion < kCredentialSchemaVersion) {
        return;
    }

    const int legacyActive = settings.value(
        kProfilesPrefix + QStringLiteral("/active"), -1).toInt();
    settings.remove(kProfilesPrefix + QStringLiteral("/active"));
    for (ProfileType type : allProfileTypes()) {
        settings.setValue(activeProfileKey(type), -1);
    }

    const QList<Profile> profiles = allProfiles();
    if (legacyActive >= 0 && legacyActive < profiles.size()) {
        settings.setValue(activeProfileKey(profiles[legacyActive].type), legacyActive);
    }
    settings.setValue(
        kProfilesPrefix + QStringLiteral("/schema_version"),
        kActiveSchemaVersion);
    settings.sync();
}

void ProfileManager::migrateCredentialPresence()
{
    QSettings settings;
    const int storedVersion = settings.value(
        kProfilesPrefix + QStringLiteral("/schema_version"), 0).toInt();
    if (storedVersion >= kCredentialPresenceSchemaVersion
            || storedVersion < kActiveSchemaVersion) {
        return;
    }

    const int profileCount = settings.value(
        kProfilesPrefix + QStringLiteral("/count"), 0).toInt();
    for (int i = 0; i < profileCount; ++i) {
        const QString id = settings.value(
            profilePath(i, QStringLiteral("id"))).toString();
        const QString credentialRef = validProfileId(id)
            ? credentialRefForId(id) : QString();
        const bool present = !credentialRef.isEmpty()
            && SecureStorage::contains(credentialRef);
        settings.setValue(
            profilePath(i, QStringLiteral("credential_ref")), credentialRef);
        settings.setValue(
            profilePath(i, QStringLiteral("has_credential")), present);
    }
    settings.setValue(
        kProfilesPrefix + QStringLiteral("/schema_version"),
        kCredentialPresenceSchemaVersion);
    settings.sync();
}

void ProfileManager::migrateCredentialBindings()
{
    QSettings settings;
    const int storedVersion = settings.value(
        kProfilesPrefix + QStringLiteral("/schema_version"), 0).toInt();
    if (storedVersion >= kCredentialBindingSchemaVersion
            || storedVersion < kCredentialPresenceSchemaVersion) {
        return;
    }

    const int profileCount = settings.value(
        kProfilesPrefix + QStringLiteral("/count"), 0).toInt();
    for (int i = 0; i < profileCount; ++i) {
        QString id = settings.value(profilePath(i, QStringLiteral("id"))).toString();
        const bool idWasValid = validProfileId(id);
        if (!idWasValid) id = newProfileId();
        const QString canonicalRef = credentialRefForId(id);
        const QString storedRef = settings.value(
            profilePath(i, QStringLiteral("credential_ref"))).toString();
        const bool bindingWasValid = idWasValid && storedRef == canonicalRef;
        settings.setValue(profilePath(i, QStringLiteral("id")), id);
        settings.setValue(profilePath(i, QStringLiteral("credential_ref")), canonicalRef);
        settings.setValue(
            profilePath(i, QStringLiteral("has_credential")),
            bindingWasValid && SecureStorage::contains(canonicalRef));
        settings.remove(profilePath(i, QStringLiteral("key_hint")));
    }
    settings.setValue(
        kProfilesPrefix + QStringLiteral("/schema_version"),
        kCredentialBindingSchemaVersion);
    settings.sync();
}

void ProfileManager::migrateWebsiteBindings()
{
    QSettings settings;
    const int storedVersion = settings.value(
        kProfilesPrefix + QStringLiteral("/schema_version"), 0).toInt();
    if (storedVersion >= kWebsiteBindingSchemaVersion
            || storedVersion < kCredentialBindingSchemaVersion) {
        return;
    }
    const int profileCount = settings.value(
        kProfilesPrefix + QStringLiteral("/count"), 0).toInt();
    for (int i = 0; i < profileCount; ++i) {
        settings.remove(profilePath(i, QStringLiteral("website_account_identity")));
        settings.remove(profilePath(i, QStringLiteral("website_key_identity")));
        settings.remove(profilePath(i, QStringLiteral("website_projection_sha256")));
    }
    settings.setValue(
        kProfilesPrefix + QStringLiteral("/schema_version"),
        kWebsiteBindingSchemaVersion);
    settings.sync();
}

void ProfileManager::ensureDefaultProfile()
{
    if (count() == 0) {
        addProfile(QStringLiteral("默认 Codex"), ProfileType::Codex);
    }
}

int ProfileManager::count() const
{
    return QSettings().value(kProfilesPrefix + QStringLiteral("/count"), 0).toInt();
}

QList<Profile> ProfileManager::allProfiles() const
{
    QSettings settings;
    const int profileCount = settings.value(
        kProfilesPrefix + QStringLiteral("/count"), 0).toInt();

    QList<Profile> result;
    result.reserve(profileCount);
    for (int i = 0; i < profileCount; ++i) {
        Profile profile;
        profile.index = i;
        profile.id = settings.value(profilePath(i, QStringLiteral("id"))).toString();
        profile.name = settings.value(
            profilePath(i, QStringLiteral("name")),
            QStringLiteral("档案 %1").arg(i + 1)).toString();
        profile.type = static_cast<ProfileType>(settings.value(
            profilePath(i, QStringLiteral("type")),
            static_cast<int>(ProfileType::Codex)).toInt());
        if (!isValidProfileType(profile.type)) {
            profile.type = ProfileType::Codex;
        }
        const QString presencePath = profilePath(i, QStringLiteral("has_credential"));
        profile.hasCredential = credentialBindingValid(settings, i, profile.id)
            && (settings.contains(presencePath)
            ? settings.value(presencePath).toBool()
            : !settings.value(profilePath(i, QStringLiteral("key"))).toString().isEmpty());
        profile.model = settings.value(profilePath(i, QStringLiteral("model"))).toString();
        profile.keyHint = settings.value(profilePath(i, QStringLiteral("key_hint"))).toString();
        if (!validCredentialFingerprint(profile.keyHint)) profile.keyHint.clear();
        ProfileWebsiteBinding website{
            settings.value(profilePath(
                i, QStringLiteral("website_account_identity"))).toString(),
            settings.value(profilePath(
                i, QStringLiteral("website_key_identity"))).toString(),
            settings.value(profilePath(
                i, QStringLiteral("website_projection_sha256"))).toString(),
        };
        if (validWebsiteBinding(website)) {
            profile.websiteAccountIdentity = website.accountIdentity;
            profile.websiteKeyIdentity = website.keyIdentity;
            profile.websiteProjectionSha256 = website.projectionSha256;
        }
        result.append(profile);
    }
    return result;
}

QString ProfileManager::maskedKeyHint(const QString &key)
{
    return maskedTail(key);
}

bool ProfileManager::isActivationSelectionValid(const QList<Profile> &profiles,
                                                int index, ProfileType type)
{
    return index >= 0 && index < profiles.size()
        && profiles.at(index).index == index
        && profiles.at(index).type == type
        && profiles.at(index).hasAnyKey();
}

void ProfileManager::backfillKeyHints()
{
    QSettings settings;
    const int profileCount = settings.value(
        kProfilesPrefix + QStringLiteral("/count"), 0).toInt();
    for (int i = 0; i < profileCount; ++i) {
        const bool hasCredential = settings.value(
            profilePath(i, QStringLiteral("has_credential")), false).toBool();
        const QString existing = settings.value(
            profilePath(i, QStringLiteral("key_hint"))).toString();
        if (!hasCredential || !existing.isEmpty()) {
            continue;
        }
        // profileWithCredential 载入凭据后会顺带补齐并持久化掩码。
        profileWithCredential(i);
    }
}

Profile ProfileManager::profileWithCredential(int index)
{
    m_lastError.clear();
    const QList<Profile> profiles = allProfiles();
    if (index < 0 || index >= profiles.size()) {
        m_lastError = QStringLiteral("档案索引无效。");
        return Profile{};
    }

    Profile profile = profiles[index];
    if (!profile.hasCredential) {
        return profile;
    }

    QSettings settings;
    if (!credentialBindingValid(settings, index, profile.id)) {
        profile.hasCredential = false;
        m_lastError = QStringLiteral("档案凭据引用无效，已拒绝读取。请重新保存该配置。");
        return profile;
    }
    const QString credentialRef = credentialRefForId(profile.id);
    profile.key = SecureStorage::loadEncrypted(credentialRef);
    if (profile.key.isEmpty()) {
        profile.key = settings.value(
            profilePath(index, QStringLiteral("key"))).toString();
    }
    if (profile.key.isEmpty()) {
        m_lastError = QStringLiteral(
            "无法读取该档案的 API Key。macOS 钥匙串询问时请选择“始终允许”，然后重试。");
    } else {
        // 补齐缺失的掩码提示，供卡片区分同类型配置。
        const QString hint = maskedTail(profile.key);
        if (!hint.isEmpty() && profile.keyHint != hint) {
            profile.keyHint = hint;
            settings.setValue(profilePath(index, QStringLiteral("key_hint")), hint);
        }
    }
    return profile;
}

int ProfileManager::activeIndex(ProfileType type) const
{
    if (!isValidProfileType(type)) {
        return -1;
    }

    const int stored = QSettings().value(activeProfileKey(type), -1).toInt();
    const QList<Profile> profiles = allProfiles();
    if (stored < 0 || stored >= profiles.size() || profiles[stored].type != type) {
        return -1;
    }
    return stored;
}

Profile ProfileManager::activeProfile(ProfileType type) const
{
    const QList<Profile> profiles = allProfiles();
    const int index = activeIndex(type);
    return index >= 0 && index < profiles.size() ? profiles[index] : Profile{};
}

bool ProfileManager::isActive(int index) const
{
    const QList<Profile> profiles = allProfiles();
    return index >= 0 && index < profiles.size()
        && activeIndex(profiles[index].type) == index;
}

int ProfileManager::addProfile(const QString &name, ProfileType type,
                               const QString &key, const QString &model,
                               const ProfileWebsiteBinding &website)
{
    m_lastError.clear();
    if (!isValidProfileType(type)) {
        type = ProfileType::Codex;
    }
    if (!validWebsiteBinding(website)) {
        m_lastError = QStringLiteral("网站配置来源绑定无效。");
        return -1;
    }

    QSettings settings;
    const int index = settings.value(
        kProfilesPrefix + QStringLiteral("/count"), 0).toInt();
    const QString id = newProfileId();
    const QString credentialRef = credentialRefForId(id);
    if (!key.isEmpty() && !SecureStorage::saveEncrypted(credentialRef, key)) {
        m_lastError = QStringLiteral(
            "无法将 API Key 保存到系统安全存储。Linux 请安装并启用 secret-tool/Secret Service。");
        return -1;
    }
    settings.setValue(profilePath(index, QStringLiteral("id")), id);
    settings.setValue(profilePath(index, QStringLiteral("name")), name);
    settings.setValue(profilePath(index, QStringLiteral("type")), static_cast<int>(type));
    settings.setValue(profilePath(index, QStringLiteral("credential_ref")), credentialRef);
    settings.setValue(
        profilePath(index, QStringLiteral("has_credential")), !key.isEmpty());
    settings.setValue(profilePath(index, QStringLiteral("model")), model);
    settings.setValue(profilePath(index, QStringLiteral("key_hint")), maskedTail(key));
    settings.setValue(profilePath(index, QStringLiteral("website_account_identity")),
                      website.accountIdentity);
    settings.setValue(profilePath(index, QStringLiteral("website_key_identity")),
                      website.keyIdentity);
    settings.setValue(profilePath(index, QStringLiteral("website_projection_sha256")),
                      website.projectionSha256);
    settings.setValue(kProfilesPrefix + QStringLiteral("/count"), index + 1);
    settings.setValue(kProfilesPrefix + QStringLiteral("/schema_version"), kSchemaVersion);
    emit profilesChanged();
    return index;
}

bool ProfileManager::updateProfile(int index, const QString &name, ProfileType type,
                                   const QString &key, const QString &model,
                                   const ProfileWebsiteBinding &website)
{
    m_lastError.clear();
    if (index < 0 || index >= count() || !isValidProfileType(type)) {
        m_lastError = QStringLiteral("档案索引或终端类型无效。");
        return false;
    }
    if (!validWebsiteBinding(website)) {
        m_lastError = QStringLiteral("网站配置来源绑定无效。");
        return false;
    }

    QSettings settings;
    const ProfileType oldType = static_cast<ProfileType>(settings.value(
        profilePath(index, QStringLiteral("type")),
        static_cast<int>(ProfileType::Codex)).toInt());
    const bool clearOldActive = isValidProfileType(oldType)
        && oldType != type && activeIndex(oldType) == index;
    const QString id = settings.value(profilePath(index, QStringLiteral("id"))).toString();
    if (!credentialBindingValid(settings, index, id)) {
        m_lastError = QStringLiteral("档案凭据引用无效，已拒绝更新。请删除后重新创建。");
        return false;
    }
    const QString credentialRef = credentialRefForId(id);
    const bool hadCredential = settings.value(
        profilePath(index, QStringLiteral("has_credential")), false).toBool();
    if (key.isEmpty()) {
        if (hadCredential && !SecureStorage::remove(credentialRef)) {
            m_lastError = QStringLiteral("无法从系统安全存储删除旧 API Key。");
            return false;
        }
    } else if (!SecureStorage::saveEncrypted(credentialRef, key)) {
            m_lastError = QStringLiteral(
                "无法将 API Key 保存到系统安全存储。Linux 请安装并启用 secret-tool/Secret Service。");
            return false;
    }
    settings.setValue(profilePath(index, QStringLiteral("id")), id);
    settings.setValue(profilePath(index, QStringLiteral("name")), name);
    settings.setValue(profilePath(index, QStringLiteral("type")), static_cast<int>(type));
    settings.setValue(profilePath(index, QStringLiteral("credential_ref")), credentialRef);
    settings.setValue(
        profilePath(index, QStringLiteral("has_credential")), !key.isEmpty());
    settings.remove(profilePath(index, QStringLiteral("key")));
    settings.setValue(profilePath(index, QStringLiteral("model")), model);
    settings.setValue(profilePath(index, QStringLiteral("key_hint")), maskedTail(key));
    settings.setValue(profilePath(index, QStringLiteral("website_account_identity")),
                      website.accountIdentity);
    settings.setValue(profilePath(index, QStringLiteral("website_key_identity")),
                      website.keyIdentity);
    settings.setValue(profilePath(index, QStringLiteral("website_projection_sha256")),
                      website.projectionSha256);
    if (clearOldActive) {
        settings.setValue(activeProfileKey(oldType), -1);
        emit activeProfileChanged(index, -1);
    }
    emit profilesChanged();
    return true;
}

void ProfileManager::removeProfile(int index)
{
    QSettings settings;
    const int profileCount = settings.value(
        kProfilesPrefix + QStringLiteral("/count"), 0).toInt();
    if (profileCount <= 1 || index < 0 || index >= profileCount) {
        return;
    }

    QList<QPair<ProfileType, int>> activeBefore;
    for (ProfileType type : allProfileTypes()) {
        activeBefore.append(qMakePair(type, activeIndex(type)));
    }

    const QString id = settings.value(profilePath(index, QStringLiteral("id"))).toString();
    const QString credentialRef = credentialBindingValid(settings, index, id)
        ? credentialRefForId(id) : QString();

    for (int i = index; i < profileCount - 1; ++i) {
        for (const QString &key : kProfileKeys) {
            settings.setValue(
                profilePath(i, key),
                settings.value(profilePath(i + 1, key)));
        }
    }
    settings.remove(QStringLiteral("%1/%2").arg(kProfilesPrefix).arg(profileCount - 1));
    settings.setValue(kProfilesPrefix + QStringLiteral("/count"), profileCount - 1);

    for (const auto &entry : activeBefore) {
        const ProfileType type = entry.first;
        const int active = entry.second;
        int nextActive = active;
        if (active == index) {
            nextActive = -1;
        } else if (active > index) {
            nextActive = active - 1;
        }
        settings.setValue(activeProfileKey(type), nextActive);
        if (nextActive != active) {
            emit activeProfileChanged(active, nextActive);
        }
    }
    if (!credentialRef.isEmpty()) {
        SecureStorage::remove(credentialRef);
    }
    emit profilesChanged();
}

void ProfileManager::setActiveIndex(int index)
{
    const QList<Profile> profiles = allProfiles();
    if (index < 0 || index >= profiles.size()) {
        return;
    }

    const ProfileType type = profiles[index].type;
    const int oldIndex = activeIndex(type);
    QSettings settings;
    settings.setValue(activeProfileKey(type), index);
    settings.setValue(kProfilesPrefix + QStringLiteral("/last_activated"), index);
    emit activeProfileChanged(oldIndex, index);
}

int ProfileManager::lastActivatedIndex() const
{
    const int stored = QSettings().value(
        kProfilesPrefix + QStringLiteral("/last_activated"), -1).toInt();
    return stored >= 0 && stored < count() ? stored : -1;
}

void ProfileManager::clearActiveProfile(ProfileType type)
{
    if (!isValidProfileType(type)) {
        return;
    }
    const int oldIndex = activeIndex(type);
    QSettings().setValue(activeProfileKey(type), -1);
    emit activeProfileChanged(oldIndex, -1);
}

bool ProfileManager::exportProfiles(const QString &filePath, const QString &password)
{
    m_lastError.clear();
    QList<ArchiveProfile> archived;
    const QList<Profile> profiles = allProfiles();
    archived.reserve(profiles.size());
    for (int i = 0; i < profiles.size(); ++i) {
        const Profile profile = profileWithCredential(i);
        if (profiles[i].hasCredential && profile.key.isEmpty()) {
            return false;
        }
        ArchiveProfile item;
        item.name = profile.name;
        item.type = static_cast<int>(profile.type);
        item.key = profile.key;
        item.model = profile.model;
        archived.append(item);
    }
    return ProfileArchive::writeEncrypted(filePath, archived, password, &m_lastError);
}

bool ProfileManager::importProfiles(const QString &filePath, const QString &password,
                                    int *importedCount)
{
    m_lastError.clear();
    QList<ArchiveProfile> archived;
    if (!ProfileArchive::readEncrypted(filePath, password, &archived, &m_lastError)) {
        return false;
    }
    if (archived.isEmpty()) {
        m_lastError = QStringLiteral("导入文件中没有档案。");
        return false;
    }

    struct StagedProfile {
        ArchiveProfile archive;
        QString id;
        QString credentialRef;
    };
    QList<StagedProfile> staged;
    staged.reserve(archived.size());
    for (const ArchiveProfile &item : archived) {
        const ProfileType type = static_cast<ProfileType>(item.type);
        if (!isValidProfileType(type)) {
            m_lastError = QStringLiteral("导入文件包含不支持的终端类型。");
            break;
        }
        StagedProfile profile;
        profile.archive = item;
        profile.id = newProfileId();
        profile.credentialRef = credentialRefForId(profile.id);
        if (!item.key.isEmpty()
                && !SecureStorage::saveEncrypted(profile.credentialRef, item.key)) {
            m_lastError = QStringLiteral(
                "无法将导入凭据保存到系统安全存储。Linux 请安装并启用 secret-tool/Secret Service。");
            break;
        }
        staged.append(profile);
    }

    if (staged.size() != archived.size()) {
        for (const StagedProfile &profile : staged) {
            SecureStorage::remove(profile.credentialRef);
        }
        return false;
    }

    QSettings settings;
    int index = settings.value(kProfilesPrefix + QStringLiteral("/count"), 0).toInt();
    for (const StagedProfile &profile : staged) {
        settings.setValue(profilePath(index, QStringLiteral("id")), profile.id);
        settings.setValue(profilePath(index, QStringLiteral("name")), profile.archive.name);
        settings.setValue(
            profilePath(index, QStringLiteral("type")), profile.archive.type);
        settings.setValue(
            profilePath(index, QStringLiteral("credential_ref")), profile.credentialRef);
        settings.setValue(
            profilePath(index, QStringLiteral("has_credential")),
            !profile.archive.key.isEmpty());
        settings.setValue(profilePath(index, QStringLiteral("model")), profile.archive.model);
        ++index;
    }
    settings.setValue(kProfilesPrefix + QStringLiteral("/count"), index);
    settings.setValue(kProfilesPrefix + QStringLiteral("/schema_version"), kSchemaVersion);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        const int firstImportedIndex = index - staged.size();
        for (int removeIndex = firstImportedIndex; removeIndex < index; ++removeIndex) {
            settings.remove(QStringLiteral("%1/%2").arg(kProfilesPrefix).arg(removeIndex));
        }
        settings.setValue(kProfilesPrefix + QStringLiteral("/count"), firstImportedIndex);
        settings.sync();
        for (const StagedProfile &profile : staged) {
            SecureStorage::remove(profile.credentialRef);
        }
        m_lastError = QStringLiteral("无法保存导入的档案元数据。");
        return false;
    }

    if (importedCount) {
        *importedCount = staged.size();
    }
    emit profilesChanged();
    return true;
}
