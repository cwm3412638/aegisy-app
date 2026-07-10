#include "connect_wizard.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QMessageBox>
#include <QStyle>
#include <QVBoxLayout>

namespace {

QString toolAccent(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("#c15f3c");
    case AiTool::CodexCli:   return QStringLiteral("#111827");
    case AiTool::GeminiCli:  return QStringLiteral("#1a73e8");
    }
    return QStringLiteral("#0f766e");
}

QString toolSoftColor(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("#fff4ef");
    case AiTool::CodexCli:   return QStringLiteral("#f3f4f6");
    case AiTool::GeminiCli:  return QStringLiteral("#eef5ff");
    }
    return QStringLiteral("#ecfdf5");
}

QString toolLetter(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("C");
    case AiTool::CodexCli:   return QStringLiteral("O");
    case AiTool::GeminiCli:  return QStringLiteral("G");
    }
    return QStringLiteral("A");
}

QString toolConfigPath(AiTool tool)
{
    return ToolManager::configFilePath(tool);
}

QString toolSelectorText(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("Claude Code\nAnthropic");
    case AiTool::CodexCli:   return QStringLiteral("Codex CLI\nOpenAI");
    case AiTool::GeminiCli:  return QStringLiteral("Gemini CLI\nGoogle");
    }
    return QString();
}

QString primaryButtonStyle()
{
    return QStringLiteral(
        "QPushButton {"
        "  background: #0f766e; color: white; border: none; border-radius: 7px;"
        "  padding: 0 18px; font-size: 13px; font-weight: 600;"
        "}"
        "QPushButton:hover { background: #0b625c; }"
        "QPushButton:pressed { background: #094f4a; }"
        "QPushButton:disabled { background: #d7dde3; color: #8a96a3; }");
}

QString secondaryButtonStyle()
{
    return QStringLiteral(
        "QPushButton {"
        "  background: white; color: #344054; border: 1px solid #d0d5dd;"
        "  border-radius: 7px; padding: 0 15px; font-size: 13px;"
        "}"
        "QPushButton:hover { background: #f8fafb; border-color: #98a2b3; }"
        "QPushButton:disabled { color: #98a2b3; background: #f8fafb; }");
}

} // namespace

ConnectWizardDialog::ConnectWizardDialog(ApiClient *client,
                                         ProfileManager *profileManager,
                                         int editIndex,
                                         QWidget *parent)
    : QDialog(parent)
    , m_apiClient(client)
    , m_profileManager(profileManager)
    , m_editIndex(editIndex)
{
    if (m_editIndex >= 0) {
        const QList<Profile> profiles = m_profileManager->allProfiles();
        if (m_editIndex < profiles.size()) {
            const Profile &profile = profiles[m_editIndex];
            m_selectedType = profile.type;
            m_existingType = profile.type;
            m_existingKey = profile.key;
            m_existingModel = profile.model;
        }
    }

    setupUi();

    setWindowTitle(m_editIndex < 0 ? QStringLiteral("新建连接配置")
                                   : QStringLiteral("编辑连接配置"));
    resize(600, 570);
    setMinimumSize(560, 540);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    connect(m_apiClient, &ApiClient::apiKeysReceived,
            this, &ConnectWizardDialog::onApiKeysReceived);
    connect(m_apiClient, &ApiClient::modelsReceived,
            this, &ConnectWizardDialog::onModelsReceived);
    connect(m_apiClient, &ApiClient::requestFailed,
            this, &ConnectWizardDialog::onRequestFailed);

    if (m_editIndex >= 0) {
        const QList<Profile> profiles = m_profileManager->allProfiles();
        if (m_editIndex < profiles.size()) {
            m_nameEdit->setText(profiles[m_editIndex].name);
        }
    }

    if (QAbstractButton *button = m_typeGroup->button(static_cast<int>(m_selectedType))) {
        button->setChecked(true);
    }
    updateToolContext();
    m_apiClient->getApiKeys();
}

void ConnectWizardDialog::setupUi()
{
    setStyleSheet(QStringLiteral(
        "QDialog { background: #f6f7f9; }"
        "QLabel { color: #182230; }"
        "QLineEdit, QComboBox {"
        "  background: white; color: #182230; border: 1px solid #d0d5dd;"
        "  border-radius: 7px; padding: 0 12px; font-size: 13px;"
        "}"
        "QLineEdit:focus, QComboBox:focus { border: 2px solid #0f766e; }"
        "QComboBox::drop-down { border: none; width: 28px; }"
        "QComboBox QAbstractItemView {"
        "  background: white; color: #182230; border: 1px solid #d0d5dd;"
        "  selection-background-color: #e7f5f2; selection-color: #134e4a;"
        "}"
        "QToolTip { background: #182230; color: white; border: none; padding: 5px; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *header = new QFrame(this);
    header->setObjectName(QStringLiteral("wizardHeader"));
    header->setFixedHeight(76);
    header->setStyleSheet(QStringLiteral(
        "QFrame#wizardHeader { background: white; border-bottom: 1px solid #e4e7ec; }"));

    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(26, 14, 26, 14);
    headerLayout->setSpacing(12);

    auto *brand = new QLabel(QStringLiteral("A"), header);
    brand->setFixedSize(40, 40);
    brand->setAlignment(Qt::AlignCenter);
    brand->setStyleSheet(QStringLiteral(
        "background: #0f766e; color: white; border-radius: 8px;"
        "font-size: 18px; font-weight: 700;"));
    headerLayout->addWidget(brand);

    auto *titleColumn = new QVBoxLayout;
    titleColumn->setSpacing(1);
    auto *title = new QLabel(
        m_editIndex < 0 ? QStringLiteral("新建连接配置") : QStringLiteral("编辑连接配置"),
        header);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700; color: #101828;"));
    auto *subtitle = new QLabel(QStringLiteral("一个配置只连接一个 AI 工具"), header);
    subtitle->setStyleSheet(QStringLiteral("font-size: 12px; color: #667085;"));
    titleColumn->addWidget(title);
    titleColumn->addWidget(subtitle);
    headerLayout->addLayout(titleColumn);
    headerLayout->addStretch();

    m_stepLabel = new QLabel(header);
    m_stepLabel->setAlignment(Qt::AlignCenter);
    m_stepLabel->setFixedSize(92, 30);
    m_stepLabel->setStyleSheet(QStringLiteral(
        "background: #ecfdf5; color: #0f766e; border: 1px solid #a7f3d0;"
        "border-radius: 7px; font-size: 12px; font-weight: 600;"));
    headerLayout->addWidget(m_stepLabel);
    root->addWidget(header);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(buildIdentityPage());
    m_stack->addWidget(buildConnectionPage());
    root->addWidget(m_stack, 1);

    auto *footer = new QFrame(this);
    footer->setObjectName(QStringLiteral("wizardFooter"));
    footer->setFixedHeight(68);
    footer->setStyleSheet(QStringLiteral(
        "QFrame#wizardFooter { background: white; border-top: 1px solid #e4e7ec; }"));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(26, 13, 26, 13);

    m_backButton = new QPushButton(QStringLiteral("上一步"), footer);
    m_backButton->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    m_backButton->setFixedHeight(40);
    m_backButton->setStyleSheet(secondaryButtonStyle());
    footerLayout->addWidget(m_backButton);
    footerLayout->addStretch();

    m_nextButton = new QPushButton(footer);
    m_nextButton->setFixedHeight(40);
    m_nextButton->setMinimumWidth(116);
    m_nextButton->setStyleSheet(primaryButtonStyle());
    footerLayout->addWidget(m_nextButton);
    root->addWidget(footer);

    connect(m_backButton, &QPushButton::clicked, this, &ConnectWizardDialog::goBack);
    connect(m_nextButton, &QPushButton::clicked, this, &ConnectWizardDialog::goNext);
    updateNavigation();
}

QWidget *ConnectWizardDialog::buildIdentityPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(32, 28, 32, 26);
    layout->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("配置基本信息"), page);
    title->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 700; color: #101828;"));
    layout->addWidget(title);

    auto *nameLabel = new QLabel(QStringLiteral("配置名称"), page);
    nameLabel->setStyleSheet(QStringLiteral("font-size: 12px; font-weight: 600; color: #475467;"));
    layout->addSpacing(10);
    layout->addWidget(nameLabel);

    m_nameEdit = new QLineEdit(page);
    m_nameEdit->setFixedHeight(44);
    m_nameEdit->setPlaceholderText(QStringLiteral("例如：工作账号 Codex"));
    layout->addWidget(m_nameEdit);

    auto *typeLabel = new QLabel(QStringLiteral("连接工具"), page);
    typeLabel->setStyleSheet(QStringLiteral("font-size: 12px; font-weight: 600; color: #475467;"));
    layout->addSpacing(14);
    layout->addWidget(typeLabel);

    auto *typeRow = new QHBoxLayout;
    typeRow->setSpacing(10);
    m_typeGroup = new QButtonGroup(this);
    m_typeGroup->setExclusive(true);

    for (ProfileType type : allProfileTypes()) {
        const AiTool tool = toolForType(type);
        auto *button = new QPushButton(toolSelectorText(tool), page);
        button->setCheckable(true);
        button->setMinimumHeight(84);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: white; color: #344054; border: 1px solid #d0d5dd;"
            "  border-radius: 8px; padding: 10px; font-size: 13px; font-weight: 600;"
            "}"
            "QPushButton:hover { border-color: %1; background: %2; }"
            "QPushButton:checked { border: 2px solid %1; background: %2; color: %1; }")
            .arg(toolAccent(tool), toolSoftColor(tool)));
        m_typeGroup->addButton(button, static_cast<int>(type));
        typeRow->addWidget(button);
    }
    layout->addLayout(typeRow);

    auto *note = new QLabel(
        QStringLiteral("切换配置时，只会更新所选工具的本地认证文件。"), page);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral(
        "background: #f0fdf9; color: #0f5f59; border: 1px solid #b7e4da;"
        "border-radius: 7px; padding: 10px 12px; font-size: 12px;"));
    layout->addSpacing(10);
    layout->addWidget(note);
    layout->addStretch();

    connect(m_typeGroup, QOverload<int>::of(&QButtonGroup::idClicked),
            this, &ConnectWizardDialog::onTypeChanged);
    return page;
}

QWidget *ConnectWizardDialog::buildConnectionPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(32, 26, 32, 26);
    layout->setSpacing(10);

    auto *context = new QFrame(page);
    context->setObjectName(QStringLiteral("toolContext"));
    context->setStyleSheet(QStringLiteral(
        "QFrame#toolContext { background: white; border: 1px solid #e4e7ec; border-radius: 8px; }"));
    auto *contextLayout = new QHBoxLayout(context);
    contextLayout->setContentsMargins(14, 12, 14, 12);
    contextLayout->setSpacing(12);

    m_toolBadge = new QLabel(context);
    m_toolBadge->setFixedSize(42, 42);
    m_toolBadge->setAlignment(Qt::AlignCenter);
    contextLayout->addWidget(m_toolBadge);

    auto *toolText = new QVBoxLayout;
    toolText->setSpacing(1);
    m_toolTitle = new QLabel(context);
    m_toolTitle->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 700; color: #101828;"));
    m_toolPath = new QLabel(context);
    m_toolPath->setStyleSheet(QStringLiteral(
        "font-size: 11px; color: #667085; font-family: monospace;"));
    toolText->addWidget(m_toolTitle);
    toolText->addWidget(m_toolPath);
    contextLayout->addLayout(toolText, 1);
    layout->addWidget(context);

    auto *keyLabel = new QLabel(QStringLiteral("API Key"), page);
    keyLabel->setStyleSheet(QStringLiteral("font-size: 12px; font-weight: 600; color: #475467;"));
    layout->addSpacing(12);
    layout->addWidget(keyLabel);

    auto *keyRow = new QHBoxLayout;
    keyRow->setSpacing(8);
    m_keyCombo = new QComboBox(page);
    m_keyCombo->setFixedHeight(42);
    m_keyCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    keyRow->addWidget(m_keyCombo, 1);

    m_queryButton = new QPushButton(QStringLiteral("查询模型"), page);
    m_queryButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_queryButton->setFixedHeight(42);
    m_queryButton->setStyleSheet(secondaryButtonStyle());
    keyRow->addWidget(m_queryButton);
    layout->addLayout(keyRow);

    m_loadingLabel = new QLabel(page);
    m_loadingLabel->setWordWrap(true);
    m_loadingLabel->setVisible(false);
    m_loadingLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #667085;"));
    layout->addWidget(m_loadingLabel);

    auto *modelLabel = new QLabel(QStringLiteral("模型"), page);
    modelLabel->setStyleSheet(QStringLiteral("font-size: 12px; font-weight: 600; color: #475467;"));
    layout->addSpacing(8);
    layout->addWidget(modelLabel);

    m_modelCombo = new QComboBox(page);
    m_modelCombo->setEditable(true);
    m_modelCombo->setInsertPolicy(QComboBox::NoInsert);
    m_modelCombo->setFixedHeight(42);
    m_modelCombo->addItem(QStringLiteral("使用工具默认模型"), QString());
    if (m_modelCombo->lineEdit()) {
        m_modelCombo->lineEdit()->setPlaceholderText(QStringLiteral("使用默认模型，或输入模型名称"));
    }
    layout->addWidget(m_modelCombo);

    auto *hint = new QLabel(
        QStringLiteral("模型可以留空；激活时会写入该工具的默认模型。"), page);
    hint->setStyleSheet(QStringLiteral("font-size: 11px; color: #667085;"));
    layout->addWidget(hint);
    layout->addStretch();

    connect(m_queryButton, &QPushButton::clicked,
            this, &ConnectWizardDialog::onQueryModels);
    return page;
}

void ConnectWizardDialog::onApiKeysReceived(const QJsonArray &keys)
{
    m_allKeys = keys;
    populateKeyDropdown();
}

void ConnectWizardDialog::populateKeyDropdown()
{
    if (!m_keyCombo) {
        return;
    }

    const QString previousKey = currentKey();
    const QString platform = ToolManager::toolPlatform(selectedTool());
    m_keyCombo->clear();
    m_keyCombo->addItem(QStringLiteral("请选择 API Key"), QString());

    for (const QJsonValue &value : m_allKeys) {
        const QJsonObject object = value.toObject();
        const QJsonObject group = object.value(QStringLiteral("group")).toObject();
        if (group.value(QStringLiteral("platform")).toString() != platform) {
            continue;
        }

        const QString key = object.value(QStringLiteral("key")).toString();
        if (key.isEmpty()) {
            continue;
        }
        QString name = object.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) {
            name = key.left(8) + QStringLiteral("...");
        }
        m_keyCombo->addItem(name, key);
    }

    QString keyToSelect = previousKey;
    if (keyToSelect.isEmpty() && m_selectedType == m_existingType) {
        keyToSelect = m_existingKey;
    }

    int selectedIndex = -1;
    for (int i = 1; i < m_keyCombo->count(); ++i) {
        if (m_keyCombo->itemData(i).toString() == keyToSelect) {
            selectedIndex = i;
            break;
        }
    }

    if (selectedIndex < 0 && !keyToSelect.isEmpty()) {
        m_keyCombo->addItem(
            QStringLiteral("当前保存的 Key (%1...)").arg(keyToSelect.left(8)),
            keyToSelect);
        selectedIndex = m_keyCombo->count() - 1;
    }
    m_keyCombo->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);

    if (m_selectedType == m_existingType && !m_existingModel.isEmpty()) {
        const int modelIndex = m_modelCombo->findText(m_existingModel);
        if (modelIndex >= 0) {
            m_modelCombo->setCurrentIndex(modelIndex);
        } else {
            m_modelCombo->addItem(m_existingModel, m_existingModel);
            m_modelCombo->setCurrentIndex(m_modelCombo->count() - 1);
        }
    } else if (m_selectedType != m_existingType) {
        m_modelCombo->setCurrentIndex(0);
    }
}

void ConnectWizardDialog::onModelsReceived(const QJsonArray &models)
{
    if (!m_waitingModels) {
        return;
    }

    m_waitingModels = false;
    const QString previousModel = currentModel();
    m_modelCombo->clear();
    m_modelCombo->addItem(QStringLiteral("使用工具默认模型"), QString());

    for (const QJsonValue &value : models) {
        QString modelId;
        if (value.isObject()) {
            modelId = value.toObject().value(QStringLiteral("id")).toString();
        } else {
            modelId = value.toString();
        }
        if (!modelId.isEmpty() && m_modelCombo->findText(modelId) < 0) {
            m_modelCombo->addItem(modelId, modelId);
        }
    }

    if (!previousModel.isEmpty()) {
        int index = m_modelCombo->findText(previousModel);
        if (index < 0) {
            m_modelCombo->addItem(previousModel, previousModel);
            index = m_modelCombo->count() - 1;
        }
        m_modelCombo->setCurrentIndex(index);
    }

    setModelLoading(false, models.isEmpty()
        ? QStringLiteral("当前 Key 未返回可用模型，可以手动输入模型名称。")
        : QStringLiteral("已加载 %1 个模型").arg(models.size()));
}

void ConnectWizardDialog::onRequestFailed(const QString &error)
{
    if (!m_waitingModels) {
        return;
    }
    m_waitingModels = false;
    setModelLoading(false, QStringLiteral("模型查询失败：%1").arg(error));
}

void ConnectWizardDialog::onQueryModels()
{
    const QString key = currentKey();
    if (key.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("请选择 Key"),
                                 QStringLiteral("请先选择一个 API Key。"));
        m_keyCombo->setFocus();
        return;
    }

    m_waitingModels = true;
    setModelLoading(true);
    m_apiClient->getModels(key);
}

void ConnectWizardDialog::setModelLoading(bool loading, const QString &message)
{
    m_queryButton->setEnabled(!loading);
    m_loadingLabel->setVisible(loading || !message.isEmpty());
    m_loadingLabel->setText(loading ? QStringLiteral("正在查询可用模型...") : message);
    m_loadingLabel->setStyleSheet(loading || !message.startsWith(QStringLiteral("模型查询失败"))
        ? QStringLiteral("font-size: 12px; color: #667085;")
        : QStringLiteral("font-size: 12px; color: #b42318;"));
}

void ConnectWizardDialog::onTypeChanged(int id)
{
    const ProfileType type = static_cast<ProfileType>(id);
    if (!isValidProfileType(type)) {
        return;
    }
    m_selectedType = type;
    updateToolContext();
    populateKeyDropdown();
}

void ConnectWizardDialog::updateToolContext()
{
    if (!m_toolBadge) {
        return;
    }

    const AiTool tool = selectedTool();
    const QString accent = toolAccent(tool);
    m_toolBadge->setText(toolLetter(tool));
    m_toolBadge->setStyleSheet(QStringLiteral(
        "background: %1; color: white; border-radius: 8px;"
        "font-size: 17px; font-weight: 700;").arg(accent));
    m_toolTitle->setText(ToolManager::toolName(tool));
    m_toolPath->setText(QStringLiteral("激活时更新 %1").arg(toolConfigPath(tool)));
}

void ConnectWizardDialog::updateNavigation()
{
    const int page = m_stack->currentIndex();
    m_backButton->setEnabled(page > 0);
    m_stepLabel->setText(page == 0 ? QStringLiteral("1 / 2  基本信息")
                                   : QStringLiteral("2 / 2  接入设置"));
    m_nextButton->setText(page == 0 ? QStringLiteral("下一步")
                                    : QStringLiteral("保存配置"));
    m_nextButton->setIcon(style()->standardIcon(
        page == 0 ? QStyle::SP_ArrowForward : QStyle::SP_DialogApplyButton));
}

void ConnectWizardDialog::goNext()
{
    if (m_stack->currentIndex() == 0) {
        if (m_nameEdit->text().trimmed().isEmpty()) {
            QMessageBox::information(this, QStringLiteral("填写配置名称"),
                                     QStringLiteral("请输入一个便于识别的配置名称。"));
            m_nameEdit->setFocus();
            return;
        }
        m_stack->setCurrentIndex(1);
        updateToolContext();
        populateKeyDropdown();
        updateNavigation();
        return;
    }

    finishProfile();
}

void ConnectWizardDialog::goBack()
{
    if (m_stack->currentIndex() == 0) {
        return;
    }
    m_stack->setCurrentIndex(0);
    updateNavigation();
}

void ConnectWizardDialog::finishProfile()
{
    const QString key = currentKey();
    if (key.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("请选择 Key"),
                                 QStringLiteral("每个连接配置都需要绑定一个 API Key。"));
        m_keyCombo->setFocus();
        return;
    }

    const QString name = m_nameEdit->text().trimmed();
    const QString model = currentModel();
    if (m_editIndex < 0) {
        m_resultIndex = m_profileManager->addProfile(
            name, m_selectedType, key, model);
    } else {
        m_profileManager->updateProfile(
            m_editIndex, name, m_selectedType, key, model);
        m_resultIndex = m_editIndex;
    }
    accept();
}

AiTool ConnectWizardDialog::selectedTool() const
{
    return toolForType(m_selectedType);
}

QString ConnectWizardDialog::currentKey() const
{
    return m_keyCombo ? m_keyCombo->currentData(Qt::UserRole).toString() : QString();
}

QString ConnectWizardDialog::currentModel() const
{
    if (!m_modelCombo) {
        return QString();
    }
    const QString model = m_modelCombo->currentText().trimmed();
    return model == QStringLiteral("使用工具默认模型") ? QString() : model;
}
