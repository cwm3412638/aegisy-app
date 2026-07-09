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

signals:
    // 登录成功信号
    void loginSuccess(const QString &token, const QJsonObject &userData);

    // 登录失败信号
    void loginFailed(const QString &errorMessage);

    // API Keys 获取成功
    void apiKeysReceived(const QJsonArray &keys);

    // 用户信息获取成功
    void userInfoReceived(const QJsonObject &userInfo);

    // 通用错误信号
    void requestFailed(const QString &errorMessage);

private slots:
    void onLoginFinished();
    void onApiKeysFinished();
    void onUserInfoFinished();

private:
    QNetworkAccessManager *m_networkManager;
    QString m_baseUrl;
    QString m_authToken;

    // 通用 POST 请求
    QNetworkReply* post(const QString &endpoint, const QJsonObject &data);

    // 通用 GET 请求
    QNetworkReply* get(const QString &endpoint);

    // 解析响应
    QJsonObject parseResponse(QNetworkReply *reply, bool &ok);
};

#endif // API_CLIENT_H
