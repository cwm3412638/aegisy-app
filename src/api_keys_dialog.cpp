#include "api_keys_dialog.h"
#include "app_theme.h"
#include "companion_config_projection.h"
#include "companion_key_management_projection.h"
#include "companion_model_projection.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QJsonObject>
#include <QDateTime>
#include <QFrame>
#include <QStyle>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QSpinBox>
#include <QUuid>

ApiKeyInfo ApiKeyInfo::fromJson(const QJsonObject &obj)
{
    ApiKeyInfo info;
    info.keyIdentity = obj.value(QStringLiteral("key_identity")).toString();
    info.updateHandle = obj.value(QStringLiteral("update_handle")).toString();
    info.deleteHandle = obj.value(QStringLiteral("delete_handle")).toString();
    info.testHandle = obj.value(QStringLiteral("test_handle")).toString();
    info.groupHandle = obj.value(QStringLiteral("group_handle")).toString();
    info.name = obj.value(QStringLiteral("display_name")).toString();
    info.status = obj.value(QStringLiteral("state")).toString();
    info.quota = obj.value(QStringLiteral("quota")).toDouble();
    info.used = obj.value(QStringLiteral("quota_used")).toDouble();
    info.groupName = obj.value(QStringLiteral("group_label")).toString();
    info.platform = obj.value(QStringLiteral("platform")).toString();
    info.createdAt = obj.value(QStringLiteral("created_at")).toString();
    info.expiresAt = obj.value(QStringLiteral("expires_at")).toString();
    return info;
}

ApiKeysDialog::ApiKeysDialog(ApiClient *apiClient, QWidget *parent)
    : QDialog(parent)
    , m_apiClient(apiClient)
{
    setupUi();
    setWindowTitle("API Keys 管理");
    resize(960, 620);
    setMinimumSize(820, 500);

    connect(m_apiClient, &ApiClient::companionConfigurationReceived,
            this, &ApiKeysDialog::onCompanionConfigurationReceived);
    connect(m_apiClient, &ApiClient::companionConfigurationFailed,
            this, &ApiKeysDialog::onCompanionConfigurationFailed);
    connect(m_apiClient, &ApiClient::companionKeyManagementReceived,
            this, &ApiKeysDialog::onManagementReceived);
    connect(m_apiClient, &ApiClient::companionKeyOperationCompleted,
            this, &ApiKeysDialog::onKeyOperationCompleted);
    connect(m_apiClient, &ApiClient::companionKeyOperationFailed,
            this, &ApiKeysDialog::onKeyOperationFailed);
    connect(m_apiClient, &ApiClient::companionModelsReceived,
            this, &ApiKeysDialog::onCompanionModelsReceived);
    connect(m_apiClient, &ApiClient::companionModelsFailed,
            this, &ApiKeysDialog::onCompanionModelsFailed);

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

    m_createButton = new QPushButton("新建 Key", this);
    m_createButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
    m_createButton->setMinimumHeight(34);
    m_createButton->setCursor(Qt::PointingHandCursor);
    m_createButton->setStyleSheet(AppTheme::primaryButtonStyle());
    toolbarLayout->addWidget(m_createButton);

    m_testButton = new QPushButton("测试 Key", this);
    m_testButton->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
    m_testButton->setMinimumHeight(34);
    m_testButton->setEnabled(false);
    m_testButton->setCursor(Qt::PointingHandCursor);
    m_testButton->setStyleSheet(ghostBtnStyle);
    toolbarLayout->addWidget(m_testButton);

    m_editButton = new QPushButton("编辑", this);
    m_editButton->setEnabled(false);
    m_editButton->setMinimumHeight(34);
    m_editButton->setStyleSheet(ghostBtnStyle);
    toolbarLayout->addWidget(m_editButton);

    m_groupButton = new QPushButton("切换分组", this);
    m_groupButton->setEnabled(false);
    m_groupButton->setMinimumHeight(34);
    m_groupButton->setStyleSheet(ghostBtnStyle);
    toolbarLayout->addWidget(m_groupButton);

    m_toggleButton = new QPushButton("禁用", this);
    m_toggleButton->setEnabled(false);
    m_toggleButton->setMinimumHeight(34);
    m_toggleButton->setStyleSheet(ghostBtnStyle);
    toolbarLayout->addWidget(m_toggleButton);

    m_deleteButton = new QPushButton("删除", this);
    m_deleteButton->setEnabled(false);
    m_deleteButton->setMinimumHeight(34);
    m_deleteButton->setStyleSheet(AppTheme::dangerButtonStyle());
    toolbarLayout->addWidget(m_deleteButton);

    toolbarLayout->addStretch();
    mainLayout->addWidget(toolbarCard);

    // ── Keys 表格 ──────────────────────────────────────────
    m_keysTable = new QTableWidget(this);
    m_keysTable->setColumnCount(8);
    m_keysTable->setHorizontalHeaderLabels({
        "名称", "分组", "状态", "安全标识", "配额", "已用", "使用率", "创建时间"
    });

    QHeaderView *header = m_keysTable->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    header->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(3, QHeaderView::Stretch);
    header->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(7, QHeaderView::ResizeToContents);

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
    connect(m_testButton, &QPushButton::clicked, this, &ApiKeysDialog::onTestKeyClicked);
    connect(m_createButton, &QPushButton::clicked, this, &ApiKeysDialog::onCreateKeyClicked);
    connect(m_editButton, &QPushButton::clicked, this, &ApiKeysDialog::onEditKeyClicked);
    connect(m_groupButton, &QPushButton::clicked, this, &ApiKeysDialog::onChangeGroupClicked);
    connect(m_toggleButton, &QPushButton::clicked, this, &ApiKeysDialog::onToggleStatusClicked);
    connect(m_deleteButton, &QPushButton::clicked, this, &ApiKeysDialog::onDeleteKeyClicked);
    connect(m_keysTable, &QTableWidget::itemSelectionChanged,
            this, &ApiKeysDialog::onTableSelectionChanged);
}

void ApiKeysDialog::loadApiKeys()
{
    m_statusLabel->setText("加载 API Keys...");
    m_statusLabel->setStyleSheet("color: #0f766e; font-size: 12px;");
    m_refreshButton->setEnabled(false);
    setMutationControlsEnabled(false);
    m_managementProjectionSha256.clear();
    m_managementRequestId.clear();
    m_testRequestId.clear();
    m_testKeyIdentity.clear();
    m_apiClient->getApiKeys();
}

void ApiKeysDialog::onRefreshClicked()  { loadApiKeys(); }

void ApiKeysDialog::onTestKeyClicked()
{
    const ApiKeyInfo selectedKey = getSelectedKey();
    if (selectedKey.keyIdentity.isEmpty() || selectedKey.testHandle.isEmpty()) {
        QMessageBox::warning(this, "未选择", "请先选择一个 API Key。");
        return;
    }
    m_testButton->setEnabled(false);
    m_statusLabel->setText(QString("正在测试 Key「%1」...").arg(selectedKey.name));
    m_statusLabel->setStyleSheet("color: #0f766e; font-size: 12px;");
    m_testRequestId = QStringLiteral("key-test-%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_testKeyIdentity = selectedKey.keyIdentity;
    m_apiClient->testCompanionApiKey(
        m_testRequestId, m_accountIdentity, selectedKey.keyIdentity,
        selectedKey.testHandle, m_configurationProjectionSha256,
        m_managementProjectionSha256);
}

void ApiKeysDialog::onCreateKeyClicked()
{
    showKeyEditor();
}

void ApiKeysDialog::onEditKeyClicked()
{
    const ApiKeyInfo selected = getSelectedKey();
    if (selected.keyIdentity.isEmpty()) return;
    showKeyEditor(&selected);
}

void ApiKeysDialog::onChangeGroupClicked()
{
    const ApiKeyInfo selected = getSelectedKey();
    if (selected.keyIdentity.isEmpty() || m_groups.isEmpty()) return;
    QStringList names;
    QStringList handles;
    int current = 0;
    for (const QJsonValue &value : m_groups) {
        const QJsonObject group = value.toObject();
        const QString handle = group.value(QStringLiteral("group_handle")).toString();
        const QString name = group.value(QStringLiteral("display_name")).toString();
        if (handle.isEmpty() || name.isEmpty()) continue;
        if (handle == selected.groupHandle) current = names.size();
        names.append(name);
        handles.append(handle);
    }
    bool accepted = false;
    const QString chosen = QInputDialog::getItem(
        this, QStringLiteral("切换 Key 分组"),
        QStringLiteral("为「%1」选择新分组").arg(selected.name),
        names, current, false, &accepted);
    if (!accepted) return;
    const int index = names.indexOf(chosen);
    if (index < 0 || handles[index] == selected.groupHandle) return;
    m_statusLabel->setText(QStringLiteral("正在切换分组..."));
    m_operationRequestId = QStringLiteral("key-update-%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_pendingAction = QStringLiteral("update");
    m_refreshButton->setEnabled(false);
    setMutationControlsEnabled(false);
    m_apiClient->updateCompanionApiKey(
        m_operationRequestId, m_accountIdentity, selected.keyIdentity,
        selected.updateHandle, m_configurationProjectionSha256,
        m_managementProjectionSha256, QJsonObject{
            { QStringLiteral("group_handle"), handles[index] }
        });
}

void ApiKeysDialog::onToggleStatusClicked()
{
    const ApiKeyInfo selected = getSelectedKey();
    if (selected.keyIdentity.isEmpty()) return;
    const bool active = selected.status.compare(QStringLiteral("active"), Qt::CaseInsensitive) == 0;
    const QString next = active ? QStringLiteral("inactive") : QStringLiteral("active");
    m_statusLabel->setText(active ? QStringLiteral("正在禁用 Key...")
                                  : QStringLiteral("正在启用 Key..."));
    m_operationRequestId = QStringLiteral("key-update-%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_pendingAction = QStringLiteral("update");
    m_refreshButton->setEnabled(false);
    setMutationControlsEnabled(false);
    m_apiClient->updateCompanionApiKey(
        m_operationRequestId, m_accountIdentity, selected.keyIdentity,
        selected.updateHandle, m_configurationProjectionSha256,
        m_managementProjectionSha256, QJsonObject{
            { QStringLiteral("status"), next }
        });
}

void ApiKeysDialog::onDeleteKeyClicked()
{
    const ApiKeyInfo selected = getSelectedKey();
    if (selected.keyIdentity.isEmpty()) return;
    if (QMessageBox::question(
            this, QStringLiteral("删除 API Key"),
            QStringLiteral("确定永久删除「%1」吗？使用该 Key 的档案和终端将立即失效。")
                .arg(selected.name),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) return;
    m_statusLabel->setText(QStringLiteral("正在删除 Key..."));
    m_operationRequestId = QStringLiteral("key-delete-%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_pendingAction = QStringLiteral("delete");
    m_refreshButton->setEnabled(false);
    setMutationControlsEnabled(false);
    m_apiClient->deleteCompanionApiKey(
        m_operationRequestId, m_accountIdentity, selected.keyIdentity,
        selected.deleteHandle, m_configurationProjectionSha256,
        m_managementProjectionSha256);
}

void ApiKeysDialog::showKeyEditor(const ApiKeyInfo *existing)
{
    if (m_groups.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("暂无可用分组"),
                                 QStringLiteral("当前账号没有可用于创建 Key 的分组。"));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(existing ? QStringLiteral("编辑 API Key")
                                   : QStringLiteral("新建 API Key"));
    dialog.setMinimumWidth(430);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(22, 20, 22, 18);
    layout->setSpacing(14);
    auto *form = new QFormLayout;
    form->setVerticalSpacing(12);

    auto *nameEdit = new QLineEdit(&dialog);
    nameEdit->setPlaceholderText(QStringLiteral("例如：Codex 生产环境"));
    if (existing) nameEdit->setText(existing->name);
    form->addRow(QStringLiteral("名称"), nameEdit);

    auto *groupCombo = new QComboBox(&dialog);
    int selectedGroup = -1;
    for (const QJsonValue &value : m_groups) {
        const QJsonObject group = value.toObject();
        const QString handle = group.value(QStringLiteral("group_handle")).toString();
        const QString name = group.value(QStringLiteral("display_name")).toString();
        const QString platform = group.value(QStringLiteral("platform")).toString();
        if (handle.isEmpty() || name.isEmpty()) continue;
        groupCombo->addItem(platform.isEmpty()
            ? name : QStringLiteral("%1  ·  %2").arg(name, platform), handle);
        groupCombo->setItemData(
            groupCombo->count() - 1,
            group.value(QStringLiteral("create_handle")), Qt::UserRole + 1);
        if (existing && handle == existing->groupHandle) {
            selectedGroup = groupCombo->count() - 1;
        }
    }
    if (selectedGroup >= 0) groupCombo->setCurrentIndex(selectedGroup);
    form->addRow(QStringLiteral("分组"), groupCombo);

    auto *quotaSpin = new QSpinBox(&dialog);
    quotaSpin->setRange(0, 2000000000);
    quotaSpin->setSpecialValueText(QStringLiteral("无限制"));
    quotaSpin->setValue(existing ? static_cast<int>(qMin<qint64>(existing->quota, 2000000000)) : 0);
    form->addRow(QStringLiteral("配额"), quotaSpin);

    QComboBox *statusCombo = nullptr;
    if (existing) {
        statusCombo = new QComboBox(&dialog);
        statusCombo->addItem(QStringLiteral("启用"), QStringLiteral("active"));
        statusCombo->addItem(QStringLiteral("禁用"), QStringLiteral("inactive"));
        statusCombo->setCurrentIndex(existing->status == QStringLiteral("active") ? 0 : 1);
        form->addRow(QStringLiteral("状态"), statusCombo);
    }
    layout->addLayout(form);

    auto *hint = new QLabel(
        QStringLiteral("创建后服务器会生成 Key；分组决定可用平台、模型和计费倍率。配额为 0 表示不单独限制。"),
        &dialog);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("font-size: 11px; color: #667085;"));
    layout->addWidget(hint);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Save, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(existing
        ? QStringLiteral("保存修改") : QStringLiteral("创建 Key"));
    buttons->button(QDialogButtonBox::Save)->setStyleSheet(AppTheme::primaryButtonStyle());
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) return;
    const QString name = nameEdit->text().trimmed();
    if (name.isEmpty() || groupCombo->currentIndex() < 0) {
        QMessageBox::warning(this, QStringLiteral("信息不完整"),
                             QStringLiteral("请输入名称并选择分组。"));
        return;
    }
    m_operationRequestId = QStringLiteral("key-%1-%2")
        .arg(existing ? QStringLiteral("update") : QStringLiteral("create"),
             QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_pendingAction = existing ? QStringLiteral("update") : QStringLiteral("create");
    m_refreshButton->setEnabled(false);
    setMutationControlsEnabled(false);
    if (existing && statusCombo) {
        m_statusLabel->setText(QStringLiteral("正在保存 Key..."));
        m_apiClient->updateCompanionApiKey(
            m_operationRequestId, m_accountIdentity, existing->keyIdentity,
            existing->updateHandle, m_configurationProjectionSha256,
            m_managementProjectionSha256, QJsonObject{
                { QStringLiteral("name"), name },
                { QStringLiteral("group_handle"), groupCombo->currentData().toString() },
                { QStringLiteral("quota"), quotaSpin->value() },
                { QStringLiteral("status"), statusCombo->currentData().toString() },
            });
    } else {
        m_statusLabel->setText(QStringLiteral("正在创建 Key..."));
        m_apiClient->createCompanionApiKey(
            m_operationRequestId, m_accountIdentity,
            m_configurationProjectionSha256, m_managementProjectionSha256,
            groupCombo->currentData(Qt::UserRole + 1).toString(),
            groupCombo->currentData().toString(), name, quotaSpin->value());
    }
}

void ApiKeysDialog::onCompanionConfigurationReceived(const QJsonObject &projection)
{
    if (!CompanionConfigProjection::validate(projection)) {
        onCompanionConfigurationFailed(QStringLiteral("projection-response-invalid"));
        return;
    }
    m_accountIdentity = projection.value(QStringLiteral("account_identity")).toString();
    m_configurationProjectionSha256 = projection.value(
        QStringLiteral("projection_sha256")).toString();
    m_managementRequestId = QStringLiteral("key-management-%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_apiClient->getCompanionKeyManagement(
        m_managementRequestId, m_accountIdentity,
        m_configurationProjectionSha256);
}

void ApiKeysDialog::onCompanionConfigurationFailed(const QString &errorCode)
{
    m_refreshButton->setEnabled(true);
    setMutationControlsEnabled(false);
    m_statusLabel->setText(QStringLiteral("配置读取失败：%1").arg(errorCode));
    m_statusLabel->setStyleSheet(QStringLiteral("color: #b42318; font-size: 12px;"));
}

void ApiKeysDialog::onManagementReceived(
    const QString &requestId, const QJsonObject &projection)
{
    if (requestId != m_managementRequestId
            || projection.value(QStringLiteral("account_identity")).toString()
                != m_accountIdentity
            || projection.value(
                QStringLiteral("configuration_projection_sha256")).toString()
                != m_configurationProjectionSha256
            || !CompanionKeyManagementProjection::validate(projection)) {
        return;
    }
    m_managementRequestId.clear();
    m_managementProjectionSha256 = projection.value(
        QStringLiteral("projection_sha256")).toString();
    m_groups = projection.value(QStringLiteral("groups")).toArray();
    m_keys.clear();
    for (const QJsonValue &value : projection.value(QStringLiteral("keys")).toArray()) {
        m_keys.append(ApiKeyInfo::fromJson(value.toObject()));
    }
    updateKeysTable(m_keys);
    m_refreshButton->setEnabled(true);
    m_totalKeysLabel->setText(QStringLiteral("共 %1 个 Key").arg(m_keys.size()));
    m_statusLabel->setText(QStringLiteral("已加载 %1 个安全 Key 元数据").arg(m_keys.size()));
    m_statusLabel->setStyleSheet(QStringLiteral("color: #16a34a; font-size: 12px;"));
    setMutationControlsEnabled(true);
}

void ApiKeysDialog::onKeyOperationCompleted(
    const QString &requestId, const QString &action,
    bool credentialCleanupComplete)
{
    if (requestId != m_operationRequestId || action != m_pendingAction) return;
    m_operationRequestId.clear();
    m_pendingAction.clear();
    QString message;
    if (action == QStringLiteral("create")) message = QStringLiteral("Key 创建成功");
    else if (action == QStringLiteral("delete")) message = QStringLiteral("Key 已删除");
    else message = QStringLiteral("Key 更新成功");
    if (!credentialCleanupComplete) {
        message += QStringLiteral("（本地安全存储清理未确认，请刷新对账）");
    }
    m_statusLabel->setText(message + QStringLiteral("，正在刷新..."));
    m_statusLabel->setStyleSheet(QStringLiteral("color: #067647; font-size: 12px;"));
    loadApiKeys();
}

void ApiKeysDialog::onKeyOperationFailed(
    const QString &requestId, const QString &action, const QString &errorCode)
{
    if (action == QStringLiteral("read") && requestId == m_managementRequestId) {
        m_managementRequestId.clear();
        m_refreshButton->setEnabled(true);
    } else if (requestId != m_operationRequestId || action != m_pendingAction) {
        return;
    } else {
        m_operationRequestId.clear();
        m_pendingAction.clear();
        m_refreshButton->setEnabled(true);
    }
    setMutationControlsEnabled(false);
    m_statusLabel->setText(QStringLiteral("操作失败：%1").arg(errorCode));
    m_statusLabel->setStyleSheet(QStringLiteral("color: #b42318; font-size: 12px;"));
}

void ApiKeysDialog::onCompanionModelsReceived(
    const QString &requestId, const QString &keyIdentity,
    const QJsonObject &projection)
{
    if (requestId != m_testRequestId || keyIdentity != m_testKeyIdentity
            || projection.value(QStringLiteral("key_identity")).toString()
                != keyIdentity
            || !CompanionModelProjection::validate(projection)) return;
    m_testRequestId.clear();
    m_testKeyIdentity.clear();
    setMutationControlsEnabled(true);
    m_statusLabel->setText(QStringLiteral("Key 可用：%1 个模型")
        .arg(projection.value(QStringLiteral("model_count")).toInt()));
    m_statusLabel->setStyleSheet(QStringLiteral("color: #067647; font-size: 12px;"));
}

void ApiKeysDialog::onCompanionModelsFailed(
    const QString &requestId, const QString &keyIdentity,
    const QString &errorCode)
{
    if (requestId != m_testRequestId || keyIdentity != m_testKeyIdentity) return;
    m_testRequestId.clear();
    m_testKeyIdentity.clear();
    setMutationControlsEnabled(true);
    m_statusLabel->setText(QStringLiteral("Key 测试失败：%1").arg(errorCode));
    m_statusLabel->setStyleSheet(QStringLiteral("color: #b42318; font-size: 12px;"));
}

void ApiKeysDialog::updateKeysTable(const QList<ApiKeyInfo> &keys)
{
    m_keysTable->setRowCount(0);

    for (int i = 0; i < keys.size(); ++i) {
        const ApiKeyInfo &info = keys[i];
        m_keysTable->insertRow(i);

        QTableWidgetItem *nameItem = new QTableWidgetItem(info.name);
        m_keysTable->setItem(i, 0, nameItem);

        // 分组
        const QString group = info.groupName;
        auto *groupItem = new QTableWidgetItem(group.isEmpty() ? QStringLiteral("未分组") : group);
        groupItem->setToolTip(info.platform);
        m_keysTable->setItem(i, 1, groupItem);

        // 状态
        QString statusText = info.status;
        if (info.status == QStringLiteral("active")) statusText = QStringLiteral("启用");
        else if (info.status == QStringLiteral("inactive")) statusText = QStringLiteral("禁用");
        else if (info.status == QStringLiteral("quota_exhausted")) statusText = QStringLiteral("配额用尽");
        else if (info.status == QStringLiteral("expired")) statusText = QStringLiteral("已过期");
        QTableWidgetItem *statusItem = new QTableWidgetItem(statusText);
        statusItem->setForeground(QBrush(QColor(
            info.status.toLower() == "active" ? "#16a34a" : "#dc2626")));
        m_keysTable->setItem(i, 2, statusItem);

        const QString hash = info.keyIdentity.section(QLatin1Char(':'), -1);
        QTableWidgetItem *keyItem = new QTableWidgetItem(
            QStringLiteral("sha256:%1").arg(hash.left(10)));
        QFont mono; mono.setFamily("Courier New");
        keyItem->setFont(mono);
        keyItem->setForeground(QBrush(QColor("#475569")));
        m_keysTable->setItem(i, 3, keyItem);

        // 配额
        QString quotaStr = info.quota > 0 ? QString::number(info.quota, 'f', 0) : "无限制";
        m_keysTable->setItem(i, 4, new QTableWidgetItem(quotaStr));

        // 已用
        m_keysTable->setItem(i, 5, new QTableWidgetItem(QString::number(info.used, 'f', 0)));

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
        m_keysTable->setItem(i, 6, usageItem);

        // 创建时间
        QString createdAt = info.createdAt;
        QDateTime dt = QDateTime::fromString(createdAt, Qt::ISODate);
        if (dt.isValid()) createdAt = dt.toString("yyyy-MM-dd");
        m_keysTable->setItem(i, 7, new QTableWidgetItem(createdAt));
    }
}

void ApiKeysDialog::onTableSelectionChanged()
{
    const bool has = !m_keysTable->selectedItems().isEmpty()
        && !m_managementProjectionSha256.isEmpty()
        && m_operationRequestId.isEmpty() && m_testRequestId.isEmpty();
    const ApiKeyInfo selected = has ? getSelectedKey() : ApiKeyInfo();
    m_testButton->setEnabled(has && selected.status == QStringLiteral("active"));
    m_editButton->setEnabled(has);
    m_groupButton->setEnabled(has && !m_groups.isEmpty());
    m_toggleButton->setEnabled(has);
    m_deleteButton->setEnabled(has);
    if (has) {
        m_toggleButton->setText(selected.status == QStringLiteral("active")
            ? QStringLiteral("禁用") : QStringLiteral("启用"));
    }
}

void ApiKeysDialog::setMutationControlsEnabled(bool enabled)
{
    m_createButton->setEnabled(enabled && !m_groups.isEmpty()
        && m_operationRequestId.isEmpty());
    if (!enabled) {
        m_testButton->setEnabled(false);
        m_editButton->setEnabled(false);
        m_groupButton->setEnabled(false);
        m_toggleButton->setEnabled(false);
        m_deleteButton->setEnabled(false);
        return;
    }
    onTableSelectionChanged();
}

ApiKeyInfo ApiKeysDialog::getSelectedKey() const
{
    int row = m_keysTable->currentRow();
    if (row >= 0 && row < m_keys.size()) return m_keys[row];
    return ApiKeyInfo();
}
