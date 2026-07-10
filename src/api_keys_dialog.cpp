#include "api_keys_dialog.h"
#include "app_theme.h"
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
#include <QFrame>
#include <QStyle>

ApiKeyInfo ApiKeyInfo::fromJson(const QJsonObject &obj)
{
    ApiKeyInfo info;
    info.id = obj["id"].isString()
        ? obj["id"].toString()
        : QString::number(obj["id"].toVariant().toLongLong());
    info.name = obj["name"].toString();
    info.key = obj["key"].toString();
    info.status = obj["status"].toString();
    info.quota = obj["quota"].toInt(0);
    info.used = obj["quota_used"].toInt(0);
    info.createdAt = obj["created_at"].toString();
    info.expiresAt = obj["expires_at"].toString();
    info.isActive = (obj["status"].toString() == "active");
    return info;
}

ApiKeysDialog::ApiKeysDialog(ApiClient *apiClient, QWidget *parent)
    : QDialog(parent)
    , m_apiClient(apiClient)
{
    QSettings settings;
    m_activeKeyId = settings.value("apikeys/activeKeyId").toString();
    settings.remove("apikeys/activeKey");

    setupUi();
    setWindowTitle("API Keys 管理");
    resize(960, 620);
    setMinimumSize(820, 500);

    connect(m_apiClient, &ApiClient::apiKeysReceived, this, &ApiKeysDialog::onKeysReceived);
    connect(m_apiClient, &ApiClient::requestFailed, this, &ApiKeysDialog::onRequestFailed);

    loadApiKeys();
}

void ApiKeysDialog::setupUi()
{
    setStyleSheet("QDialog { background-color: #f4f7f9; }");

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(12);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // ── 顶部标题卡片 ───────────────────────────────────────
    QFrame *headerCard = new QFrame(this);
    headerCard->setStyleSheet(
        "QFrame {"
        "  background: transparent;"
        "  border: none;"
        "}"
    );
    QHBoxLayout *headerLayout = new QHBoxLayout(headerCard);
    headerLayout->setContentsMargins(18, 14, 18, 14);

    QLabel *titleLabel = new QLabel("API Keys", this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(18);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    titleLabel->setStyleSheet("color: #1e293b;");
    headerLayout->addWidget(titleLabel);

    headerLayout->addStretch();

    m_totalKeysLabel = new QLabel("共 0 个 Key", this);
    m_totalKeysLabel->setStyleSheet(
        "QLabel {"
        "  color: #0f5f59;"
        "  background: #e7f5f2;"
        "  border: 1px solid #b7e4da;"
        "  border-radius: 7px;"
        "  padding: 3px 12px;"
        "  font-size: 12px;"
        "  font-weight: bold;"
        "}"
    );
    headerLayout->addWidget(m_totalKeysLabel);

    mainLayout->addWidget(headerCard);

    // ── 工具栏 ─────────────────────────────────────────────
    QFrame *toolbarCard = new QFrame(this);
    toolbarCard->setStyleSheet(
        "QFrame {"
        "  background: white;"
        "  border: 1.5px solid #e2e8f0;"
        "  border-radius: 8px;"
        "}"
    );
    QHBoxLayout *toolbarLayout = new QHBoxLayout(toolbarCard);
    toolbarLayout->setContentsMargins(14, 8, 14, 8);
    toolbarLayout->setSpacing(8);

    const QString ghostBtnStyle = AppTheme::secondaryButtonStyle();

    m_refreshButton = new QPushButton("刷新", this);
    m_refreshButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_refreshButton->setMinimumHeight(34);
    m_refreshButton->setCursor(Qt::PointingHandCursor);
    m_refreshButton->setStyleSheet(ghostBtnStyle);
    toolbarLayout->addWidget(m_refreshButton);

    m_copyButton = new QPushButton("复制 Key", this);
    m_copyButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    m_copyButton->setMinimumHeight(34);
    m_copyButton->setEnabled(false);
    m_copyButton->setCursor(Qt::PointingHandCursor);
    m_copyButton->setStyleSheet(ghostBtnStyle);
    toolbarLayout->addWidget(m_copyButton);

    m_activateButton = new QPushButton("设为首选", this);
    m_activateButton->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
    m_activateButton->setMinimumHeight(34);
    m_activateButton->setEnabled(false);
    m_activateButton->setCursor(Qt::PointingHandCursor);
    m_activateButton->setStyleSheet(AppTheme::primaryButtonStyle());
    toolbarLayout->addWidget(m_activateButton);

    m_testButton = new QPushButton("测试 Key", this);
    m_testButton->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
    m_testButton->setMinimumHeight(34);
    m_testButton->setEnabled(false);
    m_testButton->setCursor(Qt::PointingHandCursor);
    m_testButton->setStyleSheet(ghostBtnStyle);
    toolbarLayout->addWidget(m_testButton);

    toolbarLayout->addStretch();
    mainLayout->addWidget(toolbarCard);

    // ── Keys 表格 ──────────────────────────────────────────
    m_keysTable = new QTableWidget(this);
    m_keysTable->setColumnCount(7);
    m_keysTable->setHorizontalHeaderLabels({
        "名称", "状态", "Key", "配额", "已用", "使用率", "创建时间"
    });

    QHeaderView *header = m_keysTable->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    header->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(2, QHeaderView::Stretch);
    header->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(6, QHeaderView::ResizeToContents);

    m_keysTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_keysTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_keysTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_keysTable->setAlternatingRowColors(true);
    m_keysTable->verticalHeader()->setVisible(false);
    m_keysTable->setShowGrid(false);
    m_keysTable->setStyleSheet(
        "QTableWidget {"
        "  background: white;"
        "  border: 1.5px solid #e2e8f0;"
        "  border-radius: 8px;"
        "  outline: none;"
        "}"
        "QTableWidget::item {"
        "  padding: 8px 10px;"
        "  border: none;"
        "  color: #334155;"
        "}"
        "QTableWidget::item:selected {"
        "  background: #e7f5f2;"
        "  color: #0f5f59;"
        "}"
        "QTableWidget::item:alternate {"
        "  background: #fafbfc;"
        "}"
        "QHeaderView::section {"
        "  background: #f8fafc;"
        "  padding: 9px 10px;"
        "  border: none;"
        "  border-bottom: 2px solid #e2e8f0;"
        "  font-weight: bold;"
        "  color: #64748b;"
        "  font-size: 12px;"
        "}"
        "QScrollBar:vertical {"
        "  border: none;"
        "  background: #f8fafc;"
        "  width: 8px;"
        "  border-radius: 4px;"
        "}"
        "QScrollBar::handle:vertical {"
        "  background: #cbd5e1;"
        "  border-radius: 4px;"
        "}"
    );
    mainLayout->addWidget(m_keysTable);

    // ── 底栏（状态 + 关闭按钮）────────────────────────────
    QHBoxLayout *bottomLayout = new QHBoxLayout();

    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet("color: #64748b; font-size: 12px;");
    bottomLayout->addWidget(m_statusLabel);

    bottomLayout->addStretch();

    QPushButton *closeButton = new QPushButton("关闭", this);
    closeButton->setIcon(style()->standardIcon(QStyle::SP_DialogCloseButton));
    closeButton->setMinimumHeight(36);
    closeButton->setMinimumWidth(90);
    closeButton->setCursor(Qt::PointingHandCursor);
    closeButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    bottomLayout->addWidget(closeButton);

    mainLayout->addLayout(bottomLayout);

    // 信号连接
    connect(m_refreshButton,  &QPushButton::clicked, this, &ApiKeysDialog::onRefreshClicked);
    connect(m_copyButton,     &QPushButton::clicked, this, &ApiKeysDialog::onCopyKeyClicked);
    connect(m_activateButton, &QPushButton::clicked, this, &ApiKeysDialog::onActivateKeyClicked);
    connect(m_testButton, &QPushButton::clicked, this, &ApiKeysDialog::onTestKeyClicked);
    connect(m_apiClient, &ApiClient::apiKeyTested,
            this, &ApiKeysDialog::onKeyTested);
    connect(m_keysTable, &QTableWidget::itemSelectionChanged,
            this, &ApiKeysDialog::onTableSelectionChanged);
}

void ApiKeysDialog::loadApiKeys()
{
    m_statusLabel->setText("加载 API Keys...");
    m_statusLabel->setStyleSheet("color: #0f766e; font-size: 12px;");
    m_refreshButton->setEnabled(false);
    m_apiClient->getApiKeys();
}

void ApiKeysDialog::onRefreshClicked()  { loadApiKeys(); }

void ApiKeysDialog::onCopyKeyClicked()
{
    ApiKeyInfo selectedKey = getSelectedKey();
    if (selectedKey.key.isEmpty()) {
        QMessageBox::warning(this, "未选择", "请先选择一个 API Key。");
        return;
    }

    QApplication::clipboard()->setText(selectedKey.key);

    m_statusLabel->setText("✓ API Key 已复制到剪贴板！");
    m_statusLabel->setStyleSheet("color: #16a34a; font-size: 12px;");
    QTimer::singleShot(3000, this, [this]() { m_statusLabel->setText(""); });
    const QString copiedKey = selectedKey.key;
    QTimer::singleShot(60000, this, [copiedKey]() {
        if (QApplication::clipboard()->text() == copiedKey) {
            QApplication::clipboard()->clear();
        }
    });
}

void ApiKeysDialog::onActivateKeyClicked()
{
    ApiKeyInfo selectedKey = getSelectedKey();
    if (selectedKey.key.isEmpty()) {
        QMessageBox::warning(this, "未选择", "请先选择一个 API Key。");
        return;
    }

    for (int i = 0; i < m_keys.size(); ++i) {
        m_keys[i].isActive = (m_keys[i].id == selectedKey.id);
    }
    m_activeKeyId = selectedKey.id;

    QSettings settings;
    settings.setValue("apikeys/activeKeyId", m_activeKeyId);
    settings.remove("apikeys/activeKey");

    updateKeysTable(m_keys);
    emit keyActivated(selectedKey.id, selectedKey.key);

    m_statusLabel->setText(QString("Key「%1」已设为新档案首选").arg(selectedKey.name));
    m_statusLabel->setStyleSheet("color: #16a34a; font-size: 12px;");
}

void ApiKeysDialog::onTestKeyClicked()
{
    const ApiKeyInfo selectedKey = getSelectedKey();
    if (selectedKey.key.isEmpty()) {
        QMessageBox::warning(this, "未选择", "请先选择一个 API Key。");
        return;
    }
    m_testButton->setEnabled(false);
    m_statusLabel->setText(QString("正在测试 Key「%1」...").arg(selectedKey.name));
    m_statusLabel->setStyleSheet("color: #0f766e; font-size: 12px;");
    m_apiClient->testApiKey(selectedKey.id, selectedKey.key);
}

void ApiKeysDialog::onKeyTested(const QString &keyId, bool supported,
                                const QString &detail)
{
    const ApiKeyInfo selectedKey = getSelectedKey();
    if (selectedKey.id != keyId) {
        return;
    }
    m_testButton->setEnabled(true);
    m_statusLabel->setText(supported
        ? QString("Key 可用：%1").arg(detail)
        : QString("Key 不可用：%1").arg(detail));
    m_statusLabel->setStyleSheet(supported
        ? "color: #067647; font-size: 12px;"
        : "color: #b42318; font-size: 12px;");
}

void ApiKeysDialog::onKeysReceived(const QJsonArray &keys)
{
    m_keys.clear();
    for (const QJsonValue &val : keys) {
        m_keys.append(ApiKeyInfo::fromJson(val.toObject()));
    }

    updateKeysTable(m_keys);
    m_refreshButton->setEnabled(true);
    m_totalKeysLabel->setText(QString("共 %1 个 Key").arg(m_keys.size()));
    m_statusLabel->setText(QString("✓ 已加载 %1 个 API Keys").arg(m_keys.size()));
    m_statusLabel->setStyleSheet("color: #16a34a; font-size: 12px;");
}

void ApiKeysDialog::onRequestFailed(const QString &error)
{
    m_refreshButton->setEnabled(true);
    m_statusLabel->setText(QString("✗ 错误：%1").arg(error));
    m_statusLabel->setStyleSheet("color: #dc2626; font-size: 12px;");
    QMessageBox::warning(this, "错误", QString("加载 API Keys 失败：\n%1").arg(error));
}

void ApiKeysDialog::updateKeysTable(const QList<ApiKeyInfo> &keys)
{
    m_keysTable->setRowCount(0);

    for (int i = 0; i < keys.size(); ++i) {
        const ApiKeyInfo &info = keys[i];
        m_keysTable->insertRow(i);

        const bool isSelectedActive = (!m_activeKeyId.isEmpty() && info.id == m_activeKeyId);

        // 名称（活跃 Key 加星号 + 加粗）
        QString displayName = isSelectedActive ? ("当前 · " + info.name) : info.name;
        QTableWidgetItem *nameItem = new QTableWidgetItem(displayName);
        if (isSelectedActive) {
            QFont f = nameItem->font();
            f.setBold(true);
            nameItem->setFont(f);
            nameItem->setForeground(QBrush(QColor("#0f766e")));
        }
        m_keysTable->setItem(i, 0, nameItem);

        // 状态
        QTableWidgetItem *statusItem = new QTableWidgetItem(info.status);
        statusItem->setForeground(QBrush(QColor(
            info.status.toLower() == "active" ? "#16a34a" : "#dc2626")));
        m_keysTable->setItem(i, 1, statusItem);

        // Key（掩码）
        QString masked = info.key;
        if (masked.length() > 12) {
            masked = masked.left(8) + "..." + masked.right(4);
        }
        QTableWidgetItem *keyItem = new QTableWidgetItem(masked);
        QFont mono; mono.setFamily("Courier New");
        keyItem->setFont(mono);
        keyItem->setForeground(QBrush(QColor("#475569")));
        m_keysTable->setItem(i, 2, keyItem);

        // 配额
        QString quotaStr = info.quota > 0 ? QString::number(info.quota) : "无限制";
        m_keysTable->setItem(i, 3, new QTableWidgetItem(quotaStr));

        // 已用
        m_keysTable->setItem(i, 4, new QTableWidgetItem(QString::number(info.used)));

        // 使用率
        QString usagePercent = "-";
        QColor usageColor("#475569");
        if (info.quota > 0) {
            double pct = (double)info.used / info.quota * 100.0;
            usagePercent = QString::number(pct, 'f', 1) + "%";
            if (pct >= 90)      usageColor = QColor("#dc2626");
            else if (pct >= 70) usageColor = QColor("#d97706");
        }
        QTableWidgetItem *usageItem = new QTableWidgetItem(usagePercent);
        usageItem->setForeground(QBrush(usageColor));
        m_keysTable->setItem(i, 5, usageItem);

        // 创建时间
        QString createdAt = info.createdAt;
        QDateTime dt = QDateTime::fromString(createdAt, Qt::ISODate);
        if (dt.isValid()) createdAt = dt.toString("yyyy-MM-dd");
        m_keysTable->setItem(i, 6, new QTableWidgetItem(createdAt));
    }
}

void ApiKeysDialog::onTableSelectionChanged()
{
    bool has = !m_keysTable->selectedItems().isEmpty();
    m_copyButton->setEnabled(has);
    m_activateButton->setEnabled(has);
    m_testButton->setEnabled(has);
}

ApiKeyInfo ApiKeysDialog::getSelectedKey() const
{
    int row = m_keysTable->currentRow();
    if (row >= 0 && row < m_keys.size()) return m_keys[row];
    return ApiKeyInfo();
}

void ApiKeysDialog::setActiveKey(const QString &keyId)
{
    m_activeKeyId = keyId;
    updateKeysTable(m_keys);
}
