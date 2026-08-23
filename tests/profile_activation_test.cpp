#include "profile_manager.h"
#include "secure_storage.h"
#include "credential_metadata.h"

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
        QList<Profile> profiles;
        int index = 0;
        for (ProfileType type : allProfileTypes()) {
            Profile profile;
            profile.index = index++;
            profile.type = type;
            profile.hasCredential = true;
            profiles.append(profile);
        }
        for (int i = 0; i < profiles.size(); ++i) {
            if (!expect(ProfileManager::isActivationSelectionValid(
                            profiles, i, profiles.at(i).type),
                        "four-tool activation selection was rejected")) {
                return 1;
            }
        }
        if (!expect(!ProfileManager::isActivationSelectionValid(
                        profiles, 0, ProfileType::OpenCode),
                    "mismatched activation type was accepted")
                || !expect(!ProfileManager::isActivationSelectionValid(
                               profiles, profiles.size(), ProfileType::Codex),
                           "out-of-range activation index was accepted")) {
            return 1;
        }
        profiles[0].hasCredential = false;
        if (!expect(!ProfileManager::isActivationSelectionValid(
                        profiles, 0, profiles[0].type),
                    "credential-free activation selection was accepted")) {
            return 1;
        }
    }

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
        const int openCode = manager.addProfile(
            QStringLiteral("OpenCode"), ProfileType::OpenCode);

        manager.setActiveIndex(firstCodex);
        manager.setActiveIndex(claude);
        manager.setActiveIndex(gemini);
        manager.setActiveIndex(openCode);
        if (!expect(manager.activeIndex(ProfileType::Codex) == firstCodex,
                    "Codex active profile was not stored")
                || !expect(manager.activeIndex(ProfileType::Claude) == claude,
                           "Claude active profile was not stored")
                || !expect(manager.activeIndex(ProfileType::Gemini) == gemini,
                           "Gemini active profile was not stored")
                || !expect(manager.activeIndex(ProfileType::OpenCode) == openCode,
                           "OpenCode active profile was not stored")) {
            return 1;
        }

        manager.setActiveIndex(secondCodex);
        if (!expect(manager.activeIndex(ProfileType::Codex) == secondCodex,
                    "Second Codex profile did not replace the first Codex profile")
                || !expect(manager.activeIndex(ProfileType::Claude) == claude,
                           "Activating Codex unexpectedly cleared Claude")
                || !expect(manager.activeIndex(ProfileType::Gemini) == gemini,
                           "Activating Codex unexpectedly cleared Gemini")
                || !expect(manager.activeIndex(ProfileType::OpenCode) == openCode,
                           "Activating Codex unexpectedly cleared OpenCode")) {
            return 1;
        }

        manager.removeProfile(claude);
        if (!expect(manager.activeIndex(ProfileType::Claude) == -1,
                    "Removing active Claude profile did not clear Claude state")
                || !expect(manager.activeIndex(ProfileType::Codex) == secondCodex - 1,
                           "Codex active index was not shifted after removal")
                || !expect(manager.activeIndex(ProfileType::Gemini) == gemini - 1,
                           "Gemini active index was not shifted after removal")
                || !expect(manager.activeIndex(ProfileType::OpenCode) == openCode - 1,
                           "OpenCode active index was not shifted after removal")) {
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
                               QStringLiteral("profiles/schema_version")).toInt() == 7,
                           "Profile schema was not upgraded to version 7")) {
            return 1;
        }
    }

    {
        QSettings().clear();
        ProfileManager manager;
        const ProfileWebsiteBinding website{
            QStringLiteral("website-account-session:sha256:") + QString(64, QLatin1Char('a')),
            QStringLiteral("website-key:sha256:") + QString(64, QLatin1Char('b')),
            QString(64, QLatin1Char('c')),
        };
        const int websiteProfile = manager.addProfile(
            QStringLiteral("Website Codex"), ProfileType::Codex,
            QString(), QStringLiteral("gpt-test"), website);
        const QList<Profile> profiles = manager.allProfiles();
        if (!expect(websiteProfile >= 0, "website-bound profile was rejected")
                || !expect(profiles.at(websiteProfile).websiteAccountIdentity
                               == website.accountIdentity,
                           "website account identity was not persisted")
                || !expect(profiles.at(websiteProfile).websiteKeyIdentity
                               == website.keyIdentity,
                           "website Key identity was not persisted")
                || !expect(profiles.at(websiteProfile).websiteProjectionSha256
                               == website.projectionSha256,
                           "website projection identity was not persisted")) {
            return 1;
        }
        ProfileWebsiteBinding invalid = website;
        invalid.keyIdentity = QStringLiteral("raw-website-key-id");
        if (!expect(manager.addProfile(
                        QStringLiteral("Invalid"), ProfileType::Codex,
                        QString(), QString(), invalid) < 0,
                    "invalid website source binding was accepted")) {
            return 1;
        }
    }

    {
        QSettings settings;
        settings.clear();
        settings.setValue(QStringLiteral("profiles/schema_version"), 6);
        settings.setValue(QStringLiteral("profiles/count"), 1);
        settings.setValue(
            QStringLiteral("profiles/0/id"),
            QStringLiteral("11111111-1111-4111-8111-111111111111"));
        settings.setValue(QStringLiteral("profiles/0/name"), QStringLiteral("Lazy Codex"));
        settings.setValue(
            QStringLiteral("profiles/0/type"), static_cast<int>(ProfileType::Codex));
        settings.setValue(
            QStringLiteral("profiles/0/credential_ref"),
            QStringLiteral("profile/11111111-1111-4111-8111-111111111111/api-key"));
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

    if (SecureStorage::isAvailable()) {
        const QString unrelatedRef = QStringLiteral("unrelated/profile-test-sentinel");
        const QString sentinel = QStringLiteral("do-not-read-or-overwrite");
        if (!SecureStorage::saveEncrypted(unrelatedRef, sentinel)) return 1;

        QSettings settings;
        settings.clear();
        settings.setValue(QStringLiteral("profiles/schema_version"), 6);
        settings.setValue(QStringLiteral("profiles/count"), 2);
        settings.setValue(
            QStringLiteral("profiles/0/id"),
            QStringLiteral("22222222-2222-4222-8222-222222222222"));
        settings.setValue(QStringLiteral("profiles/0/name"), QStringLiteral("Tampered"));
        settings.setValue(
            QStringLiteral("profiles/0/type"), static_cast<int>(ProfileType::Codex));
        settings.setValue(QStringLiteral("profiles/0/credential_ref"), unrelatedRef);
        settings.setValue(QStringLiteral("profiles/0/has_credential"), true);
        settings.setValue(
            QStringLiteral("profiles/0/key_hint"),
            QStringLiteral("sk-secret-metadata-fragment"));
        settings.setValue(
            QStringLiteral("profiles/1/id"),
            QStringLiteral("33333333-3333-4333-8333-333333333333"));
        settings.setValue(QStringLiteral("profiles/1/name"), QStringLiteral("Safe"));
        settings.setValue(
            QStringLiteral("profiles/1/type"), static_cast<int>(ProfileType::Codex));
        settings.setValue(
            QStringLiteral("profiles/1/credential_ref"),
            QStringLiteral("profile/33333333-3333-4333-8333-333333333333/api-key"));
        settings.setValue(QStringLiteral("profiles/1/has_credential"), false);
        settings.sync();

        ProfileManager manager;
        const Profile tamperedMetadata = manager.allProfiles().at(0);
        const Profile tampered = manager.profileWithCredential(0);
        if (!expect(tamperedMetadata.keyHint.isEmpty(),
                    "credential-shaped QSettings hint reached profile metadata")
                || !expect(tampered.key.isEmpty() && !tampered.hasCredential,
                    "tampered credential ref was read")
                || !expect(!manager.updateProfile(
                               0, QStringLiteral("Changed"), ProfileType::Codex,
                               QStringLiteral("replacement"), QString()),
                           "tampered credential ref was overwritten")
                || !expect(SecureStorage::loadEncrypted(unrelatedRef) == sentinel,
                           "unrelated SecureStorage value changed during read/update")) {
            SecureStorage::remove(unrelatedRef);
            return 1;
        }
        manager.removeProfile(0);
        if (!expect(SecureStorage::loadEncrypted(unrelatedRef) == sentinel,
                    "unrelated SecureStorage value was deleted")) {
            SecureStorage::remove(unrelatedRef);
            return 1;
        }
        SecureStorage::remove(unrelatedRef);
    }

    // 凭据提示是域分离指纹，不持久化 Key 子串。
    if (!expect(ProfileManager::maskedKeyHint(QStringLiteral("sk-abc1234"))
                    == credentialFingerprint(QStringLiteral("sk-abc1234")),
                "maskedKeyHint should return the credential fingerprint")
            || !expect(ProfileManager::maskedKeyHint(QStringLiteral("ab"))
                           == credentialFingerprint(QStringLiteral("ab")),
                       "short credentials must not be persisted whole")
            || !expect(ProfileManager::maskedKeyHint(QString()).isEmpty(),
                       "maskedKeyHint should map empty to empty")
            || !expect(ProfileManager::maskedKeyHint(QStringLiteral("  key-WXYZ  "))
                           == credentialFingerprint(QStringLiteral("key-WXYZ")),
                       "credential fingerprint should normalize outer whitespace")) {
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

    {
        QSettings().clear();
        ProfileManager manager;
        const int original = 0;
        const QString originalId = manager.allProfiles().at(original).id;
        manager.setActiveIndex(original);

        const int discardedCandidate = manager.addProfile(
            QStringLiteral("Discarded replacement"), ProfileType::Codex);
        manager.removeProfile(discardedCandidate);
        if (!expect(manager.activeIndex(ProfileType::Codex) == original
                        && manager.allProfiles().at(original).id == originalId
                        && manager.lastActivatedIndex() == original,
                    "discarding a replacement changed the original active profile")) {
            return 1;
        }

        const int committedCandidate = manager.addProfile(
            QStringLiteral("Committed replacement"), ProfileType::Codex);
        const QString committedId = manager.allProfiles().at(committedCandidate).id;
        manager.setActiveIndex(committedCandidate);
        manager.removeProfile(original);
        if (!expect(manager.activeIndex(ProfileType::Codex) == 0
                        && manager.lastActivatedIndex() == 0
                        && manager.allProfiles().at(0).id == committedId,
                    "committing a replacement did not preserve shifted active state")) {
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
        if (!expect(profiles[plus].keyHint
                        == credentialFingerprint(QStringLiteral("sk-plus-PLUS")),
                    "addProfile should persist the Plus credential fingerprint")
                || !expect(profiles[pro].keyHint
                               == credentialFingerprint(QStringLiteral("sk-pro-PROX")),
                           "addProfile should persist the Pro credential fingerprint")
                || !expect(profiles[plus].keyHint != profiles[pro].keyHint,
                           "different keys should yield distinguishable hints")) {
            return 1;
        }
        for (const QString &settingKey : QSettings().allKeys()) {
            const QString value = QSettings().value(settingKey).toString();
            if (!expect(!value.contains(QStringLiteral("sk-plus-PLUS"))
                            && !value.contains(QStringLiteral("sk-pro-PROX"))
                            && !value.contains(QStringLiteral("PLUS"))
                            && !value.contains(QStringLiteral("PROX")),
                        "profile metadata persisted a credential substring")) {
                return 1;
            }
        }

        // 清掉掩码模拟存量配置，验证 backfillKeyHints 会补齐。
        QSettings().remove(QStringLiteral("profiles/%1/key_hint").arg(plus));
        QSettings().sync();
        if (!expect(manager.allProfiles()[plus].keyHint.isEmpty(),
                    "key hint should be cleared before backfill test")) {
            return 1;
        }
        manager.backfillKeyHints();
        if (!expect(manager.allProfiles()[plus].keyHint
                        == credentialFingerprint(QStringLiteral("sk-plus-PLUS")),
                    "backfillKeyHints should restore a missing fingerprint")) {
            return 1;
        }
    }

    return 0;
}
