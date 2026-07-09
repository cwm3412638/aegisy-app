#include "main_window.h"
#include "api_keys_dialog.h"
#include "env_config_dialog.h"
#include "env_manager_dialog.h"
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
    setWindowTitle("Aegisy Client");
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

    QLabel *titleLabel = new QLabel("Aegisy Configuration Manager", this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    headerLayout->addWidget(titleLabel);

    headerLayout->addStretch();

    m_userLabel = new QLabel("User: Not logged in", this);
    headerLayout->addWidget(m_userLabel);

    m_logoutButton = new QPushButton("Logout", this);
    m_logoutButton->setMaximumWidth(100);
    headerLayout->addWidget(m_logoutButton);

    mainLayout->addLayout(headerLayout);

    // Environment Detection Section
    QGroupBox *envGroup = new QGroupBox("Environment Detection", this);
    QVBoxLayout *envLayout = new QVBoxLayout(envGroup);

    QHBoxLayout *envButtonLayout = new QHBoxLayout();
    m_refreshButton = new QPushButton("Refresh Detection", this);
    m_configureButton = new QPushButton("Configure Environment", this);
    m_manageKeysButton = new QPushButton("Manage API Keys", this);
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

    m_manageEnvsButton = new QPushButton("Manage Environments", this);
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

    envButtonLayout->addWidget(m_refreshButton);
    envButtonLayout->addWidget(m_configureButton);
    envButtonLayout->addWidget(m_manageKeysButton);
    envButtonLayout->addWidget(m_manageEnvsButton);
    envButtonLayout->addStretch();

    envLayout->addLayout(envButtonLayout);

    // Environment status table
    m_envTable = new QTableWidget(this);
    m_envTable->setColumnCount(4);
    m_envTable->setHorizontalHeaderLabels({"Application", "Status", "API Key", "Base URL"});
    m_envTable->horizontalHeader()->setStretchLastSection(true);
    m_envTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_envTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_envTable->setAlternatingRowColors(true);
    m_envTable->verticalHeader()->setVisible(false);

    envLayout->addWidget(m_envTable);

    mainLayout->addWidget(envGroup);

    // Log output
    QGroupBox *logGroup = new QGroupBox("Log", this);
    QVBoxLayout *logLayout = new QVBoxLayout(logGroup);

    m_logOutput = new QTextEdit(this);
    m_logOutput->setReadOnly(true);
    m_logOutput->setMaximumHeight(150);
    m_logOutput->setStyleSheet("QTextEdit { background-color: #f8f9fa; font-family: monospace; font-size: 11px; }");

    logLayout->addWidget(m_logOutput);

    mainLayout->addWidget(logGroup);

    // Connections
    connect(m_refreshButton, &QPushButton::clicked, this, &MainWindow::onRefreshEnvClicked);
    connect(m_configureButton, &QPushButton::clicked, this, &MainWindow::onConfigureEnvClicked);
    connect(m_manageKeysButton, &QPushButton::clicked, this, &MainWindow::onManageKeysClicked);
    connect(m_manageEnvsButton, &QPushButton::clicked, this, &MainWindow::onManageEnvironmentsClicked);
    connect(m_logoutButton, &QPushButton::clicked, this, &MainWindow::onLogoutClicked);
}

void MainWindow::setAuthToken(const QString &token)
{
    m_authToken = token;
    m_apiClient->setAuthToken(token);
    m_userLabel->setText("User: Logged in");

    // 自动刷新环境检测
    onRefreshEnvClicked();
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

        // Status
        QString statusText = status.isConfigured ? "✓ Configured" : "✗ Not Configured";
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
            m_logOutput->append(QString("[✓] %1: Configured").arg(appName));
            m_logOutput->append(QString("    Config: %1").arg(status.configPath));
        } else {
            m_logOutput->append(QString("[✗] %1: Not configured").arg(appName));
            if (!status.error.isEmpty()) {
                m_logOutput->append(QString("    Error: %1").arg(status.error));
            }
        }

        row++;
    }

    m_envTable->resizeColumnsToContents();
}

void MainWindow::onConfigureEnvClicked()
{
    m_logOutput->append("[INFO] Opening environment configuration dialog...");

    EnvConfigDialog *dialog = new EnvConfigDialog(m_configManager, m_envDetector, this);

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

void MainWindow::onLogoutClicked()
{
    QMessageBox::StandardButton reply;
    reply = QMessageBox::question(this, "Logout",
                                  "Are you sure you want to logout?",
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
