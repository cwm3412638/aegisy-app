#include "profile_manager.h"

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
                               QStringLiteral("profiles/schema_version")).toInt() == 4,
                           "Profile schema was not upgraded to version 4")) {
            return 1;
        }
    }

    return 0;
}
