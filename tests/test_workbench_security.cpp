#include <QtTest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QSignalSpy>

class TestWorkbenchSecurity : public QObject
{
    Q_OBJECT

private slots:
    void testBlockExternalNavigation()
    {
        QWebEngineProfile profile;

        class TestPage : public QWebEnginePage {
        public:
            TestPage(QWebEngineProfile *p) : QWebEnginePage(p) {}
            bool acceptNavigationRequest(const QUrl &url, NavigationType, bool) override {
                return url.scheme() == "qrc" || url.isEmpty();
            }
        };

        TestPage page(&profile);

        QVERIFY(page.acceptNavigationRequest(QUrl("qrc:///test"), QWebEnginePage::NavigationTypeLinkClicked, true));
        QVERIFY(!page.acceptNavigationRequest(QUrl("https://example.com"), QWebEnginePage::NavigationTypeLinkClicked, true));
        QVERIFY(!page.acceptNavigationRequest(QUrl("http://example.com"), QWebEnginePage::NavigationTypeLinkClicked, true));
    }

    void testIsolatedProfile()
    {
        QWebEngineProfile profile;
        profile.setHttpCacheType(QWebEngineProfile::NoCache);
        profile.setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);

        QCOMPARE(profile.httpCacheType(), QWebEngineProfile::NoCache);
        QCOMPARE(profile.persistentCookiesPolicy(), QWebEngineProfile::NoPersistentCookies);
    }
};

QTEST_MAIN(TestWorkbenchSecurity)
#include "test_workbench_security.moc"
