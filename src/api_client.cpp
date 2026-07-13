#include "api_client.h"
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSslConfiguration>
#include <QUrl>
#include <QElapsedTimer>
#include <QTimer>
#include <QDate>

namespace {

constexpr int kApiKeyPageSize = 100;
constexpr int kMaxApiKeyPages = 100;

struct ImageResponseData
{
    QString base64;
    QString partialBase64;
    QString format;
    QString revisedPrompt;
    QString error;
};

void collectImageResponse(const QJsonValue &value, ImageResponseData &result)
{
    if (value.isArray()) {
        for (const QJsonValue &item : value.toArray()) collectImageResponse(item, result);
        return;
    }
    if (!value.isObject()) return;

    const QJsonObject object = value.toObject();
    for (const QString &key : { QStringLiteral("b64_json"), QStringLiteral("base64"),
                                QStringLiteral("image_base64") }) {
        const QString encoded = object.value(key).toString().trimmed();
        if (!encoded.isEmpty()) result.base64 = encoded;
    }
    const QString directResult = object.value(QStringLiteral("result")).toString().trimmed();
    if (!directResult.isEmpty()) result.base64 = directResult;
    const QString partial = object.value(QStringLiteral("partial_image_b64")).toString().trimmed();
    if (!partial.isEmpty()) result.partialBase64 = partial;

    const QString format = object.value(QStringLiteral("output_format")).toString(
        object.value(QStringLiteral("format")).toString()).trimmed();
    if (!format.isEmpty()) result.format = format;
    const QString mimeType = object.value(QStringLiteral("mime_type")).toString().trimmed();
    if (result.format.isEmpty() && mimeType.startsWith(QStringLiteral("image/"))) {
        result.format = mimeType.mid(QStringLiteral("image/").size());
    }
    const QString revisedPrompt = object.value(QStringLiteral("revised_prompt")).toString().trimmed();
    if (!revisedPrompt.isEmpty()) result.revisedPrompt = revisedPrompt;

    const QJsonValue errorValue = object.value(QStringLiteral("error"));
    if (errorValue.isObject()) {
        result.error = errorValue.toObject().value(QStringLiteral("message")).toString(
            errorValue.toObject().value(QStringLiteral("detail")).toString()).trimmed();
    } else if (errorValue.isString()) {
        result.error = errorValue.toString().trimmed();
    }
    const QString eventType = object.value(QStringLiteral("type")).toString().toLower();
    if (result.error.isEmpty() && eventType.contains(QStringLiteral("error"))) {
        result.error = object.value(QStringLiteral("message")).toString(
            object.value(QStringLiteral("detail")).toString()).trimmed();
    }

    for (const QString &key : { QStringLiteral("data"), QStringLiteral("response"),
                                QStringLiteral("output"), QStringLiteral("item"),
                                QStringLiteral("image") }) {
        const QJsonValue nested = object.value(key);
        if (!nested.isUndefined() && !nested.isNull()) collectImageResponse(nested, result);
    }
}

QString presentationContentText(const QJsonValue &value)
{
    if (value.isString()) return value.toString();
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        if (object.contains(QStringLiteral("slides"))) {
            return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
        }
        for (const QString &key : { QStringLiteral("text"), QStringLiteral("output_text"),
                                    QStringLiteral("content") }) {
            const QString nested = presentationContentText(object.value(key));
            if (!nested.isEmpty()) return nested;
        }
        return QString();
    }
    if (value.isArray()) {
        QString result;
        for (const QJsonValue &part : value.toArray()) {
            const QString text = presentationContentText(part);
            if (!text.isEmpty()) {
                if (!result.isEmpty()) result += QLatin1Char('\n');
                result += text;
            }
        }
        return result;
    }
    return QString();
}

QString firstBalancedJsonObject(const QString &text)
{
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    int start = -1;
    for (int index = 0; index < text.size(); ++index) {
        const QChar character = text.at(index);
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (character == QLatin1Char('\\')) {
                escaped = true;
            } else if (character == QLatin1Char('"')) {
                inString = false;
            }
            continue;
        }
        if (character == QLatin1Char('"')) {
            inString = true;
        } else if (character == QLatin1Char('{')) {
            if (depth == 0) start = index;
            ++depth;
        } else if (character == QLatin1Char('}') && depth > 0) {
            --depth;
            if (depth == 0 && start >= 0) return text.mid(start, index - start + 1);
        }
    }
    return QString();
}

bool validPresentationObject(const QJsonObject &candidate, QJsonObject &plan)
{
    QJsonObject normalized = candidate;
    for (const QString &key : { QStringLiteral("presentation"), QStringLiteral("plan"),
                                QStringLiteral("data") }) {
        const QJsonObject nested = normalized.value(key).toObject();
        if (!nested.isEmpty() && nested.contains(QStringLiteral("slides"))) {
            normalized = nested;
            break;
        }
    }
    const QJsonArray slides = normalized.value(QStringLiteral("slides")).toArray();
    if (slides.isEmpty()) return false;
    if (normalized.value(QStringLiteral("title")).toString().trimmed().isEmpty()) {
        normalized.insert(QStringLiteral("title"), slides.first().toObject()
            .value(QStringLiteral("title")).toString(QStringLiteral("演示文稿")));
    }
    plan = normalized;
    return true;
}

bool parsePresentationContent(const QJsonValue &contentValue,
                              QJsonObject &plan,
                              QString &error)
{
    if (contentValue.isObject()
            && validPresentationObject(contentValue.toObject(), plan)) {
        return true;
    }

    QString content = presentationContentText(contentValue).trimmed();
    content.remove(QChar::ByteOrderMark);
    QStringList candidates{ content };
    const QString balanced = firstBalancedJsonObject(content);
    if (!balanced.isEmpty() && balanced != content) candidates.append(balanced);

    QJsonParseError lastError;
    for (QString candidate : candidates) {
        for (int pass = 0; pass < 2; ++pass) {
            if (pass == 1) {
                candidate.replace(QRegularExpression(QStringLiteral(",\\s*([}\\]])")),
                                  QStringLiteral("\\1"));
            }
            const QJsonDocument document = QJsonDocument::fromJson(candidate.toUtf8(), &lastError);
            if (lastError.error == QJsonParseError::NoError && document.isObject()
                    && validPresentationObject(document.object(), plan)) {
                return true;
            }
        }
    }
    error = lastError.error == QJsonParseError::NoError
        ? QStringLiteral("缺少 slides 数组") : lastError.errorString();
    return false;
}

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
    m_authExpirationEmitted = false;
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

void ApiClient::getUsageStats(int days)
{
    const QDate endDate = QDate::currentDate();
    const QDate startDate = endDate.addDays(-qMax(1, days) + 1);
    const QString endpoint = QStringLiteral(
        "/api/v1/usage/stats?start_date=%1&end_date=%2&timezone=Asia/Shanghai")
        .arg(startDate.toString(Qt::ISODate), endDate.toString(Qt::ISODate));
    QNetworkReply *reply = get(endpoint);
    connect(reply, &QNetworkReply::finished, this, &ApiClient::onUsageStatsFinished);
}

void ApiClient::getUsageModels(int days)
{
    const QDate endDate = QDate::currentDate();
    const QDate startDate = endDate.addDays(-qMax(1, days) + 1);
    const QString endpoint = QStringLiteral(
        "/api/v1/usage/dashboard/models?start_date=%1&end_date=%2&model_source=requested&timezone=Asia/Shanghai")
        .arg(startDate.toString(Qt::ISODate), endDate.toString(Qt::ISODate));
    QNetworkReply *reply = get(endpoint);
    connect(reply, &QNetworkReply::finished, this, &ApiClient::onUsageModelsFinished);
}

void ApiClient::getApiKeyUsage(const QJsonArray &apiKeyIds)
{
    QJsonObject body;
    body.insert(QStringLiteral("api_key_ids"), apiKeyIds);
    QNetworkReply *reply = post(
        QStringLiteral("/api/v1/usage/dashboard/api-keys-usage"), body);
    connect(reply, &QNetworkReply::finished, this, &ApiClient::onApiKeyUsageFinished);
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
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(15000);
#endif

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

QNetworkReply* ApiClient::put(const QString &endpoint, const QJsonObject &data)
{
    QUrl url(m_baseUrl + endpoint);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(15000);
#endif
    if (!m_authToken.isEmpty()) {
        request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(m_authToken).toUtf8());
    }
    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(sslConfig);
    return m_networkManager->put(request, QJsonDocument(data).toJson(QJsonDocument::Compact));
}

QNetworkReply* ApiClient::deleteRequest(const QString &endpoint)
{
    QUrl url(m_baseUrl + endpoint);
    QNetworkRequest request(url);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(15000);
#endif
    if (!m_authToken.isEmpty()) {
        request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(m_authToken).toUtf8());
    }
    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(sslConfig);
    return m_networkManager->deleteResource(request);
}

QNetworkReply* ApiClient::get(const QString &endpoint, const QString &bearerToken)
{
    QUrl url(m_baseUrl + endpoint);
    QNetworkRequest request(url);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(15000);
#endif

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
        QString message = result.value(QStringLiteral("detail")).toString();
        if (message.isEmpty()) message = result.value(QStringLiteral("message")).toString();
        if (message.isEmpty()) {
            message = reply->errorString();
        }
        const int httpStatus = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString code = result.value(QStringLiteral("code")).toString();
        const QString combined = (code + QLatin1Char(' ') + message).toLower();
        if (httpStatus == 401 && !m_authToken.isEmpty()
                && !m_authExpirationEmitted
                && (combined.contains(QStringLiteral("token"))
                    || combined.contains(QStringLiteral("expired"))
                    || combined.contains(QStringLiteral("unauthorized")))) {
            m_authExpirationEmitted = true;
            emit authenticationExpired();
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
    m_authExpirationEmitted = false;
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

void ApiClient::onUsageStatsFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    bool ok = false;
    const QJsonObject response = parseResponse(reply, ok);
    reply->deleteLater();
    if (!ok) {
        emit requestFailed(response.value(QStringLiteral("error")).toString());
        return;
    }
    const int code = response.value(QStringLiteral("code")).toInt(0);
    if (code != 0 && code != 200) {
        emit requestFailed(response.value(QStringLiteral("message")).toString());
        return;
    }
    const QJsonValue data = response.value(QStringLiteral("data"));
    emit usageStatsReceived(data.isObject() ? data.toObject() : response);
}

void ApiClient::onUsageModelsFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    bool ok = false;
    const QJsonObject response = parseResponse(reply, ok);
    reply->deleteLater();
    if (!ok) {
        emit requestFailed(response.value(QStringLiteral("error")).toString());
        return;
    }
    const int code = response.value(QStringLiteral("code")).toInt(0);
    if (code != 0 && code != 200) {
        emit requestFailed(response.value(QStringLiteral("message")).toString());
        return;
    }
    const QJsonValue data = response.value(QStringLiteral("data"));
    QJsonArray models;
    if (data.isArray()) {
        models = data.toArray();
    } else if (data.isObject()) {
        const QJsonObject object = data.toObject();
        models = object.value(QStringLiteral("items")).toArray();
        if (models.isEmpty()) models = object.value(QStringLiteral("models")).toArray();
        if (models.isEmpty()) models = object.value(QStringLiteral("data")).toArray();
    }
    emit usageModelsReceived(models);
}

void ApiClient::onApiKeyUsageFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    bool ok = false;
    const QJsonObject response = parseResponse(reply, ok);
    reply->deleteLater();
    if (!ok) {
        emit requestFailed(response.value(QStringLiteral("error")).toString());
        return;
    }
    const QJsonObject data = response.value(QStringLiteral("data")).toObject();
    QJsonObject stats = data.value(QStringLiteral("stats")).toObject();
    if (stats.isEmpty()) stats = response.value(QStringLiteral("stats")).toObject();
    emit apiKeyUsageReceived(stats);
}

void ApiClient::getChannels()
{
    // 尝试 v1 API
    QNetworkReply *reply = get("/api/v1/channels?page=1&page_size=100");
    connect(reply, &QNetworkReply::finished, this, &ApiClient::onChannelsFinished);
}

void ApiClient::getGroups()
{
    QNetworkReply *reply = get(QStringLiteral("/api/v1/groups/available"));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        bool ok = false;
        const QJsonObject response = parseResponse(reply, ok);
        reply->deleteLater();
        if (!ok) {
            emit requestFailed(response.value(QStringLiteral("error")).toString());
            return;
        }
        const QJsonValue data = response.value(QStringLiteral("data"));
        QJsonArray groups;
        if (data.isArray()) groups = data.toArray();
        else if (data.isObject()) groups = data.toObject().value(QStringLiteral("items")).toArray();
        emit groupsReceived(groups);
    });
}

void ApiClient::changePassword(const QString &oldPassword, const QString &newPassword)
{
    QNetworkReply *reply = put(QStringLiteral("/api/v1/user/password"), QJsonObject{
        { QStringLiteral("old_password"), oldPassword },
        { QStringLiteral("new_password"), newPassword }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        bool ok = false;
        const QJsonObject response = parseResponse(reply, ok);
        reply->deleteLater();
        const int code = response.value(QStringLiteral("code")).toInt(0);
        if (!ok || (code != 0 && code != 200)) {
            const QString error = response.value(QStringLiteral("error")).toString(
                response.value(QStringLiteral("message")).toString(QStringLiteral("修改密码失败")));
            emit passwordChangeFailed(error);
            return;
        }
        emit passwordChanged();
    });
}

void ApiClient::redeemCode(const QString &code)
{
    QNetworkReply *reply = post(QStringLiteral("/api/v1/redeem"), QJsonObject{
        { QStringLiteral("code"), code }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        bool ok = false;
        const QJsonObject response = parseResponse(reply, ok);
        reply->deleteLater();
        const int code = response.value(QStringLiteral("code")).toInt(0);
        if (!ok || (code != 0 && code != 200)) {
            emit redeemFailed(response.value(QStringLiteral("error")).toString(
                response.value(QStringLiteral("message")).toString(QStringLiteral("兑换失败"))));
            return;
        }
        const QJsonValue data = response.value(QStringLiteral("data"));
        emit redeemCompleted(data.isObject() ? data.toObject() : response);
    });
}

void ApiClient::createApiKey(const QJsonObject &data)
{
    QNetworkReply *reply = post(QStringLiteral("/api/v1/keys"), data);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        bool ok = false;
        const QJsonObject response = parseResponse(reply, ok);
        reply->deleteLater();
        const int code = response.value(QStringLiteral("code")).toInt(0);
        if (!ok || (code != 0 && code != 200)) {
            emit apiKeyOperationFailed(QStringLiteral("create"),
                response.value(QStringLiteral("error")).toString(
                    response.value(QStringLiteral("message")).toString(QStringLiteral("创建 Key 失败"))));
            return;
        }
        emit apiKeyOperationCompleted(QStringLiteral("create"),
            response.value(QStringLiteral("data")).toObject());
    });
}

void ApiClient::updateApiKey(const QString &keyId, const QJsonObject &data)
{
    QNetworkReply *reply = put(QStringLiteral("/api/v1/keys/%1").arg(keyId), data);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        bool ok = false;
        const QJsonObject response = parseResponse(reply, ok);
        const QString action = reply->property("aegisyKeyAction").toString();
        reply->deleteLater();
        const int code = response.value(QStringLiteral("code")).toInt(0);
        if (!ok || (code != 0 && code != 200)) {
            emit apiKeyOperationFailed(action,
                response.value(QStringLiteral("error")).toString(
                    response.value(QStringLiteral("message")).toString(QStringLiteral("更新 Key 失败"))));
            return;
        }
        emit apiKeyOperationCompleted(action, response.value(QStringLiteral("data")).toObject());
    });
    reply->setProperty("aegisyKeyAction", QStringLiteral("update"));
}

void ApiClient::deleteApiKey(const QString &keyId)
{
    QNetworkReply *reply = deleteRequest(QStringLiteral("/api/v1/keys/%1").arg(keyId));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        bool ok = false;
        const QJsonObject response = parseResponse(reply, ok);
        reply->deleteLater();
        const int code = response.value(QStringLiteral("code")).toInt(0);
        if (!ok || (code != 0 && code != 200)) {
            emit apiKeyOperationFailed(QStringLiteral("delete"),
                response.value(QStringLiteral("error")).toString(
                    response.value(QStringLiteral("message")).toString(QStringLiteral("删除 Key 失败"))));
            return;
        }
        emit apiKeyOperationCompleted(QStringLiteral("delete"), QJsonObject());
    });
}

void ApiClient::getModels(const QString &apiKey)
{
    // 使用 OpenAI 兼容端点 /v1/models，Bearer 必须是 sk- 开头的 API Key
    QNetworkReply *reply = get("/v1/models", apiKey);
    connect(reply, &QNetworkReply::finished, this, &ApiClient::onModelsFinished);
}

void ApiClient::generateImage(const QString &apiKey,
                              const QString &model,
                              const QString &prompt,
                              const QString &size,
                              const QString &quality,
                              const QString &outputFormat)
{
    cancelImageGeneration();
    m_imageGenerationBuffer.clear();
    m_imageGenerationBase64.clear();
    m_imageGenerationPartialBase64.clear();
    m_imageGenerationFormat.clear();
    m_imageGenerationRevisedPrompt.clear();
    m_imageGenerationError.clear();

    QNetworkRequest request(QUrl(m_baseUrl + QStringLiteral("/v1/images/generations")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(apiKey).toUtf8());
    request.setRawHeader("Accept", "text/event-stream, application/json");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(20 * 60 * 1000);
#endif

    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(sslConfig);

    QJsonObject data;
    data[QStringLiteral("model")] = model;
    data[QStringLiteral("prompt")] = prompt;
    data[QStringLiteral("size")] = size;
    data[QStringLiteral("quality")] = quality;
    data[QStringLiteral("output_format")] = outputFormat;
    data[QStringLiteral("response_format")] = QStringLiteral("b64_json");
    data[QStringLiteral("stream")] = true;
    data[QStringLiteral("n")] = 1;

    m_imageGenerationReply = m_networkManager->post(
        request, QJsonDocument(data).toJson(QJsonDocument::Compact));
    m_imageGenerationReply->setProperty("aegisyImageOutputFormat", outputFormat);
    connect(m_imageGenerationReply, &QNetworkReply::readyRead, this, [this]() {
        if (!m_imageGenerationReply) return;
        m_imageGenerationBuffer.append(m_imageGenerationReply->readAll());
        const QString contentType = m_imageGenerationReply->header(
            QNetworkRequest::ContentTypeHeader).toString().toLower();
        if (contentType.contains(QStringLiteral("text/event-stream"))
                || m_imageGenerationBuffer.trimmed().startsWith("data:")) {
            processImageGenerationEvents(false);
        }
    });
    connect(m_imageGenerationReply, &QNetworkReply::finished,
            this, &ApiClient::onImageGenerationFinished);
}

void ApiClient::cancelImageGeneration()
{
    if (!m_imageGenerationReply) {
        return;
    }
    disconnect(m_imageGenerationReply, nullptr, this, nullptr);
    m_imageGenerationReply->abort();
    m_imageGenerationReply->deleteLater();
    m_imageGenerationReply = nullptr;
    m_imageGenerationBuffer.clear();
    m_imageGenerationBase64.clear();
    m_imageGenerationPartialBase64.clear();
    m_imageGenerationFormat.clear();
    m_imageGenerationRevisedPrompt.clear();
    m_imageGenerationError.clear();
}

void ApiClient::processImageGenerationPayload(const QByteArray &payload)
{
    const QByteArray trimmed = payload.trimmed();
    if (trimmed.isEmpty() || trimmed == "[DONE]") return;
    const QJsonDocument document = QJsonDocument::fromJson(trimmed);
    if (document.isNull()) return;

    ImageResponseData parsed;
    collectImageResponse(document.isObject() ? QJsonValue(document.object())
                                             : QJsonValue(document.array()), parsed);
    if (!parsed.base64.isEmpty()) m_imageGenerationBase64 = parsed.base64;
    if (!parsed.partialBase64.isEmpty()) m_imageGenerationPartialBase64 = parsed.partialBase64;
    if (!parsed.format.isEmpty()) m_imageGenerationFormat = parsed.format;
    if (!parsed.revisedPrompt.isEmpty()) m_imageGenerationRevisedPrompt = parsed.revisedPrompt;
    if (!parsed.error.isEmpty()) m_imageGenerationError = parsed.error;
}

void ApiClient::processImageGenerationEvents(bool flushTrailingData)
{
    while (true) {
        const int newline = m_imageGenerationBuffer.indexOf('\n');
        if (newline < 0) break;
        QByteArray line = m_imageGenerationBuffer.left(newline).trimmed();
        m_imageGenerationBuffer.remove(0, newline + 1);
        if (!line.startsWith("data:")) continue;
        processImageGenerationPayload(line.mid(5).trimmed());
    }
    if (flushTrailingData) {
        const QByteArray trailing = m_imageGenerationBuffer.trimmed();
        m_imageGenerationBuffer.clear();
        if (trailing.startsWith("data:")) {
            processImageGenerationPayload(trailing.mid(5).trimmed());
        } else if (!trailing.isEmpty()) {
            processImageGenerationPayload(trailing);
        }
    }
}

void ApiClient::sendChatMessage(const QString &requestId,
                                const QString &apiKey,
                                const QString &model,
                                const QJsonArray &messages)
{
    cancelChatMessage();
    m_chatRequestId = requestId;
    m_chatBuffer.clear();
    m_chatContent.clear();

    QNetworkRequest request(QUrl(m_baseUrl + QStringLiteral("/v1/chat/completions")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(apiKey).toUtf8());
    request.setRawHeader("Accept", "text/event-stream");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(10 * 60 * 1000);
#endif
    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(sslConfig);

    const QJsonObject body{
        { QStringLiteral("model"), model },
        { QStringLiteral("messages"), messages },
        { QStringLiteral("stream"), true },
        { QStringLiteral("stream_options"), QJsonObject{
            { QStringLiteral("include_usage"), true }
        }}
    };
    QNetworkReply *reply = m_networkManager->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    m_chatReply = reply;

    connect(reply, &QNetworkReply::readyRead, this, [this, reply, requestId]() {
        if (reply != m_chatReply) return;
        m_chatBuffer.append(reply->readAll());
        while (true) {
            const int newline = m_chatBuffer.indexOf('\n');
            if (newline < 0) break;
            QByteArray line = m_chatBuffer.left(newline).trimmed();
            m_chatBuffer.remove(0, newline + 1);
            if (!line.startsWith("data:")) continue;
            line = line.mid(5).trimmed();
            if (line == "[DONE]") continue;
            const QJsonObject event = QJsonDocument::fromJson(line).object();
            const QJsonObject usage = event.value(QStringLiteral("usage")).toObject();
            if (!usage.isEmpty()) {
                emit chatUsageReceived(
                    requestId,
                    usage.value(QStringLiteral("prompt_tokens")).toInt(),
                    usage.value(QStringLiteral("completion_tokens")).toInt(),
                    usage.value(QStringLiteral("total_tokens")).toInt());
            }
            const QJsonArray choices = event.value(QStringLiteral("choices")).toArray();
            const QJsonObject choice = choices.isEmpty() ? QJsonObject() : choices.at(0).toObject();
            const QJsonValue contentValue = choice.value(QStringLiteral("delta")).toObject()
                .value(QStringLiteral("content"));
            QString chunk;
            if (contentValue.isString()) {
                chunk = contentValue.toString();
            } else if (contentValue.isArray()) {
                for (const QJsonValue &part : contentValue.toArray()) {
                    const QJsonObject object = part.toObject();
                    chunk += object.value(QStringLiteral("text")).toString();
                }
            }
            if (!chunk.isEmpty()) {
                m_chatContent += chunk;
                emit chatChunkReceived(requestId, chunk);
            }
        }
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, requestId]() {
        if (reply != m_chatReply) {
            reply->deleteLater();
            return;
        }
        const QByteArray remaining = m_chatBuffer + reply->readAll();
        const bool canceled = reply->error() == QNetworkReply::OperationCanceledError;
        if (reply->error() != QNetworkReply::NoError && !canceled) {
            const QJsonObject errorObject = QJsonDocument::fromJson(remaining).object();
            QString error = errorObject.value(QStringLiteral("detail")).toString();
            if (error.isEmpty()) error = errorObject.value(QStringLiteral("message")).toString();
            if (error.isEmpty()) {
                error = errorObject.value(QStringLiteral("error")).toObject()
                    .value(QStringLiteral("message")).toString();
            }
            if (error.isEmpty()) error = reply->errorString();
            emit chatFailed(requestId, error);
        } else if (!canceled) {
            if (m_chatContent.isEmpty() && !remaining.trimmed().isEmpty()) {
                const QJsonObject response = QJsonDocument::fromJson(remaining).object();
                const QJsonObject usage = response.value(QStringLiteral("usage")).toObject();
                if (!usage.isEmpty()) {
                    emit chatUsageReceived(
                        requestId,
                        usage.value(QStringLiteral("prompt_tokens")).toInt(),
                        usage.value(QStringLiteral("completion_tokens")).toInt(),
                        usage.value(QStringLiteral("total_tokens")).toInt());
                }
                const QJsonArray choices = response.value(QStringLiteral("choices")).toArray();
                const QJsonObject message = choices.isEmpty() ? QJsonObject()
                    : choices.at(0).toObject().value(QStringLiteral("message")).toObject();
                m_chatContent = message.value(QStringLiteral("content")).toString();
                if (!m_chatContent.isEmpty()) {
                    emit chatChunkReceived(requestId, m_chatContent);
                }
            }
            emit chatCompleted(requestId, m_chatContent);
        }
        reply->deleteLater();
        m_chatReply = nullptr;
        m_chatBuffer.clear();
        m_chatContent.clear();
        m_chatRequestId.clear();
    });
}

void ApiClient::cancelChatMessage()
{
    if (!m_chatReply) return;
    disconnect(m_chatReply, nullptr, this, nullptr);
    m_chatReply->abort();
    m_chatReply->deleteLater();
    m_chatReply = nullptr;
    m_chatBuffer.clear();
    m_chatContent.clear();
    m_chatRequestId.clear();
}

void ApiClient::requestPresentationPlan(const QString &requestId,
                                        const QString &apiKey,
                                        const QString &model,
                                        const QString &requestText)
{
    requestPresentationPlanAttempt(requestId, apiKey, model, requestText,
                                   QString(), 0, true);
}

void ApiClient::requestPresentationPlanAttempt(const QString &requestId,
                                               const QString &apiKey,
                                               const QString &model,
                                               const QString &requestText,
                                               const QString &invalidContent,
                                               int attempt,
                                               bool structuredOutput,
                                               int exhaustedRetries)
{
    QNetworkRequest request(QUrl(m_baseUrl + QStringLiteral("/v1/chat/completions")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(apiKey).toUtf8());
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(2 * 60 * 1000);
#endif
    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(sslConfig);

    const QString systemPrompt = attempt == 0 ? QStringLiteral(
        "你是 Aegisy 演示文稿规划器。只输出一个合法 JSON 对象，不要 Markdown、解释或代码围栏。"
        "根字段必须包含 title、subtitle、theme、slides。theme 只能是 editorial 或 swiss。"
        "每个 slide 包含 layout、title，并按布局使用 subtitle、kicker、bullets、columns、metrics、quote、notes。"
        "layout 只能是 cover、section、statement、bullets、comparison、process、metrics、closing。"
        "comparison 的 columns 是两个 {title, bullets}；metrics 是 2 到 4 个 {value,label,note}。"
        "生成 6 到 12 页，形成钩子、背景、主体、转折、结论的叙事弧。"
        "每页正文最多 5 个要点，每个要点不超过 32 个中文字符；不要编造数据。")
        : QStringLiteral(
        "你是 JSON 结构修复器。把用户提供的无效演示文稿内容修复为一个合法 JSON 对象。"
        "保留原意，只修复引号、逗号、括号、字段类型和 slides 结构。"
        "只输出 JSON，不要 Markdown 或解释。");
    const QString userPrompt = attempt == 0 ? requestText : QStringLiteral(
        "原始需求：\n%1\n\n无效输出：\n%2")
        .arg(requestText, invalidContent.left(16000));
    QJsonObject body{
        { QStringLiteral("model"), model },
        { QStringLiteral("stream"), false },
        { QStringLiteral("messages"), QJsonArray{
            QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                        {QStringLiteral("content"), systemPrompt}},
            QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                        {QStringLiteral("content"), userPrompt}}
        }}
    };
    if (structuredOutput) {
        body.insert(QStringLiteral("response_format"), QJsonObject{
            { QStringLiteral("type"), QStringLiteral("json_object") }
        });
    }
    QNetworkReply *reply = m_networkManager->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, requestId, apiKey, model, requestText, invalidContent,
             attempt, structuredOutput, exhaustedRetries]() {
        const QByteArray data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            const QJsonObject root = QJsonDocument::fromJson(data).object();
            QString message = root.value(QStringLiteral("message")).toString();
            if (message.isEmpty()) message = root.value(QStringLiteral("error")).toObject()
                .value(QStringLiteral("message")).toString();
            if (message.isEmpty()) message = reply->errorString();
            const QString normalized = message.toLower();
            if (structuredOutput
                    && (normalized.contains(QStringLiteral("response_format"))
                        || normalized.contains(QStringLiteral("json_object"))
                        || normalized.contains(QStringLiteral("structured output")))) {
                reply->deleteLater();
                requestPresentationPlanAttempt(requestId, apiKey, model, requestText,
                                               invalidContent, attempt, false,
                                               exhaustedRetries);
                return;
            }
            // 账号池被占满/额度耗尽属于瞬时错误：退避后自动重试，最多 2 次。
            const bool exhausted = normalized.contains(QStringLiteral("exhausted"))
                || normalized.contains(QStringLiteral("no available"))
                || normalized.contains(QStringLiteral("all available accounts"));
            if (exhausted && exhaustedRetries < 2) {
                reply->deleteLater();
                const int delayMs = (exhaustedRetries + 1) * 2500;  // 2.5s、5s
                QTimer::singleShot(delayMs, this,
                    [this, requestId, apiKey, model, requestText, invalidContent,
                     attempt, structuredOutput, exhaustedRetries]() {
                    requestPresentationPlanAttempt(requestId, apiKey, model, requestText,
                                                   invalidContent, attempt, structuredOutput,
                                                   exhaustedRetries + 1);
                });
                return;
            }
            emit presentationPlanFailed(requestId, message);
            reply->deleteLater();
            return;
        }
        const QJsonObject root = QJsonDocument::fromJson(data).object();
        const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
        QJsonValue contentValue;
        if (!choices.isEmpty()) {
            contentValue = choices.at(0).toObject().value(QStringLiteral("message")).toObject()
                .value(QStringLiteral("content"));
        } else if (root.contains(QStringLiteral("slides"))) {
            contentValue = root;
        } else {
            contentValue = root.value(QStringLiteral("output_text"));
        }
        QJsonObject plan;
        QString parseError;
        if (!parsePresentationContent(contentValue, plan, parseError)) {
            const QString rawContent = presentationContentText(contentValue).trimmed();
            if (attempt == 0 && !rawContent.isEmpty()) {
                reply->deleteLater();
                requestPresentationPlanAttempt(requestId, apiKey, model, requestText,
                                               rawContent, attempt + 1, structuredOutput,
                                               exhaustedRetries);
                return;
            }
            emit presentationPlanFailed(
                requestId, QStringLiteral("模型没有返回有效的 PPT 结构：%1")
                    .arg(parseError));
        } else {
            emit presentationPlanReceived(requestId, plan);
        }
        reply->deleteLater();
    });
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

void ApiClient::testConnection(const QString &requestId,
                               const QString &apiKey,
                               const QString &model)
{
    QNetworkReply *reply = get(QStringLiteral("/v1/models"), apiKey);
    auto *timer = new QElapsedTimer();
    timer->start();
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, requestId, model, timer]() {
        const int latencyMs = static_cast<int>(timer->elapsed());
        delete timer;
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const QString networkError = reply->errorString();
        reply->deleteLater();

        const QJsonDocument document = QJsonDocument::fromJson(body);
        const QJsonObject response = document.isObject()
            ? document.object() : QJsonObject();
        if (http == 200) {
            const QJsonArray models = response.value(QStringLiteral("data")).toArray();
            if (!model.trimmed().isEmpty()) {
                bool found = false;
                for (const QJsonValue &value : models) {
                    const QString id = value.isObject()
                        ? value.toObject().value(QStringLiteral("id")).toString()
                        : value.toString();
                    if (id == model.trimmed()) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    emit connectionTested(
                        requestId, false,
                        QStringLiteral("Key 可用，但模型「%1」不在该 Key 的模型列表中。")
                            .arg(model.trimmed()),
                        latencyMs);
                    return;
                }
            }
            emit connectionTested(
                requestId, true,
                model.trimmed().isEmpty()
                    ? QStringLiteral("连接成功，可用模型 %1 个。 ").arg(models.size()).trimmed()
                    : QStringLiteral("连接成功，模型「%1」可用。 ").arg(model.trimmed()).trimmed(),
                latencyMs);
            return;
        }

        QString code = response.value(QStringLiteral("code")).toString();
        QString message = response.value(QStringLiteral("message")).toString();
        const QJsonValue errorValue = response.value(QStringLiteral("error"));
        if (errorValue.isObject()) {
            const QJsonObject error = errorValue.toObject();
            if (code.isEmpty()) code = error.value(QStringLiteral("code")).toString();
            if (message.isEmpty()) message = error.value(QStringLiteral("message")).toString();
        }
        const QString combined = (code + QLatin1Char(' ') + message).toLower();
        QString category;
        if (http == 401 || http == 403 || combined.contains(QStringLiteral("key"))) {
            category = QStringLiteral("API Key 无效或无权限");
        } else if (combined.contains(QStringLiteral("balance"))
                   || combined.contains(QStringLiteral("quota"))
                   || combined.contains(QStringLiteral("余额"))) {
            category = QStringLiteral("余额或配额不足");
        } else if (combined.contains(QStringLiteral("model"))
                   || combined.contains(QStringLiteral("模型"))) {
            category = QStringLiteral("模型不可用");
        } else if (combined.contains(QStringLiteral("channel"))
                   || combined.contains(QStringLiteral("渠道"))) {
            category = QStringLiteral("当前没有可用渠道");
        } else {
            category = http > 0
                ? QStringLiteral("请求失败（HTTP %1）").arg(http)
                : QStringLiteral("网络连接失败");
        }
        if (message.isEmpty()) {
            message = networkError;
        }
        emit connectionTested(requestId, false,
                              message.isEmpty() ? category
                                                : QStringLiteral("%1：%2").arg(category, message),
                              latencyMs);
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

void ApiClient::onImageGenerationFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || reply != m_imageGenerationReply) {
        return;
    }
    m_imageGenerationReply = nullptr;

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString requestedFormat = reply->property("aegisyImageOutputFormat").toString();
    m_imageGenerationBuffer.append(reply->readAll());
    const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader)
        .toString().toLower();
    const bool streamed = contentType.contains(QStringLiteral("text/event-stream"))
        || m_imageGenerationBuffer.trimmed().startsWith("data:")
        || !m_imageGenerationBase64.isEmpty()
        || !m_imageGenerationPartialBase64.isEmpty();
    if (streamed) {
        processImageGenerationEvents(true);
    } else if (!m_imageGenerationBuffer.trimmed().isEmpty()) {
        processImageGenerationPayload(m_imageGenerationBuffer);
        m_imageGenerationBuffer.clear();
    }
    const QString networkError = reply->errorString();
    const QNetworkReply::NetworkError replyError = reply->error();
    reply->deleteLater();

    if (replyError != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300) {
        QString message = m_imageGenerationError;
        if (message.isEmpty()) {
            message = httpStatus > 0
                ? QStringLiteral("HTTP %1").arg(httpStatus)
                : networkError;
        }
        emit imageGenerationFailed(message);
        return;
    }

    QString encoded = m_imageGenerationBase64.isEmpty()
        ? m_imageGenerationPartialBase64 : m_imageGenerationBase64;
    QString format = m_imageGenerationFormat;
    if (encoded.startsWith(QStringLiteral("data:image/"))) {
        const int separator = encoded.indexOf(QStringLiteral(";base64,"));
        if (separator > 11) {
            if (format.isEmpty()) format = encoded.mid(11, separator - 11);
            encoded = encoded.mid(separator + QStringLiteral(";base64,").size());
        }
    }
    const QByteArray imageData = QByteArray::fromBase64(encoded.toLatin1());
    if (imageData.isEmpty()) {
        emit imageGenerationFailed(m_imageGenerationError.isEmpty()
            ? QStringLiteral("服务器未返回可解码的图片数据。")
            : m_imageGenerationError);
        return;
    }

    if (format.isEmpty()) {
        format = requestedFormat.isEmpty() ? QStringLiteral("png") : requestedFormat;
    }
    emit imageGenerated(imageData, format, m_imageGenerationRevisedPrompt);
}
