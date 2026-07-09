#include "models_dialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QHeaderView>
#include <QClipboard>
#include <QApplication>
#include <QMessageBox>
#include <QJsonObject>
#include <QJsonArray>
#include <QGroupBox>
#include <QDateTime>
#include <QSet>
#include <QSettings>
#include <QLineEdit>

ModelInfo ModelInfo::fromJson(const QJsonObject &obj)
{
    ModelInfo info;
    // OpenAI 兼容格式：{ "id": "gpt-4", "object": "model", "created": 1687882411, "owned_by": "openai" }
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
    : QDialog(parent)
    , m_apiClient(apiClient)
{
    setupUi();
    setWindowTitle("模型列表");
    resize(760, 600);

    // 连接 API 信号
    connect(m_apiClient, &ApiClient::modelsReceived, this, &ModelsDialog::onModelsReceived);
    connect(m_apiClient, &ApiClient::apiKeysReceived, this, &ModelsDialog::onApiKeysReceived);
    connect(m_apiClient, &ApiClient::requestFailed, this, &ModelsDialog::onRequestFailed);

    // 自动获取账号下的 API Key，用于查询账号支持的模型
    loadApiKeys();
}

void ModelsDialog::setupUi()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // Header
    QHBoxLayout *headerLayout = new QHBoxLayout();

    QLabel *titleLabel = new QLabel("账号支持的模型", this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    headerLayout->addWidget(titleLabel);

    headerLayout->addStretch();

    m_totalLabel = new QLabel("总计: 0 个模型", this);
    m_totalLabel->setStyleSheet("color: #666; font-size: 13px;");
    headerLayout->addWidget(m_totalLabel);

    mainLayout->addLayout(headerLayout);

    // API Key 行（下拉从账号 API Keys 列表选择，也可手动粘贴）
    QHBoxLayout *keyLayout = new QHBoxLayout();
    QLabel *keyLabel = new QLabel("API Key:", this);
    keyLayout->addWidget(keyLabel);

    m_keyCombo = new QComboBox(this);
    m_keyCombo->setEditable(true);
    m_keyCombo->setInsertPolicy(QComboBox::NoInsert);
    m_keyCombo->setMinimumWidth(340);
    m_keyCombo->lineEdit()->setPlaceholderText("从账号 API Key 中选择，或手动粘贴 sk-...");
    keyLayout->addWidget(m_keyCombo, 1);

    m_refreshButton = new QPushButton("🔄 查询模型", this);
    m_refreshButton->setMinimumHeight(32);
    keyLayout->addWidget(m_refreshButton);

    mainLayout->addLayout(keyLayout);

    // Filters
    QGroupBox *filterGroup = new QGroupBox("筛选", this);
    QHBoxLayout *filterLayout = new QHBoxLayout(filterGroup);

    QLabel *providerLabel = new QLabel("提供方:", this);
    filterLayout->addWidget(providerLabel);

    m_providerCombo = new QComboBox(this);
    m_providerCombo->setMinimumWidth(180);
    m_providerCombo->addItem("全部提供方", "");
    filterLayout->addWidget(m_providerCombo);

    filterLayout->addSpacing(20);

    QLabel *searchLabel = new QLabel("搜索:", this);
    filterLayout->addWidget(searchLabel);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("输入模型名称搜索...");
    m_searchEdit->setMinimumWidth(250);
    filterLayout->addWidget(m_searchEdit);

    filterLayout->addStretch();

    mainLayout->addWidget(filterGroup);

    // Toolbar
    QHBoxLayout *toolbarLayout = new QHBoxLayout();

    m_copyButton = new QPushButton("📋 复制模型名称", this);
    m_copyButton->setMinimumHeight(35);
    m_copyButton->setEnabled(false);
    m_copyButton->setStyleSheet(
        "QPushButton {"
        "  background-color: #3498db;"
        "  color: white;"
        "  border: none;"
        "  border-radius: 4px;"
        "  padding: 5px 15px;"
        "  font-weight: bold;"
        "}"
        "QPushButton:hover {"
        "  background-color: #2980b9;"
        "}"
        "QPushButton:disabled {"
        "  background-color: #bdc3c7;"
        "}"
    );
    toolbarLayout->addWidget(m_copyButton);

    toolbarLayout->addStretch();

    mainLayout->addLayout(toolbarLayout);

    // Models Table
    m_modelsTable = new QTableWidget(this);
    m_modelsTable->setColumnCount(3);
    m_modelsTable->setHorizontalHeaderLabels({
        "模型名称", "提供方", "创建时间"
    });

    m_modelsTable->horizontalHeader()->setStretchLastSection(true);
    m_modelsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_modelsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_modelsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_modelsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_modelsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_modelsTable->setAlternatingRowColors(true);
    m_modelsTable->verticalHeader()->setVisible(false);
    m_modelsTable->setStyleSheet(
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

    mainLayout->addWidget(m_modelsTable);

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
    connect(m_refreshButton, &QPushButton::clicked, this, &ModelsDialog::onRefreshClicked);
    // 用户从下拉中选择某个 Key 时自动查询（activated 仅由用户操作触发，程序设置不触发）
    connect(m_keyCombo, QOverload<int>::of(&QComboBox::activated),
            this, &ModelsDialog::onRefreshClicked);
    connect(m_keyCombo->lineEdit(), &QLineEdit::returnPressed, this, &ModelsDialog::onRefreshClicked);
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
    m_statusLabel->setStyleSheet("color: #3498db; font-size: 12px;");
    m_apiClient->getApiKeys();
}

QString ModelsDialog::currentApiKey() const
{
    // 若当前文本与选中项的显示文本一致，说明是从下拉选择的 -> 取存储的完整 Key；
    // 否则说明用户手动输入/粘贴了 Key -> 直接使用输入文本
    const int idx = m_keyCombo->currentIndex();
    const QString text = m_keyCombo->currentText().trimmed();
    if (idx >= 0 && text == m_keyCombo->itemText(idx)) {
        return m_keyCombo->itemData(idx).toString();
    }
    return text;
}

void ModelsDialog::loadModels()
{
    const QString key = currentApiKey();
    if (key.isEmpty()) {
        m_statusLabel->setText("✗ 请先输入 API Key（sk- 开头）再查询模型");
        m_statusLabel->setStyleSheet("color: #e74c3c; font-size: 12px;");
        return;
    }

    m_statusLabel->setText("加载模型列表...");
    m_statusLabel->setStyleSheet("color: #3498db; font-size: 12px;");
    m_refreshButton->setEnabled(false);

    m_apiClient->getModels(key);
}

void ModelsDialog::onRefreshClicked()
{
    loadModels();
}

void ModelsDialog::onProviderChanged(int index)
{
    m_selectedProvider = m_providerCombo->itemData(index).toString();
    filterModels();
}

void ModelsDialog::onSearchTextChanged(const QString &text)
{
    Q_UNUSED(text);
    filterModels();
}

void ModelsDialog::onCopyModelClicked()
{
    ModelInfo model = getSelectedModel();
    if (model.name.isEmpty()) {
        QMessageBox::warning(this, "未选择", "请先选择一个模型。");
        return;
    }

    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(model.name);

    m_statusLabel->setText(QString("✓ 模型名称 '%1' 已复制到剪贴板！").arg(model.name));
    m_statusLabel->setStyleSheet("color: #27ae60; font-size: 12px;");
}

void ModelsDialog::onTableSelectionChanged()
{
    ModelInfo model = getSelectedModel();
    m_copyButton->setEnabled(!model.name.isEmpty());
}

void ModelsDialog::onApiKeysReceived(const QJsonArray &keys)
{
    // 若用户已手动输入 Key，则不覆盖其选择
    const bool userTyped = m_keyCombo->currentIndex() < 0
            && !m_keyCombo->currentText().trimmed().isEmpty();

    const QString persistedId = QSettings().value("apikeys/activeKeyId").toString();

    m_keyCombo->blockSignals(true);
    m_keyCombo->clear();

    int selectIdx = -1;      // 与 API Keys 页面激活的 Key 匹配
    int firstActiveIdx = -1; // 服务端状态为 active 的第一个 Key

    for (const QJsonValue &val : keys) {
        const QJsonObject obj = val.toObject();
        const QString key = obj["key"].toString();
        if (key.isEmpty()) {
            continue;
        }
        const QString id = QString::number(obj["id"].toInt());
        const QString name = obj["name"].toString();
        const QString status = obj["status"].toString();

        QString masked = key;
        if (masked.length() > 12) {
            masked = masked.left(8) + "..." + masked.right(4);
        }
        const QString display = name.isEmpty() ? masked
                                               : QString("%1 (%2)").arg(name, masked);

        const int idx = m_keyCombo->count();
        m_keyCombo->addItem(display, key);          // 完整 Key 存入 UserRole
        m_keyCombo->setItemData(idx, id, Qt::UserRole + 1);

        if (!persistedId.isEmpty() && id == persistedId) {
            selectIdx = idx;
        }
        if (firstActiveIdx < 0 && status == "active") {
            firstActiveIdx = idx;
        }
    }
    m_keyCombo->blockSignals(false);

    if (m_keyCombo->count() == 0) {
        if (!userTyped) {
            m_statusLabel->setText("✗ 未找到可用的 API Key，请到「API Keys 管理」创建后再试，或手动粘贴 Key");
            m_statusLabel->setStyleSheet("color: #e74c3c; font-size: 12px;");
        }
        return;
    }

    if (userTyped) {
        // 保留用户手动输入的 Key，仅提供下拉可选
        return;
    }

    const int idx = selectIdx >= 0 ? selectIdx
                                   : (firstActiveIdx >= 0 ? firstActiveIdx : 0);
    m_keyCombo->setCurrentIndex(idx);
    loadModels();
}

void ModelsDialog::onModelsReceived(const QJsonArray &models)
{
    m_models.clear();

    for (const QJsonValue &val : models) {
        ModelInfo info = ModelInfo::fromJson(val.toObject());
        if (info.id.isEmpty()) {
            continue;
        }
        m_models.append(info);
    }

    rebuildProviderFilter();
    filterModels();

    m_refreshButton->setEnabled(true);
    m_statusLabel->setText(QString("✓ 已加载 %1 个模型").arg(m_models.size()));
    m_statusLabel->setStyleSheet("color: #27ae60; font-size: 12px;");
}

void ModelsDialog::rebuildProviderFilter()
{
    // 依据返回的模型，收集去重后的提供方列表
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
    for (const QString &p : providers) {
        m_providerCombo->addItem(p, p);
    }

    // 尽量保留之前的选择
    int idx = m_providerCombo->findData(previous);
    m_providerCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    m_selectedProvider = m_providerCombo->currentData().toString();
    m_providerCombo->blockSignals(false);
}

void ModelsDialog::onRequestFailed(const QString &error)
{
    m_refreshButton->setEnabled(true);
    m_statusLabel->setText(QString("✗ 错误: %1").arg(error));
    m_statusLabel->setStyleSheet("color: #e74c3c; font-size: 12px;");
}

void ModelsDialog::updateModelsTable(const QList<ModelInfo> &models)
{
    m_modelsTable->setRowCount(0);

    int row = 0;
    for (const ModelInfo &model : models) {
        m_modelsTable->insertRow(row);

        // 模型名称
        QTableWidgetItem *nameItem = new QTableWidgetItem(model.name);
        nameItem->setFont(QFont("Monospace"));
        m_modelsTable->setItem(row, 0, nameItem);

        // 提供方
        QTableWidgetItem *providerItem = new QTableWidgetItem(model.provider);
        m_modelsTable->setItem(row, 1, providerItem);

        // 创建时间
        QTableWidgetItem *createdItem = new QTableWidgetItem(model.created);
        m_modelsTable->setItem(row, 2, createdItem);

        row++;
    }

    m_modelsTable->resizeColumnsToContents();
    m_modelsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
}

void ModelsDialog::filterModels()
{
    QList<ModelInfo> filteredModels;
    QString searchText = m_searchEdit->text().toLower();

    for (const ModelInfo &model : m_models) {
        // 提供方筛选
        if (!m_selectedProvider.isEmpty() && model.provider != m_selectedProvider) {
            continue;
        }

        // 搜索筛选
        if (!searchText.isEmpty() && !model.name.toLower().contains(searchText)) {
            continue;
        }

        filteredModels.append(model);
    }

    updateModelsTable(filteredModels);
    m_totalLabel->setText(QString("显示: %1 / %2 个模型")
                         .arg(filteredModels.size())
                         .arg(m_models.size()));
}

ModelInfo ModelsDialog::getSelectedModel() const
{
    int currentRow = m_modelsTable->currentRow();
    if (currentRow >= 0 && currentRow < m_modelsTable->rowCount()) {
        ModelInfo model;
        model.name = m_modelsTable->item(currentRow, 0)->text();
        model.id = model.name;
        model.provider = m_modelsTable->item(currentRow, 1)->text();
        model.created = m_modelsTable->item(currentRow, 2)->text();
        return model;
    }
    return ModelInfo();
}
