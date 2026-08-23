#include "connect_wizard.h"
#include "app_theme.h"
#include "companion_credential_broker.h"
#include "companion_model_projection.h"
#include "status_badge.h"

#include <QFrame>
#include <QCryptographicHash>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QMessageBox>
#include <QStyle>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

namespace {

QString localProfileIdentity(const QString &profileId)
{
    if (profileId.isEmpty()) return {};
    QByteArray input = QByteArrayLiteral("aegisy-local-profile-model-binding/0.1\0");
    const QByteArray value = profileId.toUtf8();
    const quint64 size = static_cast<quint64>(value.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        input.append(static_cast<char>((size >> shift) & 0xff));
    }
    input.append(value);
    return QStringLiteral("local-profile:sha256:%1").arg(
        QString::fromLatin1(QCryptographicHash::hash(
            input, QCryptographicHash::Sha256).toHex()));
}

QString toolAccent(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("#c15f3c");
    case AiTool::CodexCli:   return QStringLiteral("#111827");
    case AiTool::GeminiCli:  return QStringLiteral("#1a73e8");
    case AiTool::OpenCode:   return QStringLiteral("#059669");
    }
    return QStringLiteral("#0f766e");
}

QString toolSoftColor(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("#fff4ef");
    case AiTool::CodexCli:   return QStringLiteral("#f3f4f6");
    case AiTool::GeminiCli:  return QStringLiteral("#eef5ff");
    case AiTool::OpenCode:   return QStringLiteral("#ecfdf5");
    }
    return QStringLiteral("#ecfdf5");
}

QString toolLetter(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("C");
    case AiTool::CodexCli:   return QStringLiteral("O");
    case AiTool::GeminiCli:  return QStringLiteral("G");
    case AiTool::OpenCode:   return QStringLiteral("OC");
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
    case AiTool::OpenCode:   return QStringLiteral("OpenCode\nAnthropic");
    }
    return QString();
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
        const Profile profile = m_profileManager->profileWithCredential(m_editIndex);
        if (profile.index >= 0) {
            m_selectedType = profile.type;
            m_existingType = profile.type;
            m_existingKey = profile.key;
            m_existingModel = profile.model;
            m_existingProfileId = profile.id;
            m_existingWebsiteBinding = {
                profile.websiteAccountIdentity,
                profile.websiteKeyIdentity,
                profile.websiteProjectionSha256,
            };
        }
    }

    setupUi();

    setWindowTitle(m_editIndex < 0 ? QStringLiteral("新建连接配置")
                                   : QStringLiteral("编辑连接配置"));
    resize(640, 600);
    setMinimumSize(600, 570);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    connect(m_apiClient, &ApiClient::companionConfigurationReceived,
            this, &ConnectWizardDialog::onCompanionConfigurationReceived);
    connect(m_apiClient, &ApiClient::companionModelsReceived,
            this, &ConnectWizardDialog::onCompanionModelsReceived);
    connect(m_apiClient, &ApiClient::companionModelsFailed,
            this, &ConnectWizardDialog::onCompanionModelsFailed);
    connect(m_apiClient, &ApiClient::connectionTested,
            this, &ConnectWizardDialog::onConnectionTested);

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
    // 直接继承全局 AppTheme 样式，无需重复定义；
    // 仅追加 Header/Footer 布局框架所需的局部样式
    setStyleSheet(QStringLiteral(
        "QFrame#wizardHeader, QFrame#wizardFooter { background: white; }"
        "QFrame#wizardHeader { border-bottom: 1px solid #e4e7ec; }"
        "QFrame#wizardFooter { border-top:    1px solid #e4e7ec; }"));

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
    auto *subtitle = new QLabel(QStringLiteral("本机 AI 工具接入"), header);
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
    m_backButton->setFixedHeight(36);
    m_backButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    footerLayout->addWidget(m_backButton);
    footerLayout->addStretch();

    m_nextButton = new QPushButton(footer);
    m_nextButton->setFixedHeight(36);
    m_nextButton->setMinimumWidth(116);
    m_nextButton->setStyleSheet(AppTheme::primaryButtonStyle());
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
    m_nameEdit->setFixedHeight(36);
    m_nameEdit->setPlaceholderText(QStringLiteral("例如：工作账号 Codex"));
    layout->addWidget(m_nameEdit);

    auto *typeLabel = new QLabel(QStringLiteral("选择终端（一个档案只绑定一个）"), page);
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

    layout->addStretch();

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_typeGroup, QOverload<int>::of(&QButtonGroup::idClicked),
            this, &ConnectWizardDialog::onTypeChanged);
#else
    connect(m_typeGroup, QOverload<int>::of(&QButtonGroup::buttonClicked),
            this, &ConnectWizardDialog::onTypeChanged);
#endif
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
    m_keyCombo->setFixedHeight(36);
    m_keyCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    keyRow->addWidget(m_keyCombo, 1);

    m_queryButton = new QPushButton(QStringLiteral("刷新模型"), page);
    m_queryButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_queryButton->setFixedHeight(36);
    m_queryButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    keyRow->addWidget(m_queryButton);

    m_testButton = new QPushButton(QStringLiteral("测试连接"), page);
    m_testButton->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
    m_testButton->setFixedHeight(36);
    m_testButton->setStyleSheet(AppTheme::primaryButtonStyle());
    keyRow->addWidget(m_testButton);
    layout->addLayout(keyRow);

    m_loadingLabel = new StatusBadge(page);
    m_loadingLabel->setVisible(false);
    layout->addWidget(m_loadingLabel);

    auto *modelLabel = new QLabel(QStringLiteral("模型"), page);
    modelLabel->setStyleSheet(QStringLiteral("font-size: 12px; font-weight: 600; color: #475467;"));
    layout->addSpacing(8);
    layout->addWidget(modelLabel);

    m_modelCombo = new QComboBox(page);
    m_modelCombo->setEditable(false);
    m_modelCombo->setFixedHeight(36);
    m_modelCombo->addItem(QStringLiteral("使用工具默认模型"), QString());
    layout->addWidget(m_modelCombo);

    auto *suggestLabel = new QLabel(QStringLiteral("常用模型"), page);
    suggestLabel->setStyleSheet(QStringLiteral("font-size: 11px; color: #98a2b3;"));
    layout->addWidget(suggestLabel);

    m_modelSuggestions = new QWidget(page);
    auto *suggestLayout = new QHBoxLayout(m_modelSuggestions);
    suggestLayout->setContentsMargins(0, 0, 0, 0);
    suggestLayout->setSpacing(6);
    layout->addWidget(m_modelSuggestions);

    layout->addStretch();

    connect(m_queryButton, &QPushButton::clicked,
            this, &ConnectWizardDialog::onQueryModels);
    connect(m_keyCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ConnectWizardDialog::onKeyChanged);
    connect(m_testButton, &QPushButton::clicked,
            this, &ConnectWizardDialog::onTestConnection);
    return page;
}

void ConnectWizardDialog::onTestConnection()
{
    const QString key = currentKey();
    if (key.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("未选择 API Key"),
                             QStringLiteral("请先选择一个 API Key。"));
        return;
    }
    m_waitingConnectionTest = true;
    m_connectionRequestId = QStringLiteral("connect-wizard-test-%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_connectionRequestKeyIdentity = currentModelKeyIdentity();
    m_testButton->setEnabled(false);
    m_loadingLabel->setVisible(true);
    m_loadingLabel->setState(
        QStringLiteral("正在验证连接"), StatusBadge::Tone::Info,
        style()->standardIcon(QStyle::SP_BrowserReload));
    m_apiClient->testConnection(m_connectionRequestId, key, currentModel());
}

void ConnectWizardDialog::onConnectionTested(const QString &requestId,
                                              bool success,
                                              const QString &detail,
                                              int latencyMs)
{
    if (requestId != m_connectionRequestId || !m_waitingConnectionTest
            || m_connectionRequestKeyIdentity != currentModelKeyIdentity()) {
        return;
    }
    m_waitingConnectionTest = false;
    m_connectionRequestId.clear();
    m_connectionRequestKeyIdentity.clear();
    m_testButton->setEnabled(true);
    m_loadingLabel->setVisible(true);
    m_loadingLabel->setState(
        QStringLiteral("%1 · %2 ms").arg(detail).arg(latencyMs),
        success ? StatusBadge::Tone::Success : StatusBadge::Tone::Error,
        style()->standardIcon(success
            ? QStyle::SP_DialogApplyButton : QStyle::SP_MessageBoxCritical));
}

void ConnectWizardDialog::onCompanionConfigurationReceived(
    const QJsonObject &projection)
{
    m_waitingConnectionTest = false;
    m_connectionRequestId.clear();
    m_connectionRequestKeyIdentity.clear();
    if (m_testButton) m_testButton->setEnabled(true);
    if (m_waitingModels) {
        m_waitingModels = false;
        m_waitingCompanionModels = false;
        m_modelRequestId.clear();
        m_modelRequestKeyIdentity.clear();
        setModelLoading(false);
    }
    m_companionProjection = projection;
    populateKeyDropdown();
}

void ConnectWizardDialog::populateKeyDropdown()
{
    if (!m_keyCombo) {
        return;
    }

    // 终端切换后不复用上一终端的 Key，避免跨平台凭据被误写入。
    const QString previousHandle = m_selectedType == m_existingType
        ? m_keyCombo->currentData(Qt::UserRole).toString() : QString();
    const QString platform = ToolManager::toolPlatform(selectedTool());
    m_keyCombo->blockSignals(true);
    m_keyCombo->clear();
    m_keyCombo->addItem(QStringLiteral("请选择 API Key"), QString());

    for (const QJsonValue &value :
         m_companionProjection.value(QStringLiteral("keys")).toArray()) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("platform")).toString() != platform
                || object.value(QStringLiteral("state")).toString()
                    != QStringLiteral("active")
                || object.value(QStringLiteral("credential_state")).toString()
                    != QStringLiteral("available-in-secure-storage")) {
            continue;
        }

        const QString handle = object.value(
            QStringLiteral("credential_handle")).toString();
        const QString keyIdentity = object.value(
            QStringLiteral("key_identity")).toString();
        if (handle.isEmpty() || keyIdentity.isEmpty()) {
            continue;
        }
        QString name = object.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) name = object.value(QStringLiteral("display_name")).toString();
        m_keyCombo->addItem(name, handle);
        const int item = m_keyCombo->count() - 1;
        m_keyCombo->setItemData(item, keyIdentity, Qt::UserRole + 1);
        m_keyCombo->setItemData(
            item, m_companionProjection.value(QStringLiteral("account_identity")).toString(),
            Qt::UserRole + 2);
        m_keyCombo->setItemData(
            item, m_companionProjection.value(QStringLiteral("projection_sha256")).toString(),
            Qt::UserRole + 3);
        m_keyCombo->setItemData(item, platform, Qt::UserRole + 5);
    }

    int selectedIndex = -1;
    for (int i = 1; i < m_keyCombo->count(); ++i) {
        if (!previousHandle.isEmpty()
                && m_keyCombo->itemData(i).toString() == previousHandle) {
            selectedIndex = i;
            break;
        }
        if (previousHandle.isEmpty() && m_selectedType == m_existingType
                && !m_existingWebsiteBinding.keyIdentity.isEmpty()
                && m_keyCombo->itemData(i, Qt::UserRole + 1).toString()
                    == m_existingWebsiteBinding.keyIdentity
                && m_keyCombo->itemData(i, Qt::UserRole + 2).toString()
                    == m_existingWebsiteBinding.accountIdentity) {
            selectedIndex = i;
            break;
        }
    }

    if (selectedIndex < 0 && m_selectedType == m_existingType
            && !m_existingKey.isEmpty()) {
        m_keyCombo->addItem(QStringLiteral("当前配置中已安全保存的凭据"), QString());
        selectedIndex = m_keyCombo->count() - 1;
        m_keyCombo->setItemData(selectedIndex, true, Qt::UserRole + 4);
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
    m_keyCombo->blockSignals(false);

    if (!currentKey().isEmpty() && !m_waitingModels) {
        QTimer::singleShot(0, this, &ConnectWizardDialog::onQueryModels);
    }
}

void ConnectWizardDialog::onKeyChanged(int)
{
    m_waitingConnectionTest = false;
    m_connectionRequestId.clear();
    m_connectionRequestKeyIdentity.clear();
    m_testButton->setEnabled(true);
    m_waitingModels = false;
    m_waitingCompanionModels = false;
    m_modelRequestId.clear();
    m_modelRequestKeyIdentity.clear();
    m_modelRequestAccountIdentity.clear();
    m_modelRequestCredentialHandle.clear();
    m_modelRequestProjectionSha256.clear();
    m_modelRequestPlatform.clear();
    m_modelCombo->clear();
    m_modelCombo->addItem(QStringLiteral("使用工具默认模型"), QString());
    if (currentKey().isEmpty()) {
        setModelLoading(false);
        return;
    }
    onQueryModels();
}

void ConnectWizardDialog::onCompanionModelsReceived(
    const QString &requestId, const QString &keyIdentity,
    const QJsonObject &projection)
{
    if (!m_waitingModels || !m_waitingCompanionModels
            || requestId != m_modelRequestId
            || keyIdentity != m_modelRequestKeyIdentity
            || projection.value(QStringLiteral("key_identity")).toString()
                != keyIdentity
            || !CompanionModelProjection::validate(projection)
            || currentModelKeyIdentity() != keyIdentity
            || m_keyCombo->currentData(Qt::UserRole).toString()
                != m_modelRequestCredentialHandle
            || m_keyCombo->currentData(Qt::UserRole + 2).toString()
                != m_modelRequestAccountIdentity
            || m_keyCombo->currentData(Qt::UserRole + 3).toString()
                != m_modelRequestProjectionSha256
            || ToolManager::toolPlatform(selectedTool()) != m_modelRequestPlatform) {
        return;
    }
    m_waitingModels = false;
    m_waitingCompanionModels = false;
    m_modelRequestId.clear();
    m_modelRequestKeyIdentity.clear();
    m_modelRequestAccountIdentity.clear();
    m_modelRequestCredentialHandle.clear();
    m_modelRequestProjectionSha256.clear();
    m_modelRequestPlatform.clear();
    m_modelRequestAccountIdentity.clear();
    m_modelRequestCredentialHandle.clear();
    m_modelRequestProjectionSha256.clear();
    m_modelRequestPlatform.clear();
    QJsonArray models;
    for (const QJsonValue &value : projection.value(QStringLiteral("models")).toArray()) {
        models.append(QJsonObject{{QStringLiteral("id"), value.toString()}});
    }
    applyModels(models);
}

void ConnectWizardDialog::onCompanionModelsFailed(
    const QString &requestId, const QString &keyIdentity, const QString &errorCode)
{
    if (!m_waitingModels || !m_waitingCompanionModels
            || requestId != m_modelRequestId
            || keyIdentity != m_modelRequestKeyIdentity
            || currentModelKeyIdentity() != keyIdentity) {
        return;
    }
    m_waitingModels = false;
    m_waitingCompanionModels = false;
    m_modelRequestId.clear();
    m_modelRequestKeyIdentity.clear();
    m_modelRequestAccountIdentity.clear();
    m_modelRequestCredentialHandle.clear();
    m_modelRequestProjectionSha256.clear();
    m_modelRequestPlatform.clear();
    setModelLoading(false, QStringLiteral("模型查询失败：%1").arg(errorCode));
}

void ConnectWizardDialog::applyModels(const QJsonArray &models)
{
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
        ? QStringLiteral("当前 Key 未返回可用模型，将使用工具默认模型。")
        : QStringLiteral("已加载 %1 个模型").arg(models.size()));
}

void ConnectWizardDialog::onQueryModels()
{
    if (m_waitingModels) {
        return;
    }
    if (!m_keyCombo || m_keyCombo->currentIndex() <= 0) {
        QMessageBox::information(this, QStringLiteral("请选择 Key"),
                                 QStringLiteral("请先选择一个 API Key。"));
        m_keyCombo->setFocus();
        return;
    }

    m_waitingModels = true;
    setModelLoading(true);
    const int index = m_keyCombo->currentIndex();
    const QString handle = m_keyCombo->itemData(index, Qt::UserRole).toString();
    const QString keyIdentity = m_keyCombo->itemData(
        index, Qt::UserRole + 1).toString();
    const QString accountIdentity = m_keyCombo->itemData(
        index, Qt::UserRole + 2).toString();
    const QString projectionSha256 = m_keyCombo->itemData(
        index, Qt::UserRole + 3).toString();
    const QString platform = ToolManager::toolPlatform(selectedTool());
    m_modelRequestId = QStringLiteral("connect-wizard-model-%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!handle.isEmpty() && !keyIdentity.isEmpty() && !accountIdentity.isEmpty()) {
        m_waitingCompanionModels = true;
        m_modelRequestKeyIdentity = keyIdentity;
        m_modelRequestAccountIdentity = accountIdentity;
        m_modelRequestCredentialHandle = handle;
        m_modelRequestProjectionSha256 = projectionSha256;
        m_modelRequestPlatform = platform;
        m_apiClient->getCompanionModels(
            m_modelRequestId, accountIdentity, keyIdentity, handle,
            projectionSha256, platform);
    } else {
        const QString key = currentKey();
        const QString localIdentity = localProfileIdentity(m_existingProfileId);
        if (key.isEmpty() || localIdentity.isEmpty()) {
            m_waitingModels = false;
            setModelLoading(false, QStringLiteral("模型查询失败：本地凭据不可用"));
            return;
        }
        m_waitingCompanionModels = true;
        m_modelRequestKeyIdentity = localIdentity;
        m_modelRequestAccountIdentity.clear();
        m_modelRequestCredentialHandle.clear();
        m_modelRequestProjectionSha256.clear();
        m_modelRequestPlatform = platform;
        m_apiClient->getProfileModels(m_modelRequestId, localIdentity, key);
    }
}

void ConnectWizardDialog::setModelLoading(bool loading, const QString &message)
{
    m_queryButton->setEnabled(!loading);
    m_keyCombo->setEnabled(!loading);
    m_modelCombo->setEnabled(!loading);
    m_loadingLabel->setVisible(loading || !message.isEmpty());
    if (loading) {
        m_loadingLabel->setState(
            QStringLiteral("正在查询模型"), StatusBadge::Tone::Info,
            style()->standardIcon(QStyle::SP_BrowserReload));
    } else if (!message.isEmpty()) {
        const bool failed = message.startsWith(QStringLiteral("模型查询失败"));
        const bool empty = message.startsWith(QStringLiteral("当前 Key 未返回"));
        m_loadingLabel->setState(
            message,
            failed ? StatusBadge::Tone::Error
                   : (empty ? StatusBadge::Tone::Warning : StatusBadge::Tone::Success),
            style()->standardIcon(failed
                ? QStyle::SP_MessageBoxCritical
                : (empty ? QStyle::SP_MessageBoxWarning
                         : QStyle::SP_DialogApplyButton)));
    }
}

void ConnectWizardDialog::onTypeChanged(int id)
{
    const ProfileType type = static_cast<ProfileType>(id);
    if (!isValidProfileType(type)) {
        return;
    }
    m_selectedType = type;
    m_waitingConnectionTest = false;
    m_connectionRequestId.clear();
    m_connectionRequestKeyIdentity.clear();
    m_testButton->setEnabled(true);
    m_waitingModels = false;
    m_waitingCompanionModels = false;
    m_modelRequestId.clear();
    m_modelRequestKeyIdentity.clear();
    setModelLoading(false);
    m_modelCombo->clear();
    m_modelCombo->addItem(QStringLiteral("使用工具默认模型"), QString());
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

    if (!m_modelSuggestions) {
        return;
    }
    QHBoxLayout *suggestLayout = qobject_cast<QHBoxLayout *>(m_modelSuggestions->layout());
    if (!suggestLayout) {
        return;
    }
    while (suggestLayout->count() > 0) {
        QLayoutItem *item = suggestLayout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    const QStringList models = toolModelSuggestions(tool);
    for (const QString &modelName : models) {
        auto *chip = new QPushButton(modelName, m_modelSuggestions);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setFixedHeight(26);
        chip->setStyleSheet(QStringLiteral(
            "QPushButton {"
            " background:#f2f4f7; color:#475467; border:1px solid #e4e7ec;"
            " border-radius:5px; padding:2px 8px; font-size:11px;"
            "}"
            "QPushButton:hover {"
            " background:#e7f5f2; color:#0f5f59; border-color:#b7e4da;"
            "}"));
        connect(chip, &QPushButton::clicked, this, [this, modelName]() {
            if (m_modelCombo->findText(modelName) < 0) {
                m_modelCombo->addItem(modelName, modelName);
            }
            m_modelCombo->setCurrentIndex(m_modelCombo->findText(modelName));
        });
        suggestLayout->addWidget(chip);
    }
    suggestLayout->addStretch();
}

QStringList ConnectWizardDialog::toolModelSuggestions(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode:
        return { QStringLiteral("claude-opus-4-8"),
                 QStringLiteral("claude-sonnet-5"),
                 QStringLiteral("claude-haiku-4-5"),
                 QStringLiteral("claude-sonnet-4-5") };
    case AiTool::CodexCli:
        return { QStringLiteral("gpt-4o"),
                 QStringLiteral("gpt-4.5"),
                 QStringLiteral("o3"),
                 QStringLiteral("o4-mini"),
                 QStringLiteral("gpt-4o-mini") };
    case AiTool::GeminiCli:
        return { QStringLiteral("gemini-2.5-pro"),
                 QStringLiteral("gemini-2.5-flash"),
                 QStringLiteral("gemini-2.0-flash-001") };
    case AiTool::OpenCode:
        return { QStringLiteral("anthropic/claude-opus-4-5"),
                 QStringLiteral("anthropic/claude-sonnet-4-5"),
                 QStringLiteral("openai/gpt-4o") };
    }
    return {};
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
    const ProfileWebsiteBinding website = currentWebsiteBinding();
    if (m_editIndex < 0) {
        m_resultIndex = m_profileManager->addProfile(
            name, m_selectedType, key, model, website);
        if (m_resultIndex < 0) {
            QMessageBox::critical(this, QStringLiteral("保存失败"),
                                  m_profileManager->lastError());
            return;
        }
    } else {
        if (!m_profileManager->updateProfile(
                m_editIndex, name, m_selectedType, key, model, website)) {
            QMessageBox::critical(this, QStringLiteral("保存失败"),
                                  m_profileManager->lastError());
            return;
        }
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
    if (!m_keyCombo || m_keyCombo->currentIndex() <= 0) return {};
    const int index = m_keyCombo->currentIndex();
    if (m_keyCombo->itemData(index, Qt::UserRole + 4).toBool()) {
        return m_existingKey;
    }
    return CompanionCredentialBroker::resolve(
        m_keyCombo->itemData(index, Qt::UserRole + 2).toString(),
        m_keyCombo->itemData(index, Qt::UserRole + 1).toString(),
        m_keyCombo->itemData(index, Qt::UserRole).toString());
}

QString ConnectWizardDialog::currentModel() const
{
    if (!m_modelCombo) {
        return QString();
    }
    const QString model = m_modelCombo->currentText().trimmed();
    return model == QStringLiteral("使用工具默认模型") ? QString() : model;
}

ProfileWebsiteBinding ConnectWizardDialog::currentWebsiteBinding() const
{
    if (!m_keyCombo || m_keyCombo->currentIndex() <= 0) return {};
    const int index = m_keyCombo->currentIndex();
    if (m_keyCombo->itemData(index, Qt::UserRole + 4).toBool()) {
        return m_existingWebsiteBinding;
    }
    return {
        m_keyCombo->itemData(index, Qt::UserRole + 2).toString(),
        m_keyCombo->itemData(index, Qt::UserRole + 1).toString(),
        m_keyCombo->itemData(index, Qt::UserRole + 3).toString(),
    };
}

QString ConnectWizardDialog::currentModelKeyIdentity() const
{
    if (!m_keyCombo || m_keyCombo->currentIndex() <= 0) return {};
    const QString websiteIdentity = m_keyCombo->currentData(
        Qt::UserRole + 1).toString();
    return websiteIdentity.isEmpty()
        ? localProfileIdentity(m_existingProfileId) : websiteIdentity;
}
