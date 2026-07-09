#ifndef API_CLIENT_H
#define API_CLIENT_H

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>

class ApiClient : public QObject
{
    Q_OBJECT

public:
    explicit ApiClient(QObject *parent = nullptr);
    ~ApiClient();

    // 设置 API 基础 URL
    void setBaseUrl(const QString &url);

    // 设置认证 Token
    void setAuthToken(const QString &token);

    // API 方法
    void login(const QString &email, const QString &password);
    void getApiKeys();
    void getUserInfo();
    void getChannels();        // 获取渠道列表
    // 获取账号支持的模型列表（调用 OpenAI 兼容的 /v1/models，需传入 sk- API Key）
    void getModels(const QString &apiKey);

    // 测试某个 API Key 是否可用（对 /v1/models 发起请求），结果通过 apiKeyTested 返回
    void testApiKey(const QString &keyId, const QString &apiKey);

signals:
    // 登录成功信号
    void loginSuccess(const QString &token, const QJsonObject &userData);

    // 登录失败信号
    void loginFailed(const QString &errorMessage);

    // API Keys 获取成功
    void apiKeysReceived(const QJsonArray &keys);

    // 用户信息获取成功
    void userInfoReceived(const QJsonObject &userInfo);

    // 渠道列表获取成功
    void channelsReceived(const QJsonArray &channels);

    // 模型列表获取成功
    void modelsReceived(const QJsonArray &models);

    // 某个 API Key 测试完成：supported 表示是否可用，detail 为说明
    void apiKeyTested(const QString &keyId, bool supported, const QString &detail);

    // 通用错误信号
    void requestFailed(const QString &errorMessage);

private slots:
    void onLoginFinished();
    void onApiKeysFinished();
    void onUserInfoFinished();
    void onChannelsFinished();
    void onModelsFinished();

private:
    QNetworkAccessManager *m_networkManager;
    QString m_baseUrl;
    QString m_authToken;

    // 通用 POST 请求
    QNetworkReply* post(const QString &endpoint, const QJsonObject &data);

    // 通用 GET 请求（bearerToken 为空时使用已设置的 m_authToken）
    QNetworkReply* get(const QString &endpoint, const QString &bearerToken = QString());

    // 解析响应
    QJsonObject parseResponse(QNetworkReply *reply, bool &ok);
};

#endif // API_CLIENT_H
