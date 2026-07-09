#include "main_window.h"
#include "api_keys_dialog.h"
#include "models_dialog.h"
#include "secure_storage.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QJsonObject>
#include <QFont>

static const QString kSiteUrl = "https://www.aegisy.cc";

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_apiClient(new ApiClient(this))
    , m_toolManager(new ToolManager(this))
{
    setupUi();
    setWindowTitle("Aegisy 客户端 — AI 工具一键接入");
    resize(680, 640);

    connect(m_apiClient, &ApiClient::apiKeysReceived, this, &MainWindow::onApiKeysReceived);
    connect(m_apiClient, &ApiClient::requestFailed, this, &MainWindow::onRequestFailed);
    connect(m_toolManager, &ToolManager::installOutput, this, &MainWindow::onInstallOutput);
    connect(m_toolManager, &ToolManager::installFinished, this, &MainWindow::onInstallFinished);
}

MainWindow::~MainWindow()
{
}

void MainWindow::setAuthToken(const QString &token)
{
    m_authToken = token;
    m_apiClient->setAuthToken(token);

    logMessage("正在获取账号 API Keys...", "#3498db");
    m_apiClient->getApiKeys();

    refreshAllCards();
}

void MainWindow::setupUi()
{
    QWidget *central = new QWidget(this);
    setCentralWidget(central);

    QVBoxLayout *mainLayout = new QVBoxLayout(central);
    mainLayout->setSpacing(14);
    mainLayout->setContentsMargins(24, 18, 24, 18);

    // ===== 顶栏 =====
    QHBoxLayout *topBar = new QHBoxLayout();

    QLabel *title = new QLabel("Aegisy 客户端", this);
    QFont titleFont = title->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setStyleSheet("color: #2c3e50;");
    topBar->addWidget(title);

    topBar->addStretch();

    m_userLabel = new QLabel(this);
    m_userLabel->setStyleSheet("color: #7f8c8d;");
    topBar->addWidget(m_userLabel);

    m_logoutButton = new QPushButton("退出登录", this);
    m_logoutButton->setStyleSheet(
        "QPushButton { background: transparent; color: #e74c3c; border: 1px solid #e74c3c;"
        "  border-radius: 4px; padding: 4px 12px; }"
        "QPushButton:hover { background: #fdecea; }"
    );
    topBar->addWidget(m_logoutButton);

    mainLayout->addLayout(topBar);

    QLabel *subtitle = new QLabel("选择你要使用的 AI 工具，点击「一键接入」即可完成全部配置。", this);
    subtitle->setStyleSheet("color: #7f8c8d;");
    mainLayout->addWidget(subtitle);

    // ===== 三张工具卡片 =====
    mainLayout->addWidget(createToolCard(AiTool::ClaudeCode));
    mainLayout->addWidget(createToolCard(AiTool::CodexCli));
    mainLayout->addWidget(createToolCard(AiTool::GeminiCli));

    // ===== 高级区 =====
    QHBoxLayout *advBar = new QHBoxLayout();
    QLabel *advLabel = new QLabel("高级：", this);
    advLabel->setStyleSheet("color: #95a5a6;");
    advBar->addWidget(advLabel);

    m_manageKeysButton = new QPushButton("我的 API Keys", this);
    m_viewModelsButton = new QPushButton("查看模型", this);
    for (QPushButton *b : {m_manageKeysButton, m_viewModelsButton}) {
        b->setStyleSheet(
            "QPushButton { background: transparent; color: #3498db; border: 1px solid #d6eaf8;"
            "  border-radius: 4px; padding: 4px 12px; }"
            "QPushButton:hover { background: #eaf4fc; }"
        );
        advBar->addWidget(b);
    }
    advBar->addStretch();
    mainLayout->addLayout(advBar);

    // ===== 日志 =====
    m_logOutput = new QTextEdit(this);
    m_logOutput->setReadOnly(true);
    m_logOutput->setMaximumHeight(110);
    m_logOutput->setStyleSheet(
        "QTextEdit { background-color: #f8f9fa; border: 1px solid #eee;"
        "  border-radius: 4px; font-family: monospace; font-size: 11px; color: #666; }"
    );
    mainLayout->addWidget(m_logOutput);

    connect(m_logoutButton, &QPushButton::clicked, this, &MainWindow::onLogoutClicked);
    connect(m_manageKeysButton, &QPushButton::clicked, this, &MainWindow::onManageKeysClicked);
    connect(m_viewModelsButton, &QPushButton::clicked, this, &MainWindow::onViewModelsClicked);
}

QWidget* MainWindow::createToolCard(AiTool tool)
{
    QGroupBox *box = new QGroupBox(ToolManager::toolName(tool), this);
    box->setStyleSheet(
        "QGroupBox { border: 1px solid #dfe6e9; border-radius: 8px; margin-top: 10px;"
        "  font-weight: bold; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px;"
        "  color: #2c3e50; }"
    );

    QVBoxLayout *v = new QVBoxLayout(box);
    v->setSpacing(6);
    v->setContentsMargins(14, 10, 14, 12);

    ToolCard card;

    card.statusLabel = new QLabel("检测中...", this);
    card.statusLabel->setStyleSheet("color: #7f8c8d; font-weight: normal;");
    card.statusLabel->setWordWrap(true);
    v->addWidget(card.statusLabel);

    card.warnLabel = new QLabel(this);
    card.warnLabel->setStyleSheet("color: #e67e22; font-weight: normal;");
    card.warnLabel->setWordWrap(true);
    card.warnLabel->setVisible(false);
    v->addWidget(card.warnLabel);

    QHBoxLayout *btnRow = new QHBoxLayout();
    btnRow->addStretch();

    QPushButton *guideButton = new QPushButton("使用说明", this);
    guideButton->setStyleSheet(
        "QPushButton { background: transparent; color: #7f8c8d; border: none;"
        "  text-decoration: underline; }"
        "QPushButton:hover { color: #3498db; }"
    );
    connect(guideButton, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl(kSiteUrl));
    });
    btnRow->addWidget(guideButton);

    card.actionButton = new QPushButton("🚀 一键接入", this);
    card.actionButton->setMinimumHeight(34);
    card.actionButton->setMinimumWidth(140);
    card.actionButton->setStyleSheet(
        "QPushButton { background-color: #27ae60; color: white; border: none;"
        "  border-radius: 5px; font-weight: bold; padding: 6px 16px; }"
        "QPushButton:hover { background-color: #229954; }"
        "QPushButton:disabled { background-color: #bdc3c7; }"
    );
    connect(card.actionButton, &QPushButton::clicked, this, [this, tool]() {
        onConnectToolClicked(tool);
    });
    btnRow->addWidget(card.actionButton);

    v->addLayout(btnRow);

    m_cards.insert(tool, card);
    return box;
}

void MainWindow::refreshAllCards()
{
    refreshToolCard(AiTool::ClaudeCode);
    refreshToolCard(AiTool::CodexCli);
    refreshToolCard(AiTool::GeminiCli);
}

void MainWindow::refreshToolCard(AiTool tool)
{
    const ToolCard &card = m_cards.value(tool);
    if (!card.statusLabel) return;

    const ToolStatus status = m_toolManager->detect(tool);

    QStringList parts;
    if (!status.nodeOk) {
        parts << "✗ 未检测到 Node.js";
    } else if (status.installed) {
        parts << "✓ 已安装";
    } else {
        parts << "✗ 未安装（可一键安装）";
    }

    if (status.configured) {
        parts << QString("✓ 已接入 (Key: %1)").arg(maskKey(status.configuredKey));
        card.statusLabel->setStyleSheet("color: #27ae60; font-weight: normal;");
        card.actionButton->setText("重新接入");
    } else {
        parts << "○ 未接入";
        card.statusLabel->setStyleSheet("color: #7f8c8d; font-weight: normal;");
        card.actionButton->setText("🚀 一键接入");
    }
    card.statusLabel->setText(parts.join("  ·  "));

    card.warnLabel->setVisible(!status.conflictWarning.isEmpty());
    card.warnLabel->setText(status.conflictWarning.isEmpty()
                            ? QString()
                            : "⚠️ " + status.conflictWarning);
}

QString MainWindow::maskKey(const QString &key)
{
    if (key.length() > 12) {
        return key.left(8) + "..." + key.right(4);
    }
    return key;
}

void MainWindow::onApiKeysReceived(const QJsonArray &keys)
{
    m_keys = keys;
    m_keysLoaded = true;
    logMessage(QString("✓ 已获取 %1 个 API Key").arg(keys.size()), "#27ae60");
}

void MainWindow::onRequestFailed(const QString &error)
{
    logMessage(QString("✗ 请求失败：%1").arg(error), "#e74c3c");
}

QString MainWindow::pickKeyForTool(AiTool tool, QString *keyLabel) const
{
    const QString platform = ToolManager::toolPlatform(tool);

    for (const QJsonValue &val : m_keys) {
        const QJsonObject obj = val.toObject();
        if (obj["status"].toString() != "active") continue;
        const QString key = obj["key"].toString();
        if (key.isEmpty()) continue;

        const QJsonObject group = obj["group"].toObject();
        if (group["platform"].toString() == platform) {
            if (keyLabel) {
                *keyLabel = QString("%1 / %2 组").arg(obj["name"].toString(), group["name"].toString());
            }
            return key;
        }
    }
    return QString();
}

void MainWindow::onConnectToolClicked(AiTool tool)
{
    const ToolCard &card = m_cards.value(tool);
    const ToolStatus status = m_toolManager->detect(tool);

    // 1) Node.js 前置
    if (!status.nodeOk) {
        QMessageBox msg(this);
        msg.setWindowTitle("需要先安装 Node.js");
        msg.setIcon(QMessageBox::Information);
        msg.setText(QString("%1 依赖 Node.js（建议 LTS v20+），检测到你的电脑尚未安装。\n\n"
                            "Windows 用户可在 PowerShell 中运行：\n"
                            "    winget install OpenJS.NodeJS.LTS\n\n"
                            "或点击下方按钮到官网下载安装包。安装完成后重启本程序。")
                        .arg(ToolManager::toolName(tool)));
        QPushButton *openBtn = msg.addButton("打开 Node.js 官网", QMessageBox::AcceptRole);
        msg.addButton("我知道了", QMessageBox::RejectRole);
        msg.exec();
        if (msg.clickedButton() == openBtn) {
            QDesktopServices::openUrl(QUrl("https://nodejs.org/"));
        }
        return;
    }

    // 2) Key 前置：必须有对应分组的 Key
    if (!m_keysLoaded) {
        logMessage("API Keys 尚未加载完成，请稍候再试...", "#e67e22");
        m_apiClient->getApiKeys();
        return;
    }
    const QString apiKey = pickKeyForTool(tool);
    if (apiKey.isEmpty()) {
        const QString platform = ToolManager::toolPlatform(tool);
        QMessageBox msg(this);
        msg.setWindowTitle("没有可用的 API Key");
        msg.setIcon(QMessageBox::Information);
        msg.setText(QString("你的账号还没有「%1」分组的可用 API Key。\n\n"
                            "请到 aegisy.cc 官网 → 控制台 → API Keys，"
                            "创建一个 %1 分组的 Key 后回来重试。")
                        .arg(platform));
        QPushButton *openBtn = msg.addButton("打开官网创建", QMessageBox::AcceptRole);
        msg.addButton("取消", QMessageBox::RejectRole);
        msg.exec();
        if (msg.clickedButton() == openBtn) {
            QDesktopServices::openUrl(QUrl(kSiteUrl));
        }
        return;
    }

    card.actionButton->setEnabled(false);

    // 3) 未安装 → 先一键安装（完成后在 onInstallFinished 里继续配置）
    if (!status.installed) {
        logMessage(QString("开始安装 %1 ...").arg(ToolManager::toolName(tool)), "#3498db");
        m_toolManager->install(tool);
        return;
    }

    // 4) 已安装 → 直接配置
    configureTool(tool);
}

void MainWindow::onInstallOutput(AiTool tool, const QString &line)
{
    Q_UNUSED(tool);
    logMessage(line, "#666");
}

void MainWindow::onInstallFinished(AiTool tool, bool success)
{
    if (!success) {
        logMessage(QString("✗ %1 安装失败，请检查网络或手动运行：npm install -g %2")
                       .arg(ToolManager::toolName(tool), ToolManager::npmPackage(tool)),
                   "#e74c3c");
        const ToolCard &card = m_cards.value(tool);
        if (card.actionButton) card.actionButton->setEnabled(true);
        return;
    }
    logMessage(QString("✓ %1 安装完成").arg(ToolManager::toolName(tool)), "#27ae60");
    configureTool(tool);
}

void MainWindow::configureTool(AiTool tool)
{
    const ToolCard &card = m_cards.value(tool);

    QString keyLabel;
    const QString apiKey = pickKeyForTool(tool, &keyLabel);
    if (apiKey.isEmpty()) {
        if (card.actionButton) card.actionButton->setEnabled(true);
        return;
    }

    logMessage(QString("正在写入 %1 配置（%2）...").arg(ToolManager::toolName(tool), keyLabel), "#3498db");

    const bool ok = m_toolManager->configure(tool, apiKey);
    if (ok) {
        logMessage(QString("🎉 %1 接入完成！重新打开终端后运行 `%2` 即可使用。")
                       .arg(ToolManager::toolName(tool), ToolManager::cliCommand(tool)),
                   "#27ae60");
        QMessageBox::information(this, "接入成功",
            QString("%1 已成功接入 Aegisy！\n\n"
                    "使用的 Key：%2\n\n"
                    "请重新打开终端，运行 `%3` 开始使用。")
                .arg(ToolManager::toolName(tool), keyLabel, ToolManager::cliCommand(tool)));
    } else {
        logMessage(QString("✗ %1 配置写入失败：%2")
                       .arg(ToolManager::toolName(tool), m_toolManager->lastError()),
                   "#e74c3c");
        QMessageBox::warning(this, "接入失败",
            QString("%1 配置写入失败：\n%2")
                .arg(ToolManager::toolName(tool), m_toolManager->lastError()));
    }

    if (card.actionButton) card.actionButton->setEnabled(true);
    refreshToolCard(tool);
}

void MainWindow::onManageKeysClicked()
{
    ApiKeysDialog *dialog = new ApiKeysDialog(m_apiClient, this);
    dialog->exec();
    dialog->deleteLater();
    // Key 激活状态可能变化，刷新缓存
    m_apiClient->getApiKeys();
}

void MainWindow::onViewModelsClicked()
{
    ModelsDialog *dialog = new ModelsDialog(m_apiClient, this);
    dialog->exec();
    dialog->deleteLater();
}

void MainWindow::onLogoutClicked()
{
    QMessageBox::StandardButton reply = QMessageBox::question(
        this, "退出登录", "确定要退出登录吗？",
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return;
    }

    SecureStorage::clearToken();
    emit loggedOut();
    close();
}

void MainWindow::logMessage(const QString &message, const QString &color)
{
    m_logOutput->append(QString("<span style='color:%1'>%2</span>").arg(color, message));
}
