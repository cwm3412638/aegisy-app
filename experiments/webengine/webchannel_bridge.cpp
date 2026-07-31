#include <QApplication>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebChannel>
#include <QObject>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

class BridgeAPI : public QObject {
    Q_OBJECT
public:
    explicit BridgeAPI(QObject *parent = nullptr) : QObject(parent), nextRequestId(1) {}

public slots:
    // Handle requests from JavaScript with typed IDs, size limits, cancellation
    QVariantMap request(const QString &method, const QVariantMap &params, int requestId) {
        QVariantMap response;
        response["id"] = requestId;

        // Origin check (in real impl, check page origin)
        // Size limit check
        QJsonDocument doc = QJsonDocument::fromVariant(params);
        if (doc.toJson().size() > 1024 * 1024) { // 1MB limit
            response["error"] = "Request too large";
            return response;
        }

        // Check if cancelled
        if (cancelledRequests.contains(requestId)) {
            cancelledRequests.remove(requestId);
            response["error"] = "Cancelled";
            return response;
        }

        // Handle method
        if (method == "echo") {
            response["result"] = params;
        } else if (method == "getVersion") {
            response["result"] = "1.0.0";
        } else {
            response["error"] = "Unknown method";
        }

        return response;
    }

    void cancelRequest(int requestId) {
        cancelledRequests.insert(requestId);
        emit requestCancelled(requestId);
    }

signals:
    void requestCancelled(int requestId);

private:
    int nextRequestId;
    QSet<int> cancelledRequests;
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    QWebEngineView view;
    QWebEngineProfile profile;
    QWebEnginePage *page = new QWebEnginePage(&profile, &view);
    view.setPage(page);

    // Create QWebChannel
    QWebChannel *channel = new QWebChannel(page);
    BridgeAPI *api = new BridgeAPI();
    channel->registerObject("bridge", api);
    page->setWebChannel(channel);

    QString html = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>QWebChannel Bridge Test</title>
    <script src="qrc:///qtwebchannel/qwebchannel.js"></script>
    <style>
        body { font-family: system-ui; padding: 20px; background: #1e1e1e; color: #d4d4d4; }
        button { padding: 8px 16px; margin: 5px; }
        .result { background: #252526; padding: 10px; margin: 10px 0; border-radius: 4px; }
    </style>
</head>
<body>
    <h1>QWebChannel Bridge - Task 2.3</h1>
    <button onclick="testEcho()">Test Echo</button>
    <button onclick="testVersion()">Test GetVersion</button>
    <button onclick="testCancel()">Test Cancellation</button>
    <button onclick="testSizeLimit()">Test Size Limit</button>
    <div id="results" class="result"></div>

    <script>
        let bridge;
        let requestId = 1;

        new QWebChannel(qt.webChannelTransport, function(channel) {
            bridge = channel.objects.bridge;
            log('Bridge connected');
        });

        function log(msg) {
            document.getElementById('results').innerHTML += msg + '<br>';
        }

        function testEcho() {
            const id = requestId++;
            const result = bridge.request('echo', {message: 'Hello'}, id);
            log('Echo: ' + JSON.stringify(result));
        }

        function testVersion() {
            const id = requestId++;
            const result = bridge.request('getVersion', {}, id);
            log('Version: ' + JSON.stringify(result));
        }

        function testCancel() {
            const id = requestId++;
            bridge.cancelRequest(id);
            const result = bridge.request('echo', {}, id);
            log('Cancelled: ' + JSON.stringify(result));
        }

        function testSizeLimit() {
            const id = requestId++;
            const largeData = {data: 'x'.repeat(2 * 1024 * 1024)};
            const result = bridge.request('echo', largeData, id);
            log('Size limit: ' + JSON.stringify(result));
        }
    </script>
</body>
</html>
)HTML";

    view.setHtml(html, QUrl("qrc:///workbench/bridge.html"));
    view.resize(1024, 768);
    view.setWindowTitle("QWebChannel Bridge Experiment - Task 2.3");
    view.show();

    return app.exec();
}

#include "webchannel_bridge.moc"
