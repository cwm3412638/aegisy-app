#include "gateway_manager.h"

#include "gateway_control_contract.h"
#include "secure_storage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QEventLoop>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

namespace {

const QString kGatewayCredential = QStringLiteral("gateway/local-token");
constexpr int kGatewayPort = 43112;
constexpr int kMaxRequestLogs = 500;
constexpr int kControlTimeoutMs = 5000;

} // namespace

GatewayManager::GatewayManager(QObject *parent)
    : QObject(parent)
{
}

GatewayManager::~GatewayManager()
{
    stop();
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

QString GatewayManager::toolSlug(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("claude");
    case AiTool::CodexCli: return QStringLiteral("codex");
    case AiTool::GeminiCli: return QStringLiteral("gemini");
    case AiTool::OpenCode: return QStringLiteral("opencode");
    }
    return QStringLiteral("codex");
}

QString GatewayManager::endpoint(AiTool tool) const
{
    return QStringLiteral("http://127.0.0.1:%1/tools/%2")
        .arg(kGatewayPort).arg(toolSlug(tool));
}

QString GatewayManager::ensureGatewayScript()
{
    const QString directory = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation) + QStringLiteral("/gateway");
    if (!QDir().mkpath(directory)) {
        m_lastError = QStringLiteral("无法创建本地网关目录。 ").trimmed();
        return QString();
    }
    QFile resource(QStringLiteral(":/gateway/local_gateway.js"));
    if (!resource.open(QIODevice::ReadOnly)) {
        m_lastError = QStringLiteral("本地网关资源缺失。 ").trimmed();
        return QString();
    }
    const QByteArray content = resource.readAll();
    const QString path = directory + QStringLiteral("/local_gateway.js");
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)
        || output.write(content) != content.size()
        || !output.commit()) {
        m_lastError = QStringLiteral("无法释放本地网关脚本。 ").trimmed();
        return QString();
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return path;
}

bool GatewayManager::start()
{
    if (m_running) {
        return true;
    }
    m_lastError.clear();

    ToolManager detector;
    const QString nodeExecutable = detector.resolvedRuntimeCommand(
        QStringLiteral("node"), 1000);
    if (nodeExecutable.isEmpty()) {
        m_lastError = QStringLiteral("本地网关需要 Node.js，请先在系统体检中安装。 ").trimmed();
        return false;
    }
    const QString script = ensureGatewayScript();
    if (script.isEmpty()) {
        return false;
    }

    m_localToken = SecureStorage::loadEncrypted(kGatewayCredential);
    if (m_localToken.isEmpty()) {
        m_localToken = QStringLiteral("aegisy-local-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-')));
        if (!SecureStorage::saveEncrypted(kGatewayCredential, m_localToken)) {
            m_lastError = QStringLiteral("无法将本地网关令牌保存到系统安全存储。 ").trimmed();
            m_localToken.clear();
            return false;
        }
    }

    ++m_generation;
    const quint64 generation = m_generation;
    m_stdoutBuffer.clear();
    m_toolRevisions.clear();
    auto *process = new QProcess(this);
    m_process = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("AEGISY_GATEWAY_PORT"), QString::number(kGatewayPort));
    environment.insert(QStringLiteral("AEGISY_GATEWAY_TOKEN"), m_localToken);
    process->setProcessEnvironment(environment);
    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, process, generation]() { processOutput(process, generation); });
    connect(process, &QProcess::readyReadStandardError, this,
            [this, process, generation]() {
        const QByteArray bytes = process->readAllStandardError();
        if (generation == m_generation && !bytes.isEmpty()) {
            emit gatewayError(QStringLiteral("gateway-runtime-stderr"));
        }
    });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process, generation](int, QProcess::ExitStatus) {
        if (generation != m_generation || process != m_process) return;
        if (m_controlWaiting) {
            failCurrentGeneration(QStringLiteral("gateway-control-exit-outcome-unknown"));
            return;
        }
        const bool wasRunning = m_running;
        m_running = false;
        if (wasRunning) emit runningChanged(false);
    });

    process->start(nodeExecutable, { script });
    if (!process->waitForStarted(3000)) {
        m_lastError = QStringLiteral("无法启动本地网关：%1").arg(process->errorString());
        return false;
    }
    if (process->waitForReadyRead(3000)) {
        processOutput(process, generation);
    }
    if (!m_running) {
        m_lastError = QStringLiteral("本地网关启动超时，端口可能已被占用。 ").trimmed();
        m_process->kill();
        return false;
    }
    return true;
}

void GatewayManager::stop()
{
    if (!m_process || m_process->state() == QProcess::NotRunning) {
        if (m_running) {
            m_running = false;
            emit runningChanged(false);
        }
        return;
    }
    const QJsonObject message { { QStringLiteral("type"), QStringLiteral("shutdown") } };
    m_process->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
    if (!m_process->waitForFinished(1500)) {
        m_process->terminate();
    }
}

bool GatewayManager::configureProfile(AiTool tool, const QString &apiKey)
{
    QString transactionId;
    if (!prepareProfile(tool, apiKey, &transactionId)) return false;
    if (commitProfile(tool, transactionId)) return true;
    const QString commitError = m_lastError;
    if (m_running) abortProfile(tool, transactionId);
    m_lastError = commitError;
    return false;
}

bool GatewayManager::prepareProfile(
    AiTool tool, const QString &apiKey, QString *transactionId)
{
    if (transactionId) transactionId->clear();
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!sendControlAndWait(tool, QStringLiteral("prepare-configure"), id, apiKey)) {
        return false;
    }
    if (transactionId) *transactionId = id;
    return true;
}

bool GatewayManager::commitProfile(AiTool tool, const QString &transactionId)
{
    return sendControlAndWait(tool, QStringLiteral("commit"), transactionId);
}

bool GatewayManager::abortProfile(AiTool tool, const QString &transactionId)
{
    return sendControlAndWait(tool, QStringLiteral("abort"), transactionId);
}

bool GatewayManager::removeProfile(AiTool tool)
{
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!sendControlAndWait(tool, QStringLiteral("prepare-remove"), id)) return false;
    if (commitProfile(tool, id)) return true;
    const QString commitError = m_lastError;
    if (m_running) abortProfile(tool, id);
    m_lastError = commitError;
    return false;
}

bool GatewayManager::sendControlAndWait(
    AiTool tool, const QString &operation, const QString &transactionId,
    const QString &apiKey)
{
    m_lastError.clear();
    if (!m_running || !m_process || m_controlWaiting
            || transactionId.isEmpty()
            || (operation == QStringLiteral("prepare-configure")
                && apiKey.trimmed().isEmpty())) {
        m_lastError = QStringLiteral("gateway-control-unavailable");
        return false;
    }
    const QString slug = toolSlug(tool);
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject message{
        {QStringLiteral("schema"), QStringLiteral("aegisy-gateway-control/0.1")},
        {QStringLiteral("type"), QStringLiteral("control")},
        {QStringLiteral("request_id"), requestId},
        {QStringLiteral("transaction_id"), transactionId},
        {QStringLiteral("operation"), operation},
        {QStringLiteral("tool"), slug},
        {QStringLiteral("expected_revision"), m_toolRevisions.value(static_cast<int>(tool), 0)},
    };
    if (operation == QStringLiteral("prepare-configure")) {
        message.insert(QStringLiteral("apiKey"), apiKey);
        message.insert(QStringLiteral("upstream"), QStringLiteral("https://aegisy.cc"));
    }
    m_expectedRequestId = requestId;
    m_expectedTransactionId = transactionId;
    m_expectedOperation = operation;
    m_expectedTool = slug;
    m_expectedGeneration = m_generation;
    m_controlWaiting = true;
    m_controlSucceeded = false;
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (m_process->write(line) != line.size() || !m_process->waitForBytesWritten(1000)) {
        failCurrentGeneration(QStringLiteral("gateway-control-write-outcome-unknown"));
        return false;
    }
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    const QMetaObject::Connection ready = connect(
        this, &GatewayManager::runtimeEvent, &loop,
        [&loop](const QJsonObject &event) {
            if (event.value(QStringLiteral("type")).toString()
                    == QStringLiteral("gateway-control-finished")) loop.quit();
        });
    timer.start(kControlTimeoutMs);
    if (m_controlWaiting) loop.exec();
    disconnect(ready);
    if (m_controlWaiting) {
        failCurrentGeneration(QStringLiteral("gateway-control-timeout-outcome-unknown"));
        return false;
    }
    return m_controlSucceeded;
}

void GatewayManager::processOutput(QProcess *process, quint64 generation)
{
    if (!process || process != m_process || generation != m_generation) return;
    m_stdoutBuffer += process->readAllStandardOutput();
    if (m_stdoutBuffer.size() > 64 * 1024) {
        failCurrentGeneration(QStringLiteral("gateway-control-output-oversized"));
        return;
    }
    int newline = -1;
    while ((newline = m_stdoutBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdoutBuffer.left(newline).trimmed();
        m_stdoutBuffer.remove(0, newline + 1);
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (document.isObject()) handleEvent(document.object(), generation);
    }
}

void GatewayManager::handleEvent(const QJsonObject &event, quint64 generation)
{
    if (generation != m_generation) return;
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("ready")) {
        if (!m_running) {
            m_running = true;
            emit runningChanged(true);
        }
    } else if (type == QStringLiteral("control-result")) {
        if (!m_controlWaiting) return;
        const GatewayControlEvaluation evaluation = GatewayControlContract::evaluate(
            event, {m_expectedRequestId, m_expectedTransactionId,
                    m_expectedOperation, m_expectedTool});
        if (generation != m_expectedGeneration
                || evaluation.decision == GatewayControlDecision::Invalid) {
            failCurrentGeneration(QStringLiteral("gateway-control-protocol-invalid"));
            return;
        }
        m_controlSucceeded = evaluation.decision == GatewayControlDecision::Accepted;
        if (m_controlSucceeded && m_expectedOperation == QStringLiteral("commit")) {
            for (AiTool tool : {AiTool::ClaudeCode, AiTool::CodexCli,
                                AiTool::GeminiCli, AiTool::OpenCode}) {
                if (toolSlug(tool) == m_expectedTool) {
                    m_toolRevisions.insert(static_cast<int>(tool), evaluation.revision);
                    break;
                }
            }
        }
        if (!m_controlSucceeded) {
            m_lastError = evaluation.errorCode;
        }
        m_controlWaiting = false;
        emit runtimeEvent({{QStringLiteral("type"),
                            QStringLiteral("gateway-control-finished")}});
    } else if (type == QStringLiteral("request_started")) {
        emit runtimeEvent(event);
    } else if (type == QStringLiteral("request")) {
        m_requestLogs.prepend(event);
        while (m_requestLogs.size() > kMaxRequestLogs) m_requestLogs.removeLast();
        emit runtimeEvent(event);
        emit requestLogged(event);
    } else if (type == QStringLiteral("fatal")) {
        m_lastError = event.value(QStringLiteral("error")).toString();
        emit gatewayError(m_lastError);
    }
}

void GatewayManager::failCurrentGeneration(const QString &errorCode)
{
    m_lastError = errorCode;
    m_controlWaiting = false;
    m_controlSucceeded = false;
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
    }
    const bool wasRunning = m_running;
    m_running = false;
    emit runtimeEvent({{QStringLiteral("type"),
                        QStringLiteral("gateway-control-finished")}});
    if (wasRunning) emit runningChanged(false);
}

void GatewayManager::clearRequestLogs()
{
    m_requestLogs.clear();
}
