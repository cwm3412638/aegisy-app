#include "profile_manager.h"

#include <QSettings>
#include <QStringList>

namespace {

const QString kProfilesPrefix = QStringLiteral("profiles");
constexpr int kSchemaVersion = 2;

const QStringList kProfileKeys = {
    QStringLiteral("name"),
    QStringLiteral("type"),
    QStringLiteral("key"),
    QStringLiteral("model"),
};

QString profilePath(int index, const QString &field)
{
    return QStringLiteral("%1/%2/%3")
        .arg(kProfilesPrefix)
        .arg(index)
        .arg(field);
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
    ensureDefaultProfile();
}

void ProfileManager::migrateLegacyProfiles()
{
    QSettings settings;
    if (settings.value(kProfilesPrefix + QStringLiteral("/schema_version"), 0).toInt()
            >= kSchemaVersion) {
        return;
    }

    const int oldCount = settings.value(kProfilesPrefix + QStringLiteral("/count"), 0).toInt();
    if (oldCount <= 0) {
        settings.setValue(kProfilesPrefix + QStringLiteral("/schema_version"), kSchemaVersion);
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

    settings.remove(kProfilesPrefix);
    settings.setValue(kProfilesPrefix + QStringLiteral("/schema_version"), kSchemaVersion);
    settings.setValue(kProfilesPrefix + QStringLiteral("/count"), migrated.size());
    settings.setValue(
        kProfilesPrefix + QStringLiteral("/active"),
        newActive >= 0 ? newActive : 0);

    for (int i = 0; i < migrated.size(); ++i) {
        const Profile &profile = migrated[i];
        settings.setValue(profilePath(i, QStringLiteral("name")), profile.name);
        settings.setValue(
            profilePath(i, QStringLiteral("type")),
            static_cast<int>(profile.type));
        settings.setValue(profilePath(i, QStringLiteral("key")), profile.key);
        settings.setValue(profilePath(i, QStringLiteral("model")), profile.model);
    }
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
        profile.name = settings.value(
            profilePath(i, QStringLiteral("name")),
            QStringLiteral("档案 %1").arg(i + 1)).toString();
        profile.type = static_cast<ProfileType>(settings.value(
            profilePath(i, QStringLiteral("type")),
            static_cast<int>(ProfileType::Codex)).toInt());
        if (!isValidProfileType(profile.type)) {
            profile.type = ProfileType::Codex;
        }
        profile.key = settings.value(profilePath(i, QStringLiteral("key"))).toString();
        profile.model = settings.value(profilePath(i, QStringLiteral("model"))).toString();
        result.append(profile);
    }
    return result;
}

int ProfileManager::activeIndex() const
{
    const int profileCount = count();
    if (profileCount == 0) {
        return -1;
    }
    return qBound(
        0,
        QSettings().value(kProfilesPrefix + QStringLiteral("/active"), 0).toInt(),
        profileCount - 1);
}

Profile ProfileManager::activeProfile() const
{
    const QList<Profile> profiles = allProfiles();
    const int index = activeIndex();
    return index >= 0 && index < profiles.size() ? profiles[index] : Profile{};
}

int ProfileManager::addProfile(const QString &name, ProfileType type,
                               const QString &key, const QString &model)
{
    if (!isValidProfileType(type)) {
        type = ProfileType::Codex;
    }

    QSettings settings;
    const int index = settings.value(
        kProfilesPrefix + QStringLiteral("/count"), 0).toInt();
    settings.setValue(profilePath(index, QStringLiteral("name")), name);
    settings.setValue(profilePath(index, QStringLiteral("type")), static_cast<int>(type));
    settings.setValue(profilePath(index, QStringLiteral("key")), key);
    settings.setValue(profilePath(index, QStringLiteral("model")), model);
    settings.setValue(kProfilesPrefix + QStringLiteral("/count"), index + 1);
    settings.setValue(kProfilesPrefix + QStringLiteral("/schema_version"), kSchemaVersion);
    emit profilesChanged();
    return index;
}

void ProfileManager::updateProfile(int index, const QString &name, ProfileType type,
                                   const QString &key, const QString &model)
{
    if (index < 0 || index >= count() || !isValidProfileType(type)) {
        return;
    }

    QSettings settings;
    settings.setValue(profilePath(index, QStringLiteral("name")), name);
    settings.setValue(profilePath(index, QStringLiteral("type")), static_cast<int>(type));
    settings.setValue(profilePath(index, QStringLiteral("key")), key);
    settings.setValue(profilePath(index, QStringLiteral("model")), model);
    emit profilesChanged();
}

void ProfileManager::removeProfile(int index)
{
    QSettings settings;
    const int profileCount = settings.value(
        kProfilesPrefix + QStringLiteral("/count"), 0).toInt();
    if (profileCount <= 1 || index < 0 || index >= profileCount) {
        return;
    }

    for (int i = index; i < profileCount - 1; ++i) {
        for (const QString &key : kProfileKeys) {
            settings.setValue(
                profilePath(i, key),
                settings.value(profilePath(i + 1, key)));
        }
    }
    settings.remove(QStringLiteral("%1/%2").arg(kProfilesPrefix).arg(profileCount - 1));
    settings.setValue(kProfilesPrefix + QStringLiteral("/count"), profileCount - 1);

    const int active = settings.value(
        kProfilesPrefix + QStringLiteral("/active"), 0).toInt();
    int nextActive = active;
    if (active == index) {
        nextActive = qMin(index, profileCount - 2);
    } else if (active > index) {
        nextActive = active - 1;
    }
    settings.setValue(kProfilesPrefix + QStringLiteral("/active"), nextActive);
    emit profilesChanged();
}

void ProfileManager::setActiveIndex(int index)
{
    if (index < 0 || index >= count()) {
        return;
    }

    const int oldIndex = activeIndex();
    QSettings().setValue(kProfilesPrefix + QStringLiteral("/active"), index);
    emit activeProfileChanged(oldIndex, index);
}
