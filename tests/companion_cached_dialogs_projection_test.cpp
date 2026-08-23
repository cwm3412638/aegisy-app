#include "chat_dialog.h"
#include "companion_config_projection.h"
#include "connect_wizard.h"
#include "models_dialog.h"
#include "profile_manager.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QLineEdit>
#include <QNetworkReply>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>

#include <functional>
#include <iostream>

class CompanionCachedDialogsProjectionTestAccess
{
public:
    static void showConnectionPage(ConnectWizardDialog &dialog)
    {
        dialog.m_stack->setCurrentIndex(1);
        dialog.populateKeyDropdown();
        dialog.updateNavigation();
        dialog.updateSelectionControls();
    }

    static void query(ConnectWizardDialog &dialog) { dialog.onQueryModels(); }
    static void test(ConnectWizardDialog &dialog) { dialog.onTestConnection(); }
    static void save(ConnectWizardDialog &dialog) { dialog.finishProfile(); }
    static void deliverConfiguration(
        ConnectWizardDialog &dialog, const QJsonObject &projection)
    {
        dialog.onCompanionConfigurationReceived(projection);
    }
    static void failConfiguration(ConnectWizardDialog &dialog)
    {
        dialog.onCompanionConfigurationFailed(QStringLiteral("offline-fixture"));
    }
    static QString currentKey(const ConnectWizardDialog &dialog)
    {
        return dialog.currentKey();
    }
    static ProfileWebsiteBinding websiteBinding(const ConnectWizardDialog &dialog)
    {
        return dialog.currentWebsiteBinding();
    }

    static void refresh(ModelsDialog &dialog) { dialog.loadModels(); }
    static void copyModel(ModelsDialog &dialog) { dialog.onCopyModelClicked(); }

    static int messageCount(const ChatDialog &dialog)
    {
        if (dialog.m_currentSession < 0
                || dialog.m_currentSession >= dialog.m_sessions.size()) {
            return 0;
        }
        return dialog.m_sessions.at(dialog.m_currentSession).messages.size();
    }
    static void send(ChatDialog &dialog) { dialog.onSendClicked(); }
    static bool matchSkill(ChatDialog &dialog, const QString &text)
    {
        return dialog.startMatchedSkill(text);
    }
    static int imageCandidate(const ChatDialog &dialog)
    {
        return dialog.imageSkillCandidateIndex();
    }
    static void resend(ChatDialog &dialog, int index)
    {
        dialog.resendUserMessage(index);
    }
    static void regenerate(ChatDialog &dialog, int index)
    {
        dialog.regenerateAssistantMessage(index);
    }
};

namespace {

const QString kAccount = QStringLiteral("website-account-session:sha256:")
    + QString(64, QLatin1Char('a'));
const QString kKey = QStringLiteral("website-key:sha256:")
    + QString(64, QLatin1Char('b'));
const QString kOtherAccount = QStringLiteral("website-account-session:sha256:")
    + QString(64, QLatin1Char('f'));
const QString kObservation = QString(64, QLatin1Char('c'));

bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

CompanionConfigurationCachePresentation presentation(
    CompanionConfigurationCacheState state)
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    CompanionConfigurationCachePresentation value;
    value.state = state;
    value.accountIdentity = kAccount;
    value.provenance = QStringLiteral("aegisy-companion-cache-read-only/0.1");
    value.revision = 4;
    value.capturedAtMs = nowMs - 1000;
    value.validUntilMs = value.capturedAtMs
        + CompanionConfigurationCache::ConfigurationFreshMs;
    value.staleUntilMs = value.validUntilMs
        + CompanionConfigurationCache::ConfigurationStaleMs;
    value.sourceObservationSha256 = kObservation;
    value.contentSha256 = QString(64, QLatin1Char('d'));
    if (state == CompanionConfigurationCacheState::Fresh
            || state == CompanionConfigurationCacheState::Stale) {
        value.keys.append(CompanionCachedKeyPresentation{
            kKey, QStringLiteral("Primary <literal>"),
            QStringLiteral("Codex"), QStringLiteral("openai"),
            QStringLiteral("active"),
        });
    }
    if (state == CompanionConfigurationCacheState::Fresh) {
        value.models.append(CompanionCachedModelPresentation{
            kKey, QStringLiteral("openai"),
            {QStringLiteral("gpt-5"), QStringLiteral("gpt-5-mini")},
            nowMs - 500,
            nowMs - 500 + CompanionConfigurationCache::ModelFreshMs,
            kObservation, QString(64, QLatin1Char('e')),
        });
    }
    if (state != CompanionConfigurationCacheState::Fresh
            && state != CompanionConfigurationCacheState::Stale
            && state != CompanionConfigurationCacheState::Expired) {
        value = CompanionConfigurationCachePresentation{};
        value.state = state;
        value.accountIdentity = kAccount;
        value.provenance = QStringLiteral(
            "aegisy-companion-cache-state-only/0.1");
    }
    return value;
}

CompanionConfigurationCachePresentation nearExpiryPresentation()
{
    CompanionConfigurationCachePresentation value = presentation(
        CompanionConfigurationCacheState::Fresh);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    value.capturedAtMs = nowMs - 20;
    value.validUntilMs = nowMs + 80;
    value.staleUntilMs = nowMs + 160;
    value.models.first().capturedAtMs = nowMs - 10;
    value.models.first().validUntilMs = nowMs + 40;
    return value;
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 1500)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
    return condition();
}

QJsonObject liveConfiguration()
{
    const QJsonObject projected = CompanionConfigProjection::fromWebsiteApiKeys(
        QJsonArray{QJsonObject{
            {QStringLiteral("id"), QStringLiteral("live-key-id")},
            {QStringLiteral("name"), QStringLiteral("Live Primary")},
            {QStringLiteral("status"), QStringLiteral("active")},
            {QStringLiteral("group"), QJsonObject{
                {QStringLiteral("name"), QStringLiteral("Codex")},
                {QStringLiteral("platform"), QStringLiteral("openai")},
            }},
        }},
        kAccount, QStringLiteral("https://aegisy.cc"), 1800000000000LL);
    const QString identity = projected.value(QStringLiteral("keys")).toArray()
        .first().toObject().value(QStringLiteral("key_identity")).toString();
    return CompanionConfigProjection::withCredentialHandles(
        projected,
        QHash<QString, QString>{{
            identity,
            QStringLiteral("website-credential:sha256:")
                + QString(64, QLatin1Char('f')),
        }});
}

bool noLiveRoles(const QComboBox *combo, int index, int lastRole)
{
    if (!combo || index < 0 || index >= combo->count()) return false;
    for (int role = Qt::UserRole; role <= lastRole; ++role) {
        if (combo->itemData(index, role).isValid()) return false;
    }
    return true;
}

bool noNetworkReplies(const ApiClient &client)
{
    return client.findChildren<QNetworkReply *>().isEmpty();
}

bool connectWizardBoundary(ProfileManager *profiles)
{
    ApiClient client;
    const int beforeProfiles = profiles->count();
    ConnectWizardDialog dialog(
        &client, profiles, -1, kAccount,
        presentation(CompanionConfigurationCacheState::Fresh));
    dialog.show();
    QApplication::processEvents();
    auto *keys = dialog.findChild<QComboBox *>(
        QStringLiteral("connectWizardKeyCombo"));
    auto *models = dialog.findChild<QComboBox *>(
        QStringLiteral("connectWizardModelCombo"));
    auto *query = dialog.findChild<QPushButton *>(
        QStringLiteral("connectWizardQueryModelsButton"));
    auto *test = dialog.findChild<QPushButton *>(
        QStringLiteral("connectWizardTestButton"));
    auto *save = dialog.findChild<QPushButton *>(
        QStringLiteral("connectWizardSaveButton"));
    if (!require(keys && models && query && test && save && keys->count() == 2,
                 "ConnectWizard did not render one cached Key")) return false;
    keys->setCurrentIndex(1);
    CompanionCachedDialogsProjectionTestAccess::showConnectionPage(dialog);
    QApplication::processEvents();
    if (!require(noLiveRoles(keys, 1, Qt::UserRole + 6),
                 "ConnectWizard cached row populated live roles")
            || !require(models->count() == 2 && !models->isEnabled(),
                        "ConnectWizard did not show cached models read-only")
            || !require(!query->isEnabled() && !test->isEnabled()
                            && !save->isEnabled(),
                        "ConnectWizard enabled a cached operation")
            || !require(CompanionCachedDialogsProjectionTestAccess::currentKey(dialog)
                            .isEmpty()
                            && CompanionCachedDialogsProjectionTestAccess::websiteBinding(
                                   dialog).accountIdentity.isEmpty(),
                        "ConnectWizard derived authority from a cached row")) {
        return false;
    }
    CompanionCachedDialogsProjectionTestAccess::query(dialog);
    CompanionCachedDialogsProjectionTestAccess::test(dialog);
    CompanionCachedDialogsProjectionTestAccess::save(dialog);
    if (!require(noNetworkReplies(client) && profiles->count() == beforeProfiles,
                 "ConnectWizard cached entry point dispatched or saved")) {
        return false;
    }

    const QJsonObject live = liveConfiguration();
    if (!require(CompanionConfigProjection::validate(live),
                 "live configuration fixture is invalid")) return false;
    CompanionCachedDialogsProjectionTestAccess::deliverConfiguration(dialog, live);
    keys->setCurrentIndex(1);
    CompanionCachedDialogsProjectionTestAccess::showConnectionPage(dialog);
    if (!require(keys->count() == 2
                     && keys->itemData(1, Qt::UserRole).isValid()
                     && query->isEnabled() && test->isEnabled() && save->isEnabled(),
                 "ConnectWizard live configuration did not replace cached rows")) {
        return false;
    }
    CompanionCachedDialogsProjectionTestAccess::failConfiguration(dialog);
    keys->setCurrentIndex(1);
    CompanionCachedDialogsProjectionTestAccess::showConnectionPage(dialog);
    return require(noLiveRoles(keys, 1, Qt::UserRole + 6)
                       && !query->isEnabled() && !test->isEnabled()
                       && !save->isEnabled(),
                   "ConnectWizard failure did not restore read-only cache");
}

bool modelsBoundary()
{
    ApiClient client;
    ModelsDialog dialog(
        &client, kAccount,
        presentation(CompanionConfigurationCacheState::Fresh));
    dialog.show();
    QApplication::processEvents();
    auto *keys = dialog.findChild<QComboBox *>(QStringLiteral("modelsCacheKeyCombo"));
    auto *query = dialog.findChild<QPushButton *>(QStringLiteral("modelsQueryButton"));
    auto *copy = dialog.findChild<QPushButton *>(QStringLiteral("modelsCopyButton"));
    auto *search = dialog.findChild<QLineEdit *>(QStringLiteral("modelsSearchEdit"));
    auto *table = dialog.findChild<QTableWidget *>(QStringLiteral("modelsTable"));
    int modelSelections = 0;
    QObject::connect(&dialog, &ModelsDialog::modelSelected, &dialog,
                     [&](const QString &) { ++modelSelections; });
    if (!require(keys && query && copy && search && table
                     && keys->count() == 1 && table->rowCount() == 2,
                 "ModelsDialog did not render cached rows")
            || !require(noLiveRoles(keys, 0, Qt::UserRole + 5),
                        "ModelsDialog cached row populated live roles")
            || !require(!query->isEnabled()
                            && table->item(0, 2)->text().contains(
                                QStringLiteral("缓存")),
                        "ModelsDialog cache provenance or query gate is missing")) {
        return false;
    }
    table->selectRow(0);
    QApplication::processEvents();
    if (!require(copy->isEnabled(), "ModelsDialog disabled safe cached-model copy")) {
        return false;
    }
    CompanionCachedDialogsProjectionTestAccess::copyModel(dialog);
    if (!require(QApplication::clipboard()->text() == QStringLiteral("gpt-5")
                     && modelSelections == 0,
                 "ModelsDialog cached copy granted model-selection authority")) {
        return false;
    }
    search->setText(QStringLiteral("mini"));
    QApplication::processEvents();
    if (!require(table->rowCount() == 1,
                 "ModelsDialog cached-model search failed")) return false;
    CompanionCachedDialogsProjectionTestAccess::refresh(dialog);
    if (!require(noNetworkReplies(client),
                 "ModelsDialog cached refresh dispatched a request")) return false;

    ModelsDialog stale(
        &client, kAccount,
        presentation(CompanionConfigurationCacheState::Stale));
    auto *staleTable = stale.findChild<QTableWidget *>(QStringLiteral("modelsTable"));
    auto *staleKeys = stale.findChild<QComboBox *>(
        QStringLiteral("modelsCacheKeyCombo"));
    if (!require(staleKeys && staleKeys->count() == 1
                     && staleTable && staleTable->rowCount() == 0,
                 "ModelsDialog Stale state exposed models or lost safe Key metadata")) {
        return false;
    }
    ModelsDialog expired(
        &client, kAccount,
        presentation(CompanionConfigurationCacheState::Expired));
    auto *expiredKeys = expired.findChild<QComboBox *>(
        QStringLiteral("modelsCacheKeyCombo"));
    if (!require(expiredKeys && !expiredKeys->isEnabled()
                     && expiredKeys->count() == 1,
                 "ModelsDialog Expired state exposed cache rows")) return false;
    CompanionConfigurationCachePresentation wrongAccount = presentation(
        CompanionConfigurationCacheState::Fresh);
    wrongAccount.accountIdentity = kOtherAccount;
    ModelsDialog isolated(&client, kAccount, wrongAccount);
    auto *isolatedKeys = isolated.findChild<QComboBox *>(
        QStringLiteral("modelsCacheKeyCombo"));
    if (!require(isolatedKeys && !isolatedKeys->isEnabled()
                     && isolatedKeys->count() == 1,
                 "ModelsDialog exposed another account's cached rows")) return false;

    ApiClient timerClient;
    ModelsDialog aging(&timerClient, kAccount, nearExpiryPresentation());
    aging.show();
    auto *agingTable = aging.findChild<QTableWidget *>(QStringLiteral("modelsTable"));
    auto *agingKeys = aging.findChild<QComboBox *>(
        QStringLiteral("modelsCacheKeyCombo"));
    if (!require(agingTable && agingKeys && agingTable->rowCount() == 2,
                 "ModelsDialog aging fixture did not start Fresh")) return false;
    if (!require(waitUntil([&]() { return agingTable->rowCount() == 0; })
                     && agingKeys->isEnabled(),
                 "open ModelsDialog did not remove expired models")) return false;
    return require(waitUntil([&]() { return !agingKeys->isEnabled(); })
                       && agingKeys->count() == 1,
                   "open ModelsDialog did not remove Expired cache rows");
}

bool chatBoundary()
{
    ApiClient client;
    ChatDialog dialog(
        &client, nullptr, nullptr, nullptr, kAccount,
        presentation(CompanionConfigurationCacheState::Fresh));
    dialog.show();
    QApplication::processEvents();
    auto *keys = dialog.findChild<QComboBox *>(QStringLiteral("chatCacheKeyCombo"));
    auto *models = dialog.findChild<QComboBox *>(
        QStringLiteral("chatCacheModelCombo"));
    auto *send = dialog.findChild<QPushButton *>(QStringLiteral("chatSendButton"));
    auto *image = dialog.findChild<QPushButton *>(
        QStringLiteral("chatImageSkillButton"));
    auto *presentationButton = dialog.findChild<QPushButton *>(
        QStringLiteral("chatPresentationSkillButton"));
    auto *input = dialog.findChild<QPlainTextEdit *>(
        QStringLiteral("chatComposerInput"));
    if (!require(keys && models && send && image && presentationButton && input
                     && keys->count() == 1 && models->count() == 2,
                 "ChatDialog did not render cached summary")
            || !require(noLiveRoles(keys, 0, Qt::UserRole + 6)
                            && !models->currentData().isValid(),
                        "ChatDialog cached row populated live authority roles")
            || !require(!keys->isEnabled() && !models->isEnabled()
                            && !send->isEnabled() && !image->isEnabled()
                            && !presentationButton->isEnabled(),
                        "ChatDialog enabled cached operations")) {
        return false;
    }
    const int beforeMessages =
        CompanionCachedDialogsProjectionTestAccess::messageCount(dialog);
    input->setPlainText(QStringLiteral("cached request must stay local"));
    CompanionCachedDialogsProjectionTestAccess::send(dialog);
    CompanionCachedDialogsProjectionTestAccess::resend(dialog, 0);
    CompanionCachedDialogsProjectionTestAccess::regenerate(dialog, 1);
    if (!require(!CompanionCachedDialogsProjectionTestAccess::matchSkill(
                     dialog, QStringLiteral("/image no dispatch"))
                     && CompanionCachedDialogsProjectionTestAccess::imageCandidate(
                            dialog) < 0,
                 "ChatDialog cache entered a Skill path")) return false;
    return require(
        noNetworkReplies(client)
            && CompanionCachedDialogsProjectionTestAccess::messageCount(dialog)
                == beforeMessages,
        "ChatDialog cached entry point dispatched or changed history");
}

} // namespace

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settingsRoot;
    if (!require(settingsRoot.isValid(), "settings directory unavailable")) return 1;
    QCoreApplication::setOrganizationName(QStringLiteral("AegisyCacheDialogTest"));
    QCoreApplication::setApplicationName(QStringLiteral("AegisyCacheDialogTest"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(
        QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());
    QSettings().clear();
    ProfileManager profiles;
    return connectWizardBoundary(&profiles)
            && modelsBoundary()
            && chatBoundary() ? 0 : 1;
}
