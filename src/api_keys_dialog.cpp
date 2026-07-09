#include "api_keys_dialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QClipboard>
#include <QApplication>
#include <QMessageBox>
#include <QJsonObject>
#include <QDateTime>
#include <QTimer>
#include <QSettings>

ApiKeyInfo ApiKeyInfo::fromJson(const QJsonObject &obj)
{
    ApiKeyInfo info;
    info.id = QString::number(obj["id"].toInt());  // id 是数字
    info.name = obj["name"].toString();
    info.key = obj["key"].toString();
    info.status = obj["status"].toString();
    info.quota = obj["quota"].toInt(0);
    info.used = obj["quota_used"].toInt(0);  // 字段名是 quota_used
    info.createdAt = obj["created_at"].toString();
    info.expiresAt = obj["expires_at"].toString();

    // 判断是否活跃：status == "active"
    info.isActive = (obj["status"].toString() == "active");

    return info;
}

ApiKeysDialog::ApiKeysDialog(ApiClient *apiClient, QWidget *parent)
    : QDialog(parent)
    , m_apiClient(apiClient)
{
    // 读取上次激活的 Key（持久化，退出后仍记住）
    QSettings settings;
    m_activeKeyId = settings.value("apikeys/activeKeyId").toString();

    setupUi();
    setWindowTitle("API Keys 管理");
    resize(900, 500);
    // 铺满整个屏幕
    setWindowState(windowState() | Qt::WindowMaximized);

    // 连接 API 客户端信号
    connect(m_apiClient, &ApiClient::apiKeysReceived, this, &ApiKeysDialog::onKeysReceived);
    connect(m_apiClient, &ApiClient::requestFailed, this, &ApiKeysDialog::onRequestFailed);

    // 自动加载
    loadApiKeys();
}

void ApiKeysDialog::setupUi()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // Header
    QHBoxLayout *headerLayout = new QHBoxLayout();

    QLabel *titleLabel = new QLabel("API Keys 管理", this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    headerLayout->addWidget(titleLabel);

    headerLayout->addStretch();

    m_totalKeysLabel = new QLabel("总计: 0 个 Key", this);
    m_totalKeysLabel->setStyleSheet("color: #666; font-size: 13px;");
    headerLayout->addWidget(m_totalKeysLabel);

    mainLayout->addLayout(headerLayout);

    // Toolbar
    QHBoxLayout *toolbarLayout = new QHBoxLayout();

    m_refreshButton = new QPushButton("🔄 刷新", this);
    m_refreshButton->setMinimumHeight(35);
    toolbarLayout->addWidget(m_refreshButton);

    m_copyButton = new QPushButton("📋 复制 Key", this);
    m_copyButton->setMinimumHeight(35);
    m_copyButton->setEnabled(false);
    toolbarLayout->addWidget(m_copyButton);

    m_activateButton = new QPushButton("✓ 设为活跃", this);
    m_activateButton->setMinimumHeight(35);
    m_activateButton->setEnabled(false);
    m_activateButton->setStyleSheet(
        "QPushButton {"
        "  background-color: #27ae60;"
        "  color: white;"
        "  border: none;"
        "  border-radius: 4px;"
        "  padding: 5px 15px;"
        "}"
        "QPushButton:hover {"
        "  background-color: #229954;"
        "}"
        "QPushButton:disabled {"
        "  background-color: #bdc3c7;"
        "}"
    );
    toolbarLayout->addWidget(m_activateButton);

    toolbarLayout->addStretch();

    mainLayout->addLayout(toolbarLayout);

    // Keys Table
    m_keysTable = new QTableWidget(this);
    m_keysTable->setColumnCount(7);
    m_keysTable->setHorizontalHeaderLabels({
        "名称", "状态", "Key", "配额", "已用", "使用率", "创建时间"
    });

    // 让各列铺满整个表格宽度：名称、Key 两列自动拉伸，其余按内容自适应
    QHeaderView *header = m_keysTable->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(0, QHeaderView::Stretch);          // 名称
    header->setSectionResizeMode(1, QHeaderView::ResizeToContents); // 状态
    header->setSectionResizeMode(2, QHeaderView::Stretch);          // Key
    header->setSectionResizeMode(3, QHeaderView::ResizeToContents); // 配额
    header->setSectionResizeMode(4, QHeaderView::ResizeToContents); // 已用
    header->setSectionResizeMode(5, QHeaderView::ResizeToContents); // 使用率
    header->setSectionResizeMode(6, QHeaderView::ResizeToContents); // 创建时间
    m_keysTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_keysTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_keysTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_keysTable->setAlternatingRowColors(true);
    m_keysTable->verticalHeader()->setVisible(false);
    m_keysTable->setStyleSheet(
        "QTableWidget {"
        "  border: 1px solid #ddd;"
        "  border-radius: 4px;"
        "  gridline-color: #e0e0e0;"
        "}"
        "QHeaderView::section {"
        "  background-color: #f5f5f5;"
        "  padding: 8px;"
        "  border: none;"
        "  border-bottom: 2px solid #ddd;"
        "  font-weight: bold;"
        "}"
    );

    mainLayout->addWidget(m_keysTable);

    // Status Label
    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet("color: #666; font-size: 12px;");
    m_statusLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(m_statusLabel);

    // Close button
    QHBoxLayout *bottomLayout = new QHBoxLayout();
    bottomLayout->addStretch();

    QPushButton *closeButton = new QPushButton("关闭", this);
    closeButton->setMinimumHeight(35);
    closeButton->setMinimumWidth(100);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    bottomLayout->addWidget(closeButton);

    mainLayout->addLayout(bottomLayout);

    // Connections
    connect(m_refreshButton, &QPushButton::clicked, this, &ApiKeysDialog::onRefreshClicked);
    connect(m_copyButton, &QPushButton::clicked, this, &ApiKeysDialog::onCopyKeyClicked);
    connect(m_activateButton, &QPushButton::clicked, this, &ApiKeysDialog::onActivateKeyClicked);
    connect(m_keysTable, &QTableWidget::itemSelectionChanged, this, &ApiKeysDialog::onTableSelectionChanged);
}

void ApiKeysDialog::loadApiKeys()
{
    m_statusLabel->setText("加载 API Keys...");
    m_statusLabel->setStyleSheet("color: #3498db; font-size: 12px;");
    m_refreshButton->setEnabled(false);

    m_apiClient->getApiKeys();
}

void ApiKeysDialog::onRefreshClicked()
{
    loadApiKeys();
}

void ApiKeysDialog::onCopyKeyClicked()
{
    ApiKeyInfo selectedKey = getSelectedKey();
    if (selectedKey.key.isEmpty()) {
        QMessageBox::warning(this, "未选择", "请先选择一个 API Key。");
        return;
    }

    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(selectedKey.key);

    m_statusLabel->setText("✓ API Key 已复制到剪贴板！");
    m_statusLabel->setStyleSheet("color: #27ae60; font-size: 12px;");

    // 临时提示
    QTimer::singleShot(3000, this, [this]() {
        m_statusLabel->setText("");
    });
}

void ApiKeysDialog::onActivateKeyClicked()
{
    ApiKeyInfo selectedKey = getSelectedKey();
    if (selectedKey.key.isEmpty()) {
        QMessageBox::warning(this, "未选择", "请先选择一个 API Key。");
        return;
    }

    // 更新本地状态
    for (int i = 0; i < m_keys.size(); ++i) {
        m_keys[i].isActive = (m_keys[i].id == selectedKey.id);
    }

    m_activeKeyId = selectedKey.id;

    // 持久化激活的 Key，退出后仍记住
    QSettings settings;
    settings.setValue("apikeys/activeKeyId", m_activeKeyId);
    settings.setValue("apikeys/activeKey", selectedKey.key);

    // 刷新表格显示
    updateKeysTable(m_keys);

    // 发送信号
    emit keyActivated(selectedKey.id, selectedKey.key);

    m_statusLabel->setText(QString("✓ Key %1 已设为活跃！").arg(selectedKey.name));
    m_statusLabel->setStyleSheet("color: #27ae60; font-size: 12px;");
}

void ApiKeysDialog::onKeysReceived(const QJsonArray &keys)
{
    m_keys.clear();

    for (const QJsonValue &val : keys) {
        ApiKeyInfo info = ApiKeyInfo::fromJson(val.toObject());
        m_keys.append(info);
    }

    updateKeysTable(m_keys);

    m_refreshButton->setEnabled(true);
    m_totalKeysLabel->setText(QString("总计: %1 个 Key").arg(m_keys.size()));
    m_statusLabel->setText(QString("✓ 已加载 %1 个 API Keys").arg(m_keys.size()));
    m_statusLabel->setStyleSheet("color: #27ae60; font-size: 12px;");
}

void ApiKeysDialog::onRequestFailed(const QString &error)
{
    m_refreshButton->setEnabled(true);
    m_statusLabel->setText(QString("✗ 错误: %1").arg(error));
    m_statusLabel->setStyleSheet("color: #e74c3c; font-size: 12px;");

    QMessageBox::warning(this, "错误", QString("Failed to load API keys:\n%1").arg(error));
}

void ApiKeysDialog::updateKeysTable(const QList<ApiKeyInfo> &keys)
{
    m_keysTable->setRowCount(0);

    for (int i = 0; i < keys.size(); ++i) {
        const ApiKeyInfo &info = keys[i];
        m_keysTable->insertRow(i);

        // Name（★ 仅标记用户选中的激活 Key，与服务端 status 区分开）
        const bool isSelectedActive = (!m_activeKeyId.isEmpty() && info.id == m_activeKeyId);
        QString displayName = info.name;
        if (isSelectedActive) {
            displayName = "★ " + displayName;
        }
        QTableWidgetItem *nameItem = new QTableWidgetItem(displayName);
        if (isSelectedActive) {
            QFont font = nameItem->font();
            font.setBold(true);
            nameItem->setFont(font);
            nameItem->setForeground(QBrush(QColor("#27ae60")));
        }
        m_keysTable->setItem(i, 0, nameItem);

        // Status
        QTableWidgetItem *statusItem = new QTableWidgetItem(info.status);
        if (info.status.toLower() == "active") {
            statusItem->setForeground(QBrush(QColor("#27ae60")));
        } else {
            statusItem->setForeground(QBrush(QColor("#e74c3c")));
        }
        m_keysTable->setItem(i, 1, statusItem);

        // Key (masked)
        QString maskedKey = info.key;
        if (maskedKey.length() > 12) {
            maskedKey = maskedKey.left(8) + "..." + maskedKey.right(4);
        }
        m_keysTable->setItem(i, 2, new QTableWidgetItem(maskedKey));

        // Quota
        QString quotaStr = info.quota > 0 ? QString::number(info.quota) : "无限制";
        m_keysTable->setItem(i, 3, new QTableWidgetItem(quotaStr));

        // Used
        m_keysTable->setItem(i, 4, new QTableWidgetItem(QString::number(info.used)));

        // Usage %
        QString usagePercent = "-";
        if (info.quota > 0) {
            double percent = (double)info.used / info.quota * 100;
            usagePercent = QString::number(percent, 'f', 1) + "%";
        }
        QTableWidgetItem *usageItem = new QTableWidgetItem(usagePercent);
        if (info.quota > 0) {
            double percent = (double)info.used / info.quota * 100;
            if (percent >= 90) {
                usageItem->setForeground(QBrush(QColor("#e74c3c")));
            } else if (percent >= 70) {
                usageItem->setForeground(QBrush(QColor("#f39c12")));
            }
        }
        m_keysTable->setItem(i, 5, usageItem);

        // Created At
        QString createdAt = info.createdAt;
        if (!createdAt.isEmpty()) {
            QDateTime dt = QDateTime::fromString(createdAt, Qt::ISODate);
            if (dt.isValid()) {
                createdAt = dt.toString("yyyy-MM-dd");
            }
        }
        m_keysTable->setItem(i, 6, new QTableWidgetItem(createdAt));
    }
    // 注意：不要调用 resizeColumnsToContents()，否则会覆盖上面的 Stretch 设置，
    // 导致各列挤在左侧无法铺满窗口宽度。
}

void ApiKeysDialog::onTableSelectionChanged()
{
    bool hasSelection = !m_keysTable->selectedItems().isEmpty();
    m_copyButton->setEnabled(hasSelection);
    m_activateButton->setEnabled(hasSelection);
}

ApiKeyInfo ApiKeysDialog::getSelectedKey() const
{
    int currentRow = m_keysTable->currentRow();
    if (currentRow >= 0 && currentRow < m_keys.size()) {
        return m_keys[currentRow];
    }
    return ApiKeyInfo();
}

void ApiKeysDialog::setActiveKey(const QString &keyId)
{
    m_activeKeyId = keyId;
    updateKeysTable(m_keys);
}
