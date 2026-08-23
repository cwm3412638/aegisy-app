#include <QtTest>
#include "agent_workbench_window.h"
#include "feature_flags.h"

class TestWorkbenchWindow : public QObject
{
    Q_OBJECT

private slots:
    void testWindowCreation()
    {
        AgentWorkbenchWindow window;
        QVERIFY(window.windowTitle() == "Aegisy Codex Programming");
        QVERIFY(window.width() == 1200);
        QVERIFY(window.height() == 800);
    }

    void testFeatureFlagIntegration()
    {
        QVERIFY(!FeatureFlags::isAgentWorkbenchEnabled());

        FeatureFlags::setAgentWorkbenchEnabled(true);
        QVERIFY(FeatureFlags::isAgentWorkbenchEnabled());

        FeatureFlags::setAgentWorkbenchEnabled(false);
        QVERIFY(!FeatureFlags::isAgentWorkbenchEnabled());
    }
};

QTEST_MAIN(TestWorkbenchWindow)
#include "test_workbench_window.moc"
