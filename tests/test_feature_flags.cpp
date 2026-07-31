#include <QtTest>
#include "feature_flags.h"

class TestFeatureFlags : public QObject
{
    Q_OBJECT

private slots:
    void testAgentWorkbenchDisabledByDefault()
    {
        QSettings().clear();
        QVERIFY(!FeatureFlags::isAgentWorkbenchEnabled());
    }

    void testAgentWorkbenchCanBeEnabled()
    {
        FeatureFlags::setAgentWorkbenchEnabled(true);
        QVERIFY(FeatureFlags::isAgentWorkbenchEnabled());

        FeatureFlags::setAgentWorkbenchEnabled(false);
        QVERIFY(!FeatureFlags::isAgentWorkbenchEnabled());
    }

    void testChannelDetection()
    {
        auto channel = FeatureFlags::getChannel();
#ifdef AEGISY_INTERNAL_BUILD
        QCOMPARE(channel, FeatureFlags::Channel::Internal);
#else
        QCOMPARE(channel, FeatureFlags::Channel::Stable);
#endif
    }
};

QTEST_MAIN(TestFeatureFlags)
#include "test_feature_flags.moc"
