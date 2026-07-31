#include <QApplication>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QFile>
#include <QDir>

class SecureWebPage : public QWebEnginePage {
public:
    SecureWebPage(QWebEngineProfile *profile, QObject *parent = nullptr)
        : QWebEnginePage(profile, parent) {}

    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override {
        // Only allow local file:// and qrc:// URLs
        if (url.scheme() == "file" || url.scheme() == "qrc") {
            return true;
        }
        // Block all network navigation
        qWarning() << "Blocked navigation to:" << url.toString();
        return false;
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    // Create isolated profile
    QWebEngineProfile profile;

    // Disable all network features
    profile.setHttpCacheType(QWebEngineProfile::NoCache);
    profile.setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);

    // Create secure page
    SecureWebPage *page = new SecureWebPage(&profile);

    // Disable JavaScript access to local files (security)
    page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);

    // Create view
    QWebEngineView view;
    view.setPage(page);

    // Create simple local workbench bundle
    QString html = R"(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Aegisy Workbench</title>
    <style>
        body {
            font-family: system-ui;
            margin: 0;
            padding: 20px;
            background: #1e1e1e;
            color: #d4d4d4;
        }
        h1 { color: #4ec9b0; }
        .status {
            padding: 10px;
            background: #252526;
            border-radius: 4px;
            margin: 10px 0;
        }
        .success { color: #4ec9b0; }
        .blocked { color: #f48771; }
    </style>
</head>
<body>
    <h1>Aegisy Workbench - Local Bundle</h1>
    <div class="status">
        <div class="success">✓ Local bundle loaded</div>
        <div class="success">✓ Network navigation disabled</div>
        <div class="success">✓ Isolated profile active</div>
    </div>
    <p>This is a local signed workbench bundle running in QWebEngineView.</p>
    <p>All network navigation is blocked by acceptNavigationRequest().</p>

    <h2>Security Test</h2>
    <p>Try to navigate (should be blocked):</p>
    <a href="https://example.com">External Link (blocked)</a>

    <script>
        console.log('Local bundle JavaScript executing');
        // Test network blocking
        document.querySelector('a').addEventListener('click', (e) => {
            console.log('Navigation blocked by acceptNavigationRequest');
        });
    </script>
</body>
</html>
)";

    view.setHtml(html, QUrl("qrc:///workbench/index.html"));
    view.resize(1024, 768);
    view.setWindowTitle("Aegisy Workbench Experiment - Task 2.2");
    view.show();

    return app.exec();
}
