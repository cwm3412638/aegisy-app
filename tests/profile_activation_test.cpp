#include "profile_manager.h"
#include "secure_storage.h"

#include <QCoreApplication>
#include <QDebug>
#include <QSettings>
#include <QTemporaryDir>

namespace {

bool expect(bool condition, const char *message)
{
    if (!condition) {
        qCritical() << message;
    }
    return condition;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AegisyTest"));
    QCoreApplication::setApplicationName(QStringLiteral("ProfileActivation"));

    QTemporaryDir settingsRoot;
    if (!settingsRoot.isValid()) {
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());

    {
        QSettings().clear();
        ProfileManager manager;
        const int firstCodex = 0;
        const int claude = manager.addProfile(
            QStringLiteral("Claude"), ProfileType::Claude);
        const int secondCodex = manager.addProfile(
            QStringLiteral("Codex 2"), ProfileType::Codex);
        const int gemini = manager.addProfile(
            QStringLiteral("Gemini"), ProfileType::Gemini);

        manager.setActiveIndex(firstCodex);
        manager.setActiveIndex(claude);
        manager.setActiveIndex(gemini);
        if (!expect(manager.activeIndex(ProfileType::Codex) == firstCodex,
                    "Codex active profile was not stored")
                || !expect(manager.activeIndex(ProfileType::Claude) == claude,
                           "Claude active profile was not stored")
                || !expect(manager.activeIndex(ProfileType::Gemini) == gemini,
                           "Gemini active profile was not stored")) {
            return 1;
        }

        manager.setActiveIndex(secondCodex);
        if (!expect(manager.activeIndex(ProfileType::Codex) == secondCodex,
                    "Second Codex profile did not replace the first Codex profile")
                || !expect(manager.activeIndex(ProfileType::Claude) == claude,
                           "Activating Codex unexpectedly cleared Claude")
                || !expect(manager.activeIndex(ProfileType::Gemini) == gemini,
                           "Activating Codex unexpectedly cleared Gemini")) {
            return 1;
        }

        manager.removeProfile(claude);
        if (!expect(manager.activeIndex(ProfileType::Claude) == -1,
                    "Removing active Claude profile did not clear Claude state")
                || !expect(manager.activeIndex(ProfileType::Codex) == secondCodex - 1,
                           "Codex active index was not shifted after removal")
                || !expect(manager.activeIndex(ProfileType::Gemini) == gemini - 1,
                           "Gemini active index was not shifted after removal")) {
            return 1;
        }
    }

    {
        QSettings settings;
        settings.clear();
        settings.setValue(QStringLiteral("profiles/schema_version"), 3);
        settings.setValue(QStringLiteral("profiles/count"), 3);
        settings.setValue(QStringLiteral("profiles/active"), 1);
        const ProfileType types[] = {
            ProfileType::Claude, ProfileType::Codex, ProfileType::Gemini,
        };
        for (int i = 0; i < 3; ++i) {
            const QString base = QStringLiteral("profiles/%1/").arg(i);
            settings.setValue(base + QStringLiteral("id"), QStringLiteral("legacy-%1").arg(i));
            settings.setValue(base + QStringLiteral("name"), QStringLiteral("Legacy %1").arg(i));
            settings.setValue(base + QStringLiteral("type"), static_cast<int>(types[i]));
            settings.setValue(base + QStringLiteral("model"), QString());
        }
        settings.sync();

        ProfileManager migrated;
        if (!expect(migrated.activeIndex(ProfileType::Codex) == 1,
                    "Legacy active profile was not migrated")
                || !expect(migrated.activeIndex(ProfileType::Claude) == -1,
                           "Legacy migration incorrectly activated Claude")
                || !expect(migrated.activeIndex(ProfileType::Gemini) == -1,
                           "Legacy migration incorrectly activated Gemini")
                || !expect(QSettings().value(
                               QStringLiteral("profiles/schema_version")).toInt() == 5,
                           "Profile schema was not upgraded to version 5")) {
            return 1;
        }
    }

    {
        QSettings settings;
        settings.clear();
        settings.setValue(QStringLiteral("profiles/schema_version"), 5);
        settings.setValue(QStringLiteral("profiles/count"), 1);
        settings.setValue(QStringLiteral("profiles/0/id"), QStringLiteral("lazy-profile"));
        settings.setValue(QStringLiteral("profiles/0/name"), QStringLiteral("Lazy Codex"));
        settings.setValue(
            QStringLiteral("profiles/0/type"), static_cast<int>(ProfileType::Codex));
        settings.setValue(
            QStringLiteral("profiles/0/credential_ref"),
            QStringLiteral("profile/lazy-profile/api-key"));
        settings.setValue(QStringLiteral("profiles/0/has_credential"), true);
        settings.sync();

        ProfileManager manager;
        const QList<Profile> profiles = manager.allProfiles();
        if (!expect(profiles.size() == 1, "Lazy profile was not loaded")
                || !expect(profiles[0].hasAnyKey(),
                           "Saved credential presence was not exposed")
                || !expect(profiles[0].key.isEmpty(),
                           "Profile list unexpectedly loaded the credential plaintext")) {
            return 1;
        }
    }

    // maskedKeyHint 是纯函数，不依赖安全存储。
    if (!expect(ProfileManager::maskedKeyHint(QStringLiteral("sk-abc1234"))
                    == QStringLiteral("1234"),
                "maskedKeyHint should return the last four characters")
            || !expect(ProfileManager::maskedKeyHint(QStringLiteral("ab"))
                           == QStringLiteral("ab"),
                       "maskedKeyHint should return short keys whole")
            || !expect(ProfileManager::maskedKeyHint(QString()).isEmpty(),
                       "maskedKeyHint should map empty to empty")
            || !expect(ProfileManager::maskedKeyHint(QStringLiteral("  key-WXYZ  "))
                           == QStringLiteral("WXYZ"),
                       "maskedKeyHint should trim before taking the tail")) {
        return 1;
    }

    {
        QSettings().clear();
        ProfileManager manager;
        // lastActivatedIndex 跨类型跟踪最近一次激活。
        const int codex = 0;   // ensureDefaultProfile 生成的默认 Codex
        const int claude = manager.addProfile(
            QStringLiteral("Claude"), ProfileType::Claude);
        if (!expect(manager.lastActivatedIndex() == -1,
                    "lastActivatedIndex should start unset")) {
            return 1;
        }
        manager.setActiveIndex(codex);
        if (!expect(manager.lastActivatedIndex() == codex,
                    "lastActivatedIndex should track the Codex activation")) {
            return 1;
        }
        manager.setActiveIndex(claude);
        if (!expect(manager.lastActivatedIndex() == claude,
                    "lastActivatedIndex should follow the newest activation")) {
            return 1;
        }
    }

    // 掩码持久化与回填依赖系统安全存储，仅在可用时验证。
    if (SecureStorage::isAvailable()) {
        QSettings().clear();
        ProfileManager manager;
        const int plus = manager.addProfile(
            QStringLiteral("Codex Plus"), ProfileType::Codex,
            QStringLiteral("sk-plus-PLUS"));
        const int pro = manager.addProfile(
            QStringLiteral("Codex Pro"), ProfileType::Codex,
            QStringLiteral("sk-pro-PROX"));
        if (plus < 0 || pro < 0) {
            qCritical() << "addProfile with key failed unexpectedly";
            return 1;
        }
        QList<Profile> profiles = manager.allProfiles();
        if (!expect(profiles[plus].keyHint == QStringLiteral("PLUS"),
                    "addProfile should persist the Plus key hint")
                || !expect(profiles[pro].keyHint == QStringLiteral("PROX"),
                           "addProfile should persist the Pro key hint")
                || !expect(profiles[plus].keyHint != profiles[pro].keyHint,
                           "different keys should yield distinguishable hints")) {
            return 1;
        }

        // 清掉掩码模拟存量配置，验证 backfillKeyHints 会补齐。
        QSettings().remove(QStringLiteral("profiles/%1/key_hint").arg(plus));
        QSettings().sync();
        if (!expect(manager.allProfiles()[plus].keyHint.isEmpty(),
                    "key hint should be cleared before backfill test")) {
            return 1;
        }
        manager.backfillKeyHints();
        if (!expect(manager.allProfiles()[plus].keyHint == QStringLiteral("PLUS"),
                    "backfillKeyHints should restore a missing hint")) {
            return 1;
        }
    }

    return 0;
}
