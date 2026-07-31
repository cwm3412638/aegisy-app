#include <QApplication>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include "main_window.h"
#include "login_dialog.h"
#include "api_client.h"
#include "secure_storage.h"
#include "app_theme.h"
#include "update_manager.h"
#include "feature_flags.h"
#include "agent_workbench_window.h"

// 显示登录对话框并在成功后打开主窗口
static void showLoginFlow(ApiClient *apiClient, UpdateManager *updateManager);

static void showAgentWorkbench()
{
    auto *workbench = new AgentWorkbenchWindow();
    workbench->setAttribute(Qt::WA_DeleteOnClose);
    workbench->show();
}

static void showMainWindow(const QString &token, UpdateManager *updateManager)
{
    // Check if Agent Workbench is enabled
    if (FeatureFlags::isAgentWorkbenchEnabled()) {
        showAgentWorkbench();
        return;
    }

    MainWindow *mainWindow = new MainWindow(updateManager);
    mainWindow->setAttribute(Qt::WA_DeleteOnClose);

    // 退出登录：token 已清，重启应用回到登录页
    QObject::connect(mainWindow, &MainWindow::loggedOut, []() {
        QProcess::startDetached(QApplication::applicationFilePath(), QStringList());
        QApplication::quit();
    });

    mainWindow->setAuthToken(token);
    mainWindow->show();
}

static void showLoginFlow(ApiClient *apiClient, UpdateManager *updateManager)
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

        if (loginDialog->shouldRememberMe() && !SecureStorage::saveToken(token)) {
            QMessageBox::warning(
                loginDialog,
                QStringLiteral("无法记住登录状态"),
                QStringLiteral("系统安全存储不可用，本次仍可继续使用，但下次启动需要重新登录。"));
        }

        loginDialog->hide();
        showMainWindow(token, updateManager);
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

    AppTheme::apply(app);

    QApplication::setApplicationName("AegisyClient");
    QApplication::setApplicationVersion(AEGISY_APP_VERSION);
    QApplication::setOrganizationName("Aegisy");
    QApplication::setOrganizationDomain("aegisy.cc");

    // 清理旧版本曾写入普通设置的完整 Key，仅保留不敏感的首选 Key ID。
    QSettings().remove(QStringLiteral("apikeys/activeKey"));

    ApiClient *apiClient = new ApiClient(&app);
    UpdateManager *updateManager = new UpdateManager(&app);

    // 有保存的 Token 直接进主界面
    const QString savedToken = SecureStorage::loadToken();
    if (!savedToken.isEmpty()) {
        apiClient->setAuthToken(savedToken);
        showMainWindow(savedToken, updateManager);
    } else {
        showLoginFlow(apiClient, updateManager);
    }

    return app.exec();
}
