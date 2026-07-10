#include "update_manager.h"

UpdateManager::UpdateManager(QObject *parent)
    : QObject(parent)
{
}

UpdateManager::~UpdateManager() = default;

bool UpdateManager::isSupported() const
{
    return false;
}

bool UpdateManager::automaticallyChecksForUpdates() const
{
    return false;
}

void UpdateManager::checkForUpdates()
{
}

void UpdateManager::setAutomaticallyChecksForUpdates(bool enabled)
{
    Q_UNUSED(enabled);
}
