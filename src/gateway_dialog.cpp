#include "gateway_dialog.h"

#include "app_theme.h"
#include "gateway_manager.h"

#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>

GatewayDialog::GatewayDialog(GatewayManager *manager, QWidget *parent)
    : QDialog(parent)
    , m_manager(manager)
{
    setupUi();
    setWindowTitle(QStringLiteral("本地网关与请求监控"));
    setMinimumSize(820, 520);
    resize(960, 600);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    connect(m_manager, &GatewayManager::runningChanged,
            this, &GatewayDialog::refreshState);
    connect(m_manager, &GatewayManager::requestLogged,
            this, &GatewayDialog::refreshLogs);
    connect(m_manager, &GatewayManager::gatewayError,
            this, &GatewayDialog::onGatewayError);
    refreshState();
    refreshLogs();
}

void GatewayDialog::setupUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(22, 20, 22, 18);
    root->setSpacing(12);
    auto *header = new QHBoxLayout();
    auto *title = new QLabel(QStringLiteral("本地网关"), this);
    title->setStyleSheet(QStringLiteral("font-size: 20px; font-weight: 700; color: #101828;"));
    header->addWidget(title);
    m_stateLabel = new QLabel(this);
    header->addWidget(m_stateLabel);
    header->addStretch();
    m_toggleButton = new QPushButton(this);
    header->addWidget(m_toggleButton);
    root->addLayout(header);

    m_endpointLabel = new QLabel(this);
    m_endpointLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_endpointLabel->setStyleSheet(QStringLiteral(
        "font-family: monospace; font-size: 12px; color: #475467;"
        "background: white; border: 1px solid #e4e7ec; border-radius: 8px; padding: 10px;"));
    root->addWidget(m_endpointLabel);

    auto *privacy = new QLabel(QStringLiteral(
        "请求监控仅保存时间、工具、模型、路径、状态码和耗时，不保存提示词、回复或工具参数。"), this);
    privacy->setWordWrap(true);
    privacy->setStyleSheet(QStringLiteral(
        "color: #0f5f59; background: #e7f5f2; border: 1px solid #b7e4da;"
        "border-radius: 7px; padding: 9px 11px; font-size: 11px;"));
    root->addWidget(privacy);

    m_logTable = new QTableWidget(this);
    m_logTable->setColumnCount(7);
    m_logTable->setHorizontalHeaderLabels({
        QStringLiteral("时间"), QStringLiteral("工具"), QStringLiteral("方法"),
        QStringLiteral("路径"), QStringLiteral("模型"), QStringLiteral("状态"),
        QStringLiteral("耗时") });
    m_logTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_logTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_logTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_logTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_logTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_logTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_logTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    m_logTable->verticalHeader()->setVisible(false);
    m_logTable->setEditTriggers(QTableWidget::NoEditTriggers);
    root->addWidget(m_logTable, 1);

    auto *footer = new QHBoxLayout();
    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #667085;"));
    footer->addWidget(m_statusLabel, 1);
    auto *clearButton = new QPushButton(QStringLiteral("清空记录"), this);
    clearButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    footer->addWidget(clearButton);
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), this);
    closeButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    footer->addWidget(closeButton);
    root->addLayout(footer);

    connect(m_toggleButton, &QPushButton::clicked, this, &GatewayDialog::toggleGateway);
    connect(clearButton, &QPushButton::clicked, this, [this]() {
        m_manager->clearRequestLogs();
        refreshLogs();
    });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
}

void GatewayDialog::toggleGateway()
{
    if (m_manager->isRunning()) {
        if (QMessageBox::question(
                this, QStringLiteral("关闭本地网关"),
                QStringLiteral("关闭后，当前终端配置会恢复为直接连接 Aegisy。确定继续吗？"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        QSettings().setValue(QStringLiteral("gateway/enabled"), false);
        m_manager->stop();
    } else {
        if (QMessageBox::question(
                this, QStringLiteral("启用本地网关"),
                QStringLiteral("应用将备份并把当前终端配置指向 127.0.0.1。真实 Key 只保留在网关进程内存中。确定继续吗？"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes) {
            return;
        }
        QSettings().setValue(QStringLiteral("gateway/enabled"), true);
        if (!m_manager->start()) {
            QSettings().setValue(QStringLiteral("gateway/enabled"), false);
            QMessageBox::warning(this, QStringLiteral("启动失败"), m_manager->lastError());
            return;
        }
    }
    refreshState();
}

void GatewayDialog::refreshState()
{
    const bool running = m_manager->isRunning();
    m_stateLabel->setText(running ? QStringLiteral("运行中") : QStringLiteral("已停止"));
    m_stateLabel->setStyleSheet(running
        ? QStringLiteral("color:#067647; background:#ecfdf3; border:1px solid #abefc6; border-radius:7px; padding:3px 9px;")
        : QStringLiteral("color:#667085; background:#f2f4f7; border:1px solid #e4e7ec; border-radius:7px; padding:3px 9px;"));
    m_toggleButton->setText(running ? QStringLiteral("关闭网关") : QStringLiteral("启用网关"));
    m_toggleButton->setStyleSheet(running ? AppTheme::dangerButtonStyle()
                                          : AppTheme::primaryButtonStyle());
    m_endpointLabel->setText(QStringLiteral(
        "Codex:  %1/v1\nClaude: %2\nGemini: %3")
        .arg(m_manager->endpoint(AiTool::CodexCli),
             m_manager->endpoint(AiTool::ClaudeCode),
             m_manager->endpoint(AiTool::GeminiCli)));
    m_statusLabel->setText(running
        ? QStringLiteral("网关仅监听本机 127.0.0.1:43112")
        : QStringLiteral("直接连接模式"));
}

void GatewayDialog::refreshLogs()
{
    const QList<QJsonObject> logs = m_manager->requestLogs();
    m_logTable->setRowCount(0);
    for (const QJsonObject &entry : logs) {
        const int row = m_logTable->rowCount();
        m_logTable->insertRow(row);
        const QString timestamp = entry.value(QStringLiteral("timestamp")).toString();
        m_logTable->setItem(row, 0, new QTableWidgetItem(timestamp.mid(11, 8)));
        m_logTable->setItem(row, 1, new QTableWidgetItem(entry.value(QStringLiteral("tool")).toString()));
        m_logTable->setItem(row, 2, new QTableWidgetItem(entry.value(QStringLiteral("method")).toString()));
        m_logTable->setItem(row, 3, new QTableWidgetItem(entry.value(QStringLiteral("path")).toString()));
        m_logTable->setItem(row, 4, new QTableWidgetItem(entry.value(QStringLiteral("model")).toString()));
        m_logTable->setItem(row, 5, new QTableWidgetItem(QString::number(entry.value(QStringLiteral("status")).toInt())));
        m_logTable->setItem(row, 6, new QTableWidgetItem(QStringLiteral("%1 ms").arg(
            entry.value(QStringLiteral("latency_ms")).toInt())));
    }
}

void GatewayDialog::onGatewayError(const QString &error)
{
    m_statusLabel->setText(QStringLiteral("网关错误：%1").arg(error));
    m_statusLabel->setStyleSheet(QStringLiteral("font-size:12px; color:#b42318;"));
}
