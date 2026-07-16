#include "gateway_manager.h"

#include "secure_storage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace {

const QString kGatewayCredential = QStringLiteral("gateway/local-token");
constexpr int kGatewayPort = 43112;
constexpr int kMaxRequestLogs = 500;

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

    if (m_process) {
        m_process->deleteLater();
    }
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("AEGISY_GATEWAY_PORT"), QString::number(kGatewayPort));
    environment.insert(QStringLiteral("AEGISY_GATEWAY_TOKEN"), m_localToken);
    m_process->setProcessEnvironment(environment);
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &GatewayManager::processOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
        const QString error = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
        if (!error.isEmpty()) emit gatewayError(error.left(300));
    });
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int, QProcess::ExitStatus) {
        const bool wasRunning = m_running;
        m_running = false;
        if (wasRunning) emit runningChanged(false);
    });

    m_process->start(nodeExecutable, { script });
    if (!m_process->waitForStarted(3000)) {
        m_lastError = QStringLiteral("无法启动本地网关：%1").arg(m_process->errorString());
        return false;
    }
    if (m_process->waitForReadyRead(3000)) {
        processOutput();
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
    if (!m_running || !m_process || apiKey.trimmed().isEmpty()) {
        m_lastError = QStringLiteral("本地网关未运行或档案凭据为空。 ").trimmed();
        return false;
    }
    QJsonObject message;
    message.insert(QStringLiteral("type"), QStringLiteral("configure"));
    message.insert(QStringLiteral("tool"), toolSlug(tool));
    message.insert(QStringLiteral("apiKey"), apiKey);
    message.insert(QStringLiteral("upstream"), QStringLiteral("https://aegisy.cc"));
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    return m_process->write(line) == line.size();
}

void GatewayManager::processOutput()
{
    if (!m_process) return;
    m_stdoutBuffer += m_process->readAllStandardOutput();
    int newline = -1;
    while ((newline = m_stdoutBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdoutBuffer.left(newline).trimmed();
        m_stdoutBuffer.remove(0, newline + 1);
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (document.isObject()) handleEvent(document.object());
    }
}

void GatewayManager::handleEvent(const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("ready")) {
        if (!m_running) {
            m_running = true;
            emit runningChanged(true);
        }
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

void GatewayManager::clearRequestLogs()
{
    m_requestLogs.clear();
}
