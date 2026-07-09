#include "main_window.h"
#include "api_keys_dialog.h"
#include "env_config_dialog.h"
#include "env_manager_dialog.h"
#include "models_dialog.h"
#include "quick_setup_wizard.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QMessageBox>
#include <QThread>
#include <QTimer>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_apiClient(new ApiClient(this))
    , m_configManager(new ConfigManager(this))
    , m_envDetector(new EnvDetector(this))
{
    setupUi();
    setWindowTitle("Aegisy 客户端");
    resize(800, 600);

    // 加载配置
    m_configManager->load();
}

MainWindow::~MainWindow()
{
}

void MainWindow::setupUi()
{
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // Header
    QHBoxLayout *headerLayout = new QHBoxLayout();

    QLabel *titleLabel = new QLabel("Aegisy 配置管理器", this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    headerLayout->addWidget(titleLabel);

    headerLayout->addStretch();

    m_userLabel = new QLabel("用户: 未登录", this);
    headerLayout->addWidget(m_userLabel);

    m_logoutButton = new QPushButton("退出登录", this);
    m_logoutButton->setMaximumWidth(100);
    headerLayout->addWidget(m_logoutButton);

    mainLayout->addLayout(headerLayout);

    // Environment Detection Section
    QGroupBox *envGroup = new QGroupBox("环境检测", this);
    QVBoxLayout *envLayout = new QVBoxLayout(envGroup);

    QHBoxLayout *envButtonLayout = new QHBoxLayout();

    m_quickSetupButton = new QPushButton("🚀 快速配置", this);
    m_quickSetupButton->setStyleSheet(
        "QPushButton {"
        "  background-color: #27ae60;"
        "  color: white;"
        "  border: none;"
        "  border-radius: 4px;"
        "  padding: 6px 12px;"
        "  font-weight: bold;"
        "  font-size: 14px;"
        "}"
        "QPushButton:hover {"
        "  background-color: #229954;"
        "}"
    );

    m_refreshButton = new QPushButton("刷新检测", this);
    m_configureButton = new QPushButton("配置环境", this);
    m_manageKeysButton = new QPushButton("管理 API Keys", this);
    m_manageKeysButton->setStyleSheet(
        "QPushButton {"
        "  background-color: #3498db;"
        "  color: white;"
        "  border: none;"
        "  border-radius: 4px;"
        "  padding: 6px 12px;"
        "  font-weight: bold;"
        "}"
        "QPushButton:hover {"
        "  background-color: #2980b9;"
        "}"
    );

    m_manageEnvsButton = new QPushButton("管理环境", this);
    m_manageEnvsButton->setStyleSheet(
        "QPushButton {"
        "  background-color: #9b59b6;"
        "  color: white;"
        "  border: none;"
        "  border-radius: 4px;"
        "  padding: 6px 12px;"
        "  font-weight: bold;"
        "}"
        "QPushButton:hover {"
        "  background-color: #8e44ad;"
        "}"
    );

    m_viewModelsButton = new QPushButton("查看模型", this);
    m_viewModelsButton->setStyleSheet(
        "QPushButton {"
        "  background-color: #e67e22;"
        "  color: white;"
        "  border: none;"
        "  border-radius: 4px;"
        "  padding: 6px 12px;"
        "  font-weight: bold;"
        "}"
        "QPushButton:hover {"
        "  background-color: #d35400;"
        "}"
    );

    envButtonLayout->addWidget(m_quickSetupButton);
    envButtonLayout->addWidget(m_refreshButton);
    envButtonLayout->addWidget(m_configureButton);
    envButtonLayout->addWidget(m_manageKeysButton);
    envButtonLayout->addWidget(m_manageEnvsButton);
    envButtonLayout->addWidget(m_viewModelsButton);
    envButtonLayout->addStretch();

    envLayout->addLayout(envButtonLayout);

    // Environment status table
    m_envTable = new QTableWidget(this);
    m_envTable->setColumnCount(4);
    m_envTable->setHorizontalHeaderLabels({"应用", "状态", "API Key", "Base URL"});
    m_envTable->horizontalHeader()->setStretchLastSection(true);
    m_envTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_envTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_envTable->setAlternatingRowColors(true);
    m_envTable->verticalHeader()->setVisible(false);

    envLayout->addWidget(m_envTable);

    mainLayout->addWidget(envGroup);

    // Log output
    QGroupBox *logGroup = new QGroupBox("日志", this);
    QVBoxLayout *logLayout = new QVBoxLayout(logGroup);

    m_logOutput = new QTextEdit(this);
    m_logOutput->setReadOnly(true);
    m_logOutput->setMaximumHeight(150);
    m_logOutput->setStyleSheet("QTextEdit { background-color: #f8f9fa; font-family: monospace; font-size: 11px; }");

    logLayout->addWidget(m_logOutput);

    mainLayout->addWidget(logGroup);

    // Connections
    connect(m_quickSetupButton, &QPushButton::clicked, this, &MainWindow::onQuickSetupClicked);
    connect(m_refreshButton, &QPushButton::clicked, this, &MainWindow::onRefreshEnvClicked);
    connect(m_configureButton, &QPushButton::clicked, this, &MainWindow::onConfigureEnvClicked);
    connect(m_manageKeysButton, &QPushButton::clicked, this, &MainWindow::onManageKeysClicked);
    connect(m_manageEnvsButton, &QPushButton::clicked, this, &MainWindow::onManageEnvironmentsClicked);
    connect(m_viewModelsButton, &QPushButton::clicked, this, &MainWindow::onViewModelsClicked);
    connect(m_logoutButton, &QPushButton::clicked, this, &MainWindow::onLogoutClicked);
}

void MainWindow::setAuthToken(const QString &token)
{
    m_authToken = token;
    m_apiClient->setAuthToken(token);
    m_userLabel->setText("用户: 已登录");

    // 自动刷新环境检测
    onRefreshEnvClicked();

    // 首次登录自动弹出快速配置向导
    QTimer::singleShot(1000, this, [this]() {
        QMessageBox::StandardButton reply;
        reply = QMessageBox::question(this, "快速配置",
                                      "🚀 欢迎使用 Aegisy 客户端！\n\n"
                                      "是否立即使用「快速配置向导」自动完成所有配置？\n\n"
                                      "只需 10-20 秒，无需任何操作！",
                                      QMessageBox::Yes | QMessageBox::No);

        if (reply == QMessageBox::Yes) {
            onQuickSetupClicked();
        }
    });
}

void MainWindow::onRefreshEnvClicked()
{
    m_logOutput->append("[INFO] Starting environment detection...");
    m_refreshButton->setEnabled(false);

    // 在后台线程中执行检测
    QThread::msleep(100);  // 简单模拟，实际应使用 QtConcurrent

    QMap<QString, EnvStatus> results = m_envDetector->detectAll();
    updateEnvDisplay(results);

    m_logOutput->append("[INFO] Environment detection completed");
    m_refreshButton->setEnabled(true);
}

void MainWindow::updateEnvDisplay(const QMap<QString, EnvStatus> &envStatuses)
{
    m_envTable->setRowCount(0);

    int row = 0;
    for (auto it = envStatuses.begin(); it != envStatuses.end(); ++it) {
        const QString &appName = it.key();
        const EnvStatus &status = it.value();

        m_envTable->insertRow(row);

        // Application name
        QTableWidgetItem *nameItem = new QTableWidgetItem(appName);
        m_envTable->setItem(row, 0, nameItem);

        // Status（Codex 这类以“是否安装”为主，措辞用已安装/未安装）
        const bool isInstallCheck = appName.startsWith("Codex");
        QString statusText;
        if (isInstallCheck) {
            statusText = status.isConfigured ? "✓ 已安装" : "✗ 未安装";
        } else {
            statusText = status.isConfigured ? "✓ 已配置" : "✗ 未配置";
        }
        QTableWidgetItem *statusItem = new QTableWidgetItem(statusText);
        if (status.isConfigured) {
            statusItem->setForeground(QBrush(QColor("#27ae60")));
        } else {
            statusItem->setForeground(QBrush(QColor("#e74c3c")));
        }
        m_envTable->setItem(row, 1, statusItem);

        // API Key (masked)
        QString maskedKey = status.apiKey;
        if (maskedKey.length() > 10) {
            maskedKey = maskedKey.left(7) + "..." + maskedKey.right(4);
        }
        QTableWidgetItem *keyItem = new QTableWidgetItem(maskedKey);
        m_envTable->setItem(row, 2, keyItem);

        // Base URL
        QTableWidgetItem *urlItem = new QTableWidgetItem(status.baseUrl);
        m_envTable->setItem(row, 3, urlItem);

        // Log details
        if (status.isConfigured) {
            m_logOutput->append(QString("[✓] %1: 已配置").arg(appName));
            m_logOutput->append(QString("    配置: %1").arg(status.configPath));
        } else {
            m_logOutput->append(QString("[✗] %1: 未配置").arg(appName));
            if (!status.error.isEmpty()) {
                m_logOutput->append(QString("    状态: %1").arg(status.error));
            }
        }

        row++;
    }

    m_envTable->resizeColumnsToContents();
}

void MainWindow::onConfigureEnvClicked()
{
    m_logOutput->append("[INFO] Opening environment configuration dialog...");

    EnvConfigDialog *dialog = new EnvConfigDialog(m_configManager, m_envDetector, m_apiClient, this);

    // 连接配置应用信号
    connect(dialog, &EnvConfigDialog::configurationApplied,
            [this]() {
        m_logOutput->append("[✓] Configuration applied successfully");

        // 自动刷新环境检测
        QTimer::singleShot(1000, this, [this]() {
            onRefreshEnvClicked();
        });
    });

    dialog->exec();
    dialog->deleteLater();

    m_logOutput->append("[INFO] Configuration dialog closed");
}

void MainWindow::onManageKeysClicked()
{
    m_logOutput->append("[INFO] Opening API Keys management dialog...");

    ApiKeysDialog *dialog = new ApiKeysDialog(m_apiClient, this);

    // 连接 Key 激活信号
    connect(dialog, &ApiKeysDialog::keyActivated,
            [this](const QString &keyId, const QString &key) {
        m_logOutput->append(QString("[✓] API Key activated: %1").arg(keyId));

        // 这里可以自动应用到环境配置
        // 后续版本实现
    });

    dialog->exec();
    dialog->deleteLater();

    m_logOutput->append("[INFO] API Keys dialog closed");
}

void MainWindow::onManageEnvironmentsClicked()
{
    m_logOutput->append("[INFO] Opening environment manager...");

    EnvManagerDialog *dialog = new EnvManagerDialog(m_configManager, this);

    // 连接环境切换信号
    connect(dialog, &EnvManagerDialog::environmentSwitched,
            [this](const QString &envId) {
        m_logOutput->append(QString("[✓] Environment switched: %1").arg(envId));

        // 自动刷新环境检测
        QTimer::singleShot(500, this, [this]() {
            onRefreshEnvClicked();
        });
    });

    dialog->exec();
    dialog->deleteLater();

    m_logOutput->append("[INFO] Environment manager closed");
}

void MainWindow::onViewModelsClicked()
{
    m_logOutput->append("[INFO] 打开模型和价格管理...");

    ModelsDialog *dialog = new ModelsDialog(m_apiClient, this);

    // 连接模型选择信号
    connect(dialog, &ModelsDialog::modelSelected,
            [this](const QString &modelName) {
        m_logOutput->append(QString("[✓] 模型已选择: %1").arg(modelName));
    });

    dialog->exec();
    dialog->deleteLater();

    m_logOutput->append("[INFO] 模型管理对话框已关闭");
}

void MainWindow::onQuickSetupClicked()
{
    m_logOutput->append("[INFO] 启动快速配置向导...");

    QuickSetupWizard *wizard = new QuickSetupWizard(m_apiClient, m_configManager, m_envDetector, this);

    // 连接配置完成信号
    connect(wizard, &QuickSetupWizard::setupCompleted,
            [this]() {
        m_logOutput->append("[✓] 快速配置已完成！");

        // 自动刷新环境检测
        QTimer::singleShot(1000, this, [this]() {
            onRefreshEnvClicked();
        });
    });

    wizard->exec();
    wizard->deleteLater();

    m_logOutput->append("[INFO] 快速配置向导已关闭");
}

void MainWindow::onLogoutClicked()
{
    QMessageBox::StandardButton reply;
    reply = QMessageBox::question(this, "Logout",
                                  "确定要退出登录吗？",
                                  QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        m_logOutput->append("[INFO] User logged out");
        close();
        // 实际应用中应该清除 token 并返回登录界面
    }
}

void MainWindow::onEnvDetectionFinished()
{
    m_refreshButton->setEnabled(true);
}
