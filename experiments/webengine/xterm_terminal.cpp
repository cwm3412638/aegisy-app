// Task 2.5: xterm.js integration with PTY, resize, Unicode, copy/paste, links, virtualization
#include <QApplication>
#include <QMainWindow>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebChannel>
#include <QVBoxLayout>
#include <QPushButton>
#include <QProcess>
#include <QFile>
#include <QTextStream>
#include <QDebug>

class TerminalAPI : public QObject {
    Q_OBJECT
public:
    explicit TerminalAPI(QObject *parent = nullptr) : QObject(parent) {
        process = new QProcess(this);
        connect(process, &QProcess::readyReadStandardOutput, this, &TerminalAPI::onOutput);
        connect(process, &QProcess::readyReadStandardError, this, &TerminalAPI::onOutput);
    }

public slots:
    void startShell() {
        process->start("/bin/zsh", QStringList() << "-i");
    }

    void writeInput(const QString &data) {
        process->write(data.toUtf8());
    }

    void resize(int cols, int rows) {
        qDebug() << "Terminal resize:" << cols << "x" << rows;
    }

signals:
    void output(const QString &data);

private slots:
    void onOutput() {
        QString data = QString::fromUtf8(process->readAllStandardOutput());
        if (!data.isEmpty()) emit output(data);
    }

private:
    QProcess *process;
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    QMainWindow window;
    window.setWindowTitle("xterm.js Test - Task 2.5");
    window.resize(1000, 700);

    auto *central = new QWidget(&window);
    auto *layout = new QVBoxLayout(central);

    auto *view = new QWebEngineView(central);
    auto *profile = new QWebEngineProfile(view);
    profile->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);

    auto *page = new QWebEnginePage(profile, view);
    view->setPage(page);

    auto *channel = new QWebChannel(page);
    auto *api = new TerminalAPI(&window);
    channel->registerObject("terminalAPI", api);
    page->setWebChannel(channel);

    QString htmlPath = QCoreApplication::applicationDirPath() + "/../experiments/webengine/xterm_test.html";
    QFile htmlFile(htmlPath);
    if (!htmlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Failed to load xterm_test.html";
        page->setHtml("<h1>Error: xterm_test.html not found</h1>");
    } else {
        QString html = QTextStream(&htmlFile).readAll();
        page->setHtml(html, QUrl("file://" + htmlPath));
    }

    auto *btnLayout = new QHBoxLayout();
    auto *startBtn = new QPushButton("Start Shell", central);
    auto *testBtn = new QPushButton("Test Features", central);

    QObject::connect(startBtn, &QPushButton::clicked, [=]() {
        api->startShell();
    });

    QObject::connect(testBtn, &QPushButton::clicked, [=]() {
        page->runJavaScript("runAllTests()");
    });

    QObject::connect(api, &TerminalAPI::output, [=](const QString &data) {
        QString escaped = data;
        escaped.replace("\\", "\\\\").replace("'", "\\'").replace("\n", "\\n").replace("\r", "\\r");
        page->runJavaScript(QString("if(window.term) term.write('%1')").arg(escaped));
    });

    btnLayout->addWidget(startBtn);
    btnLayout->addWidget(testBtn);
    layout->addLayout(btnLayout);
    layout->addWidget(view);

    window.setCentralWidget(central);
    window.show();

    return app.exec();
}

#include "xterm_terminal.moc"
