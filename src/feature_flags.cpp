#include "feature_flags.h"

QSettings& FeatureFlags::settings()
{
    static QSettings s;
    return s;
}

bool FeatureFlags::isAgentWorkbenchEnabled()
{
    return settings().value(QStringLiteral("features/agentWorkbench"), false).toBool();
}

void FeatureFlags::setAgentWorkbenchEnabled(bool enabled)
{
    settings().setValue(QStringLiteral("features/agentWorkbench"), enabled);
}

FeatureFlags::Channel FeatureFlags::getChannel()
{
#ifdef AEGISY_INTERNAL_BUILD
    return Channel::Internal;
#else
    return Channel::Stable;
#endif
}
