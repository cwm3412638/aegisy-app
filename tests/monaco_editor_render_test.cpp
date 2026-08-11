#include "agent_runtime_client.h"
#include "agent_workbench_widget.h"
#include "app_theme.h"
#include "qt_test_failure_sink.h"

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QClipboard>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QSettings>
#include <QSGRendererInterface>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QThread>
#include <QTreeWidget>
#include <QVariant>
#include <QWebEnginePage>
#include <QWebEngineView>

#include <memory>

namespace {

using FailureCode = aegisy::test::FailureCode;

FailureCode failureStage = FailureCode::MONACO_DATA_ROOT;

void setFailureStage(FailureCode code) noexcept
{
    failureStage = code;
}

bool expect(bool condition, const char *message,
            FailureCode code)
{
    if (!condition) {
        aegisy::test::reportFailure(code);
        aegisy::test::reportLocalDiagnostic(message);
    }
    return condition;
}

bool expect(bool condition, const char *message)
{
    return expect(condition, message, failureStage);
}

template <typename Predicate>
bool waitUntil(QApplication &application, Predicate predicate, int timeoutMs = 6000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        application.processEvents();
        QThread::msleep(10);
    }
    application.processEvents();
    return predicate();
}

struct JavaScriptResult {
    bool complete = false;
    QVariant value;
};

bool evaluate(QApplication &application, QWebEnginePage *page,
              const QString &script, QVariant *value = nullptr)
{
    auto result = std::make_shared<JavaScriptResult>();
    page->runJavaScript(script, [result](const QVariant &returned) {
        result->value = returned;
        result->complete = true;
    });
    const bool complete = waitUntil(application, [result]() { return result->complete; });
    if (complete && value) *value = result->value;
    return complete;
}

int nonWhitePixels(const QImage &image)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() > 32
                    && (color.red() < 238 || color.green() < 238 || color.blue() < 238)) {
                ++count;
            }
        }
    }
    return count;
}

int nonWhitePixels(const QImage &image, const QRect &region)
{
    int count = 0;
    const QRect clipped = region.intersected(image.rect());
    for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
        for (int x = clipped.left(); x <= clipped.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() > 32
                    && (color.red() < 238 || color.green() < 238 || color.blue() < 238)) {
                ++count;
            }
        }
    }
    return count;
}

#ifdef Q_OS_WIN
bool verifyWindowsWebEngineRenderer(QApplication &application, QWebEngineView *webView)
{
    if (!expect(QQuickWindow::graphicsApi() == QSGRendererInterface::Direct3D11,
                "Qt Quick did not select the requested D3D11 graphics API",
                FailureCode::MONACO_D3D11_PRESENTATION)) {
        return false;
    }

    QQuickWidget *quickWidget = webView ? webView->findChild<QQuickWidget *>() : nullptr;
    if (!expect(quickWidget != nullptr,
                "WebEngine did not expose its internal QQuickWidget renderer",
                FailureCode::MONACO_D3D11_PRESENTATION)) {
        return false;
    }

    QQuickWindow *quickWindow = quickWidget->quickWindow();
    if (!expect(quickWindow != nullptr,
                "WebEngine QQuickWidget did not expose a QQuickWindow",
                FailureCode::MONACO_D3D11_PRESENTATION)) {
        return false;
    }
    if (!expect(waitUntil(application, [quickWindow]() {
                    return quickWindow->isSceneGraphInitialized();
                }),
                "WebEngine QQuickWindow scene graph did not initialize",
                FailureCode::MONACO_D3D11_PRESENTATION)) {
        return false;
    }

    QSGRendererInterface *renderer = quickWindow->rendererInterface();
    return expect(renderer != nullptr
                      && renderer->graphicsApi() == QSGRendererInterface::Direct3D11,
                  "WebEngine scene graph did not initialize with D3D11",
                  FailureCode::MONACO_D3D11_PRESENTATION);
}
#endif

} // namespace

int main(int argc, char *argv[])
{
    if (aegisy::test::isFailureChannelSelfTest(argc, argv)) {
        return aegisy::test::runFailureChannelSelfTest();
    }
#ifdef Q_OS_WIN
    // Headless Windows runners cannot always launch sandboxed WebEngine
    // renderer processes; the render fixtures exercise the trusted local
    // bundle, not the Chromium sandbox itself.
    if (qEnvironmentVariableIsEmpty("QTWEBENGINE_DISABLE_SANDBOX")) {
        qputenv("QTWEBENGINE_DISABLE_SANDBOX", "1");
    }
    if (qEnvironmentVariableIsEmpty("QTWEBENGINE_CHROMIUM_FLAGS")) {
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
                "--no-sandbox --enable-logging=stderr");
    }
    if (qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND")) {
        qputenv("QSG_RHI_BACKEND", "d3d11");
    }
    if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND")) {
        qputenv("QT_QUICK_BACKEND", "rhi");
    }
    if (qEnvironmentVariableIsEmpty("QSG_RHI_PREFER_SOFTWARE_RENDERER")) {
        qputenv("QSG_RHI_PREFER_SOFTWARE_RENDERER", "1");
    }
    if (qEnvironmentVariableIsEmpty("QT_FORCE_STDERR_LOGGING")) {
        qputenv("QT_FORCE_STDERR_LOGGING", "1");
    }
#endif
    QApplication application(argc, argv);
    AppTheme::apply(application);

    QTemporaryDir workbenchData;
    if (!expect(workbenchData.isValid(), "cannot create isolated Workbench data root",
                FailureCode::MONACO_DATA_ROOT)) {
        return 1;
    }
    qputenv("AEGISY_WORKBENCH_DATA_ROOT", workbenchData.path().toUtf8());

    AgentWorkbenchWidget workbench;
    workbench.resize(1100, 700);
    workbench.show();

    QLabel *runtimeStatus = workbench.findChild<QLabel *>(QStringLiteral("agentRuntimeStatus"));
    AgentRuntimeClient *runtime = workbench.findChild<AgentRuntimeClient *>();
    QTreeWidget *fileTree = workbench.findChild<QTreeWidget *>(QStringLiteral("agentFileTree"));
    QWebEngineView *monaco = workbench.findChild<QWebEngineView *>(
        QStringLiteral("agentMonacoEditor"));
    QWebEngineView *xterm = workbench.findChild<QWebEngineView *>(
        QStringLiteral("agentXtermTerminal"));
    QTabWidget *workspaceTabs = workbench.findChild<QTabWidget *>(
        QStringLiteral("agentWorkspaceTabs"));
    QComboBox *terminalPicker = workbench.findChild<QComboBox *>(
        QStringLiteral("agentTerminalPicker"));
    QListWidget *sessionList = workbench.findChild<QListWidget *>(
        QStringLiteral("agentSessionList"));
    QAction *newForeground = workbench.findChild<QAction *>(
        QStringLiteral("agentTerminalNewForegroundAction"));
    QPushButton *terminalRemove = workbench.findChild<QPushButton *>(
        QStringLiteral("agentTerminalRemoveButton"));
    QPushButton *save = workbench.findChild<QPushButton *>(
        QStringLiteral("agentEditorSaveButton"));
    QPushButton *split = workbench.findChild<QPushButton *>(
        QStringLiteral("agentEditorSplitButton"));
    bool missing = false;
    auto requireControl = [&missing](const QObject *control, const char *name) {
        if (!control) {
            const QByteArray message = QByteArrayLiteral("missing workbench host control: ")
                + name;
            aegisy::test::reportFailure(
                aegisy::test::FailureCode::MONACO_HOST_CONTROL);
            aegisy::test::reportLocalDiagnostic(message.constData());
            missing = true;
        }
    };
    requireControl(runtimeStatus, "agentRuntimeStatus");
    requireControl(runtime, "AgentRuntimeClient");
    requireControl(fileTree, "agentFileTree");
    requireControl(monaco, "agentMonacoEditor");
    requireControl(xterm, "agentXtermTerminal");
    requireControl(workspaceTabs, "agentWorkspaceTabs");
    requireControl(terminalPicker, "agentTerminalPicker");
    requireControl(sessionList, "agentSessionList");
    requireControl(newForeground, "agentTerminalNewForegroundAction");
    requireControl(terminalRemove, "agentTerminalRemoveButton");
    requireControl(save, "agentEditorSaveButton");
    requireControl(split, "agentEditorSplitButton");
    if (!expect(!missing, "Web workbench host controls are missing",
                FailureCode::MONACO_HOST_CONTROL)) {
        return 1;
    }
    QVariant value;
    setFailureStage(FailureCode::MONACO_RUNTIME_READY);
    if (!expect(waitUntil(application, [runtimeStatus]() {
                    return runtimeStatus->text().startsWith(QStringLiteral("●"));
                }),
                "AAP runtime did not become ready")) {
        return 1;
    }

    setFailureStage(FailureCode::MONACO_WORKSPACE_FIXTURE);
    QTemporaryDir project;
    const QString sourcePath = project.filePath(QStringLiteral("main.cpp"));
    QFile source(sourcePath);
    if (!expect(project.isValid() && source.open(QIODevice::WriteOnly),
                "could not create Monaco source fixture")) {
        return 1;
    }
    source.write("int main() { return 0; }\n");
    source.close();
    const QString secondaryPath = project.filePath(QStringLiteral("secondary.cpp"));
    QFile secondary(secondaryPath);
    if (!expect(secondary.open(QIODevice::WriteOnly),
                "could not create secondary Monaco source fixture")) {
        return 1;
    }
    secondary.write("int secondary() { return 7; }\n");
    secondary.close();
    runtime->openProject(project.path());
    if (!expect(waitUntil(application, [fileTree]() {
                    return !fileTree->findItems(QStringLiteral("main.cpp"),
                                                Qt::MatchExactly | Qt::MatchRecursive).isEmpty()
                        && !fileTree->findItems(QStringLiteral("secondary.cpp"),
                                                Qt::MatchExactly | Qt::MatchRecursive).isEmpty();
                }),
                "workspace did not populate the Monaco source fixture")) {
        return 1;
    }
    setFailureStage(FailureCode::MONACO_TERMINAL_BRIDGE);
    int terminalTab = -1;
    for (int index = 0; index < workspaceTabs->count(); ++index) {
        if (workspaceTabs->tabText(index) == QStringLiteral("终端")) terminalTab = index;
    }
    workspaceTabs->setCurrentIndex(terminalTab);
    if (!expect(terminalTab >= 0
                    && waitUntil(application, [&application, xterm, &value]() {
                        return evaluate(application, xterm->page(), QStringLiteral(
                            "JSON.stringify({ready: window.aegisyTerminalTest?.isReady(), "
                            "dimensions: window.aegisyTerminalTest?.getDimensions()})"), &value)
                            && value.toString().contains(QStringLiteral("\"ready\":true"))
                            && !value.toString().contains(QStringLiteral("\"width\":0"))
                            && !value.toString().contains(QStringLiteral("\"height\":0"));
                    }),
                "xterm.js did not load with visible fitted dimensions")) {
        return 1;
    }
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    newForeground->trigger();
    if (!expect(waitUntil(application, [terminalPicker, sessionList]() {
                    return terminalPicker->count() == 1 && sessionList->count() > 0
                        && sessionList->item(0)->text().startsWith(
                            QStringLiteral("项目任务 · "));
                }, 5000),
                "xterm host did not create a terminal-bound Work session")) {
        return 1;
    }
    const QString sessionRow = sessionList->item(0)->text();
    const QString terminalSessionId = sessionRow.mid(
        sessionRow.lastIndexOf(QStringLiteral(" · ")) + 3);
    const QString terminalId = terminalPicker->currentData().toString();
#ifdef Q_OS_WIN
    const QByteArray terminalCommand("echo Aegisy xterm bridge\r\n");
#else
    const QByteArray terminalCommand("printf 'Aegisy xterm bridge\\n'\n");
#endif
    runtime->inputUserTerminal(terminalSessionId, terminalId, terminalCommand);
    if (!expect(waitUntil(application, [&application, xterm, &value]() {
                    return evaluate(application, xterm->page(), QStringLiteral(
                            "window.aegisyTerminalTest?.getText() || ''"), &value)
                        && value.toString().contains(QStringLiteral("Aegisy xterm bridge"));
                }, 5000),
                "PTY output did not cross WebChannel into xterm.js")) {
        return 1;
    }
    if (!expect(evaluate(application, xterm->page(), QStringLiteral(
                    "window.aegisyTerminalTest.selectAllAndCopy(); true"))
                    && waitUntil(application, []() {
                        return QApplication::clipboard()->text().contains(
                            QStringLiteral("Aegisy xterm bridge"));
                    }),
                "xterm selection did not copy through the native clipboard bridge")) {
        return 1;
    }
#ifdef Q_OS_WIN
    QApplication::clipboard()->setText(QStringLiteral("echo Aegisy clipboard paste\r\n"));
#else
    QApplication::clipboard()->setText(
        QStringLiteral("printf 'Aegisy clipboard paste\\n'\n"));
#endif
    if (!expect(evaluate(application, xterm->page(), QStringLiteral(
                    "window.aegisyTerminalTest.requestPaste(); true"))
                    && waitUntil(application, [&application, xterm, &value]() {
                        return evaluate(application, xterm->page(), QStringLiteral(
                            "window.aegisyTerminalTest.getText()"), &value)
                            && value.toString().contains(
                                QStringLiteral("Aegisy clipboard paste"));
                    }, 5000),
                "native clipboard paste did not reach the PTY through xterm")) {
        return 1;
    }
#ifdef Q_OS_WIN
    runtime->inputUserTerminal(terminalSessionId, terminalId, QByteArray("exit 0\r\n"));
#else
    runtime->inputUserTerminal(terminalSessionId, terminalId, QByteArray("exit 0\n"));
#endif
    if (!expect(waitUntil(application, [terminalRemove]() {
                    return terminalRemove->isEnabled();
                }, 5000),
                "terminal exit did not enable lifecycle cleanup")) {
        return 1;
    }
    terminalRemove->click();
    if (!expect(waitUntil(application, [terminalPicker]() {
                    return terminalPicker->count() == 0;
                }),
                "xterm host did not remove the exited terminal")) {
        return 1;
    }
#endif
    const QImage terminalImage = xterm->grab().toImage().convertToFormat(QImage::Format_ARGB32);
    if (!expect(!terminalImage.isNull()
                    && nonWhitePixels(terminalImage) > terminalImage.width()
                        * terminalImage.height() * 3 / 4,
                "xterm.js rendered a blank or incorrectly framed terminal")) {
        return 1;
    }
    setFailureStage(FailureCode::MONACO_EDITOR_LIFECYCLE);
    QTreeWidgetItem *item = fileTree->findItems(
        QStringLiteral("main.cpp"), Qt::MatchExactly | Qt::MatchRecursive).first();
    QMetaObject::invokeMethod(fileTree, "itemActivated", Qt::DirectConnection,
                              Q_ARG(QTreeWidgetItem *, item), Q_ARG(int, 0));

    if (!expect(waitUntil(application, [&application, monaco, &value]() {
                    return evaluate(application, monaco->page(),
                                    QStringLiteral("window.aegisyEditorTest?.getValue() || ''"),
                                    &value)
                        && value.toString() == QStringLiteral("int main() { return 0; }\n");
                }),
                "Monaco did not render the workspace file model")) {
        return 1;
    }
#ifdef Q_OS_WIN
    if (!verifyWindowsWebEngineRenderer(application, monaco)) {
        return 1;
    }
#endif
    if (!expect(evaluate(application, monaco->page(),
                         QStringLiteral("window.aegisyEditorTest.getLanguage()"), &value)
                    && value.toString() == QStringLiteral("cpp"),
                "Monaco did not select the C++ language model")) {
        return 1;
    }
    QTreeWidgetItem *secondaryItem = fileTree->findItems(
        QStringLiteral("secondary.cpp"), Qt::MatchExactly | Qt::MatchRecursive).first();
    QMetaObject::invokeMethod(fileTree, "itemActivated", Qt::DirectConnection,
                              Q_ARG(QTreeWidgetItem *, secondaryItem), Q_ARG(int, 0));
    if (!expect(waitUntil(application, [&application, monaco, &value]() {
                    return evaluate(application, monaco->page(), QStringLiteral(
                        "window.aegisyEditorTest?.getPath(0) || ''"), &value)
                        && value.toString() == QStringLiteral("secondary.cpp");
                }),
                "second Monaco model did not load before save")) {
        return 1;
    }
    QMetaObject::invokeMethod(fileTree, "itemActivated", Qt::DirectConnection,
                              Q_ARG(QTreeWidgetItem *, item), Q_ARG(int, 0));
    if (!expect(waitUntil(application, [&application, monaco, &value]() {
                    return evaluate(application, monaco->page(), QStringLiteral(
                        "window.aegisyEditorTest?.getPath(0) || ''"), &value)
                        && value.toString() == QStringLiteral("main.cpp");
                }),
                "primary Monaco model did not reactivate")) {
        return 1;
    }
    if (!expect(evaluate(application, monaco->page(), QStringLiteral(
                    "window.aegisyEditorTest.setValue('int answer() { return 42; }\\n'); true")),
                "could not edit the Monaco model")) {
        return 1;
    }
    if (!expect(waitUntil(application, [save]() { return save->isEnabled(); }),
                "Monaco edits did not reach the native dirty-state model")) {
        return 1;
    }
    save->click();
    if (!expect(waitUntil(application, [&sourcePath]() {
                    QFile saved(sourcePath);
                    return saved.open(QIODevice::ReadOnly)
                        && saved.readAll() == QByteArray("int answer() { return 42; }\n");
                }),
                "native save did not persist Monaco content")) {
        return 1;
    }

    if (!split->isEnabled()) {
        aegisy::test::reportFailure(FailureCode::MONACO_SPLIT_LIFECYCLE);
        aegisy::test::reportLocalDiagnostic(
            "split control did not enable after loading two Monaco models");
        return 1;
    }
    setFailureStage(FailureCode::MONACO_SPLIT_LIFECYCLE);
    split->click();
    if (!expect(waitUntil(application, [&application, monaco, &value]() {
                    return evaluate(application, monaco->page(), QStringLiteral(
                        "JSON.stringify({split: window.aegisyEditorTest?.isSplit(), "
                        "left: window.aegisyEditorTest?.getPath(0), "
                        "right: window.aegisyEditorTest?.getPath(1), "
                        "leftSize: window.aegisyEditorTest?.getDimensions(0), "
                        "rightSize: window.aegisyEditorTest?.getDimensions(1)})"), &value)
                        && value.toString().contains(QStringLiteral("\"split\":true"))
                        && value.toString().contains(QStringLiteral("\"left\":\"main.cpp\""))
                        && value.toString().contains(QStringLiteral("\"right\":\"secondary.cpp\""))
                        && !value.toString().contains(QStringLiteral("\"width\":0"))
                        && !value.toString().contains(QStringLiteral("\"height\":0"));
                }),
                "Monaco split groups did not bind two file models")) {
        return 1;
    }
    if (!expect(evaluate(application, monaco->page(), QStringLiteral(
                    "const left = window.aegisyEditorTest.getDimensions(0);"
                    "const right = window.aegisyEditorTest.getDimensions(1);"
                    "left.width > 100 && left.height > 100 && "
                    "right.width > 100 && right.height > 100"), &value)
                    && value.toBool(),
                "Monaco split groups did not receive visible layout dimensions")) {
        return 1;
    }
    const QByteArray digest = QCryptographicHash::hash(
        QFileInfo(project.path()).canonicalFilePath().toUtf8(),
        QCryptographicHash::Sha256).toHex();
    const QString settingsKey = QStringLiteral("agent_workbench/editor/%1")
        .arg(QString::fromLatin1(digest.left(20)));
    QSettings settings;
    if (!expect(settings.value(settingsKey + QStringLiteral("/split_enabled")).toBool()
                    && settings.value(settingsKey + QStringLiteral("/group_0_file")).toString()
                        == QStringLiteral("main.cpp")
                    && settings.value(settingsKey + QStringLiteral("/group_1_file")).toString()
                        == QStringLiteral("secondary.cpp"),
                "Monaco split group state was not persisted per project")) {
        return 1;
    }
    if (!expect(evaluate(application, monaco->page(), QStringLiteral(
                    "window.aegisyEditorTest.focusGroup(1); true")),
                "could not focus the secondary Monaco group")) {
        return 1;
    }
    if (!expect(waitUntil(application, [&application, monaco, &value]() {
                    return evaluate(application, monaco->page(), QStringLiteral(
                        "window.aegisyEditorTest.getActiveGroup()"), &value)
                        && value.toInt() == 1;
                }),
                "secondary Monaco group did not become active")) {
        return 1;
    }
    if (!expect(evaluate(application, monaco->page(), QStringLiteral(
                    "window.aegisyEditorTest.setValue('int secondary() { return 84; }\\n', 1); true")),
                "could not edit the secondary Monaco group")) {
        return 1;
    }
    if (!expect(waitUntil(application, [save]() { return save->isEnabled(); }),
                "secondary Monaco edit did not become the native save target")) {
        return 1;
    }
    save->click();
    if (!expect(waitUntil(application, [&sourcePath, &secondaryPath]() {
                    QFile saved(sourcePath);
                    QFile untouched(secondaryPath);
                    return saved.open(QIODevice::ReadOnly)
                        && untouched.open(QIODevice::ReadOnly)
                        && saved.readAll() == QByteArray("int answer() { return 42; }\n")
                        && untouched.readAll() == QByteArray("int secondary() { return 84; }\n");
                }),
                "split-group save targeted the wrong workspace file")) {
        return 1;
    }
    if (!expect(evaluate(application, monaco->page(), QStringLiteral(
                    "JSON.stringify([window.aegisyEditorTest.getValue(0), "
                    "window.aegisyEditorTest.getValue(1)])"), &value)
                    && value.toString().contains(QStringLiteral("int answer()"))
                    && value.toString().contains(QStringLiteral("int secondary()"))
                    && value.toString().contains(QStringLiteral("return 84")),
                "split groups lost a Monaco model after save")) {
        return 1;
    }
    QImage splitImage;
    int leftPixels = 0;
    int rightPixels = 0;
    const bool groupsRendered = waitUntil(application, [&]() {
        splitImage = workbench.grab().toImage().convertToFormat(QImage::Format_ARGB32);
        const QPoint origin = monaco->mapTo(&workbench, QPoint(0, 0));
        const int halfWidth = monaco->width() / 2;
        const QRect leftContent(origin.x() + 8, origin.y() + 8,
                                qMax(1, halfWidth - 32), 80);
        const QRect rightContent(origin.x() + halfWidth + 8, origin.y() + 8,
                                 qMax(1, monaco->width() - halfWidth - 32), 80);
        leftPixels = nonWhitePixels(splitImage, leftContent);
        rightPixels = nonWhitePixels(splitImage, rightContent);
        return leftPixels > 30 && rightPixels > 30;
    });
    if (!groupsRendered) {
        splitImage.save(QStringLiteral("/tmp/aegisy-split-pixel-failure.png"));
        evaluate(application, monaco->page(), QStringLiteral(
            "JSON.stringify({left: window.aegisyEditorTest.getDimensions(0), "
            "right: window.aegisyEditorTest.getDimensions(1), "
            "paths: [window.aegisyEditorTest.getPath(0), "
            "window.aegisyEditorTest.getPath(1)], "
            "loadingHidden: document.getElementById('loading').hidden})"), &value);
        aegisy::test::reportFailure(aegisy::test::FailureCode::MONACO_SPLIT_BLANK);
        aegisy::test::reportLocalDiagnostic("one Monaco split group rendered blank");
        if (aegisy::test::localDiagnosticsEnabled()) {
            qCritical().noquote() << "Monaco split rendering diagnostics"
                        << "left pixels" << leftPixels << "right pixels" << rightPixels
                        << "dimensions" << value
                        << "visible" << monaco->isVisible()
                        << "size" << monaco->size()
                        << "origin" << monaco->mapTo(&workbench, QPoint(0, 0));
        }
        return 1;
    }
    if (argc > 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--snapshot")) {
        if (!workbench.grab().save(QString::fromLocal8Bit(argv[2]))) {
            aegisy::test::reportFailure(
                aegisy::test::FailureCode::MONACO_SNAPSHOT_SAVE);
            aegisy::test::reportLocalDiagnostic(
                "failed to save Monaco workbench snapshot");
            return 1;
        }
    }
    setFailureStage(FailureCode::MONACO_SPLIT_RESTORE);
    {
        AgentWorkbenchWidget restoredWorkbench;
        restoredWorkbench.resize(1100, 700);
        restoredWorkbench.show();
        AgentRuntimeClient *restoredRuntime = restoredWorkbench.findChild<AgentRuntimeClient *>();
        QLabel *restoredStatus = restoredWorkbench.findChild<QLabel *>(
            QStringLiteral("agentRuntimeStatus"));
        QWebEngineView *restoredMonaco = restoredWorkbench.findChild<QWebEngineView *>(
            QStringLiteral("agentMonacoEditor"));
        QPushButton *restoredSplit = restoredWorkbench.findChild<QPushButton *>(
            QStringLiteral("agentEditorSplitButton"));
        if (!expect(restoredRuntime && restoredStatus && restoredMonaco && restoredSplit
                        && waitUntil(application, [restoredStatus]() {
                            return restoredStatus->text().startsWith(QStringLiteral("●"));
                        }),
                    "restored workbench runtime did not become ready")) {
            return 1;
        }
        restoredRuntime->openProject(project.path());
        const bool splitRestored = waitUntil(
            application, [&application, restoredMonaco, restoredSplit, &value]() {
                        return restoredSplit->isChecked()
                            && evaluate(application, restoredMonaco->page(), QStringLiteral(
                                "JSON.stringify({split: window.aegisyEditorTest?.isSplit(), "
                                "left: window.aegisyEditorTest?.getPath(0), "
                                "right: window.aegisyEditorTest?.getPath(1)})"), &value)
                            && value.toString().contains(QStringLiteral("\"split\":true"))
                            && value.toString().contains(QStringLiteral("\"left\":\"main.cpp\""))
                            && value.toString().contains(
                                QStringLiteral("\"right\":\"secondary.cpp\""));
                    });
        if (!splitRestored) {
            aegisy::test::reportFailure(
                aegisy::test::FailureCode::MONACO_SPLIT_RESTORE);
            aegisy::test::reportLocalDiagnostic(
                "persisted Monaco split groups did not restore");
            if (aegisy::test::localDiagnosticsEnabled()) {
                qCritical().noquote()
                    << "Monaco split restore diagnostics"
                    << "web state" << value
                    << "checked" << restoredSplit->isChecked()
                    << "enabled" << restoredSplit->isEnabled()
                    << "saved split"
                    << settings.value(settingsKey + QStringLiteral("/split_enabled"))
                    << "saved group 0"
                    << settings.value(settingsKey + QStringLiteral("/group_0_file"))
                    << "saved group 1"
                    << settings.value(settingsKey + QStringLiteral("/group_1_file"));
            }
            return 1;
        }
        restoredWorkbench.hide();
    }
    setFailureStage(FailureCode::MONACO_SECURITY_BOUNDARY);
    const QUrl trustedUrl = monaco->url();
    evaluate(application, monaco->page(),
             QStringLiteral("location.href = 'https://example.com/'; true"));
    application.processEvents();
    if (!expect(monaco->url() == trustedUrl && trustedUrl.isLocalFile(),
                "Monaco host allowed external navigation")) {
        return 1;
    }

    return 0;
}
