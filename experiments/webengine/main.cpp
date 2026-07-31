#include <QApplication>
#include <QWebEngineView>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QWebEngineView view;
    view.setHtml("<html><body><h1>Qt WebEngine Experiment</h1></body></html>");
    view.resize(800, 600);
    view.show();
    return app.exec();
}
