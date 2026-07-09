#include <QApplication>
#include <QMessageBox>
#include <QProcess>
#include "main_window.h"
#include "login_dialog.h"
#include "api_client.h"
#include "secure_storage.h"

// 显示登录对话框并在成功后打开主窗口
static void showLoginFlow(ApiClient *apiClient);

static void showMainWindow(const QString &token)
{
    MainWindow *mainWindow = new MainWindow();
    mainWindow->setAttribute(Qt::WA_DeleteOnClose);
    mainWindow->setAuthToken(token);

    // 退出登录：token 已清，重启应用回到登录页
    QObject::connect(mainWindow, &MainWindow::loggedOut, []() {
        QProcess::startDetached(QApplication::applicationFilePath(), QStringList());
        QApplication::quit();
    });

    mainWindow->show();
}

static void showLoginFlow(ApiClient *apiClient)
{
    LoginDialog *loginDialog = new LoginDialog();

    QObject::connect(loginDialog, &LoginDialog::loginRequested,
                     [=](const QString &email, const QString &password) {
        loginDialog->setLoading(true);
        apiClient->login(email, password);
    });

    QObject::connect(apiClient, &ApiClient::loginSuccess,
                     [=](const QString &token, const QJsonObject &userData) {
        Q_UNUSED(userData);
        loginDialog->setLoading(false);

        if (loginDialog->shouldRememberMe()) {
            SecureStorage::saveToken(token);
        }

        loginDialog->hide();
        showMainWindow(token);
        loginDialog->deleteLater();
    });

    QObject::connect(apiClient, &ApiClient::loginFailed,
                     [=](const QString &errorMessage) {
        loginDialog->setLoading(false);
        loginDialog->showError(errorMessage);
    });

    loginDialog->show();
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QApplication::setApplicationName("AegisyClient");
    QApplication::setApplicationVersion("2.0.0");
    QApplication::setOrganizationName("Aegisy");
    QApplication::setOrganizationDomain("aegisy.cc");

    ApiClient *apiClient = new ApiClient(&app);

    // 有保存的 Token 直接进主界面
    const QString savedToken = SecureStorage::loadToken();
    if (!savedToken.isEmpty()) {
        apiClient->setAuthToken(savedToken);
        showMainWindow(savedToken);
    } else {
        showLoginFlow(apiClient);
    }

    return app.exec();
}
