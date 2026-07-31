// Task 2.4: Monaco editor integration with open/edit/save, large files, diff, theme, fonts, IME
#include <QApplication>
#include <QMainWindow>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebChannel>
#include <QVBoxLayout>
#include <QPushButton>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QDebug>

class EditorAPI : public QObject {
    Q_OBJECT
public:
    explicit EditorAPI(QObject *parent = nullptr) : QObject(parent) {}

public slots:
    QString loadFile(const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QString();
        }
        QTextStream in(&file);
        return in.readAll();
    }

    bool saveFile(const QString &path, const QString &content) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return false;
        }
        QTextStream out(&file);
        out << content;
        return true;
    }

signals:
    void fileLoaded(const QString &content);
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    QMainWindow window;
    window.setWindowTitle("Monaco Editor Test - Task 2.4");
    window.resize(1200, 800);

    auto *central = new QWidget(&window);
    auto *layout = new QVBoxLayout(central);

    auto *view = new QWebEngineView(central);
    auto *profile = new QWebEngineProfile(view);
    profile->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);

    auto *page = new QWebEnginePage(profile, view);
    view->setPage(page);

    auto *channel = new QWebChannel(page);
    auto *api = new EditorAPI(&window);
    channel->registerObject("editorAPI", api);
    page->setWebChannel(channel);

    // Load Monaco test HTML
    QString htmlPath = QCoreApplication::applicationDirPath() + "/../experiments/webengine/monaco_test.html";
    QFile htmlFile(htmlPath);
    if (!htmlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Failed to load monaco_test.html from" << htmlPath;
        page->setHtml("<h1>Error: monaco_test.html not found</h1>");
    } else {
        QString html = QTextStream(&htmlFile).readAll();
        page->setHtml(html, QUrl("file://" + htmlPath));
    }

    auto *btnLayout = new QHBoxLayout();
    auto *openBtn = new QPushButton("Open File", central);
    auto *saveBtn = new QPushButton("Save File", central);

    QObject::connect(openBtn, &QPushButton::clicked, [&]() {
        QString path = QFileDialog::getOpenFileName(&window, "Open File");
        if (!path.isEmpty()) {
            QString content = api->loadFile(path);
            page->runJavaScript(QString("status.textContent = 'Loaded: %1 chars'").arg(content.length()));
        }
    });

    QObject::connect(saveBtn, &QPushButton::clicked, [&]() {
        QString path = QFileDialog::getSaveFileName(&window, "Save File");
        if (!path.isEmpty()) {
            page->runJavaScript("status.textContent", [=](const QVariant &result) {
                api->saveFile(path, result.toString());
            });
        }
    });

    btnLayout->addWidget(openBtn);
    btnLayout->addWidget(saveBtn);
    layout->addLayout(btnLayout);
    layout->addWidget(view);

    window.setCentralWidget(central);
    window.show();

    return app.exec();
}

#include "monaco_editor.moc"
