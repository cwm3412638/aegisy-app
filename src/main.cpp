#include <QApplication>
#include <QMessageBox>
#include "main_window.h"
#include "login_dialog.h"
#include "api_client.h"
#include "secure_storage.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // 设置应用信息
    QApplication::setApplicationName("AegisyClient");
    QApplication::setApplicationVersion("1.0.0");
    QApplication::setOrganizationName("Aegisy");
    QApplication::setOrganizationDomain("aegisy.cc");

    // 创建 API 客户端
    ApiClient *apiClient = new ApiClient(&app);

    // 检查是否有保存的 Token
    QString savedToken = SecureStorage::loadToken();
    if (!savedToken.isEmpty()) {
        // 尝试使用保存的 Token
        apiClient->setAuthToken(savedToken);

        // 直接显示主窗口
        MainWindow *mainWindow = new MainWindow();
        mainWindow->setAuthToken(savedToken);
        mainWindow->show();

        return app.exec();
    }

    // 显示登录对话框
    LoginDialog *loginDialog = new LoginDialog();

    // 连接登录信号
    QObject::connect(loginDialog, &LoginDialog::loginRequested,
                     [=](const QString &email, const QString &password) {
        loginDialog->setLoading(true);

        // 执行登录
        apiClient->login(email, password);
    });

    // 登录成功
    QObject::connect(apiClient, &ApiClient::loginSuccess,
                     [=](const QString &token, const QJsonObject &userData) {
        loginDialog->setLoading(false);

        // 保存 Token（如果选择了记住我）
        if (loginDialog->shouldRememberMe()) {
            SecureStorage::saveToken(token);
        }

        // 隐藏登录对话框
        loginDialog->hide();

        // 显示主窗口
        MainWindow *mainWindow = new MainWindow();
        mainWindow->setAuthToken(token);
        mainWindow->show();

        // 删除登录对话框
        loginDialog->deleteLater();
    });

    // 登录失败
    QObject::connect(apiClient, &ApiClient::loginFailed,
                     [=](const QString &errorMessage) {
        loginDialog->setLoading(false);
        loginDialog->showError(errorMessage);
    });

    loginDialog->show();

    return app.exec();
}
