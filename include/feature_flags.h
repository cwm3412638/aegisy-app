#ifndef FEATURE_FLAGS_H
#define FEATURE_FLAGS_H

#include <QString>
#include <QSettings>

class FeatureFlags
{
public:
    enum class Channel {
        Internal,
        Preview,
        Beta,
        Stable
    };

    static bool isAgentWorkbenchEnabled();
    static void setAgentWorkbenchEnabled(bool enabled);

    static Channel getChannel();

private:
    static QSettings& settings();
};

#endif // FEATURE_FLAGS_H
