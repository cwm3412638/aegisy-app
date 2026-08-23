#include "usage_dialog.h"

#include "api_client.h"
#include "app_theme.h"
#include "companion_config_projection.h"
#include "companion_usage_projection.h"

#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonValue>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QUuid>

namespace {

double findNumber(const QJsonObject &object, const QStringList &keys, int depth = 0)
{
    for (const QString &key : keys) {
        const QJsonValue value = object.value(key);
        if (value.isDouble()) return value.toDouble();
        if (value.isString()) {
            bool ok = false;
            const double number = value.toString().toDouble(&ok);
            if (ok) return number;
        }
    }
    if (depth >= 3) return 0.0;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.value().isObject()) {
            const double nested = findNumber(it.value().toObject(), keys, depth + 1);
            if (nested != 0.0) return nested;
        }
    }
    return 0.0;
}

QString formatNumber(double value, int decimals = 0)
{
    return QLocale(QLocale::English).toString(value, 'f', decimals);
}

} // namespace

UsageDialog::UsageDialog(ApiClient *apiClient, QWidget *parent)
    : QDialog(parent)
    , m_apiClient(apiClient)
{
    setupUi();
    setWindowTitle(QStringLiteral("用量中心"));
    setMinimumSize(820, 560);
    resize(960, 640);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    connect(m_apiClient, &ApiClient::usageStatsReceived,
            this, &UsageDialog::onStatsReceived);
    connect(m_apiClient, &ApiClient::usageModelsReceived,
            this, &UsageDialog::onModelsReceived);
    connect(m_apiClient, &ApiClient::companionConfigurationReceived,
            this, &UsageDialog::onCompanionConfigurationReceived);
    connect(m_apiClient, &ApiClient::companionConfigurationFailed,
            this, &UsageDialog::onCompanionConfigurationFailed);
    connect(m_apiClient, &ApiClient::companionApiKeyUsageReceived,
            this, &UsageDialog::onCompanionApiKeyUsageReceived);
    connect(m_apiClient, &ApiClient::companionApiKeyUsageFailed,
            this, &UsageDialog::onCompanionApiKeyUsageFailed);
    connect(m_apiClient, &ApiClient::requestFailed,
            this, &UsageDialog::onRequestFailed);
    refreshData();
}

void UsageDialog::setupUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(22, 20, 22, 18);
    root->setSpacing(12);

    auto *header = new QHBoxLayout();
    auto *title = new QLabel(QStringLiteral("账号用量"), this);
    title->setStyleSheet(QStringLiteral(
        "font-size: 20px; font-weight: 700; color: #101828;"));
    header->addWidget(title);
    header->addStretch();
    m_rangeCombo = new QComboBox(this);
    m_rangeCombo->addItem(QStringLiteral("今日"), 1);
    m_rangeCombo->addItem(QStringLiteral("最近 7 天"), 7);
    m_rangeCombo->addItem(QStringLiteral("最近 30 天"), 30);
    m_rangeCombo->setCurrentIndex(1);
    header->addWidget(m_rangeCombo);
    m_refreshButton = new QPushButton(QStringLiteral("刷新"), this);
    m_refreshButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_refreshButton->setStyleSheet(AppTheme::primaryButtonStyle());
    header->addWidget(m_refreshButton);
    root->addLayout(header);

    auto *summary = new QHBoxLayout();
    summary->setSpacing(10);
    auto addMetric = [this, summary](const QString &label, QLabel **valueLabel) {
        auto *panel = new QWidget(this);
        panel->setStyleSheet(QStringLiteral(
            "QWidget { background: white; border: 1px solid #e4e7ec; border-radius: 8px; }"
            "QLabel { border: none; background: transparent; }"));
        auto *layout = new QVBoxLayout(panel);
        layout->setContentsMargins(14, 10, 14, 10);
        auto *caption = new QLabel(label, panel);
        caption->setStyleSheet(QStringLiteral("font-size: 11px; color: #667085;"));
        *valueLabel = new QLabel(QStringLiteral("--"), panel);
        (*valueLabel)->setStyleSheet(QStringLiteral(
            "font-size: 20px; font-weight: 700; color: #101828;"));
        layout->addWidget(caption);
        layout->addWidget(*valueLabel);
        summary->addWidget(panel, 1);
    };
    addMetric(QStringLiteral("消费金额"), &m_costValue);
    addMetric(QStringLiteral("请求次数"), &m_requestsValue);
    addMetric(QStringLiteral("Token 总量"), &m_tokensValue);
    root->addLayout(summary);

    auto *tabs = new QTabWidget(this);
    m_keysTable = new QTableWidget(tabs);
    m_keysTable->setColumnCount(8);
    m_keysTable->setHorizontalHeaderLabels({
        QStringLiteral("Key"), QStringLiteral("分组"), QStringLiteral("状态"),
        QStringLiteral("今日消费"), QStringLiteral("累计消费"),
        QStringLiteral("已用额度"), QStringLiteral("额度"), QStringLiteral("使用率") });
    m_keysTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_keysTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_keysTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_keysTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_keysTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_keysTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_keysTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    m_keysTable->horizontalHeader()->setSectionResizeMode(7, QHeaderView::ResizeToContents);
    m_keysTable->verticalHeader()->setVisible(false);
    m_keysTable->setEditTriggers(QTableWidget::NoEditTriggers);
    m_keysTable->setSelectionBehavior(QTableWidget::SelectRows);
    tabs->addTab(m_keysTable, QStringLiteral("API Key"));

    m_modelsTable = new QTableWidget(tabs);
    m_modelsTable->setColumnCount(4);
    m_modelsTable->setHorizontalHeaderLabels({
        QStringLiteral("模型"), QStringLiteral("请求"),
        QStringLiteral("Token"), QStringLiteral("消费") });
    m_modelsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_modelsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_modelsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_modelsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_modelsTable->verticalHeader()->setVisible(false);
    m_modelsTable->setEditTriggers(QTableWidget::NoEditTriggers);
    tabs->addTab(m_modelsTable, QStringLiteral("模型统计"));
    root->addWidget(tabs, 1);

    auto *footer = new QHBoxLayout();
    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #667085;"));
    footer->addWidget(m_statusLabel, 1);
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), this);
    closeButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    footer->addWidget(closeButton);
    root->addLayout(footer);

    connect(m_refreshButton, &QPushButton::clicked, this, &UsageDialog::refreshData);
    connect(m_rangeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &UsageDialog::refreshData);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
}

void UsageDialog::refreshData()
{
    if (m_companionConfigurationRetired) return;
    const int days = m_rangeCombo->currentData().toInt();
    m_pendingRequests = 3;
    m_refreshButton->setEnabled(false);
    m_statusLabel->setText(QStringLiteral("正在读取用量数据..."));
    m_apiClient->getUsageStats(days);
    m_apiClient->getUsageModels(days);
    m_apiClient->getApiKeys();
}

void UsageDialog::onStatsReceived(const QJsonObject &stats)
{
    if (m_companionConfigurationRetired) return;
    m_stats = stats;
    updateSummary();
    if (--m_pendingRequests <= 0) {
        m_refreshButton->setEnabled(true);
        m_statusLabel->setText(QStringLiteral("用量数据已更新。"));
    }
}

void UsageDialog::onModelsReceived(const QJsonArray &models)
{
    if (m_companionConfigurationRetired) return;
    m_models = models;
    updateModelsTable();
    if (--m_pendingRequests <= 0) {
        m_refreshButton->setEnabled(true);
        m_statusLabel->setText(QStringLiteral("用量数据已更新。"));
    }
}

void UsageDialog::onCompanionConfigurationReceived(
    const QJsonObject &projection)
{
    if (!CompanionConfigProjection::validate(projection)) {
        onCompanionConfigurationFailed(QStringLiteral("projection-response-invalid"));
        return;
    }
    m_companionConfigurationRetired = false;
    m_rangeCombo->setEnabled(true);
    m_keys = projection.value(QStringLiteral("keys")).toArray();
    updateKeysTable();
    if (!m_keys.isEmpty()) {
        ++m_pendingRequests;
        m_usageRequestId = QStringLiteral("usage-dialog-%1").arg(
            QUuid::createUuid().toString(QUuid::WithoutBraces));
        m_usageAccountIdentity = projection.value(
            QStringLiteral("account_identity")).toString();
        m_usageConfigurationProjectionSha256 = projection.value(
            QStringLiteral("projection_sha256")).toString();
        m_apiClient->getCompanionApiKeyUsage(
            m_usageRequestId, m_usageAccountIdentity,
            m_usageConfigurationProjectionSha256);
    }
    if (--m_pendingRequests <= 0) {
        m_refreshButton->setEnabled(true);
        m_statusLabel->setText(QStringLiteral("用量数据已更新。"));
    }
}

void UsageDialog::onCompanionConfigurationFailed(const QString &errorCode)
{
    m_companionConfigurationRetired = true;
    m_pendingRequests = 0;
    m_usageRequestId.clear();
    m_usageAccountIdentity.clear();
    m_usageConfigurationProjectionSha256.clear();
    m_stats = QJsonObject();
    m_models = QJsonArray();
    m_keys = QJsonArray();
    updateModelsTable();
    updateKeysTable();
    m_costValue->setText(QStringLiteral("--"));
    m_requestsValue->setText(QStringLiteral("--"));
    m_tokensValue->setText(QStringLiteral("--"));
    m_rangeCombo->setEnabled(false);
    m_refreshButton->setEnabled(false);
    m_statusLabel->setText(QStringLiteral("账号 Key 元数据读取失败：%1").arg(errorCode));
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #b42318;"));
}

void UsageDialog::onCompanionApiKeyUsageReceived(
    const QString &requestId, const QJsonObject &projection)
{
    if (requestId != m_usageRequestId
            || projection.value(QStringLiteral("account_identity")).toString()
                != m_usageAccountIdentity
            || projection.value(
                QStringLiteral("configuration_projection_sha256")).toString()
                != m_usageConfigurationProjectionSha256
            || !CompanionUsageProjection::validate(projection)) {
        return;
    }
    m_usageRequestId.clear();
    m_usageAccountIdentity.clear();
    m_usageConfigurationProjectionSha256.clear();
    m_keys = projection.value(QStringLiteral("keys")).toArray();
    updateKeysTable();
    if (--m_pendingRequests <= 0) {
        m_refreshButton->setEnabled(true);
        m_statusLabel->setText(QStringLiteral("用量数据已更新。"));
    }
}

void UsageDialog::onCompanionApiKeyUsageFailed(
    const QString &requestId, const QString &errorCode)
{
    if (requestId != m_usageRequestId) return;
    m_usageRequestId.clear();
    m_usageAccountIdentity.clear();
    m_usageConfigurationProjectionSha256.clear();
    m_pendingRequests = qMax(0, m_pendingRequests - 1);
    m_refreshButton->setEnabled(true);
    m_statusLabel->setText(QStringLiteral("逐 Key 用量读取失败：%1").arg(errorCode));
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #b42318;"));
}

void UsageDialog::updateSummary()
{
    const double cost = findNumber(m_stats,
        { QStringLiteral("total_cost"), QStringLiteral("total_amount"),
          QStringLiteral("total_spend"), QStringLiteral("cost") });
    const double requests = findNumber(m_stats,
        { QStringLiteral("total_requests"), QStringLiteral("request_count"),
          QStringLiteral("requests") });
    const double tokens = findNumber(m_stats,
        { QStringLiteral("total_tokens"), QStringLiteral("tokens") });
    m_costValue->setText(QStringLiteral("$%1").arg(formatNumber(cost, 4)));
    m_requestsValue->setText(formatNumber(requests));
    m_tokensValue->setText(formatNumber(tokens));
}

void UsageDialog::updateModelsTable()
{
    m_modelsTable->setRowCount(0);
    for (const QJsonValue &value : m_models) {
        const QJsonObject object = value.toObject();
        const int row = m_modelsTable->rowCount();
        m_modelsTable->insertRow(row);
        QString name = object.value(QStringLiteral("model")).toString();
        if (name.isEmpty()) name = object.value(QStringLiteral("model_name")).toString();
        if (name.isEmpty()) name = object.value(QStringLiteral("name")).toString();
        m_modelsTable->setItem(row, 0, new QTableWidgetItem(name));
        m_modelsTable->setItem(row, 1, new QTableWidgetItem(formatNumber(
            findNumber(object, { QStringLiteral("requests"), QStringLiteral("request_count") }))));
        m_modelsTable->setItem(row, 2, new QTableWidgetItem(formatNumber(
            findNumber(object, { QStringLiteral("total_tokens"), QStringLiteral("tokens") }))));
        m_modelsTable->setItem(row, 3, new QTableWidgetItem(QStringLiteral("$%1").arg(formatNumber(
            findNumber(object, { QStringLiteral("cost"), QStringLiteral("total_cost"),
                                 QStringLiteral("amount") }), 4))));
    }
}

void UsageDialog::updateKeysTable()
{
    m_keysTable->setRowCount(0);
    for (const QJsonValue &value : m_keys) {
        const QJsonObject object = value.toObject();
        const int row = m_keysTable->rowCount();
        m_keysTable->insertRow(row);
        QString name = object.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) name = object.value(QStringLiteral("display_name")).toString();
        if (name.isEmpty()) name = QStringLiteral("未命名 Key");
        QString group = object.value(QStringLiteral("group_label")).toString();
        if (group.isEmpty()) {
            group = object.value(QStringLiteral("group")).toObject()
                .value(QStringLiteral("name")).toString();
        }
        const double used = object.value(QStringLiteral("quota_used")).toDouble();
        const double quota = object.value(QStringLiteral("quota")).toDouble();
        const double percent = quota > 0.0 ? used / quota * 100.0 : 0.0;
        m_keysTable->setItem(row, 0, new QTableWidgetItem(name));
        m_keysTable->setItem(row, 1, new QTableWidgetItem(group));
        QString state = object.value(QStringLiteral("state")).toString();
        if (state.isEmpty()) state = object.value(QStringLiteral("status")).toString();
        m_keysTable->setItem(row, 2, new QTableWidgetItem(state));
        m_keysTable->setItem(row, 3, new QTableWidgetItem(QStringLiteral("$%1").arg(
            formatNumber(object.value(QStringLiteral("today_actual_cost")).toDouble(), 4))));
        m_keysTable->setItem(row, 4, new QTableWidgetItem(QStringLiteral("$%1").arg(
            formatNumber(object.value(QStringLiteral("total_actual_cost")).toDouble(), 4))));
        m_keysTable->setItem(row, 5, new QTableWidgetItem(formatNumber(used, 4)));
        m_keysTable->setItem(row, 6, new QTableWidgetItem(quota > 0.0 ? formatNumber(quota, 4)
                                                                      : QStringLiteral("不限")));
        m_keysTable->setItem(row, 7, new QTableWidgetItem(quota > 0.0
            ? QStringLiteral("%1%").arg(formatNumber(percent, 1)) : QStringLiteral("--")));
    }
}

void UsageDialog::onRequestFailed(const QString &error)
{
    if (m_companionConfigurationRetired) return;
    m_pendingRequests = qMax(0, m_pendingRequests - 1);
    m_refreshButton->setEnabled(true);
    m_statusLabel->setText(QStringLiteral("部分用量数据加载失败：%1").arg(error));
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #b42318;"));
}
