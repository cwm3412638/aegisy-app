#include "update_manager.h"

#include <QApplication>
#include <QMetaObject>
#include <QSettings>

#include <winsparkle.h>

#include <string>

namespace {

constexpr int kUpdateIntervalSeconds = 24 * 60 * 60;

int __cdecl canShutdownForUpdate()
{
    return 1;
}

void __cdecl requestShutdownForUpdate()
{
    if (!qApp) {
        return;
    }
    QMetaObject::invokeMethod(qApp, []() {
        QApplication::quit();
    }, Qt::QueuedConnection);
}

} // namespace

UpdateManager::UpdateManager(QObject *parent)
    : QObject(parent)
{
    const std::wstring version = QApplication::applicationVersion().toStdWString();
    win_sparkle_set_lang("zh-CN");
    win_sparkle_set_appcast_url(AEGISY_WINDOWS_UPDATE_FEED_URL);
    if (!win_sparkle_set_eddsa_public_key(AEGISY_UPDATE_PUBLIC_KEY)) {
        return;
    }
    win_sparkle_set_app_details(
        L"Aegisy", L"Aegisy Client", version.c_str());
    win_sparkle_set_app_build_version(version.c_str());
    win_sparkle_set_update_check_interval(kUpdateIntervalSeconds);
    win_sparkle_set_can_shutdown_callback(canShutdownForUpdate);
    win_sparkle_set_shutdown_request_callback(requestShutdownForUpdate);

    QSettings settings;
    const QString configuredKey = QStringLiteral("updates/automaticChecksConfigured");
    const QString enabledKey = QStringLiteral("updates/automaticChecksEnabled");
    if (!settings.value(configuredKey, false).toBool()) {
        settings.setValue(configuredKey, true);
        settings.setValue(enabledKey, true);
    }
    win_sparkle_set_automatic_check_for_updates(
        settings.value(enabledKey, true).toBool() ? 1 : 0);
    win_sparkle_init();
    m_platformUpdater = this;
}

UpdateManager::~UpdateManager()
{
    if (m_platformUpdater) {
        win_sparkle_cleanup();
        m_platformUpdater = nullptr;
    }
}

bool UpdateManager::isSupported() const
{
    return m_platformUpdater != nullptr;
}

bool UpdateManager::automaticallyChecksForUpdates() const
{
    return isSupported() && win_sparkle_get_automatic_check_for_updates() != 0;
}

void UpdateManager::checkForUpdates()
{
    if (isSupported()) {
        win_sparkle_check_update_with_ui();
    }
}

void UpdateManager::setAutomaticallyChecksForUpdates(bool enabled)
{
    if (!isSupported()) {
        return;
    }
    win_sparkle_set_automatic_check_for_updates(enabled ? 1 : 0);
    QSettings().setValue(QStringLiteral("updates/automaticChecksEnabled"), enabled);
    emit automaticChecksChanged(enabled);
}
