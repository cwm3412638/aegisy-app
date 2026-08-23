#include "api_client.h"
#include "companion_config_projection.h"
#include "companion_credential_broker.h"
#include "companion_model_projection.h"
#include "companion_usage_projection.h"
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSslConfiguration>
#include <QUrl>
#include <QElapsedTimer>
#include <QSet>
#include <QTimer>
#include <QDate>

#include <cmath>

namespace {

constexpr int kApiKeyPageSize = 100;
constexpr int kMaxApiKeyPages = 100;
constexpr int kMaxProjectedApiKeys = 1000;
constexpr qint64 kMaxApiKeyResponseBytes = 1024 * 1024;
constexpr qint64 kMaxUserInfoResponseBytes = 256 * 1024;
constexpr qint64 kMaxCompanionModelResponseBytes = 1024 * 1024;
constexpr qint64 kMaxCompanionUsageResponseBytes = 1024 * 1024;

bool validPrefixedLowerSha256(const QString &value, const QString &prefix)
{
    if (!value.startsWith(prefix) || value.size() != prefix.size() + 64) return false;
    for (const QChar character : value.mid(prefix.size())) {
        if (!character.isDigit()
                && !(character >= QLatin1Char('a') && character <= QLatin1Char('f'))) {
            return false;
        }
    }
    return true;
}

bool validAuthorizationCredential(const QString &credential)
{
    const QByteArray utf8 = credential.toUtf8();
    if (utf8.isEmpty() || utf8.size() > 16 * 1024) return false;
    for (const QChar character : credential) {
        if (character.isNull() || character.category() == QChar::Other_Control
                || character.category() == QChar::Other_Surrogate) {
            return false;
        }
    }
    return true;
}

bool validGraphicalText(const QString &value, int maximumBytes)
{
    if (value.isEmpty() || value.toUtf8().size() > maximumBytes) return false;
    for (const QChar character : value) {
        if (character.isNull() || character.category() == QChar::Other_Control
                || character.category() == QChar::Other_Surrogate) {
            return false;
        }
    }
    return true;
}

QString rawWebsiteIdentifier(const QJsonValue &value)
{
    if (value.isString()) return value.toString().trimmed();
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (std::isfinite(number) && number >= 0.0
                && std::floor(number) == number
                && number <= 9007199254740991.0) {
            return QString::number(static_cast<qulonglong>(number));
        }
    }
    return {};
}

double boundedNonnegativeMetric(const QJsonValue &value)
{
    const double metric = value.toDouble(0.0);
    return std::isfinite(metric) && metric >= 0.0
            && metric <= 9007199254740991.0 ? metric : 0.0;
}

bool validOptionalProviderMetric(const QJsonValue &value)
{
    return value.isUndefined() || value.isNull()
        || (value.isDouble() && std::isfinite(value.toDouble())
            && value.toDouble() >= 0.0
            && value.toDouble() <= 9007199254740991.0);
}

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
    if (url != m_baseUrl) {
        retireCompanionModelRequests(QStringLiteral("companion-model-origin-changed"));
        retireCompanionUsageRequests(QStringLiteral("companion-usage-origin-changed"));
        retireCompanionOperationRequests(
            QStringLiteral("companion-operation-origin-changed"));
        ++m_authGeneration;
        ++m_apiKeyGeneration;
        m_apiKeyAccumulator = QJsonArray();
        m_verifiedCompanionAccountIdentity.clear();
        m_verifiedAccountAuthGeneration = 0;
        m_currentCompanionProjection = QJsonObject();
        m_companionUsageSources.clear();
    }
    m_baseUrl = url;
}

void ApiClient::setAuthToken(const QString &token)
{
    if (token != m_authToken) {
        retireCompanionModelRequests(QStringLiteral("companion-model-auth-changed"));
        retireCompanionUsageRequests(QStringLiteral("companion-usage-auth-changed"));
        retireCompanionOperationRequests(
            QStringLiteral("companion-operation-auth-changed"));
        ++m_authGeneration;
        ++m_apiKeyGeneration;
        m_apiKeyAccumulator = QJsonArray();
        m_verifiedCompanionAccountIdentity.clear();
        m_verifiedAccountAuthGeneration = 0;
        m_currentCompanionProjection = QJsonObject();
        m_companionUsageSources.clear();
    }
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
    if (m_authToken.isEmpty()
            || m_verifiedCompanionAccountIdentity.isEmpty()
            || m_verifiedAccountAuthGeneration != m_authGeneration) {
        emit companionConfigurationFailed(QStringLiteral("projection-account-unverified"));
        return;
    }
    if (!CompanionConfigProjection::isTrustedWebsiteOrigin(m_baseUrl)) {
        emit companionConfigurationFailed(QStringLiteral("projection-origin-untrusted"));
        return;
    }
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
    const QUrl url(m_baseUrl + endpoint);
    QNetworkRequest request(url);
    request.setRawHeader("Authorization",
                         QStringLiteral("Bearer %1").arg(m_authToken).toUtf8());
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(15000);
#endif
    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(sslConfig);
    QNetworkReply *reply = m_networkManager->get(request);
    reply->setProperty("aegisyApiKeyGeneration", generation);
    reply->setProperty("aegisyApiKeyPage", page);
    reply->setProperty("aegisyApiKeyAuthGeneration",
                       QVariant::fromValue<qulonglong>(m_authGeneration));
    reply->setProperty("aegisyApiKeyExpectedUrl", url.toString(QUrl::FullyEncoded));
    reply->setProperty("aegisyApiKeySourceOrigin", m_baseUrl);
    reply->setProperty("aegisyApiKeyOverflow", false);
    connect(reply, &QNetworkReply::readyRead, this, [reply]() {
        if (reply->bytesAvailable() > kMaxApiKeyResponseBytes) {
            reply->setProperty("aegisyApiKeyOverflow", true);
            reply->abort();
        }
    });
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

void ApiClient::getCompanionApiKeyUsage(
    const QString &requestId, const QString &accountIdentity,
    const QString &projectionSha256)
{
    bool sourcesMatch = m_companionUsageSources.size()
        == m_currentCompanionProjection.value(QStringLiteral("keys")).toArray().size();
    const QJsonArray projectedKeys = m_currentCompanionProjection.value(
        QStringLiteral("keys")).toArray();
    QJsonArray rawIds;
    if (sourcesMatch) {
        for (int index = 0; index < m_companionUsageSources.size(); ++index) {
            const CompanionUsageSource &source = m_companionUsageSources.at(index);
            if (source.keyIdentity != projectedKeys.at(index).toObject().value(
                    QStringLiteral("key_identity")).toString()
                    || source.rawLookupKey.isEmpty()) {
                sourcesMatch = false;
                break;
            }
            rawIds.append(source.rawKeyId);
        }
    }
    if (!validGraphicalText(requestId, 128)
            || m_pendingCompanionUsageRequests.contains(requestId)
            || accountIdentity != m_verifiedCompanionAccountIdentity
            || accountIdentity != m_currentCompanionProjection.value(
                QStringLiteral("account_identity")).toString()
            || projectionSha256 != m_currentCompanionProjection.value(
                QStringLiteral("projection_sha256")).toString()
            || m_verifiedAccountAuthGeneration != m_authGeneration
            || m_currentCompanionProjection.value(QStringLiteral("source_origin")).toString()
                != m_baseUrl
            || !CompanionConfigProjection::validate(m_currentCompanionProjection)
            || !CompanionConfigProjection::isTrustedWebsiteOrigin(m_baseUrl)
            || !sourcesMatch) {
        emit companionApiKeyUsageFailed(
            requestId, QStringLiteral("companion-usage-binding-invalid"));
        return;
    }

    PendingCompanionUsageRequest pending;
    pending.accountIdentity = accountIdentity;
    pending.projectionSha256 = projectionSha256;
    pending.authGeneration = m_authGeneration;
    pending.sources = m_companionUsageSources;
    m_pendingCompanionUsageRequests.insert(requestId, pending);

    const QUrl url(m_baseUrl
        + QStringLiteral("/api/v1/usage/dashboard/api-keys-usage"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization",
                         QStringLiteral("Bearer %1").arg(m_authToken).toUtf8());
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(15000);
#endif
    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(sslConfig);
    const QJsonObject body{{QStringLiteral("api_key_ids"), rawIds}};
    QNetworkReply *reply = m_networkManager->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    reply->setProperty("aegisyCompanionUsageRequestId", requestId);
    reply->setProperty("aegisyCompanionUsageExpectedUrl", url.toString(QUrl::FullyEncoded));
    reply->setProperty("aegisyCompanionUsageOverflow", false);
    connect(reply, &QNetworkReply::readyRead, this, [reply]() {
        if (reply->bytesAvailable() > kMaxCompanionUsageResponseBytes) {
            reply->setProperty("aegisyCompanionUsageOverflow", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished,
            this, &ApiClient::onCompanionApiKeyUsageFinished);
}

void ApiClient::getWorkbenchEmergencyPolicy()
{
    if (m_authToken.isEmpty() || m_authToken.size() > 16 * 1024
        || m_authToken.contains(QLatin1Char('\r'))
        || m_authToken.contains(QLatin1Char('\n'))) {
        emit workbenchEmergencyPolicyFailed(QStringLiteral("policy-auth-unavailable"));
        return;
    }
    const QUrl base(m_baseUrl);
    if (!base.isValid() || base.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0
        || base.host().isEmpty() || !base.userInfo().isEmpty()
        || (!base.path().isEmpty() && base.path() != QStringLiteral("/"))
        || base.hasQuery() || !base.fragment().isEmpty()) {
        emit workbenchEmergencyPolicyFailed(QStringLiteral("policy-origin-untrusted"));
        return;
    }
    QUrl url = base;
    url.setPath(QStringLiteral("/api/v1/client/workbench-policy"));
    QNetworkRequest request(url);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(15000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
#endif
    request.setRawHeader("Authorization",
                         QStringLiteral("Bearer %1").arg(m_authToken).toUtf8());
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(sslConfig);
    QNetworkReply *reply = m_networkManager->get(request);
    reply->setProperty("aegisyEmergencyPolicyOverflow", false);
    connect(reply, &QNetworkReply::readyRead, this, [reply]() {
        if (reply->bytesAvailable() > 16 * 1024) {
            reply->setProperty("aegisyEmergencyPolicyOverflow", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const bool overflow = reply->property("aegisyEmergencyPolicyOverflow").toBool();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader)
            .toString().section(QLatin1Char(';'), 0, 0).trimmed().toLower();
        const QUrl redirect = reply->attribute(
            QNetworkRequest::RedirectionTargetAttribute).toUrl();
        const QByteArray bytes = reply->readAll();
        if (overflow || bytes.size() > 16 * 1024) {
            emit workbenchEmergencyPolicyFailed(QStringLiteral("policy-response-too-large"));
        } else if (reply->error() != QNetworkReply::NoError
                   || status < 200 || status >= 300 || !redirect.isEmpty()) {
            emit workbenchEmergencyPolicyFailed(QStringLiteral("policy-transport-failed"));
        } else if (contentType != QStringLiteral("application/json")) {
            emit workbenchEmergencyPolicyFailed(QStringLiteral("policy-response-invalid"));
        } else {
            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
            const QJsonObject response = document.isObject() ? document.object() : QJsonObject{};
            const int code = response.value(QStringLiteral("code")).toInt(-1);
            const QJsonObject policy = response.value(QStringLiteral("data")).toObject();
            if (error.error != QJsonParseError::NoError || code != 0 || policy.isEmpty()) {
                emit workbenchEmergencyPolicyFailed(QStringLiteral("policy-response-invalid"));
            } else {
                emit workbenchEmergencyPolicyReceived(policy);
            }
        }
        reply->deleteLater();
    });
}

void ApiClient::requestUserInfo(const QString &endpoint)
{
    const QUrl url(m_baseUrl + endpoint);
    QNetworkRequest request(url);
    if (!m_authToken.isEmpty()) {
        request.setRawHeader("Authorization",
                             QStringLiteral("Bearer %1").arg(m_authToken).toUtf8());
    }
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(15000);
#endif
    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(sslConfig);
    QNetworkReply *reply = m_networkManager->get(request);
    reply->setProperty("aegisyUserInfoEndpoint", endpoint);
    reply->setProperty("aegisyUserInfoAuthGeneration",
                       QVariant::fromValue<qulonglong>(m_authGeneration));
    reply->setProperty("aegisyUserInfoExpectedUrl", url.toString(QUrl::FullyEncoded));
    reply->setProperty("aegisyUserInfoSourceOrigin", m_baseUrl);
    reply->setProperty("aegisyUserInfoOverflow", false);
    connect(reply, &QNetworkReply::readyRead, this, [reply]() {
        if (reply->bytesAvailable() > kMaxUserInfoResponseBytes) {
            reply->setProperty("aegisyUserInfoOverflow", true);
            reply->abort();
        }
    });
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

    setAuthToken(token);
    emit loginSuccess(token, data);

    reply->deleteLater();
}

void ApiClient::onApiKeysFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    const int generation = reply->property("aegisyApiKeyGeneration").toInt();
    const int page = reply->property("aegisyApiKeyPage").toInt();
    const quint64 authGeneration = reply->property(
        "aegisyApiKeyAuthGeneration").toULongLong();
    if (generation != m_apiKeyGeneration || authGeneration != m_authGeneration) {
        reply->deleteLater();
        return;
    }

    const QUrl expectedUrl(reply->property("aegisyApiKeyExpectedUrl").toString());
    const QUrl redirect = reply->attribute(
        QNetworkRequest::RedirectionTargetAttribute).toUrl();
    const int httpStatus = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString contentType = reply->header(
        QNetworkRequest::ContentTypeHeader).toString().section(QLatin1Char(';'), 0, 0)
        .trimmed().toLower();
    if (reply->property("aegisyApiKeyOverflow").toBool()
            || !redirect.isEmpty() || reply->url() != expectedUrl
            || (httpStatus >= 200 && httpStatus < 300
                && contentType != QStringLiteral("application/json"))) {
        m_apiKeyAccumulator = QJsonArray();
        emit companionConfigurationFailed(QStringLiteral("projection-response-untrusted"));
        emit requestFailed(QStringLiteral("网站配置响应未通过安全校验"));
        reply->deleteLater();
        return;
    }

    bool ok;
    QJsonObject response = parseResponse(reply, ok);

    if (!ok) {
        m_apiKeyAccumulator = QJsonArray();
        emit companionConfigurationFailed(QStringLiteral("projection-transport-failed"));
        emit requestFailed(QStringLiteral("website-keys-transport-failed"));
        reply->deleteLater();
        return;
    }

    int code = response["code"].toInt(-1);
    if (code != 0 && code != 200) {
        m_apiKeyAccumulator = QJsonArray();
        emit companionConfigurationFailed(QStringLiteral("projection-response-invalid"));
        emit requestFailed(QStringLiteral("website-keys-response-invalid"));
        reply->deleteLater();
        return;
    }

    // 正确解析：data.items 才是实际的 keys 数组
    QJsonObject data = response["data"].toObject();
    QJsonArray keys = data["items"].toArray();

    for (const QJsonValue &key : keys) {
        m_apiKeyAccumulator.append(key);
    }
    if (m_apiKeyAccumulator.size() > kMaxProjectedApiKeys) {
        m_apiKeyAccumulator = QJsonArray();
        emit companionConfigurationFailed(QStringLiteral("projection-key-limit-exceeded"));
        emit requestFailed(QStringLiteral("website-keys-limit-exceeded"));
        reply->deleteLater();
        return;
    }

    const int total = data.value(QStringLiteral("total")).toInt(-1);
    const bool hasMoreByTotal = total >= 0 && m_apiKeyAccumulator.size() < total;
    const bool hasMoreByPageSize = total < 0 && keys.size() == kApiKeyPageSize;
    const bool hasMore = hasMoreByTotal || hasMoreByPageSize;
    if (hasMore && page < kMaxApiKeyPages) {
        reply->deleteLater();
        requestApiKeysPage(page + 1, generation);
        return;
    }

    if (hasMore) {
        emit companionConfigurationFailed(QStringLiteral("projection-page-limit-exceeded"));
        m_apiKeyAccumulator = QJsonArray();
    } else {
        QString projectionError;
        const QJsonObject projection = CompanionConfigProjection::fromWebsiteApiKeys(
            m_apiKeyAccumulator, m_verifiedCompanionAccountIdentity,
            reply->property("aegisyApiKeySourceOrigin").toString(),
            QDateTime::currentMSecsSinceEpoch(), &projectionError);
        if (projection.isEmpty()) {
            m_apiKeyAccumulator = QJsonArray();
            emit companionConfigurationFailed(
                projectionError.isEmpty()
                    ? QStringLiteral("projection-response-invalid")
                    : projectionError);
        } else {
            const QJsonObject stagedProjection = CompanionCredentialBroker::stage(
                m_apiKeyAccumulator, projection, &projectionError);
            if (stagedProjection.isEmpty()) {
                m_apiKeyAccumulator = QJsonArray();
                emit companionConfigurationFailed(
                    projectionError.isEmpty()
                        ? QStringLiteral("credential-broker-failed")
                        : projectionError);
                reply->deleteLater();
                return;
            }
            QList<CompanionUsageSource> usageSources;
            const QJsonArray projectedKeys = stagedProjection.value(
                QStringLiteral("keys")).toArray();
            for (int index = 0; index < m_apiKeyAccumulator.size(); ++index) {
                const QJsonObject raw = m_apiKeyAccumulator.at(index).toObject();
                CompanionUsageSource source;
                source.rawKeyId = raw.value(QStringLiteral("id"));
                source.rawLookupKey = rawWebsiteIdentifier(source.rawKeyId);
                source.keyIdentity = projectedKeys.at(index).toObject().value(
                    QStringLiteral("key_identity")).toString();
                source.quotaUsed = boundedNonnegativeMetric(
                    raw.value(QStringLiteral("quota_used")));
                source.quota = boundedNonnegativeMetric(
                    raw.value(QStringLiteral("quota")));
                if (source.rawLookupKey.isEmpty()) {
                    emit companionConfigurationFailed(
                        QStringLiteral("companion-usage-source-invalid"));
                    m_apiKeyAccumulator = QJsonArray();
                    reply->deleteLater();
                    return;
                }
                usageSources.append(source);
            }
            if (!m_currentCompanionProjection.isEmpty()
                    && m_currentCompanionProjection.value(
                        QStringLiteral("projection_sha256"))
                        != stagedProjection.value(QStringLiteral("projection_sha256"))) {
                retireCompanionUsageRequests(
                    QStringLiteral("companion-usage-projection-changed"));
                retireCompanionOperationRequests(
                    QStringLiteral("companion-operation-projection-changed"));
            }
            m_currentCompanionProjection = stagedProjection;
            m_companionUsageSources = usageSources;
            emit companionConfigurationReceived(stagedProjection);
            qDebug() << "Received" << m_apiKeyAccumulator.size() << "API keys";
            emit apiKeysReceived(m_apiKeyAccumulator);
            m_apiKeyAccumulator = QJsonArray();
        }
    }

    reply->deleteLater();
}

void ApiClient::onUserInfoFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    const QString endpoint = reply->property("aegisyUserInfoEndpoint").toString();
    const quint64 authGeneration = reply->property(
        "aegisyUserInfoAuthGeneration").toULongLong();
    if (authGeneration != m_authGeneration) {
        reply->deleteLater();
        return;
    }
    const QUrl expectedUrl(reply->property("aegisyUserInfoExpectedUrl").toString());
    const QUrl redirect = reply->attribute(
        QNetworkRequest::RedirectionTargetAttribute).toUrl();
    const int httpStatus = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString contentType = reply->header(
        QNetworkRequest::ContentTypeHeader).toString().section(QLatin1Char(';'), 0, 0)
        .trimmed().toLower();
    if (reply->property("aegisyUserInfoOverflow").toBool()
            || !redirect.isEmpty() || reply->url() != expectedUrl
            || (httpStatus >= 200 && httpStatus < 300
                && contentType != QStringLiteral("application/json"))) {
        emit companionConfigurationFailed(QStringLiteral("projection-account-response-untrusted"));
        emit requestFailed(QStringLiteral("账号响应未通过安全校验"));
        reply->deleteLater();
        return;
    }
    if (httpStatus == 404 && endpoint == QStringLiteral("/api/v1/auth/me")) {
        reply->deleteLater();
        requestUserInfo(QStringLiteral("/api/v1/user/profile"));
        return;
    }

    bool ok;
    QJsonObject response = parseResponse(reply, ok);

    if (!ok) {
        emit companionConfigurationFailed(QStringLiteral("projection-account-unavailable"));
        emit requestFailed(QStringLiteral("website-account-transport-failed"));
        reply->deleteLater();
        return;
    }

    int code = response["code"].toInt(-1);
    if (code != 0 && code != 200) {
        emit companionConfigurationFailed(QStringLiteral("projection-account-invalid"));
        emit requestFailed(QStringLiteral("website-account-response-invalid"));
        reply->deleteLater();
        return;
    }

    QJsonObject userInfo = response["data"].toObject();
    QJsonValue accountId = userInfo.value(QStringLiteral("id"));
    if (accountId.isUndefined() || accountId.isNull()) {
        accountId = userInfo.value(QStringLiteral("user_id"));
    }
    m_verifiedCompanionAccountIdentity =
        CompanionConfigProjection::isTrustedWebsiteOrigin(
            reply->property("aegisyUserInfoSourceOrigin").toString())
        ? CompanionConfigProjection::accountIdentityForWebsiteId(accountId)
        : QString();
    m_verifiedAccountAuthGeneration = m_verifiedCompanionAccountIdentity.isEmpty()
        ? 0 : m_authGeneration;
    if (m_verifiedCompanionAccountIdentity.isEmpty()) {
        emit companionConfigurationFailed(QStringLiteral("projection-account-invalid"));
    }
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

void ApiClient::onCompanionApiKeyUsageFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply) return;
    const QString requestId = reply->property(
        "aegisyCompanionUsageRequestId").toString();
    const auto pendingIt = m_pendingCompanionUsageRequests.constFind(requestId);
    if (pendingIt == m_pendingCompanionUsageRequests.cend()) {
        reply->deleteLater();
        return;
    }
    const PendingCompanionUsageRequest pending = pendingIt.value();
    const auto failRequest = [this, requestId](const QString &errorCode) {
        m_pendingCompanionUsageRequests.remove(requestId);
        emit companionApiKeyUsageFailed(requestId, errorCode);
    };
    if (pending.authGeneration != m_authGeneration
            || pending.accountIdentity != m_verifiedCompanionAccountIdentity
            || pending.accountIdentity != m_currentCompanionProjection.value(
                QStringLiteral("account_identity")).toString()
            || pending.projectionSha256 != m_currentCompanionProjection.value(
                QStringLiteral("projection_sha256")).toString()
            || m_currentCompanionProjection.value(QStringLiteral("source_origin")).toString()
                != m_baseUrl
            || !CompanionConfigProjection::validate(m_currentCompanionProjection)) {
        failRequest(QStringLiteral("companion-usage-request-stale"));
        reply->deleteLater();
        return;
    }
    const QUrl expectedUrl(reply->property(
        "aegisyCompanionUsageExpectedUrl").toString());
    const QUrl redirect = reply->attribute(
        QNetworkRequest::RedirectionTargetAttribute).toUrl();
    const int status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString contentType = reply->header(
        QNetworkRequest::ContentTypeHeader).toString().section(QLatin1Char(';'), 0, 0)
        .trimmed().toLower();
    const qint64 contentLength = reply->header(
        QNetworkRequest::ContentLengthHeader).toLongLong();
    const QByteArray contentEncoding = reply->rawHeader(
        "Content-Encoding").trimmed().toLower();
    if (reply->property("aegisyCompanionUsageOverflow").toBool()
            || !redirect.isEmpty() || reply->url() != expectedUrl
            || status < 200 || status >= 300
            || contentType != QStringLiteral("application/json")
            || contentLength > kMaxCompanionUsageResponseBytes
            || (!contentEncoding.isEmpty()
                && contentEncoding != QByteArrayLiteral("identity"))) {
        failRequest(QStringLiteral("companion-usage-response-untrusted"));
        reply->deleteLater();
        return;
    }
    const QByteArray body = reply->readAll();
    reply->deleteLater();
    if (body.size() > kMaxCompanionUsageResponseBytes) {
        failRequest(QStringLiteral("companion-usage-response-too-large"));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        failRequest(QStringLiteral("companion-usage-response-invalid"));
        return;
    }
    const QJsonObject response = document.object();
    const int code = response.value(QStringLiteral("code")).toInt(0);
    if (code != 0 && code != 200) {
        failRequest(QStringLiteral("companion-usage-response-failed"));
        return;
    }
    const QJsonObject data = response.value(QStringLiteral("data")).toObject();
    QJsonValue statsValue = data.value(QStringLiteral("stats"));
    if (statsValue.isUndefined()) statsValue = response.value(QStringLiteral("stats"));
    if (!statsValue.isObject()) {
        failRequest(QStringLiteral("companion-usage-response-invalid"));
        return;
    }
    const QJsonObject stats = statsValue.toObject();

    QSet<QString> expectedRawIds;
    QHash<QString, QJsonObject> metrics;
    for (const CompanionUsageSource &source : pending.sources) {
        expectedRawIds.insert(source.rawLookupKey);
        const QJsonValue rawValue = stats.value(source.rawLookupKey);
        if (!rawValue.isUndefined() && !rawValue.isObject()) {
            failRequest(QStringLiteral("companion-usage-response-metric-invalid"));
            return;
        }
        const QJsonObject raw = rawValue.toObject();
        if (!validOptionalProviderMetric(raw.value(QStringLiteral("today_actual_cost")))
                || !validOptionalProviderMetric(
                    raw.value(QStringLiteral("total_actual_cost")))) {
            failRequest(QStringLiteral("companion-usage-response-metric-invalid"));
            return;
        }
        metrics.insert(source.keyIdentity, QJsonObject{
            { QStringLiteral("today_actual_cost"), boundedNonnegativeMetric(
                raw.value(QStringLiteral("today_actual_cost"))) },
            { QStringLiteral("total_actual_cost"), boundedNonnegativeMetric(
                raw.value(QStringLiteral("total_actual_cost"))) },
            { QStringLiteral("quota_used"), source.quotaUsed },
            { QStringLiteral("quota"), source.quota },
        });
    }
    for (auto it = stats.constBegin(); it != stats.constEnd(); ++it) {
        if (!expectedRawIds.contains(it.key())) {
            failRequest(QStringLiteral("companion-usage-response-key-invalid"));
            return;
        }
    }
    QString projectionError;
    const QJsonObject projection = CompanionUsageProjection::fromConfiguration(
        m_currentCompanionProjection, metrics, &projectionError);
    if (projection.isEmpty()) {
        failRequest(projectionError.isEmpty()
            ? QStringLiteral("companion-usage-projection-invalid") : projectionError);
        return;
    }
    m_pendingCompanionUsageRequests.remove(requestId);
    emit companionApiKeyUsageReceived(requestId, projection);
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

bool ApiClient::companionBindingIsCurrent(
    const CompanionCredentialBinding &binding) const
{
    if (binding.isEmpty()
            || binding.authGeneration != m_authGeneration
            || m_verifiedAccountAuthGeneration != m_authGeneration
            || binding.accountIdentity != m_verifiedCompanionAccountIdentity
            || binding.accountIdentity != m_currentCompanionProjection.value(
                QStringLiteral("account_identity")).toString()
            || binding.projectionSha256 != m_currentCompanionProjection.value(
                QStringLiteral("projection_sha256")).toString()
            || m_currentCompanionProjection.value(QStringLiteral("source_origin")).toString()
                != m_baseUrl
            || !CompanionConfigProjection::validate(m_currentCompanionProjection)
            || !CompanionConfigProjection::isTrustedWebsiteOrigin(m_baseUrl)) {
        return false;
    }
    for (const QJsonValue &value : m_currentCompanionProjection.value(
         QStringLiteral("keys")).toArray()) {
        const QJsonObject candidate = value.toObject();
        if (candidate.value(QStringLiteral("key_identity")).toString()
                    == binding.keyIdentity
                && candidate.value(QStringLiteral("credential_handle")).toString()
                    == binding.credentialHandle
                && candidate.value(QStringLiteral("credential_state")).toString()
                    == QStringLiteral("available-in-secure-storage")
                && candidate.value(QStringLiteral("state")).toString()
                    == QStringLiteral("active")
                && candidate.value(QStringLiteral("platform")).toString()
                    == binding.platform) {
            return true;
        }
    }
    return false;
}

bool ApiClient::resolveCompanionCredential(
    const QString &requestId, const QString &accountIdentity,
    const QString &keyIdentity, const QString &credentialHandle,
    const QString &projectionSha256, const QString &platform,
    CompanionCredentialBinding *binding, QString *credential,
    QString *errorCode) const
{
    CompanionCredentialBinding candidate;
    candidate.requestId = requestId;
    candidate.accountIdentity = accountIdentity;
    candidate.keyIdentity = keyIdentity;
    candidate.credentialHandle = credentialHandle;
    candidate.projectionSha256 = projectionSha256;
    candidate.platform = platform;
    candidate.authGeneration = m_authGeneration;
    if (!validGraphicalText(requestId, 128)
            || !validPrefixedLowerSha256(
                keyIdentity, QStringLiteral("website-key:sha256:"))
            || !companionBindingIsCurrent(candidate)) {
        if (errorCode) {
            *errorCode = QStringLiteral("companion-operation-binding-invalid");
        }
        return false;
    }

    QString brokerError;
    const QString resolved = CompanionCredentialBroker::resolve(
        accountIdentity, keyIdentity, credentialHandle, &brokerError);
    if (!validAuthorizationCredential(resolved)) {
        if (errorCode) {
            *errorCode = brokerError.isEmpty()
                ? QStringLiteral("companion-operation-credential-unavailable")
                : brokerError;
        }
        return false;
    }
    if (binding) *binding = candidate;
    if (credential) *credential = resolved;
    if (errorCode) errorCode->clear();
    return true;
}

void ApiClient::sendCompanionChatMessage(
    const QString &requestId, const QString &accountIdentity,
    const QString &keyIdentity, const QString &credentialHandle,
    const QString &projectionSha256, const QString &platform,
    const QString &model, const QJsonArray &messages)
{
    CompanionCredentialBinding binding;
    QString credential;
    QString errorCode;
    if (!resolveCompanionCredential(
            requestId, accountIdentity, keyIdentity, credentialHandle,
            projectionSha256, platform, &binding, &credential, &errorCode)) {
        emit chatFailed(requestId, errorCode);
        return;
    }
    sendChatMessage(requestId, credential, model, messages);
    m_companionChatBinding = binding;
}

void ApiClient::generateCompanionImage(
    const QString &requestId, const QString &accountIdentity,
    const QString &keyIdentity, const QString &credentialHandle,
    const QString &projectionSha256, const QString &platform,
    const QString &model, const QString &prompt, const QString &size,
    const QString &quality, const QString &outputFormat)
{
    CompanionCredentialBinding binding;
    QString credential;
    QString errorCode;
    if (!resolveCompanionCredential(
            requestId, accountIdentity, keyIdentity, credentialHandle,
            projectionSha256, platform, &binding, &credential, &errorCode)) {
        emit companionImageFailed(requestId, errorCode);
        return;
    }
    generateImage(credential, model, prompt, size, quality, outputFormat);
    m_companionImageBinding = binding;
}

void ApiClient::requestCompanionPresentationPlan(
    const QString &requestId, const QString &accountIdentity,
    const QString &keyIdentity, const QString &credentialHandle,
    const QString &projectionSha256, const QString &platform,
    const QString &model, const QString &requestText)
{
    if (m_companionPresentationBindings.contains(requestId)) {
        emit presentationPlanFailed(
            requestId, QStringLiteral("companion-operation-request-conflict"));
        return;
    }
    CompanionCredentialBinding binding;
    QString credential;
    QString errorCode;
    if (!resolveCompanionCredential(
            requestId, accountIdentity, keyIdentity, credentialHandle,
            projectionSha256, platform, &binding, &credential, &errorCode)) {
        emit presentationPlanFailed(requestId, errorCode);
        return;
    }
    m_companionPresentationBindings.insert(requestId, binding);
    requestPresentationPlanAttempt(
        requestId, credential, model, requestText, QString(), 0, true, 0, true);
}

void ApiClient::retireCompanionOperationRequests(const QString &errorCode)
{
    if (!m_companionChatBinding.isEmpty()) {
        const QString requestId = m_companionChatBinding.requestId;
        cancelChatMessage();
        emit chatFailed(requestId, errorCode);
    }
    if (!m_companionImageBinding.isEmpty()) {
        const QString requestId = m_companionImageBinding.requestId;
        cancelImageGeneration();
        emit companionImageFailed(requestId, errorCode);
    }
    const QStringList presentationRequests =
        m_companionPresentationBindings.keys();
    m_companionPresentationBindings.clear();
    for (const QString &requestId : presentationRequests) {
        emit presentationPlanFailed(requestId, errorCode);
    }
}

void ApiClient::getCompanionModels(
    const QString &requestId, const QString &accountIdentity,
    const QString &keyIdentity, const QString &credentialHandle,
    const QString &projectionSha256, const QString &platform)
{
    const auto validGraphical = [](const QString &value, int maximum) {
        if (value.isEmpty() || value.toUtf8().size() > maximum) return false;
        for (const QChar character : value) {
            if (character.isNull() || character.category() == QChar::Other_Control
                    || character.category() == QChar::Other_Surrogate) {
                return false;
            }
        }
        return true;
    };
    bool candidateMatches = false;
    if (projectionSha256 == m_currentCompanionProjection.value(
            QStringLiteral("projection_sha256")).toString()
            && accountIdentity == m_currentCompanionProjection.value(
                QStringLiteral("account_identity")).toString()) {
        for (const QJsonValue &value : m_currentCompanionProjection.value(
             QStringLiteral("keys")).toArray()) {
            const QJsonObject candidate = value.toObject();
            if (candidate.value(QStringLiteral("key_identity")).toString() == keyIdentity
                    && candidate.value(QStringLiteral("credential_handle")).toString()
                        == credentialHandle
                    && candidate.value(QStringLiteral("credential_state")).toString()
                        == QStringLiteral("available-in-secure-storage")
                    && candidate.value(QStringLiteral("state")).toString()
                        == QStringLiteral("active")
                    && candidate.value(QStringLiteral("platform")).toString() == platform) {
                candidateMatches = true;
                break;
            }
        }
    }
    if (!validGraphical(requestId, 128)
            || accountIdentity != m_verifiedCompanionAccountIdentity
            || m_verifiedAccountAuthGeneration != m_authGeneration
            || !validPrefixedLowerSha256(
                keyIdentity, QStringLiteral("website-key:sha256:"))
            || !candidateMatches
            || !CompanionConfigProjection::isTrustedWebsiteOrigin(m_baseUrl)) {
        emit companionModelsFailed(
            requestId, keyIdentity, QStringLiteral("companion-model-binding-invalid"));
        return;
    }
    QString brokerError;
    const QString credential = CompanionCredentialBroker::resolve(
        accountIdentity, keyIdentity, credentialHandle, &brokerError);
    if (credential.isEmpty()) {
        emit companionModelsFailed(
            requestId, keyIdentity,
            brokerError.isEmpty()
                ? QStringLiteral("companion-model-credential-unavailable") : brokerError);
        return;
    }
    startCorrelatedModelRequest(
        requestId, accountIdentity, keyIdentity, credential,
        projectionSha256, platform, credentialHandle);
}

void ApiClient::getProfileModels(
    const QString &requestId, const QString &profileIdentity,
    const QString &credential)
{
    if (!validPrefixedLowerSha256(
            profileIdentity, QStringLiteral("local-profile:sha256:"))) {
        emit companionModelsFailed(
            requestId, profileIdentity, QStringLiteral("companion-model-binding-invalid"));
        return;
    }
    startCorrelatedModelRequest(requestId, QString(), profileIdentity, credential);
}

void ApiClient::startCorrelatedModelRequest(
    const QString &requestId, const QString &accountIdentity,
    const QString &keyIdentity, const QString &credential,
    const QString &projectionSha256, const QString &platform,
    const QString &credentialHandle)
{
    const auto validGraphical = [](const QString &value, int maximum) {
        if (value.isEmpty() || value.toUtf8().size() > maximum) return false;
        for (const QChar character : value) {
            if (character.isNull() || character.category() == QChar::Other_Control
                    || character.category() == QChar::Other_Surrogate) {
                return false;
            }
        }
        return true;
    };
    if (!validGraphical(requestId, 128) || !validGraphical(keyIdentity, 128)
            || !validAuthorizationCredential(credential)
            || m_pendingCompanionModelRequests.contains(requestId)
            || !CompanionConfigProjection::isTrustedWebsiteOrigin(m_baseUrl)) {
        emit companionModelsFailed(
            requestId, keyIdentity, QStringLiteral("companion-model-request-invalid"));
        return;
    }
    m_pendingCompanionModelRequests.insert(requestId, keyIdentity);

    const QUrl url(m_baseUrl + QStringLiteral("/v1/models"));
    QNetworkRequest request(url);
    request.setRawHeader("Authorization",
                         QStringLiteral("Bearer %1").arg(credential).toUtf8());
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(15000);
#endif
    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(sslConfig);
    QNetworkReply *reply = m_networkManager->get(request);
    reply->setProperty("aegisyCompanionModelRequestId", requestId);
    reply->setProperty("aegisyCompanionModelAccountIdentity", accountIdentity);
    reply->setProperty("aegisyCompanionModelKeyIdentity", keyIdentity);
    reply->setProperty("aegisyCompanionModelProjectionSha256", projectionSha256);
    reply->setProperty("aegisyCompanionModelPlatform", platform);
    reply->setProperty("aegisyCompanionModelCredentialHandle", credentialHandle);
    reply->setProperty("aegisyCompanionModelAuthGeneration",
                       QVariant::fromValue<qulonglong>(m_authGeneration));
    reply->setProperty("aegisyCompanionModelExpectedUrl", url.toString(QUrl::FullyEncoded));
    reply->setProperty("aegisyCompanionModelOverflow", false);
    connect(reply, &QNetworkReply::readyRead, this, [reply]() {
        if (reply->bytesAvailable() > kMaxCompanionModelResponseBytes) {
            reply->setProperty("aegisyCompanionModelOverflow", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, &ApiClient::onCompanionModelsFinished);
}

void ApiClient::retireCompanionModelRequests(const QString &errorCode)
{
    const auto pending = m_pendingCompanionModelRequests;
    m_pendingCompanionModelRequests.clear();
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        emit companionModelsFailed(it.key(), it.value(), errorCode);
    }
}

void ApiClient::retireCompanionUsageRequests(const QString &errorCode)
{
    const QStringList requests = m_pendingCompanionUsageRequests.keys();
    m_pendingCompanionUsageRequests.clear();
    for (const QString &requestId : requests) {
        emit companionApiKeyUsageFailed(requestId, errorCode);
    }
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
    m_companionImageBinding = CompanionCredentialBinding();
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
    m_chatSawStreamEvent = false;
    m_chatSawDone = false;
    m_chatMalformedEvent = false;

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
        const bool eventStream = reply->header(QNetworkRequest::ContentTypeHeader)
            .toString().contains(QStringLiteral("text/event-stream"), Qt::CaseInsensitive);
        if (eventStream) processChatEvents(false);
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, requestId]() {
        if (reply != m_chatReply) {
            reply->deleteLater();
            return;
        }
        const bool companionBound = m_companionChatBinding.requestId == requestId;
        const bool companionCurrent = !companionBound
            || companionBindingIsCurrent(m_companionChatBinding);
        const bool eventStream = reply->header(QNetworkRequest::ContentTypeHeader)
            .toString().contains(QStringLiteral("text/event-stream"), Qt::CaseInsensitive);
        m_chatBuffer.append(reply->readAll());
        QByteArray remaining;
        if (eventStream) {
            processChatEvents(true);
        } else {
            remaining = m_chatBuffer;
            m_chatBuffer.clear();
        }
        const bool canceled = reply->error() == QNetworkReply::OperationCanceledError;
        if (!companionCurrent && !canceled) {
            emit chatFailed(
                requestId, QStringLiteral("companion-operation-request-stale"));
        } else if (reply->error() != QNetworkReply::NoError && !canceled) {
            QString error = reply->errorString();
            if (m_chatSawStreamEvent || m_chatMalformedEvent) {
                error = QStringLiteral("stream disconnected before completion");
            }
            emit chatFailed(requestId, error);
        } else if (!canceled) {
            if (eventStream && (!m_chatSawDone || m_chatMalformedEvent)) {
                emit chatFailed(requestId, QStringLiteral("stream disconnected before completion"));
            } else if (m_chatContent.isEmpty() && !remaining.trimmed().isEmpty()) {
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
            } else {
                emit chatCompleted(requestId, m_chatContent);
            }
        }
        reply->deleteLater();
        m_chatReply = nullptr;
        m_chatBuffer.clear();
        m_chatContent.clear();
        m_chatRequestId.clear();
        m_chatSawStreamEvent = false;
        m_chatSawDone = false;
        m_chatMalformedEvent = false;
        if (companionBound) m_companionChatBinding = CompanionCredentialBinding();
    });
}

void ApiClient::processChatEvents(bool flushTrailingData)
{
    while (true) {
        const int newline = m_chatBuffer.indexOf('\n');
        if (newline < 0) break;
        const QByteArray line = m_chatBuffer.left(newline);
        m_chatBuffer.remove(0, newline + 1);
        processChatEventLine(line);
    }
    if (flushTrailingData) {
        const QByteArray trailing = m_chatBuffer.trimmed();
        m_chatBuffer.clear();
        if (!trailing.isEmpty()) processChatEventLine(trailing);
    }
}

void ApiClient::processChatEventLine(const QByteArray &rawLine)
{
    QByteArray line = rawLine.trimmed();
    if (!line.startsWith("data:")) return;
    line = line.mid(5).trimmed();
    if (line.isEmpty()) return;
    if (line == "[DONE]") {
        m_chatSawDone = true;
        return;
    }
    m_chatSawStreamEvent = true;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        m_chatMalformedEvent = true;
        return;
    }
    const QJsonObject event = document.object();
    const QJsonObject usage = event.value(QStringLiteral("usage")).toObject();
    if (!usage.isEmpty()) {
        emit chatUsageReceived(
            m_chatRequestId,
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
        emit chatChunkReceived(m_chatRequestId, chunk);
    }
}

void ApiClient::cancelChatMessage()
{
    m_companionChatBinding = CompanionCredentialBinding();
    if (!m_chatReply) return;
    disconnect(m_chatReply, nullptr, this, nullptr);
    m_chatReply->abort();
    m_chatReply->deleteLater();
    m_chatReply = nullptr;
    m_chatBuffer.clear();
    m_chatContent.clear();
    m_chatRequestId.clear();
    m_chatSawStreamEvent = false;
    m_chatSawDone = false;
    m_chatMalformedEvent = false;
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
                                               int exhaustedRetries,
                                               bool companionBound)
{
    if (companionBound) {
        const auto binding = m_companionPresentationBindings.constFind(requestId);
        if (binding == m_companionPresentationBindings.cend()) return;
        if (!companionBindingIsCurrent(binding.value())) {
            m_companionPresentationBindings.remove(requestId);
            emit presentationPlanFailed(
                requestId, QStringLiteral("companion-operation-request-stale"));
            return;
        }
    }
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
             attempt, structuredOutput, exhaustedRetries, companionBound]() {
        if (companionBound) {
            const auto binding = m_companionPresentationBindings.constFind(requestId);
            if (binding == m_companionPresentationBindings.cend()) {
                reply->deleteLater();
                return;
            }
            if (!companionBindingIsCurrent(binding.value())) {
                m_companionPresentationBindings.remove(requestId);
                emit presentationPlanFailed(
                    requestId, QStringLiteral("companion-operation-request-stale"));
                reply->deleteLater();
                return;
            }
        }
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
                                               exhaustedRetries, companionBound);
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
                     attempt, structuredOutput, exhaustedRetries, companionBound]() {
                    requestPresentationPlanAttempt(requestId, apiKey, model, requestText,
                                                   invalidContent, attempt, structuredOutput,
                                                   exhaustedRetries + 1, companionBound);
                });
                return;
            }
            if (companionBound) {
                m_companionPresentationBindings.remove(requestId);
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
                                               exhaustedRetries, companionBound);
                return;
            }
            if (companionBound) {
                m_companionPresentationBindings.remove(requestId);
            }
            emit presentationPlanFailed(
                requestId, QStringLiteral("模型没有返回有效的 PPT 结构：%1")
                    .arg(parseError));
        } else {
            if (companionBound) {
                m_companionPresentationBindings.remove(requestId);
            }
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
        emit requestFailed(QStringLiteral("model-response-invalid"));
        return;
    }

    QJsonObject response = doc.object();

    // 错误响应带有 message 字段
    if (response.contains("message") && !response.contains("data")) {
        emit requestFailed(QStringLiteral("model-request-failed"));
        return;
    }

    QJsonArray models = response["data"].toArray();
    qDebug() << "Received" << models.size() << "models";
    emit modelsReceived(models);
}

void ApiClient::onCompanionModelsFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply) return;
    const QString requestId = reply->property(
        "aegisyCompanionModelRequestId").toString();
    const QString accountIdentity = reply->property(
        "aegisyCompanionModelAccountIdentity").toString();
    const QString keyIdentity = reply->property(
        "aegisyCompanionModelKeyIdentity").toString();
    const QString projectionSha256 = reply->property(
        "aegisyCompanionModelProjectionSha256").toString();
    const QString platform = reply->property(
        "aegisyCompanionModelPlatform").toString();
    const QString credentialHandle = reply->property(
        "aegisyCompanionModelCredentialHandle").toString();
    const quint64 authGeneration = reply->property(
        "aegisyCompanionModelAuthGeneration").toULongLong();
    if (m_pendingCompanionModelRequests.value(requestId) != keyIdentity) {
        reply->deleteLater();
        return;
    }
    const auto failRequest = [this, &requestId, &keyIdentity](const QString &code) {
        m_pendingCompanionModelRequests.remove(requestId);
        emit companionModelsFailed(requestId, keyIdentity, code);
    };
    const bool accountMatches = accountIdentity.isEmpty()
        || (accountIdentity == m_verifiedCompanionAccountIdentity
            && m_verifiedAccountAuthGeneration == m_authGeneration);
    bool projectionMatches = projectionSha256.isEmpty();
    if (!projectionSha256.isEmpty()
            && projectionSha256 == m_currentCompanionProjection.value(
                QStringLiteral("projection_sha256")).toString()
            && accountIdentity == m_currentCompanionProjection.value(
                QStringLiteral("account_identity")).toString()) {
        for (const QJsonValue &value : m_currentCompanionProjection.value(
             QStringLiteral("keys")).toArray()) {
            const QJsonObject candidate = value.toObject();
            if (candidate.value(QStringLiteral("key_identity")).toString() == keyIdentity
                    && candidate.value(QStringLiteral("platform")).toString() == platform
                    && candidate.value(QStringLiteral("credential_handle")).toString()
                        == credentialHandle
                    && candidate.value(QStringLiteral("state")).toString()
                        == QStringLiteral("active")
                    && candidate.value(QStringLiteral("credential_state")).toString()
                        == QStringLiteral("available-in-secure-storage")) {
                projectionMatches = true;
                break;
            }
        }
    }
    if (authGeneration != m_authGeneration || !accountMatches || !projectionMatches) {
        failRequest(QStringLiteral("companion-model-request-stale"));
        reply->deleteLater();
        return;
    }

    const QUrl expectedUrl(reply->property(
        "aegisyCompanionModelExpectedUrl").toString());
    const QUrl redirect = reply->attribute(
        QNetworkRequest::RedirectionTargetAttribute).toUrl();
    const int status = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString contentType = reply->header(
        QNetworkRequest::ContentTypeHeader).toString().section(QLatin1Char(';'), 0, 0)
        .trimmed().toLower();
    const qint64 contentLength = reply->header(
        QNetworkRequest::ContentLengthHeader).toLongLong();
    const QByteArray contentEncoding = reply->rawHeader("Content-Encoding").trimmed().toLower();
    if (reply->property("aegisyCompanionModelOverflow").toBool()
            || !redirect.isEmpty() || reply->url() != expectedUrl
            || status < 200 || status >= 300
            || contentType != QStringLiteral("application/json")
            || contentLength > kMaxCompanionModelResponseBytes
            || (!contentEncoding.isEmpty() && contentEncoding != QByteArrayLiteral("identity"))) {
        failRequest(QStringLiteral("companion-model-response-untrusted"));
        reply->deleteLater();
        return;
    }

    const QByteArray body = reply->readAll();
    reply->deleteLater();
    if (body.size() > kMaxCompanionModelResponseBytes) {
        failRequest(QStringLiteral("companion-model-response-too-large"));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        failRequest(QStringLiteral("companion-model-response-invalid"));
        return;
    }
    QString projectionError;
    const QJsonObject projection = CompanionModelProjection::fromProviderResponse(
        keyIdentity, document.object(), &projectionError);
    if (projection.isEmpty()) {
        failRequest(
            projectionError.isEmpty()
                ? QStringLiteral("companion-model-response-invalid") : projectionError);
        return;
    }
    m_pendingCompanionModelRequests.remove(requestId);
    emit companionModelsReceived(requestId, keyIdentity, projection);
}

void ApiClient::onImageGenerationFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || reply != m_imageGenerationReply) {
        return;
    }
    m_imageGenerationReply = nullptr;
    const CompanionCredentialBinding companionBinding = m_companionImageBinding;
    const bool companionBound = !companionBinding.isEmpty();
    const bool companionCurrent = !companionBound
        || companionBindingIsCurrent(companionBinding);
    m_companionImageBinding = CompanionCredentialBinding();
    const auto failImage = [this, companionBound, companionBinding](const QString &error) {
        if (companionBound) {
            emit companionImageFailed(companionBinding.requestId, error);
        } else {
            emit imageGenerationFailed(error);
        }
    };

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

    if (!companionCurrent) {
        failImage(QStringLiteral("companion-operation-request-stale"));
        return;
    }
    if (replyError != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300) {
        QString message = m_imageGenerationError;
        if (message.isEmpty()) {
            message = httpStatus > 0
                ? QStringLiteral("HTTP %1").arg(httpStatus)
                : networkError;
        }
        failImage(message);
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
        failImage(m_imageGenerationError.isEmpty()
            ? QStringLiteral("服务器未返回可解码的图片数据。")
            : m_imageGenerationError);
        return;
    }

    if (format.isEmpty()) {
        format = requestedFormat.isEmpty() ? QStringLiteral("png") : requestedFormat;
    }
    if (companionBound) {
        emit companionImageGenerated(
            companionBinding.requestId, imageData, format,
            m_imageGenerationRevisedPrompt);
    } else {
        emit imageGenerated(imageData, format, m_imageGenerationRevisedPrompt);
    }
}
