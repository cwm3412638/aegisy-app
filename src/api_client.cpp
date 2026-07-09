#include "api_client.h"
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSslConfiguration>

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
    // 添加分页参数
    QString endpoint = "/api/v1/keys?page=1&page_size=100&sort_by=created_at&sort_order=desc";
    QNetworkReply *reply = get(endpoint);
    connect(reply, &QNetworkReply::finished, this, &ApiClient::onApiKeysFinished);
}

void ApiClient::getUserInfo()
{
    QNetworkReply *reply = get("/api/v1/user");
    connect(reply, &QNetworkReply::finished, this, &ApiClient::onUserInfoFinished);
}

QNetworkReply* ApiClient::post(const QString &endpoint, const QJsonObject &data)
{
    QUrl url(m_baseUrl + endpoint);
    QNetworkRequest request(url);

    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

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

QNetworkReply* ApiClient::get(const QString &endpoint)
{
    QUrl url(m_baseUrl + endpoint);
    QNetworkRequest request(url);

    if (!m_authToken.isEmpty()) {
        request.setRawHeader("Authorization", QString("Bearer %1").arg(m_authToken).toUtf8());
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

    if (reply->error() != QNetworkReply::NoError) {
        result["error"] = reply->errorString();
        return result;
    }

    QByteArray responseData = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(responseData);

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

    // 调试：打印完整响应
    qDebug() << "Login response:" << response;

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

    qDebug() << "Extracted token:" << token;

    if (token.isEmpty()) {
        // 打印详细错误信息
        qDebug() << "Response keys:" << response.keys();
        qDebug() << "Data keys:" << data.keys();
        emit loginFailed("No token received. Response: " + QString(QJsonDocument(response).toJson()));
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

    qDebug() << "Received" << keys.size() << "API keys";
    emit apiKeysReceived(keys);

    reply->deleteLater();
}

void ApiClient::onUserInfoFinished()
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

void ApiClient::getModels()
{
    // 尝试 v1 API
    QNetworkReply *reply = get("/api/v1/models?page=1&page_size=100");
    connect(reply, &QNetworkReply::finished, this, &ApiClient::onModelsFinished);
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
    QJsonArray models = data["items"].toArray();

    qDebug() << "Received" << models.size() << "models";
    emit modelsReceived(models);

    reply->deleteLater();
}
