#include "models_dialog.h"
#include "app_theme.h"
#include "companion_config_projection.h"
#include "companion_model_projection.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QClipboard>
#include <QApplication>
#include <QMessageBox>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QSet>
#include <QLineEdit>
#include <QFrame>
#include <QStyle>
#include <QTimer>
#include <QUuid>

#include <limits>

namespace {

constexpr int kModelRowKindRole = Qt::UserRole + 31;
constexpr int kCachedKeyIdentityRole = Qt::UserRole + 32;
constexpr int kCachedPlatformRole = Qt::UserRole + 33;
constexpr int kCachedObservationRole = Qt::UserRole + 34;
constexpr int kCachedRevisionRole = Qt::UserRole + 35;
constexpr int kLiveWebsiteRow = 1;
constexpr int kCachedWebsiteRow = 2;

} // namespace

ModelInfo ModelInfo::fromJson(const QJsonObject &obj)
{
    ModelInfo info;
    info.id = obj["id"].toString();
    info.name = info.id;
    info.provider = obj["owned_by"].toString();
    qint64 created = obj["created"].toVariant().toLongLong();
    if (created > 0) {
        info.created = QDateTime::fromSecsSinceEpoch(created).toString("yyyy-MM-dd");
    }
    return info;
}

ModelsDialog::ModelsDialog(ApiClient *apiClient, QWidget *parent)
    : ModelsDialog(
        apiClient, QString(), CompanionConfigurationCachePresentation{}, parent)
{
}

ModelsDialog::ModelsDialog(
    ApiClient *apiClient,
    const QString &expectedAccountIdentity,
    const CompanionConfigurationCachePresentation &cachedPresentation,
    QWidget *parent)
    : QDialog(parent)
    , m_apiClient(apiClient)
    , m_expectedAccountIdentity(expectedAccountIdentity)
    , m_cachedPresentation(cachedPresentation)
{
    setupUi();
    setWindowTitle("模型列表");
    resize(760, 600);

    connect(m_apiClient, &ApiClient::companionConfigurationReceived,
            this, &ModelsDialog::onCompanionConfigurationReceived);
    connect(m_apiClient, &ApiClient::companionConfigurationFailed,
            this, &ModelsDialog::onCompanionConfigurationFailed);
    connect(m_apiClient, &ApiClient::companionModelsReceived,
            this, &ModelsDialog::onCompanionModelsReceived);
    connect(m_apiClient, &ApiClient::companionModelsFailed,
            this, &ModelsDialog::onCompanionModelsFailed);
    connect(m_apiClient, &ApiClient::authenticationExpired, this, [this]() {
        m_cachedPresentation = CompanionConfigurationCachePresentation{};
        m_expectedAccountIdentity.clear();
        onCompanionConfigurationFailed(QStringLiteral("authentication-expired"));
    });

    renderCachedPresentation();
    loadApiKeys();
}

void ModelsDialog::setupUi()
{
    setStyleSheet("QDialog { background-color: #f4f7f9; }");

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(12);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // ── 标题卡片 ────────────────────────────────────────────
    QFrame *headerCard = new QFrame(this);
    headerCard->setStyleSheet(
        "QFrame {"
        "  background: transparent;"
        "  border: none;"
        "}"
    );
    QHBoxLayout *headerLayout = new QHBoxLayout(headerCard);
    headerLayout->setContentsMargins(18, 14, 18, 14);

    QLabel *titleLabel = new QLabel("账号支持的模型", this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(18);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    titleLabel->setStyleSheet("color: #1e293b;");
    headerLayout->addWidget(titleLabel);

    headerLayout->addStretch();

    m_totalLabel = new QLabel("共 0 个模型", this);
    m_totalLabel->setStyleSheet(
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
    headerLayout->addWidget(m_totalLabel);

    mainLayout->addWidget(headerCard);

    // ── API Key 选择行 ─────────────────────────────────────
    QFrame *keyCard = new QFrame(this);
    keyCard->setStyleSheet(
        "QFrame {"
        "  background: white;"
        "  border: 1.5px solid #e2e8f0;"
        "  border-radius: 8px;"
        "}"
    );
    QHBoxLayout *keyLayout = new QHBoxLayout(keyCard);
    keyLayout->setContentsMargins(14, 10, 14, 10);
    keyLayout->setSpacing(10);

    QLabel *keyLabel = new QLabel("API Key", this);
    keyLabel->setStyleSheet("color: #374151; font-size: 13px; font-weight: bold;");
    keyLayout->addWidget(keyLabel);

    m_keyCombo = new QComboBox(this);
    m_keyCombo->setObjectName(QStringLiteral("modelsCacheKeyCombo"));
    m_keyCombo->setEditable(false);
    m_keyCombo->setInsertPolicy(QComboBox::NoInsert);
    m_keyCombo->setMinimumWidth(340);
    m_keyCombo->setMinimumHeight(34);
    m_keyCombo->setStyleSheet(
        "QComboBox {"
        "  background: white;"
        "  border: 1.5px solid #e2e8f0;"
        "  border-radius: 7px;"
        "  padding: 0 10px;"
        "  font-size: 13px;"
        "  color: #1e293b;"
        "}"
        "QComboBox:focus {"
        "  border-color: #0f766e;"
        "}"
        "QComboBox::drop-down {"
        "  border: none;"
        "  padding-right: 8px;"
        "}"
        "QComboBox QAbstractItemView {"
        "  border: 1.5px solid #e2e8f0;"
        "  border-radius: 6px;"
        "  background: white;"
        "  selection-background-color: #e7f5f2;"
        "  selection-color: #0f5f59;"
        "}"
    );
    keyLayout->addWidget(m_keyCombo, 1);

    m_refreshButton = new QPushButton("查询模型", this);
    m_refreshButton->setObjectName(QStringLiteral("modelsQueryButton"));
    m_refreshButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_refreshButton->setMinimumHeight(34);
    m_refreshButton->setCursor(Qt::PointingHandCursor);
    m_refreshButton->setStyleSheet(AppTheme::primaryButtonStyle());
    keyLayout->addWidget(m_refreshButton);

    mainLayout->addWidget(keyCard);

    // ── 筛选区 ─────────────────────────────────────────────
    QFrame *filterCard = new QFrame(this);
    filterCard->setStyleSheet(
        "QFrame {"
        "  background: white;"
        "  border: 1.5px solid #e2e8f0;"
        "  border-radius: 8px;"
        "}"
    );
    QHBoxLayout *filterLayout = new QHBoxLayout(filterCard);
    filterLayout->setContentsMargins(14, 10, 14, 10);
    filterLayout->setSpacing(12);

    const QString labelStyle = "color: #374151; font-size: 13px; font-weight: bold;";
    const QString inputStyle =
        "QComboBox, QLineEdit {"
        "  background: #f8fafc;"
        "  border: 1.5px solid #e2e8f0;"
        "  border-radius: 6px;"
        "  padding: 0 10px;"
        "  font-size: 13px;"
        "  color: #1e293b;"
        "  min-height: 32px;"
        "}"
        "QComboBox:focus, QLineEdit:focus {"
        "  border-color: #0f766e;"
        "  background: white;"
        "}";

    QLabel *providerLabel = new QLabel("提供方", this);
    providerLabel->setStyleSheet(labelStyle);
    filterLayout->addWidget(providerLabel);

    m_providerCombo = new QComboBox(this);
    m_providerCombo->setObjectName(QStringLiteral("modelsProviderFilter"));
    m_providerCombo->setMinimumWidth(180);
    m_providerCombo->addItem("全部提供方", "");
    m_providerCombo->setStyleSheet(inputStyle +
        "QComboBox::drop-down { border: none; padding-right: 8px; }"
        "QComboBox QAbstractItemView {"
        "  border: 1.5px solid #e2e8f0; background: white;"
        "  selection-background-color: #e7f5f2; selection-color: #0f5f59;"
        "}");
    filterLayout->addWidget(m_providerCombo);

    // 竖分隔线
    QFrame *vLine = new QFrame(this);
    vLine->setFrameShape(QFrame::VLine);
    vLine->setFixedHeight(18);
    vLine->setStyleSheet("color: #e2e8f0;");
    filterLayout->addWidget(vLine);

    QLabel *searchLabel = new QLabel("搜索", this);
    searchLabel->setStyleSheet(labelStyle);
    filterLayout->addWidget(searchLabel);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setObjectName(QStringLiteral("modelsSearchEdit"));
    m_searchEdit->setPlaceholderText("输入模型名称搜索...");
    m_searchEdit->setMinimumWidth(260);
    m_searchEdit->setStyleSheet(inputStyle);
    filterLayout->addWidget(m_searchEdit, 1);

    mainLayout->addWidget(filterCard);

    // ── 工具栏 ─────────────────────────────────────────────
    QHBoxLayout *toolbarLayout = new QHBoxLayout();
    toolbarLayout->setContentsMargins(0, 0, 0, 0);

    m_copyButton = new QPushButton("复制模型名称", this);
    m_copyButton->setObjectName(QStringLiteral("modelsCopyButton"));
    m_copyButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    m_copyButton->setMinimumHeight(34);
    m_copyButton->setEnabled(false);
    m_copyButton->setCursor(Qt::PointingHandCursor);
    m_copyButton->setStyleSheet(AppTheme::primaryButtonStyle());
    toolbarLayout->addWidget(m_copyButton);
    toolbarLayout->addStretch();
    mainLayout->addLayout(toolbarLayout);

    // ── 模型表格 ────────────────────────────────────────────
    m_modelsTable = new QTableWidget(this);
    m_modelsTable->setObjectName(QStringLiteral("modelsTable"));
    m_modelsTable->setColumnCount(4);
    m_modelsTable->setHorizontalHeaderLabels(
        { "模型名称", "提供方", "来源", "捕获时间" });

    m_modelsTable->horizontalHeader()->setStretchLastSection(true);
    m_modelsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_modelsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_modelsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_modelsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_modelsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_modelsTable->setAlternatingRowColors(true);
    m_modelsTable->verticalHeader()->setVisible(false);
    m_modelsTable->setShowGrid(false);
    m_modelsTable->setStyleSheet(
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
        "QTableWidget::item:alternate { background: #fafbfc; }"
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
    mainLayout->addWidget(m_modelsTable);

    // ── 底栏 ────────────────────────────────────────────────
    QHBoxLayout *bottomLayout = new QHBoxLayout();

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("modelsCacheStatus"));
    m_statusLabel->setTextFormat(Qt::PlainText);
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
    connect(m_refreshButton, &QPushButton::clicked, this, &ModelsDialog::onRefreshClicked);
    connect(m_keyCombo, QOverload<int>::of(&QComboBox::activated),
            this, &ModelsDialog::onRefreshClicked);
    connect(m_copyButton, &QPushButton::clicked, this, &ModelsDialog::onCopyModelClicked);
    connect(m_providerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ModelsDialog::onProviderChanged);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &ModelsDialog::onSearchTextChanged);
    connect(m_modelsTable, &QTableWidget::itemSelectionChanged,
            this, &ModelsDialog::onTableSelectionChanged);
}

void ModelsDialog::loadApiKeys()
{
    m_statusLabel->setText("正在读取账号 API Key...");
    m_statusLabel->setStyleSheet("color: #0f766e; font-size: 12px;");
    m_apiClient->getApiKeys();
}

QString ModelsDialog::currentKeyIdentity() const
{
    if (!m_keyCombo || m_keyCombo->currentIndex() < 0) return {};
    return m_sourceMode == SourceMode::CachedDisplay
        ? m_keyCombo->currentData(kCachedKeyIdentityRole).toString()
        : m_keyCombo->currentData(Qt::UserRole + 1).toString();
}

void ModelsDialog::loadModels()
{
    if (m_sourceMode == SourceMode::CachedDisplay) {
        renderCachedModels();
        return;
    }
    if (!m_keyCombo || m_sourceMode != SourceMode::LiveWebsite
            || m_keyCombo->currentData(kModelRowKindRole).toInt()
                != kLiveWebsiteRow) {
        clearModels();
        m_statusLabel->setText(QStringLiteral("没有可查询的实时网站配置"));
        m_refreshButton->setEnabled(false);
        return;
    }
    if (m_keyCombo->currentIndex() < 0
            || currentKeyIdentity().isEmpty()) {
        m_statusLabel->setText("✗ 请先选择一个已验证的 API Key");
        m_statusLabel->setStyleSheet("color: #dc2626; font-size: 12px;");
        return;
    }
    clearModels();
    m_statusLabel->setText("加载模型列表...");
    m_statusLabel->setStyleSheet("color: #0f766e; font-size: 12px;");
    m_refreshButton->setEnabled(false);
    const int index = m_keyCombo->currentIndex();
    m_modelRequestId = QStringLiteral("models-dialog-%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_modelRequestKeyIdentity = m_keyCombo->itemData(
        index, Qt::UserRole + 1).toString();
    m_modelRequestAccountIdentity = m_keyCombo->itemData(
        index, Qt::UserRole + 2).toString();
    m_modelRequestProjectionSha256 = m_keyCombo->itemData(
        index, Qt::UserRole + 3).toString();
    m_modelRequestHandle = m_keyCombo->itemData(index, Qt::UserRole).toString();
    m_modelRequestPlatform = m_keyCombo->itemData(
        index, Qt::UserRole + 5).toString();
    m_apiClient->getCompanionModels(
        m_modelRequestId, m_modelRequestAccountIdentity,
        m_modelRequestKeyIdentity, m_modelRequestHandle,
        m_modelRequestProjectionSha256, m_modelRequestPlatform);
}

void ModelsDialog::onRefreshClicked()      { loadModels(); }
void ModelsDialog::onProviderChanged(int i) {
    m_selectedProvider = m_providerCombo->itemData(i).toString();
    filterModels();
}
void ModelsDialog::onSearchTextChanged(const QString &) { filterModels(); }

void ModelsDialog::onCopyModelClicked()
{
    ModelInfo model = getSelectedModel();
    if (model.name.isEmpty()) {
        QMessageBox::warning(this, "未选择", "请先选择一个模型。");
        return;
    }
    QApplication::clipboard()->setText(model.name);
    m_statusLabel->setText(QString("✓ 已复制模型名称「%1」").arg(model.name));
    m_statusLabel->setStyleSheet("color: #16a34a; font-size: 12px;");
}

void ModelsDialog::onTableSelectionChanged()
{
    m_copyButton->setEnabled(!getSelectedModel().name.isEmpty());
}

void ModelsDialog::onCompanionConfigurationReceived(const QJsonObject &projection)
{
    if (!CompanionConfigProjection::validate(projection)) {
        onCompanionConfigurationFailed(QStringLiteral("projection-response-invalid"));
        return;
    }
    const QString projectionAccount = projection.value(
        QStringLiteral("account_identity")).toString();
    if (!m_expectedAccountIdentity.isEmpty()
            && projectionAccount != m_expectedAccountIdentity) {
        m_cachedPresentation = CompanionConfigurationCachePresentation{};
        onCompanionConfigurationFailed(
            QStringLiteral("projection-account-mismatch"));
        return;
    }
    const QString previousHandle = m_keyCombo->currentData(Qt::UserRole).toString();
    m_modelRequestId.clear();
    m_modelRequestKeyIdentity.clear();
    m_modelRequestHandle.clear();
    m_modelRequestAccountIdentity.clear();
    m_modelRequestProjectionSha256.clear();
    m_modelRequestPlatform.clear();
    clearModels();
    m_sourceMode = SourceMode::LiveWebsite;
    m_refreshButton->setEnabled(true);
    m_companionProjection = projection;

    m_keyCombo->blockSignals(true);
    m_keyCombo->clear();
    int selectedIndex = -1;
    for (const QJsonValue &value : projection.value(QStringLiteral("keys")).toArray()) {
        const QJsonObject candidate = value.toObject();
        if (candidate.value(QStringLiteral("state")).toString()
                    != QStringLiteral("active")
                || candidate.value(QStringLiteral("credential_state")).toString()
                    != QStringLiteral("available-in-secure-storage")) {
            continue;
        }
        const QString handle = candidate.value(
            QStringLiteral("credential_handle")).toString();
        if (handle.isEmpty()) continue;
        const QString display = QStringLiteral("%1 · %2")
            .arg(candidate.value(QStringLiteral("display_name")).toString(),
                 candidate.value(QStringLiteral("group_label")).toString());
        m_keyCombo->addItem(display, handle);
        const int index = m_keyCombo->count() - 1;
        m_keyCombo->setItemData(index, kLiveWebsiteRow, kModelRowKindRole);
        m_keyCombo->setItemData(
            index, candidate.value(QStringLiteral("key_identity")), Qt::UserRole + 1);
        m_keyCombo->setItemData(
            index, projection.value(QStringLiteral("account_identity")), Qt::UserRole + 2);
        m_keyCombo->setItemData(
            index, projection.value(QStringLiteral("projection_sha256")), Qt::UserRole + 3);
        m_keyCombo->setItemData(
            index, candidate.value(QStringLiteral("platform")), Qt::UserRole + 5);
        if (!previousHandle.isEmpty() && handle == previousHandle) selectedIndex = index;
    }
    m_keyCombo->blockSignals(false);
    m_keyCombo->setEnabled(true);
    m_providerCombo->setEnabled(true);
    m_searchEdit->setEnabled(true);

    if (m_keyCombo->count() == 0) {
        m_statusLabel->setText("✗ 未找到可用的已验证 API Key");
        m_statusLabel->setStyleSheet("color: #dc2626; font-size: 12px;");
        return;
    }
    m_keyCombo->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
    loadModels();
}

void ModelsDialog::onCompanionConfigurationFailed(const QString &errorCode)
{
    m_companionProjection = QJsonObject();
    m_modelRequestId.clear();
    m_modelRequestKeyIdentity.clear();
    m_modelRequestHandle.clear();
    m_modelRequestAccountIdentity.clear();
    m_modelRequestProjectionSha256.clear();
    m_modelRequestPlatform.clear();
    renderCachedPresentation(errorCode);
}

void ModelsDialog::onCompanionModelsReceived(
    const QString &requestId, const QString &keyIdentity,
    const QJsonObject &projection)
{
    if (m_sourceMode != SourceMode::LiveWebsite
            || requestId != m_modelRequestId
            || keyIdentity != m_modelRequestKeyIdentity
            || currentKeyIdentity() != keyIdentity
            || m_keyCombo->currentData(Qt::UserRole).toString() != m_modelRequestHandle
            || m_keyCombo->currentData(Qt::UserRole + 2).toString()
                != m_modelRequestAccountIdentity
            || m_keyCombo->currentData(Qt::UserRole + 3).toString()
                != m_modelRequestProjectionSha256
            || m_keyCombo->currentData(Qt::UserRole + 5).toString()
                != m_modelRequestPlatform
            || !CompanionModelProjection::validate(projection)) {
        return;
    }
    m_models.clear();
    for (const QJsonValue &value : projection.value(QStringLiteral("models")).toArray()) {
        ModelInfo info;
        info.id = value.toString();
        info.name = info.id;
        info.provider = m_modelRequestPlatform;
        info.source = QStringLiteral("网站实时");
        m_models.append(info);
    }
    m_modelRequestId.clear();
    m_modelRequestKeyIdentity.clear();
    m_modelRequestHandle.clear();
    m_modelRequestAccountIdentity.clear();
    m_modelRequestProjectionSha256.clear();
    m_modelRequestPlatform.clear();
    rebuildProviderFilter();
    filterModels();
    m_refreshButton->setEnabled(true);
    m_statusLabel->setText(QString("✓ 已加载 %1 个模型").arg(m_models.size()));
    m_statusLabel->setStyleSheet("color: #16a34a; font-size: 12px;");
}

void ModelsDialog::onCompanionModelsFailed(
    const QString &requestId, const QString &keyIdentity, const QString &errorCode)
{
    if (m_sourceMode != SourceMode::LiveWebsite
            || requestId != m_modelRequestId
            || keyIdentity != m_modelRequestKeyIdentity
            || currentKeyIdentity() != keyIdentity) {
        return;
    }
    m_modelRequestId.clear();
    m_modelRequestKeyIdentity.clear();
    m_modelRequestHandle.clear();
    m_modelRequestAccountIdentity.clear();
    m_modelRequestProjectionSha256.clear();
    m_modelRequestPlatform.clear();
    clearModels();
    m_refreshButton->setEnabled(true);
    m_statusLabel->setText(QStringLiteral("✗ 模型查询失败：%1").arg(errorCode));
    m_statusLabel->setStyleSheet("color: #dc2626; font-size: 12px;");
}

void ModelsDialog::rebuildProviderFilter()
{
    QStringList providers;
    QSet<QString> seen;
    for (const ModelInfo &model : m_models) {
        if (!model.provider.isEmpty() && !seen.contains(model.provider)) {
            seen.insert(model.provider);
            providers.append(model.provider);
        }
    }
    providers.sort(Qt::CaseInsensitive);

    const QString previous = m_selectedProvider;
    m_providerCombo->blockSignals(true);
    m_providerCombo->clear();
    m_providerCombo->addItem("全部提供方", "");
    for (const QString &p : providers) m_providerCombo->addItem(p, p);
    int idx = m_providerCombo->findData(previous);
    m_providerCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    m_selectedProvider = m_providerCombo->currentData().toString();
    m_providerCombo->blockSignals(false);
}

void ModelsDialog::updateModelsTable(const QList<ModelInfo> &models)
{
    m_modelsTable->setRowCount(0);
    int row = 0;
    for (const ModelInfo &model : models) {
        m_modelsTable->insertRow(row);

        QTableWidgetItem *nameItem = new QTableWidgetItem(model.name);
        QFont mono; mono.setFamily("Courier New");
        nameItem->setFont(mono);
        m_modelsTable->setItem(row, 0, nameItem);
        m_modelsTable->setItem(row, 1, new QTableWidgetItem(model.provider));
        m_modelsTable->setItem(row, 2, new QTableWidgetItem(model.source));
        m_modelsTable->setItem(row, 3, new QTableWidgetItem(model.created));
        row++;
    }
    m_modelsTable->resizeColumnsToContents();
    m_modelsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
}

void ModelsDialog::filterModels()
{
    QList<ModelInfo> filtered;
    const QString searchText = m_searchEdit->text().toLower();
    for (const ModelInfo &model : m_models) {
        if (!m_selectedProvider.isEmpty() && model.provider != m_selectedProvider) continue;
        if (!searchText.isEmpty() && !model.name.toLower().contains(searchText)) continue;
        filtered.append(model);
    }
    updateModelsTable(filtered);
    m_totalLabel->setText(filtered.size() == m_models.size()
        ? QString("共 %1 个模型").arg(m_models.size())
        : QString("显示 %1 / %2 个模型").arg(filtered.size()).arg(m_models.size()));
}

ModelInfo ModelsDialog::getSelectedModel() const
{
    int row = m_modelsTable->currentRow();
    if (row >= 0 && row < m_modelsTable->rowCount()) {
        ModelInfo model;
        model.name     = m_modelsTable->item(row, 0)->text();
        model.id       = model.name;
        model.provider = m_modelsTable->item(row, 1)->text();
        model.source   = m_modelsTable->item(row, 2)->text();
        model.created  = m_modelsTable->item(row, 3)->text();
        return model;
    }
    return ModelInfo();
}

void ModelsDialog::clearModels()
{
    m_models.clear();
    m_selectedProvider.clear();
    rebuildProviderFilter();
    filterModels();
    m_copyButton->setEnabled(false);
}

void ModelsDialog::renderCachedPresentation(const QString &liveError)
{
    CompanionConfigurationCachePresentationAdapter::ageForDisplay(
        &m_cachedPresentation, QDateTime::currentMSecsSinceEpoch());
    m_companionProjection = QJsonObject();
    m_sourceMode = SourceMode::None;
    clearModels();
    m_keyCombo->blockSignals(true);
    m_keyCombo->clear();
    if (!m_expectedAccountIdentity.isEmpty()
            && m_cachedPresentation.accountIdentity
                == m_expectedAccountIdentity
            && (m_cachedPresentation.state
                    == CompanionConfigurationCacheState::Fresh
                || m_cachedPresentation.state
                    == CompanionConfigurationCacheState::Stale)) {
        for (const CompanionCachedKeyPresentation &cached
             : m_cachedPresentation.keys) {
            if (cached.state != QStringLiteral("active")) continue;
            m_keyCombo->addItem(
                QStringLiteral("%1 · %2（缓存，只读）")
                    .arg(cached.displayName, cached.groupLabel));
            const int index = m_keyCombo->count() - 1;
            m_keyCombo->setItemData(index, kCachedWebsiteRow, kModelRowKindRole);
            m_keyCombo->setItemData(
                index, cached.keyIdentity, kCachedKeyIdentityRole);
            m_keyCombo->setItemData(
                index, cached.platform, kCachedPlatformRole);
            m_keyCombo->setItemData(
                index, m_cachedPresentation.sourceObservationSha256,
                kCachedObservationRole);
            m_keyCombo->setItemData(
                index, m_cachedPresentation.revision, kCachedRevisionRole);
        }
    }
    m_keyCombo->blockSignals(false);
    if (m_keyCombo->count() > 0) {
        m_sourceMode = SourceMode::CachedDisplay;
        m_keyCombo->setCurrentIndex(0);
        m_keyCombo->setEnabled(true);
        m_providerCombo->setEnabled(true);
        m_searchEdit->setEnabled(true);
        m_refreshButton->setEnabled(false);
        renderCachedModels();
        m_statusLabel->setText(
            liveError.isEmpty()
                ? QStringLiteral("正在显示本地认证缓存（只读）")
                : QStringLiteral("网站同步失败（%1），显示本地认证缓存（只读）")
                    .arg(liveError));
        m_statusLabel->setStyleSheet("color: #b54708; font-size: 12px;");
        scheduleCachedPresentationRefresh();
        return;
    }
    m_keyCombo->addItem(QStringLiteral("网站配置不可用"), QString());
    m_keyCombo->setEnabled(false);
    m_providerCombo->setEnabled(false);
    m_searchEdit->setEnabled(false);
    m_refreshButton->setEnabled(false);
    m_statusLabel->setText(
        liveError.isEmpty()
            ? QStringLiteral("没有可显示的本地配置缓存")
            : QStringLiteral("网站配置读取失败：%1").arg(liveError));
    m_statusLabel->setStyleSheet("color: #dc2626; font-size: 12px;");
    scheduleCachedPresentationRefresh();
}

void ModelsDialog::renderCachedModels()
{
    clearModels();
    if (m_sourceMode != SourceMode::CachedDisplay) return;
    CompanionConfigurationCachePresentationAdapter::ageForDisplay(
        &m_cachedPresentation, QDateTime::currentMSecsSinceEpoch());
    if (m_cachedPresentation.state != CompanionConfigurationCacheState::Fresh
            && m_cachedPresentation.state
                != CompanionConfigurationCacheState::Stale) {
        renderCachedPresentation();
        return;
    }
    const QString keyIdentity = m_keyCombo->currentData(
        kCachedKeyIdentityRole).toString();
    const QString platform = m_keyCombo->currentData(
        kCachedPlatformRole).toString();
    for (const CompanionCachedModelPresentation &row
         : m_cachedPresentation.models) {
        if (row.keyIdentity != keyIdentity || row.platform != platform) continue;
        const QString captured = QDateTime::fromMSecsSinceEpoch(
            row.capturedAtMs).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        for (const QString &modelId : row.modelIds) {
            ModelInfo info;
            info.id = modelId;
            info.name = modelId;
            info.provider = platform;
            info.source = QStringLiteral("本地认证缓存（只读）");
            info.created = captured;
            m_models.append(info);
        }
        break;
    }
    rebuildProviderFilter();
    filterModels();
    m_refreshButton->setEnabled(false);
}

void ModelsDialog::scheduleCachedPresentationRefresh()
{
    const quint64 generation = ++m_cachePresentationTimerGeneration;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 transitionAt =
        CompanionConfigurationCachePresentationAdapter::ageForDisplay(
            &m_cachedPresentation, nowMs);
    if (transitionAt <= nowMs) return;
    const qint64 delay = qMin(
        transitionAt - nowMs,
        static_cast<qint64>(std::numeric_limits<int>::max()));
    QTimer::singleShot(static_cast<int>(qMax<qint64>(1, delay)), this,
                       [this, generation]() {
        if (generation != m_cachePresentationTimerGeneration) return;
        CompanionConfigurationCachePresentationAdapter::ageForDisplay(
            &m_cachedPresentation, QDateTime::currentMSecsSinceEpoch());
        if (m_sourceMode == SourceMode::CachedDisplay) {
            renderCachedPresentation();
        } else {
            scheduleCachedPresentationRefresh();
        }
    });
}
