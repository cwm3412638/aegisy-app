#include "agent_workbench_window.h"
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QVBoxLayout>
#include <QLabel>

class SecureWorkbenchPage : public QWebEnginePage {
public:
    SecureWorkbenchPage(QWebEngineProfile *profile, QObject *parent = nullptr)
        : QWebEnginePage(profile, parent) {}

    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override {
        if (url.scheme() == "qrc" || url.isEmpty()) {
            return true;
        }
        qWarning() << "Blocked navigation to:" << url.toString();
        return false;
    }

protected:
    QWebEnginePage* createWindow(QWebEnginePage::WebWindowType) override {
        return nullptr;
    }
};

AgentWorkbenchWindow::AgentWorkbenchWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_profile(new QWebEngineProfile(this))
    , m_page(nullptr)
    , m_view(nullptr)
{
    setupUi();
    loadWorkbenchBundle();

    connect(m_page, &QWebEnginePage::renderProcessTerminated,
            this, [this](QWebEnginePage::RenderProcessTerminationStatus status, int exitCode) {
        Q_UNUSED(exitCode);
        if (status == QWebEnginePage::CrashedTerminationStatus ||
            status == QWebEnginePage::AbnormalTerminationStatus) {
            QString crashHtml = "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><style>"
                "body { font-family: system-ui; margin: 0; padding: 40px; background: #1e1e1e; color: #d4d4d4; text-align: center; }"
                "h1 { color: #f48771; }"
                "button { padding: 10px 20px; background: #0e639c; color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 14px; }"
                "button:hover { background: #1177bb; }"
                "</style></head><body>"
                "<h1>Renderer Process Crashed</h1>"
                "<p>The workbench renderer has stopped unexpectedly.</p>"
                "<button onclick=\"location.reload()\">Reload</button>"
                "</body></html>";
            m_page->setHtml(crashHtml);
        }
    });
}

AgentWorkbenchWindow::~AgentWorkbenchWindow() = default;

void AgentWorkbenchWindow::setupUi()
{
    setWindowTitle(tr("Aegisy Agent Workbench"));
    resize(1200, 800);

    m_profile->setHttpCacheType(QWebEngineProfile::NoCache);
    m_profile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);

    m_page = new SecureWorkbenchPage(m_profile, this);
    m_page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    m_page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);

    m_view = new QWebEngineView(this);
    m_view->setPage(m_page);

    setCentralWidget(m_view);
}

void AgentWorkbenchWindow::loadWorkbenchBundle()
{
    QString html = "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
        "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'\">"
        "<title>Aegisy Agent Workbench</title><style>"
        "* { margin: 0; padding: 0; box-sizing: border-box; }"
        "body { font-family: system-ui; display: flex; height: 100vh; background: #1e1e1e; color: #d4d4d4; overflow: hidden; }"
        ".rail { width: 48px; background: #252526; border-right: 1px solid #3e3e42; display: flex; flex-direction: column; flex-shrink: 0; }"
        ".rail-btn { width: 48px; height: 48px; border: none; background: transparent; color: #858585; cursor: pointer; display: flex; align-items: center; justify-content: center; font-size: 20px; }"
        ".rail-btn:hover { background: #2a2d2e; color: #d4d4d4; }"
        ".rail-btn.active { color: #ffffff; border-left: 2px solid #0e639c; }"
        ".panes { flex: 1; display: flex; min-width: 0; }"
        ".pane { background: #1e1e1e; border-right: 1px solid #3e3e42; overflow: auto; }"
        ".pane-left { width: 280px; min-width: 200px; flex-shrink: 0; }"
        ".pane-center { flex: 1; min-width: 400px; }"
        ".pane-right { width: 320px; min-width: 280px; flex-shrink: 0; }"
        ".pane-header { padding: 12px 16px; background: #252526; border-bottom: 1px solid #3e3e42; font-weight: 500; }"
        ".pane-content { padding: 16px; }"
        "h1 { color: #4ec9b0; margin-bottom: 16px; font-size: 20px; }"
        ".status { padding: 10px; background: #252526; border-radius: 4px; margin: 10px 0; }"
        ".success { color: #4ec9b0; font-size: 14px; }"
        "@media (max-width: 1024px) { .pane-right { display: none; } }"
        "</style></head><body>"
        "<div class=\"rail\">"
        "<button class=\"rail-btn active\" title=\"Chat\">💬</button>"
        "<button class=\"rail-btn\" title=\"Work\">🔧</button>"
        "<button class=\"rail-btn\" title=\"Projects\">📁</button>"
        "<button class=\"rail-btn\" title=\"Sessions\">📋</button>"
        "<button class=\"rail-btn\" title=\"Extensions\">🧩</button>"
        "<div style=\"flex: 1\"></div>"
        "<button class=\"rail-btn\" title=\"Settings\">⚙️</button>"
        "</div>"
        "<div class=\"panes\">"
        "<div class=\"pane pane-left\">"
        "<div class=\"pane-header\">Sessions</div>"
        "<div class=\"pane-content\">Session list placeholder</div>"
        "</div>"
        "<div class=\"pane pane-center\">"
        "<div class=\"pane-header\">Timeline</div>"
        "<div class=\"pane-content\">"
        "<h1>Aegisy Agent Workbench</h1>"
        "<div class=\"status\">"
        "<div class=\"success\">✓ Local bundle loaded</div>"
        "<div class=\"success\">✓ Network navigation blocked</div>"
        "<div class=\"success\">✓ CSP enforced</div>"
        "<div class=\"success\">✓ Product rail active</div>"
        "<div class=\"success\">✓ Three-pane layout</div>"
        "</div></div>"
        "</div>"
        "<div class=\"pane pane-right\">"
        "<div class=\"pane-header\">Context</div>"
        "<div class=\"pane-content\">Context panel placeholder</div>"
        "</div>"
        "</div></body></html>";

    m_page->setHtml(html, QUrl("qrc:///workbench/index.html"));
}
