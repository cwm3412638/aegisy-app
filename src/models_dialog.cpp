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

ModelInfo ModelInfo::fromJson(const QJsonObject &obj)
{
    ModelInfo info;
    info.id = obj["id"].toString();
    info.name = obj["name"].toString();
    info.channel = obj["channel"].toString();

    // 价格信息（每百万 tokens）
    info.inputPrice = obj["input_price"].toString();
    info.outputPrice = obj["output_price"].toString();
    info.description = obj["description"].toString();

    return info;
}

ChannelInfo ChannelInfo::fromJson(const QJsonObject &obj)
{
    ChannelInfo info;
    info.id = obj["id"].toString();
    info.name = obj["name"].toString();
    info.type = obj["type"].toString();

    QJsonArray modelsArray = obj["models"].toArray();
    for (const QJsonValue &val : modelsArray) {
        info.models.append(val.toString());
    }

    return info;
}

ModelsDialog::ModelsDialog(ApiClient *apiClient, QWidget *parent)
    : QDialog(parent)
    , m_apiClient(apiClient)
{
    setupUi();
    setWindowTitle("模型和价格管理");
    resize(1000, 600);

    // 连接 API 信号
    connect(m_apiClient, &ApiClient::modelsReceived, this, &ModelsDialog::onModelsReceived);
    connect(m_apiClient, &ApiClient::channelsReceived, this, &ModelsDialog::onChannelsReceived);
    connect(m_apiClient, &ApiClient::requestFailed, this, &ModelsDialog::onRequestFailed);

    // 加载数据
    loadChannels();
    loadModels();
}

void ModelsDialog::setupUi()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // Header
    QHBoxLayout *headerLayout = new QHBoxLayout();

    QLabel *titleLabel = new QLabel("模型和价格管理", this);
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

    // Filters
    QGroupBox *filterGroup = new QGroupBox("筛选", this);
    QHBoxLayout *filterLayout = new QHBoxLayout(filterGroup);

    QLabel *channelLabel = new QLabel("渠道:", this);
    filterLayout->addWidget(channelLabel);

    m_channelCombo = new QComboBox(this);
    m_channelCombo->setMinimumWidth(200);
    m_channelCombo->addItem("所有渠道", "");
    filterLayout->addWidget(m_channelCombo);

    filterLayout->addSpacing(20);

    QLabel *searchLabel = new QLabel("搜索:", this);
    filterLayout->addWidget(searchLabel);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("输入模型名称搜索...");
    m_searchEdit->setMinimumWidth(250);
    filterLayout->addWidget(m_searchEdit);

    filterLayout->addStretch();

    m_refreshButton = new QPushButton("🔄 刷新", this);
    m_refreshButton->setMinimumHeight(35);
    filterLayout->addWidget(m_refreshButton);

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
    m_modelsTable->setColumnCount(5);
    m_modelsTable->setHorizontalHeaderLabels({
        "模型名称", "渠道", "输入价格 ($/1M tokens)", "输出价格 ($/1M tokens)", "说明"
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
    connect(m_copyButton, &QPushButton::clicked, this, &ModelsDialog::onCopyModelClicked);
    connect(m_channelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ModelsDialog::onChannelChanged);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &ModelsDialog::onSearchTextChanged);
    connect(m_modelsTable, &QTableWidget::itemSelectionChanged,
            this, &ModelsDialog::onTableSelectionChanged);
}

void ModelsDialog::loadModels()
{
    m_statusLabel->setText("加载模型列表...");
    m_statusLabel->setStyleSheet("color: #3498db; font-size: 12px;");
    m_refreshButton->setEnabled(false);

    m_apiClient->getModels();
}

void ModelsDialog::loadChannels()
{
    m_apiClient->getChannels();
}

void ModelsDialog::onRefreshClicked()
{
    loadChannels();
    loadModels();
}

void ModelsDialog::onChannelChanged(int index)
{
    m_selectedChannel = m_channelCombo->itemData(index).toString();
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

void ModelsDialog::onModelsReceived(const QJsonArray &models)
{
    m_models.clear();

    for (const QJsonValue &val : models) {
        ModelInfo info = ModelInfo::fromJson(val.toObject());
        m_models.append(info);
    }

    filterModels();

    m_refreshButton->setEnabled(true);
    m_totalLabel->setText(QString("总计: %1 个模型").arg(m_models.size()));
    m_statusLabel->setText(QString("✓ 已加载 %1 个模型").arg(m_models.size()));
    m_statusLabel->setStyleSheet("color: #27ae60; font-size: 12px;");
}

void ModelsDialog::onChannelsReceived(const QJsonArray &channels)
{
    m_channels.clear();
    m_channelCombo->clear();
    m_channelCombo->addItem("所有渠道", "");

    for (const QJsonValue &val : channels) {
        ChannelInfo info = ChannelInfo::fromJson(val.toObject());
        m_channels.append(info);
        m_channelCombo->addItem(info.name, info.id);
    }
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

        // 渠道
        QTableWidgetItem *channelItem = new QTableWidgetItem(model.channel);
        m_modelsTable->setItem(row, 1, channelItem);

        // 输入价格
        QTableWidgetItem *inputPriceItem = new QTableWidgetItem(model.inputPrice);
        inputPriceItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_modelsTable->setItem(row, 2, inputPriceItem);

        // 输出价格
        QTableWidgetItem *outputPriceItem = new QTableWidgetItem(model.outputPrice);
        outputPriceItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_modelsTable->setItem(row, 3, outputPriceItem);

        // 说明
        QTableWidgetItem *descItem = new QTableWidgetItem(model.description);
        m_modelsTable->setItem(row, 4, descItem);

        row++;
    }

    m_modelsTable->resizeColumnsToContents();
}

void ModelsDialog::filterModels()
{
    QList<ModelInfo> filteredModels;
    QString searchText = m_searchEdit->text().toLower();

    for (const ModelInfo &model : m_models) {
        // 渠道筛选
        if (!m_selectedChannel.isEmpty() && model.channel != m_selectedChannel) {
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
        model.channel = m_modelsTable->item(currentRow, 1)->text();
        model.inputPrice = m_modelsTable->item(currentRow, 2)->text();
        model.outputPrice = m_modelsTable->item(currentRow, 3)->text();
        model.description = m_modelsTable->item(currentRow, 4)->text();
        return model;
    }
    return ModelInfo();
}
