#include "api_client.h"
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSslConfiguration>

namespace {

constexpr int kApiKeyPageSize = 100;
constexpr int kMaxApiKeyPages = 100;

} // namespace

ApiClient::ApiClient(QObject *parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
    , m_baseUrl("https://www.aegisy.cc")
{
}

ApiClient::~ApiClient()
{
}

void ApiClient::setBaseUrl(const QString &url)
{
    m_baseUrl = url;
}

void ApiClient::setAuthToken(const QString &token)
{
    m_authToken = token;
}

void ApiClient::login(const QString &email, const QString &password)
{
    QJsonObject data;
    data["email"] = email;
    data["password"] = password;

    QNetworkReply *reply = post("/api/v1/auth/login", data);
    connect(reply, &QNetworkReply::finished, this, &ApiClient::onLoginFinished);
}

void ApiClient::getApiKeys()
{
    ++m_apiKeyGeneration;
    m_apiKeyAccumulator = QJsonArray();
    requestApiKeysPage(1, m_apiKeyGeneration);
}

void ApiClient::requestApiKeysPage(int page, int generation)
{
    const QString endpoint = QStringLiteral(
        "/api/v1/keys?page=%1&page_size=%2&sort_by=created_at&sort_order=desc")
        .arg(page)
        .arg(kApiKeyPageSize);
    QNetworkReply *reply = get(endpoint);
    reply->setProperty("aegisyApiKeyGeneration", generation);
    reply->setProperty("aegisyApiKeyPage", page);
    connect(reply, &QNetworkReply::finished, this, &ApiClient::onApiKeysFinished);
}

void ApiClient::getUserInfo()
{
    requestUserInfo(QStringLiteral("/api/v1/auth/me"));
}

void ApiClient::requestUserInfo(const QString &endpoint)
{
    QNetworkReply *reply = get(endpoint);
    reply->setProperty("aegisyUserInfoEndpoint", endpoint);
    connect(reply, &QNetworkReply::finished, this, &ApiClient::onUserInfoFinished);
}

QNetworkReply* ApiClient::post(const QString &endpoint, const QJsonObject &data)
{
    QUrl url(m_baseUrl + endpoint);
    QNetworkRequest request(url);

    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(15000);

    if (!m_authToken.isEmpty()) {
        request.setRawHeader("Authorization", QString("Bearer %1").arg(m_authToken).toUtf8());
    }

    // SSL 配置（用于证书锁定，后续可增强）
    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(sslConfig);

    QJsonDocument doc(data);
    return m_networkManager->post(request, doc.toJson());
}

QNetworkReply* ApiClient::get(const QString &endpoint, const QString &bearerToken)
{
    QUrl url(m_baseUrl + endpoint);
    QNetworkRequest request(url);
    request.setTransferTimeout(15000);

    // 优先使用显式传入的 token（例如调用 /v1/models 时需要 sk- API Key），
    // 否则回退到登录得到的 JWT token
    const QString token = bearerToken.isEmpty() ? m_authToken : bearerToken;
    if (!token.isEmpty()) {
        request.setRawHeader("Authorization", QString("Bearer %1").arg(token).toUtf8());
    }

    // SSL 配置
    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(sslConfig);

    return m_networkManager->get(request);
}

QJsonObject ApiClient::parseResponse(QNetworkReply *reply, bool &ok)
{
    ok = false;
    QJsonObject result;
    const QByteArray responseData = reply->readAll();
    const QJsonDocument doc = QJsonDocument::fromJson(responseData);

    if (reply->error() != QNetworkReply::NoError) {
        if (doc.isObject()) {
            result = doc.object();
        }
        QString message = result.value(QStringLiteral("message")).toString();
        if (message.isEmpty()) {
            message = reply->errorString();
        }
        result["error"] = message;
        return result;
    }

    if (doc.isNull() || !doc.isObject()) {
        result["error"] = "Invalid JSON response";
        return result;
    }

    result = doc.object();
    ok = true;
    return result;
}

void ApiClient::onLoginFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    bool ok;
    QJsonObject response = parseResponse(reply, ok);

    if (!ok) {
        emit loginFailed(response["error"].toString());
        reply->deleteLater();
        return;
    }

    // 检查 sub2api 响应格式
    int code = response["code"].toInt(-1);
    if (code != 0 && code != 200) {
        QString message = response["message"].toString("Login failed");
        emit loginFailed(message);
        reply->deleteLater();
        return;
    }

    // 提取 token - 尝试多种可能的位置
    QString token;

    // 尝试 1: data.token
    QJsonObject data = response["data"].toObject();
    token = data["token"].toString();

    // 尝试 2: 直接在 response.token
    if (token.isEmpty()) {
        token = response["token"].toString();
    }

    // 尝试 3: data.access_token (常见的 JWT 命名)
    if (token.isEmpty()) {
        token = data["access_token"].toString();
    }

    if (token.isEmpty()) {
        emit loginFailed(QStringLiteral("服务器响应中缺少登录凭据。"));
        reply->deleteLater();
        return;
    }

    m_authToken = token;
    emit loginSuccess(token, data);

    reply->deleteLater();
}

void ApiClient::onApiKeysFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    const int generation = reply->property("aegisyApiKeyGeneration").toInt();
    const int page = reply->property("aegisyApiKeyPage").toInt();
    if (generation != m_apiKeyGeneration) {
        reply->deleteLater();
        return;
    }

    bool ok;
    QJsonObject response = parseResponse(reply, ok);

    if (!ok) {
        emit requestFailed(response["error"].toString());
        reply->deleteLater();
        return;
    }

    int code = response["code"].toInt(-1);
    if (code != 0 && code != 200) {
        emit requestFailed(response["message"].toString());
        reply->deleteLater();
        return;
    }

    // 正确解析：data.items 才是实际的 keys 数组
    QJsonObject data = response["data"].toObject();
    QJsonArray keys = data["items"].toArray();

    for (const QJsonValue &key : keys) {
        m_apiKeyAccumulator.append(key);
    }

    const int total = data.value(QStringLiteral("total")).toInt(-1);
    const bool hasMoreByTotal = total >= 0 && m_apiKeyAccumulator.size() < total;
    const bool hasMoreByPageSize = total < 0 && keys.size() == kApiKeyPageSize;
    if ((hasMoreByTotal || hasMoreByPageSize) && page < kMaxApiKeyPages) {
        reply->deleteLater();
        requestApiKeysPage(page + 1, generation);
        return;
    }

    qDebug() << "Received" << m_apiKeyAccumulator.size() << "API keys";
    emit apiKeysReceived(m_apiKeyAccumulator);

    reply->deleteLater();
}

void ApiClient::onUserInfoFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    const QString endpoint = reply->property("aegisyUserInfoEndpoint").toString();
    const int httpStatus = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (httpStatus == 404 && endpoint == QStringLiteral("/api/v1/auth/me")) {
        reply->deleteLater();
        requestUserInfo(QStringLiteral("/api/v1/user/profile"));
        return;
    }

    bool ok;
    QJsonObject response = parseResponse(reply, ok);

    if (!ok) {
        emit requestFailed(response["error"].toString());
        reply->deleteLater();
        return;
    }

    int code = response["code"].toInt(-1);
    if (code != 0 && code != 200) {
        emit requestFailed(response["message"].toString());
        reply->deleteLater();
        return;
    }

    QJsonObject userInfo = response["data"].toObject();
    emit userInfoReceived(userInfo);

    reply->deleteLater();
}

void ApiClient::getChannels()
{
    // 尝试 v1 API
    QNetworkReply *reply = get("/api/v1/channels?page=1&page_size=100");
    connect(reply, &QNetworkReply::finished, this, &ApiClient::onChannelsFinished);
}

void ApiClient::getModels(const QString &apiKey)
{
    // 使用 OpenAI 兼容端点 /v1/models，Bearer 必须是 sk- 开头的 API Key
    QNetworkReply *reply = get("/v1/models", apiKey);
    connect(reply, &QNetworkReply::finished, this, &ApiClient::onModelsFinished);
}

void ApiClient::testApiKey(const QString &keyId, const QString &apiKey)
{
    // 用被测 key 请求 /v1/models：200 表示可用。
    // 用按 reply 的 lambda 关联 keyId，支持并发测试多个 key 而不串扰。
    QNetworkReply *reply = get("/v1/models", apiKey);
    connect(reply, &QNetworkReply::finished, this, [this, reply, keyId]() {
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const QString errStr = reply->errorString();
        reply->deleteLater();

        const QJsonDocument doc = QJsonDocument::fromJson(body);
        const QJsonObject obj = doc.isObject() ? doc.object() : QJsonObject();

        if (http == 200) {
            const int n = obj["data"].toArray().size();
            emit apiKeyTested(keyId, true, QStringLiteral("可用（%1 个模型）").arg(n));
        } else {
            QString detail = obj["message"].toString();
            if (detail.isEmpty()) {
                detail = http > 0 ? QStringLiteral("HTTP %1").arg(http) : errStr;
            }
            emit apiKeyTested(keyId, false, detail);
        }
    });
}

void ApiClient::onChannelsFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    bool ok;
    QJsonObject response = parseResponse(reply, ok);

    if (!ok) {
        emit requestFailed(response["error"].toString());
        reply->deleteLater();
        return;
    }

    int code = response["code"].toInt(-1);
    if (code != 0 && code != 200) {
        emit requestFailed(response["message"].toString());
        reply->deleteLater();
        return;
    }

    // 解析：data.items 才是实际数组
    QJsonObject data = response["data"].toObject();
    QJsonArray channels = data["items"].toArray();

    qDebug() << "Received" << channels.size() << "channels";
    emit channelsReceived(channels);

    reply->deleteLater();
}

void ApiClient::onModelsFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    // /v1/models 是 OpenAI 兼容端点，成功返回 {"object":"list","data":[...]}，
    // 出错（如 401）返回 {"code":"INVALID_API_KEY","message":"..."}。
    // 这里直接读取 body，即使 HTTP 401 也能拿到错误信息。
    const QByteArray body = reply->readAll();
    reply->deleteLater();

    QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isNull() || !doc.isObject()) {
        emit requestFailed(QStringLiteral("无效的响应: %1").arg(QString::fromUtf8(body.left(200))));
        return;
    }

    QJsonObject response = doc.object();

    // 错误响应带有 message 字段
    if (response.contains("message") && !response.contains("data")) {
        emit requestFailed(response["message"].toString(QStringLiteral("请求失败")));
        return;
    }

    QJsonArray models = response["data"].toArray();
    qDebug() << "Received" << models.size() << "models";
    emit modelsReceived(models);
}
