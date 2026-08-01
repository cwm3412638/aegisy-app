#include "agent_workbench_window.h"
#include "timeline_api.h"
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebChannel>
#include <QVBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMenu>
#include <QAction>

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
    , m_channel(new QWebChannel(this))
    , m_timelineAPI(new TimelineAPI(this))
{
    setupUi();
    setupMenuBar();
    setupWebChannel();
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

void AgentWorkbenchWindow::setupMenuBar()
{
    auto *viewMenu = menuBar()->addMenu(tr("&View"));

    auto *toggleLeftAction = viewMenu->addAction(tr("Toggle &Left Pane"));
    toggleLeftAction->setShortcut(QKeySequence(tr("Ctrl+B")));
    connect(toggleLeftAction, &QAction::triggered, this, [this]() { executeCommand("togglePane('leftPane')"); });

    auto *toggleRightAction = viewMenu->addAction(tr("Toggle &Right Pane"));
    toggleRightAction->setShortcut(QKeySequence(tr("Ctrl+Shift+B")));
    connect(toggleRightAction, &QAction::triggered, this, [this]() { executeCommand("togglePane('rightPane')"); });

    viewMenu->addSeparator();

    auto *resetAction = viewMenu->addAction(tr("&Reset Layout"));
    resetAction->setShortcut(QKeySequence(tr("Ctrl+Shift+R")));
    connect(resetAction, &QAction::triggered, this, [this]() { executeCommand("resetLayout()"); });

    auto *windowMenu = menuBar()->addMenu(tr("&Window"));

    auto *cmdPaletteAction = windowMenu->addAction(tr("Command &Palette"));
    cmdPaletteAction->setShortcut(QKeySequence(tr("Ctrl+K")));
    connect(cmdPaletteAction, &QAction::triggered, this, [this]() {
        executeCommand("document.getElementById('cmdPalette').classList.toggle('show');"
                      "document.getElementById('cmdInput').focus();");
    });
}

void AgentWorkbenchWindow::setupWebChannel()
{
    m_channel->registerObject("timelineAPI", m_timelineAPI);
    m_page->setWebChannel(m_channel);
}

void AgentWorkbenchWindow::executeCommand(const QString &cmd)
{
    m_page->runJavaScript(cmd);
}

void AgentWorkbenchWindow::loadWorkbenchBundle()
{
    QString html = "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
        "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline' qrc:\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<script src=\"qrc:///qtwebchannel/qwebchannel.js\"></script>"
        "<title>Aegisy Agent Workbench</title><style>"
        ":root { --bg-primary: #1e1e1e; --bg-secondary: #252526; --bg-tertiary: #2a2d2e; --border: #3e3e42; --text-primary: #d4d4d4; --text-secondary: #858585; --accent: #0e639c; --success: #4ec9b0; --warning: #f48771; --error: #f14c4c; }"
        "@media (prefers-color-scheme: light) { :root { --bg-primary: #ffffff; --bg-secondary: #f3f3f3; --bg-tertiary: #e8e8e8; --border: #d4d4d4; --text-primary: #1e1e1e; --text-secondary: #616161; } }"
        "@media (prefers-contrast: high) { :root { --border: #000000; } }"
        "@media (prefers-reduced-motion: reduce) { *, *::before, *::after { animation-duration: 0.01ms !important; animation-iteration-count: 1 !important; transition-duration: 0.01ms !important; } }"
        "* { margin: 0; padding: 0; box-sizing: border-box; }"
        "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', system-ui, sans-serif; display: flex; height: 100vh; background: var(--bg-primary); color: var(--text-primary); overflow: hidden; }"
        ".rail { width: 48px; background: var(--bg-secondary); border-right: 1px solid var(--border); display: flex; flex-direction: column; flex-shrink: 0; }"
        ".rail-btn { width: 48px; height: 48px; border: none; background: transparent; color: var(--text-secondary); cursor: pointer; display: flex; align-items: center; justify-content: center; font-size: 20px; }"
        ".rail-btn:hover { background: var(--bg-tertiary); color: var(--text-primary); }"
        ".rail-btn.active { color: #ffffff; border-left: 2px solid var(--accent); }"
        ".rail-btn:focus-visible { outline: 2px solid var(--accent); outline-offset: -2px; }"
        ".panes { flex: 1; display: flex; min-width: 0; }"
        ".pane { background: var(--bg-primary); border-right: 1px solid var(--border); overflow: auto; }"
        ".pane.hidden { display: none; }"
        ".pane-left { width: 280px; min-width: 200px; flex-shrink: 0; }"
        ".pane-center { flex: 1; min-width: 400px; }"
        ".pane-right { width: 320px; min-width: 280px; flex-shrink: 0; }"
        ".pane-header { padding: 12px 16px; background: var(--bg-secondary); border-bottom: 1px solid var(--border); font-weight: 500; display: flex; justify-content: space-between; align-items: center; }"
        ".pane-content { padding: 16px; }"
        ".pane-toggle { background: transparent; border: none; color: var(--text-secondary); cursor: pointer; padding: 4px; font-size: 16px; }"
        ".pane-toggle:hover { color: var(--text-primary); }"
        ".pane-toggle:focus-visible { outline: 2px solid var(--accent); }"
        ".session-item { padding: 8px 12px; margin: 4px 0; background: var(--bg-secondary); border-radius: 4px; cursor: pointer; display: flex; align-items: center; gap: 8px; }"
        ".session-item:hover { background: var(--bg-tertiary); }"
        ".session-item:focus-visible { outline: 2px solid var(--accent); outline-offset: -2px; }"
        ".badge { width: 8px; height: 8px; border-radius: 50%; flex-shrink: 0; }"
        ".badge-running { background: var(--success); animation: pulse 2s infinite; }"
        ".badge-approval { background: var(--warning); }"
        ".badge-failed { background: var(--error); }"
        ".badge-interrupted { background: #ce9178; }"
        ".badge-background { background: var(--text-secondary); }"
        "@keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.5; } }"
        ".resizer { width: 4px; background: var(--border); cursor: col-resize; flex-shrink: 0; }"
        ".resizer:hover { background: var(--accent); }"
        "h1 { color: var(--success); margin-bottom: 16px; font-size: 20px; }"
        ".status { padding: 10px; background: var(--bg-secondary); border-radius: 4px; margin: 10px 0; }"
        ".success { color: var(--success); font-size: 14px; }"
        ".cmd-palette { position: fixed; top: 50%; left: 50%; transform: translate(-50%, -50%); background: var(--bg-secondary); border: 1px solid var(--accent); border-radius: 4px; padding: 8px; min-width: 500px; display: none; z-index: 1000; }"
        ".cmd-palette.show { display: block; }"
        ".cmd-input { width: 100%; background: var(--bg-primary); border: none; color: var(--text-primary); padding: 8px; font-size: 14px; outline: none; }"
        ".cmd-input:focus-visible { outline: 2px solid var(--accent); outline-offset: -2px; }"
        ".cmd-item { padding: 8px; cursor: pointer; }"
        ".cmd-item:hover { background: var(--bg-tertiary); }"
        ".cmd-item:focus-visible { outline: 2px solid var(--accent); outline-offset: -2px; }"
        ".sr-only { position: absolute; width: 1px; height: 1px; padding: 0; margin: -1px; overflow: hidden; clip: rect(0,0,0,0); white-space: nowrap; border: 0; }"
        ".timeline { display: flex; flex-direction: column; gap: 12px; }"
        ".timeline-item { display: flex; gap: 12px; padding: 12px; background: var(--bg-secondary); border-radius: 4px; }"
        ".timeline-avatar { width: 32px; height: 32px; display: flex; align-items: center; justify-content: center; font-size: 20px; flex-shrink: 0; }"
        ".timeline-content { flex: 1; min-width: 0; }"
        ".timeline-content strong { display: block; margin-bottom: 4px; color: var(--text-primary); }"
        ".timeline-content p { margin: 0; color: var(--text-primary); }"
        ".timeline-content code { background: var(--bg-primary); padding: 2px 6px; border-radius: 3px; font-family: 'SF Mono', Monaco, monospace; font-size: 13px; }"
        ".timeline-user { border-left: 3px solid #569cd6; }"
        ".timeline-agent { border-left: 3px solid var(--success); }"
        ".timeline-command { border-left: 3px solid #ce9178; }"
        ".timeline-usage { border-left: 3px solid var(--text-secondary); }"
        ".timeline-error { border-left: 3px solid var(--error); }"
        ".timeline-approval { border-left: 3px solid var(--warning); }"
        ".timeline-item.streaming { opacity: 0.8; }"
        ".timeline-item.complete { opacity: 1; }"
        ".timeline-status { display: inline-flex; align-items: center; gap: 4px; font-size: 12px; color: var(--text-secondary); margin-top: 4px; }"
        ".timeline-status .badge { width: 6px; height: 6px; }"
        ".composer { position: sticky; bottom: 0; background: var(--bg-secondary); border-top: 1px solid var(--border); padding: 12px; }"
        ".composer-header { display: flex; align-items: center; gap: 8px; margin-bottom: 8px; font-size: 12px; color: var(--text-secondary); }"
        ".composer-badge { padding: 2px 6px; background: var(--bg-tertiary); border-radius: 3px; font-size: 11px; }"
        ".composer-input { width: 100%; min-height: 80px; background: var(--bg-primary); border: 1px solid var(--border); border-radius: 4px; padding: 8px; color: var(--text-primary); font-family: inherit; font-size: 14px; resize: vertical; }"
        ".composer-input:focus { outline: 2px solid var(--accent); outline-offset: -2px; border-color: var(--accent); }"
        ".composer-actions { display: flex; justify-content: space-between; align-items: center; margin-top: 8px; }"
        ".composer-btn { padding: 6px 12px; background: var(--accent); color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 13px; }"
        ".composer-btn:hover { background: #1177bb; }"
        ".composer-btn:disabled { opacity: 0.5; cursor: not-allowed; }"
        ".composer-info { font-size: 11px; color: var(--text-secondary); }"
        ".approval-card { background: var(--bg-tertiary); border: 1px solid var(--warning); border-radius: 4px; padding: 12px; margin-top: 8px; }"
        ".approval-header { display: flex; align-items: center; gap: 8px; margin-bottom: 8px; font-weight: 500; color: var(--warning); }"
        ".approval-command { background: var(--bg-primary); padding: 8px; border-radius: 3px; font-family: 'SF Mono', Monaco, monospace; font-size: 13px; margin: 8px 0; }"
        ".approval-risk { display: inline-block; padding: 2px 6px; border-radius: 3px; font-size: 11px; margin-right: 4px; }"
        ".approval-risk.high { background: var(--error); color: white; }"
        ".approval-risk.medium { background: var(--warning); color: white; }"
        ".approval-risk.low { background: var(--success); color: white; }"
        ".approval-actions { display: flex; gap: 8px; margin-top: 8px; }"
        ".approval-btn { padding: 6px 12px; border: none; border-radius: 4px; cursor: pointer; font-size: 13px; }"
        ".approval-btn.approve { background: var(--success); color: white; }"
        ".approval-btn.deny { background: var(--error); color: white; }"
        ".approval-btn:hover { opacity: 0.9; }"
        ".question-card { background: var(--bg-tertiary); border: 1px solid var(--accent); border-radius: 4px; padding: 12px; margin-top: 8px; }"
        ".question-header { font-weight: 500; margin-bottom: 8px; color: var(--accent); }"
        ".question-options { display: flex; flex-direction: column; gap: 6px; margin: 8px 0; }"
        ".question-option { padding: 8px 12px; background: var(--bg-primary); border: 1px solid var(--border); border-radius: 4px; cursor: pointer; }"
        ".question-option:hover { background: var(--bg-secondary); border-color: var(--accent); }"
        ".question-option.selected { background: var(--accent); color: white; border-color: var(--accent); }"
        ".question-actions { display: flex; gap: 8px; margin-top: 8px; }"
        ".attachment { display: inline-flex; align-items: center; gap: 6px; padding: 4px 8px; background: var(--bg-secondary); border: 1px solid var(--border); border-radius: 4px; font-size: 12px; margin: 4px 4px 0 0; cursor: pointer; }"
        ".attachment:hover { background: var(--bg-tertiary); border-color: var(--accent); }"
        ".attachment-icon { font-size: 14px; }"
        ".attachment-name { max-width: 150px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }"
        ".attachment-size { color: var(--text-secondary); font-size: 11px; }"
        ".attachment-remove { color: var(--text-secondary); cursor: pointer; padding: 0 2px; }"
        ".attachment-remove:hover { color: var(--error); }"
        ".empty-state { display: flex; flex-direction: column; align-items: center; justify-content: center; height: 100%; text-align: center; padding: 40px; }"
        ".empty-state-icon { font-size: 64px; margin-bottom: 16px; opacity: 0.5; }"
        ".empty-state-title { font-size: 20px; font-weight: 500; margin-bottom: 8px; color: var(--text-primary); }"
        ".empty-state-text { color: var(--text-secondary); margin-bottom: 24px; max-width: 400px; }"
        ".empty-state-btn { padding: 10px 20px; background: var(--accent); color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 14px; }"
        ".empty-state-btn:hover { background: #1177bb; }"
        "@media (max-width: 1024px) { .pane-right { display: none; } }"
        "</style></head><body role=\"application\" aria-label=\"Aegisy Agent Workbench\">"
        "<nav class=\"rail\" role=\"navigation\" aria-label=\"Main navigation\">"
        "<button class=\"rail-btn active\" title=\"Chat\" aria-label=\"Chat\" aria-pressed=\"true\">💬</button>"
        "<button class=\"rail-btn\" title=\"Work\" aria-label=\"Work\" aria-pressed=\"false\">🔧</button>"
        "<button class=\"rail-btn\" title=\"Projects\" aria-label=\"Projects\" aria-pressed=\"false\">📁</button>"
        "<button class=\"rail-btn\" title=\"Sessions\" aria-label=\"Sessions\" aria-pressed=\"false\">📋</button>"
        "<button class=\"rail-btn\" title=\"Extensions\" aria-label=\"Extensions\" aria-pressed=\"false\">🧩</button>"
        "<div style=\"flex: 1\"></div>"
        "<button class=\"rail-btn\" title=\"Settings\" aria-label=\"Settings\" aria-pressed=\"false\">⚙️</button>"
        "</nav>"
        "<div class=\"panes\">"
        "<aside class=\"pane pane-left\" id=\"leftPane\" role=\"complementary\" aria-label=\"Sessions panel\">"
        "<div class=\"pane-header\">Sessions<button class=\"pane-toggle\" onclick=\"togglePane('leftPane')\" aria-label=\"Close sessions panel\">✕</button></div>"
        "<div class=\"pane-content\" role=\"list\">"
        "<div class=\"session-item\" role=\"listitem\" tabindex=\"0\"><span class=\"badge badge-running\" aria-label=\"Running\"></span>Chat Session</div>"
        "<div class=\"session-item\" role=\"listitem\" tabindex=\"0\"><span class=\"badge badge-approval\" aria-label=\"Approval needed\"></span>Work: Refactor API</div>"
        "<div class=\"session-item\" role=\"listitem\" tabindex=\"0\"><span class=\"badge badge-failed\" aria-label=\"Failed\"></span>Deploy to staging</div>"
        "<div class=\"session-item\" role=\"listitem\" tabindex=\"0\"><span class=\"badge badge-interrupted\" aria-label=\"Interrupted\"></span>Code review</div>"
        "<div class=\"session-item\" role=\"listitem\" tabindex=\"0\"><span class=\"badge badge-background\" aria-label=\"Background\"></span>Background task</div>"
        "</div>"
        "</aside>"
        "<div class=\"resizer\" id=\"leftResizer\" role=\"separator\" aria-label=\"Resize sessions panel\"></div>"
        "<main class=\"pane pane-center\" role=\"main\" aria-label=\"Timeline\">"
        "<div class=\"pane-header\">Timeline</div>"
        "<div class=\"pane-content\">"
        "<div class=\"timeline\">"
        "<div class=\"timeline-item timeline-user complete\"><div class=\"timeline-avatar\">👤</div><div class=\"timeline-content\"><strong>User</strong><p>Help me refactor the authentication module</p></div></div>"
        "<div class=\"timeline-item timeline-agent streaming\"><div class=\"timeline-avatar\">🤖</div><div class=\"timeline-content\"><strong>Agent</strong><p>I'll help you refactor the authentication module. Let me analyze the current implementation...</p><div class=\"timeline-status\"><span class=\"badge badge-running\"></span>Streaming</div></div></div>"
        "<div class=\"timeline-item timeline-command complete\"><div class=\"timeline-avatar\">⚡</div><div class=\"timeline-content\"><strong>Command</strong><code>grep -r \"auth\" src/</code><div class=\"timeline-status\">Exit code: 0</div></div></div>"
        "<div class=\"timeline-item timeline-error complete\"><div class=\"timeline-avatar\">❌</div><div class=\"timeline-content\"><strong>Error</strong><p>Failed to connect to database</p><div class=\"timeline-status\">Retryable</div></div></div>"
        "<div class=\"timeline-item timeline-approval complete\"><div class=\"timeline-avatar\">✋</div><div class=\"timeline-content\"><strong>Approval Required</strong><p>Execute: <code>rm -rf dist/</code></p><div class=\"timeline-status\">Pending user decision</div>"
        "<div class=\"approval-card\">"
        "<div class=\"approval-header\">⚠️ High Risk Command</div>"
        "<div class=\"approval-command\">rm -rf dist/</div>"
        "<p><strong>Scope:</strong> Delete directory recursively</p>"
        "<p><strong>Risk:</strong> <span class=\"approval-risk high\">HIGH</span> Irreversible deletion</p>"
        "<p><strong>Reason:</strong> Clean build artifacts before deployment</p>"
        "<div class=\"approval-actions\">"
        "<button class=\"approval-btn approve\">Approve</button>"
        "<button class=\"approval-btn deny\">Deny</button>"
        "</div>"
        "</div>"
        "</div></div>"
        "<div class=\"timeline-item timeline-agent complete\"><div class=\"timeline-avatar\">🤖</div><div class=\"timeline-content\"><strong>Agent</strong><p>I need to know your preference for the authentication approach.</p>"
        "<div class=\"question-card\">"
        "<div class=\"question-header\">Which authentication method should we use?</div>"
        "<div class=\"question-options\">"
        "<div class=\"question-option\" role=\"button\" tabindex=\"0\">JWT tokens with refresh mechanism</div>"
        "<div class=\"question-option\" role=\"button\" tabindex=\"0\">Session-based authentication</div>"
        "<div class=\"question-option\" role=\"button\" tabindex=\"0\">OAuth 2.0 integration</div>"
        "</div>"
        "<div class=\"question-actions\">"
        "<button class=\"approval-btn approve\">Submit</button>"
        "<button class=\"approval-btn deny\">Cancel</button>"
        "</div>"
        "</div>"
        "</div></div>"
        "<div class=\"timeline-item timeline-usage complete\"><div class=\"timeline-avatar\">📊</div><div class=\"timeline-content\"><strong>Usage</strong><p>Tokens: 1,234 | Context: 8,192</p></div></div>"
        "</div>"
        "<div class=\"composer\">"
        "<div class=\"composer-header\">"
        "<span class=\"composer-badge\">Work Mode</span>"
        "<span class=\"composer-badge\">Project: aegisy-app</span>"
        "<span class=\"composer-badge\">Model: Claude Opus 5</span>"
        "<span class=\"composer-badge\">Read Only</span>"
        "</div>"
        "<div style=\"display: flex; flex-wrap: wrap;\">"
        "<div class=\"attachment\"><span class=\"attachment-icon\">📄</span><span class=\"attachment-name\">auth.ts</span><span class=\"attachment-size\">2.4 KB</span><span class=\"attachment-remove\">✕</span></div>"
        "<div class=\"attachment\"><span class=\"attachment-icon\">🖼️</span><span class=\"attachment-name\">screenshot.png</span><span class=\"attachment-size\">156 KB</span><span class=\"attachment-remove\">✕</span></div>"
        "<div class=\"attachment\"><span class=\"attachment-icon\">⚠️</span><span class=\"attachment-name\">3 diagnostics</span><span class=\"attachment-remove\">✕</span></div>"
        "</div>"
        "<textarea class=\"composer-input\" placeholder=\"Ask a question or describe a task...\" aria-label=\"Message input\"></textarea>"
        "<div class=\"composer-actions\">"
        "<button class=\"composer-btn\" aria-label=\"Send message\">Send</button>"
        "<span class=\"composer-info\">Cmd+Enter to send</span>"
        "</div>"
        "</div>"
        "<h1>Aegisy Agent Workbench</h1>"
        "<div class=\"status\">"
        "<div class=\"success\">✓ Local bundle loaded</div>"
        "<div class=\"success\">✓ Network navigation blocked</div>"
        "<div class=\"success\">✓ CSP enforced</div>"
        "<div class=\"success\">✓ Product rail active</div>"
        "<div class=\"success\">✓ Three-pane layout</div>"
        "<div class=\"success\">✓ Pane resize & toggle</div>"
        "<div class=\"success\">✓ Command palette (Cmd+K)</div>"
        "<div class=\"success\">✓ Native menu & shortcuts</div>"
        "<div class=\"success\">✓ Live-state badges</div>"
        "<div class=\"success\">✓ Theme & accessibility</div>"
        "</div></div>"
        "</main>"
        "<div class=\"resizer\" id=\"rightResizer\" role=\"separator\" aria-label=\"Resize context panel\"></div>"
        "<aside class=\"pane pane-right\" id=\"rightPane\" role=\"complementary\" aria-label=\"Context panel\">"
        "<div class=\"pane-header\">Context<button class=\"pane-toggle\" onclick=\"togglePane('rightPane')\" aria-label=\"Close context panel\">✕</button></div>"
        "<div class=\"pane-content\">"
        "<h3 style=\"margin-bottom: 12px; font-size: 14px;\">Selected Files (3)</h3>"
        "<div style=\"display: flex; flex-direction: column; gap: 6px; margin-bottom: 16px;\">"
        "<div class=\"attachment\"><span class=\"attachment-icon\">📄</span><span class=\"attachment-name\">auth.ts</span><span class=\"attachment-size\">2.4 KB</span></div>"
        "<div class=\"attachment\"><span class=\"attachment-icon\">📄</span><span class=\"attachment-name\">login.tsx</span><span class=\"attachment-size\">3.1 KB</span></div>"
        "<div class=\"attachment\"><span class=\"attachment-icon\">📄</span><span class=\"attachment-name\">types.ts</span><span class=\"attachment-size\">1.8 KB</span></div>"
        "</div>"
        "<h3 style=\"margin-bottom: 12px; font-size: 14px;\">Token Budget</h3>"
        "<div style=\"background: var(--bg-secondary); padding: 12px; border-radius: 4px;\">"
        "<div style=\"display: flex; justify-content: space-between; margin-bottom: 4px; font-size: 13px;\">"
        "<span>Used</span><span>8,192 / 200,000</span>"
        "</div>"
        "<div style=\"height: 4px; background: var(--bg-primary); border-radius: 2px; overflow: hidden;\">"
        "<div style=\"width: 4%; height: 100%; background: var(--success);\"></div>"
        "</div>"
        "</div>"
        "</div>"
        "</aside>"
        "</div>"
        "<div class=\"cmd-palette\" id=\"cmdPalette\" role=\"dialog\" aria-label=\"Command palette\">"
        "<input class=\"cmd-input\" placeholder=\"Type a command...\" id=\"cmdInput\" aria-label=\"Command input\">"
        "<div class=\"cmd-item\" onclick=\"togglePane('leftPane')\" role=\"button\" tabindex=\"0\">Toggle Left Pane</div>"
        "<div class=\"cmd-item\" onclick=\"togglePane('rightPane')\" role=\"button\" tabindex=\"0\">Toggle Right Pane</div>"
        "<div class=\"cmd-item\" onclick=\"resetLayout()\" role=\"button\" tabindex=\"0\">Reset Layout</div>"
        "</div>"
        "<script>"
        "function togglePane(id){const p=document.getElementById(id);p.classList.toggle('hidden');saveLayout();}"
        "function resetLayout(){document.getElementById('leftPane').style.width='280px';document.getElementById('rightPane').style.width='320px';"
        "document.getElementById('leftPane').classList.remove('hidden');document.getElementById('rightPane').classList.remove('hidden');localStorage.removeItem('wbLayout');}"
        "function saveLayout(){const l={left:document.getElementById('leftPane').style.width||'280px',right:document.getElementById('rightPane').style.width||'320px',"
        "leftHidden:document.getElementById('leftPane').classList.contains('hidden'),rightHidden:document.getElementById('rightPane').classList.contains('hidden')};"
        "localStorage.setItem('wbLayout',JSON.stringify(l));}"
        "function loadLayout(){try{const l=JSON.parse(localStorage.getItem('wbLayout'));if(l){document.getElementById('leftPane').style.width=l.left;"
        "document.getElementById('rightPane').style.width=l.right;if(l.leftHidden)document.getElementById('leftPane').classList.add('hidden');"
        "if(l.rightHidden)document.getElementById('rightPane').classList.add('hidden');}}catch(e){}}"
        "let resizing=null;document.getElementById('leftResizer').addEventListener('mousedown',()=>resizing='left');"
        "document.getElementById('rightResizer').addEventListener('mousedown',()=>resizing='right');"
        "document.addEventListener('mousemove',e=>{if(!resizing)return;if(resizing==='left'){const w=Math.max(200,e.clientX-48);"
        "document.getElementById('leftPane').style.width=w+'px';}else{const w=Math.max(280,window.innerWidth-e.clientX);"
        "document.getElementById('rightPane').style.width=w+'px';}});"
        "document.addEventListener('mouseup',()=>{if(resizing){saveLayout();resizing=null;}});"
        "document.addEventListener('keydown',e=>{if((e.metaKey||e.ctrlKey)&&e.key==='k'){e.preventDefault();"
        "const p=document.getElementById('cmdPalette');p.classList.toggle('show');if(p.classList.contains('show'))document.getElementById('cmdInput').focus();}});"
        "document.getElementById('cmdPalette').addEventListener('click',e=>{if(e.target.id==='cmdPalette')e.target.classList.remove('show');});"
        "loadLayout();"
        "new QWebChannel(qt.webChannelTransport,ch=>{window.timelineAPI=ch.objects.timelineAPI;"
        "timelineAPI.itemAppended.connect(item=>appendTimelineItem(item));"
        "document.querySelector('.composer-btn').addEventListener('click',()=>{"
        "const input=document.querySelector('.composer-input');if(input.value.trim()){timelineAPI.sendMessage(input.value);input.value='';}});});"
        "function appendTimelineItem(item){const tl=document.querySelector('.timeline');"
        "const div=document.createElement('div');div.className='timeline-item timeline-'+item.type+' '+item.state;"
        "const avatars={'user':'👤','agent':'🤖','command':'⚡','usage':'📊','error':'❌','approval':'✋'};"
        "div.innerHTML='<div class=\"timeline-avatar\">'+avatars[item.type]+'</div><div class=\"timeline-content\"><strong>'+item.type.charAt(0).toUpperCase()+item.type.slice(1)+'</strong><p>'+item.content+'</p></div>';"
        "tl.appendChild(div);tl.scrollTop=tl.scrollHeight;}"
        "</script></body></html>";

    m_page->setHtml(html, QUrl("qrc:///workbench/index.html"));
}
