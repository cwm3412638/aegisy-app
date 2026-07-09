#include "connect_wizard.h"
#include "tool_manager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QScrollArea>
#include <QMessageBox>
#include <QButtonGroup>
#include <QDesktopServices>
#include <QUrl>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static QString toolLabel(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QString::fromUtf8("🤖 Claude Code");
    case AiTool::CodexCli:   return QString::fromUtf8("⚡ Codex CLI");
    case AiTool::GeminiCli:  return QString::fromUtf8("💎 Gemini CLI");
    }
    return {};
}

static QString toolAccent(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("#6366f1");
    case AiTool::CodexCli:   return QStringLiteral("#3b82f6");
    case AiTool::GeminiCli:  return QStringLiteral("#0d9488");
    }
    return QStringLiteral("#6366f1");
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ConnectWizardDialog::ConnectWizardDialog(ApiClient *client,
                                          ProfileManager *pm,
                                          int editIndex,
                                          QWidget *parent)
    : QDialog(parent)
    , m_apiClient(client)
    , m_profileManager(pm)
    , m_editIndex(editIndex)
{
    setWindowTitle(editIndex == -1
                   ? QString::fromUtf8("新建配置档案")
                   : QString::fromUtf8("编辑配置档案"));
    setFixedSize(500, 600);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    connect(m_apiClient, &ApiClient::apiKeysReceived,
            this, &ConnectWizardDialog::onApiKeysReceived);
    connect(m_apiClient, &ApiClient::modelsReceived,
            this, &ConnectWizardDialog::onModelsReceived);
    connect(m_apiClient, &ApiClient::requestFailed,
            this, &ConnectWizardDialog::onRequestFailed);

    setupUi();

    // Kick off key load
    m_apiClient->getApiKeys();

    // 编辑时回填名称和类型
    if (m_editIndex >= 0) {
        const auto profiles = m_profileManager->allProfiles();
        if (m_editIndex < profiles.size()) {
            m_nameEdit->setText(profiles[m_editIndex].name);
            m_selectedType = profiles[m_editIndex].type;
            const int typeId = static_cast<int>(m_selectedType);
            if (auto *btn = m_typeGroup->button(typeId))
                btn->setChecked(true);
            // buildPage2 尚未初始化，updateSectionVisibility 会在进入第2步时由
            // goNext() 末尾手动调用一次；此处只更新按钮状态即可
        }
    }
}

// ---------------------------------------------------------------------------
// setupUi
// ---------------------------------------------------------------------------

void ConnectWizardDialog::setupUi()
{
    setStyleSheet(QStringLiteral("QDialog { background: #ffffff; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Header ──────────────────────────────────────────────────────────────
    auto *header = new QFrame(this);
    header->setObjectName(QStringLiteral("wizardHeader"));
    header->setStyleSheet(QStringLiteral(
        "QFrame#wizardHeader {"
        "  background: qlineargradient(x1:0,y1:0,x2:1,y2:1,"
        "    stop:0 #6366f1, stop:1 #8b5cf6);"
        "  border: none;"
        "}"));
    header->setFixedHeight(64);

    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(24, 0, 24, 0);

    auto *titleLabel = new QLabel(
        m_editIndex == -1
            ? QString::fromUtf8("✨ 新建配置档案")
            : QString::fromUtf8("✏️ 编辑配置档案"),
        header);
    titleLabel->setStyleSheet(QStringLiteral(
        "color: white; font-size: 16px; font-weight: bold;"));

    m_stepLabel = new QLabel(QString::fromUtf8("第 1 步 / 共 3 步"), header);
    m_stepLabel->setStyleSheet(QStringLiteral(
        "color: rgba(255,255,255,0.8); font-size: 12px;"));
    m_stepLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();
    headerLayout->addWidget(m_stepLabel);
    root->addWidget(header);

    // ── Stacked pages ───────────────────────────────────────────────────────
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(buildPage1());
    m_stack->addWidget(buildPage2());
    m_stack->addWidget(buildPage3());
    root->addWidget(m_stack, 1);

    // ── Nav bar ─────────────────────────────────────────────────────────────
    auto *navBar = new QFrame(this);
    navBar->setStyleSheet(QStringLiteral(
        "QFrame { background: #f9fafb; border-top: 1px solid #e5e7eb; }"));
    navBar->setFixedHeight(60);

    auto *navLayout = new QHBoxLayout(navBar);
    navLayout->setContentsMargins(24, 12, 24, 12);

    m_backBtn = new QPushButton(QString::fromUtf8("← 上一步"), navBar);
    m_backBtn->setFixedSize(100, 36);
    m_backBtn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: #ffffff;"
        "  color: #374151;"
        "  border: 1px solid #d1d5db;"
        "  border-radius: 6px;"
        "  font-size: 13px;"
        "}"
        "QPushButton:hover { background: #f3f4f6; }"
        "QPushButton:disabled { color: #9ca3af; border-color: #e5e7eb; }"));

    m_nextBtn = new QPushButton(QString::fromUtf8("下一步 →"), navBar);
    m_nextBtn->setFixedSize(120, 36);
    m_nextBtn->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: qlineargradient(x1:0,y1:0,x2:1,y2:1,"
        "    stop:0 #6366f1, stop:1 #8b5cf6);"
        "  color: white;"
        "  border: none;"
        "  border-radius: 6px;"
        "  font-size: 13px;"
        "  font-weight: bold;"
        "}"
        "QPushButton:hover {"
        "  background: qlineargradient(x1:0,y1:0,x2:1,y2:1,"
        "    stop:0 #4f46e5, stop:1 #7c3aed);"
        "}"
        "QPushButton:disabled { background: #d1d5db; color: #9ca3af; }"));

    navLayout->addWidget(m_backBtn);
    navLayout->addStretch();
    navLayout->addWidget(m_nextBtn);
    root->addWidget(navBar);

    connect(m_backBtn, &QPushButton::clicked, this, &ConnectWizardDialog::goBack);
    connect(m_nextBtn, &QPushButton::clicked, this, &ConnectWizardDialog::goNext);

    updateNavButtons();
}

// ---------------------------------------------------------------------------
// buildPage1 — profile name
// ---------------------------------------------------------------------------

QWidget *ConnectWizardDialog::buildPage1()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(32, 40, 32, 32);
    layout->setSpacing(12);

    auto *iconLabel = new QLabel(QString::fromUtf8("📋"));
    iconLabel->setAlignment(Qt::AlignCenter);
    iconLabel->setStyleSheet(QStringLiteral("font-size: 48px;"));

    auto *headLabel = new QLabel(QString::fromUtf8("给档案起个名字"));
    headLabel->setAlignment(Qt::AlignCenter);
    headLabel->setStyleSheet(QStringLiteral(
        "font-size: 18px; font-weight: bold; color: #111827;"));

    auto *descLabel = new QLabel(
        QString::fromUtf8(
            "档案用于保存一组 AI 工具的 API Key 和模型配置，\n"
            "您可以随时切换不同的档案。"));
    descLabel->setAlignment(Qt::AlignCenter);
    descLabel->setWordWrap(true);
    descLabel->setStyleSheet(QStringLiteral(
        "font-size: 13px; color: #6b7280;"));

    m_nameEdit = new QLineEdit;
    m_nameEdit->setPlaceholderText(QString::fromUtf8("给这个档案起个名字..."));
    m_nameEdit->setFixedHeight(44);
    m_nameEdit->setStyleSheet(QStringLiteral(
        "QLineEdit {"
        "  border: 2px solid #e5e7eb;"
        "  border-radius: 8px;"
        "  padding: 0 16px;"
        "  font-size: 14px;"
        "  color: #111827;"
        "  background: #ffffff;"
        "}"
        "QLineEdit:focus { border-color: #6366f1; }"));

    // ── 类型选择 ──────────────────────────────────────────────────
    auto *typeLabel = new QLabel(QString::fromUtf8("配置类型"), page);
    typeLabel->setStyleSheet(QStringLiteral(
        "font-size: 11px; color: #6b7280; font-weight: 500; margin-top: 8px;"));

    auto *typePillRow = new QHBoxLayout;
    typePillRow->setSpacing(6);

    m_typeGroup = new QButtonGroup(this);
    m_typeGroup->setExclusive(true);

    const struct { int id; QString label; } kTypes[] = {
        { 0, QString::fromUtf8("混合") },
        { 1, QStringLiteral("Claude") },
        { 2, QStringLiteral("Codex") },
        { 3, QStringLiteral("Gemini") },
    };
    for (const auto &t : kTypes) {
        auto *btn = new QPushButton(t.label, page);
        btn->setCheckable(true);
        btn->setChecked(t.id == 0);
        btn->setFixedHeight(30);
        btn->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: #f3f4f6; color: #374151; border: 1.5px solid #d1d5db;"
            "  border-radius: 6px; font-size: 12px; padding: 0 12px; }"
            "QPushButton:checked {"
            "  background: #6366f1; color: white; border-color: #6366f1; }"
            "QPushButton:hover:!checked { background: #e5e7eb; }"));
        m_typeGroup->addButton(btn, t.id);
        typePillRow->addWidget(btn);
    }
    typePillRow->addStretch();

    connect(m_typeGroup, QOverload<int>::of(&QButtonGroup::idClicked),
            this, &ConnectWizardDialog::onTypeChanged);

    layout->addStretch();
    layout->addWidget(iconLabel);
    layout->addSpacing(8);
    layout->addWidget(headLabel);
    layout->addWidget(descLabel);
    layout->addSpacing(24);
    layout->addWidget(m_nameEdit);
    layout->addSpacing(12);
    layout->addWidget(typeLabel);
    layout->addLayout(typePillRow);
    layout->addStretch();

    return page;
}

// ---------------------------------------------------------------------------
// buildPage2 — tool configuration
// ---------------------------------------------------------------------------

QWidget *ConnectWizardDialog::buildPage2()
{
    auto *page = new QWidget;
    auto *outerLayout = new QVBoxLayout(page);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    // Title strip
    auto *titleWidget = new QWidget;
    auto *titleLayout = new QVBoxLayout(titleWidget);
    titleLayout->setContentsMargins(32, 20, 32, 10);
    titleLayout->setSpacing(2);

    auto *titleLabel = new QLabel(QString::fromUtf8("配置 AI 工具"));
    titleLabel->setStyleSheet(QStringLiteral(
        "font-size: 16px; font-weight: bold; color: #111827;"));
    auto *subLabel = new QLabel(
        QString::fromUtf8("为每个工具选择 API Key 并查询可用模型"));
    subLabel->setStyleSheet(QStringLiteral(
        "font-size: 12px; color: #6b7280;"));
    titleLayout->addWidget(titleLabel);
    titleLayout->addWidget(subLabel);
    outerLayout->addWidget(titleWidget);

    // Scroll area
    auto *scrollArea = new QScrollArea;
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setStyleSheet(QStringLiteral(
        "QScrollArea { background: #f9fafb; border: none; }"));

    auto *scrollContent = new QWidget;
    scrollContent->setStyleSheet(QStringLiteral("background: #f9fafb;"));
    auto *scrollLayout = new QVBoxLayout(scrollContent);
    scrollLayout->setContentsMargins(24, 8, 24, 24);
    scrollLayout->setSpacing(12);

    for (AiTool tool : { AiTool::ClaudeCode, AiTool::CodexCli, AiTool::GeminiCli }) {
        const QString accent = toolAccent(tool);

        ToolSection sec;
        sec.tool = tool;

        // Card
        auto *card = new QFrame;
        sec.card = card;   // 保存引用，供 updateSectionVisibility 显隐
        card->setStyleSheet(QStringLiteral(
            "QFrame {"
            "  background: #ffffff;"
            "  border: 1px solid #e5e7eb;"
            "  border-left: 4px solid %1;"
            "  border-radius: 8px;"
            "}").arg(accent));

        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(16, 14, 16, 14);
        cardLayout->setSpacing(10);

        // Title row with enable checkbox
        auto *titleRow = new QHBoxLayout;
        sec.enableCheck = new QCheckBox(toolLabel(tool));
        sec.enableCheck->setChecked(true);
        sec.enableCheck->setStyleSheet(QStringLiteral(
            "QCheckBox {"
            "  font-size: 14px; font-weight: bold; color: #111827; spacing: 8px;"
            "}"
            "QCheckBox::indicator { width: 16px; height: 16px; border-radius: 3px;"
            "  border: 2px solid #d1d5db; }"
            "QCheckBox::indicator:checked {"
            "  background: %1; border-color: %1;"
            "}").arg(accent));
        titleRow->addWidget(sec.enableCheck);
        titleRow->addStretch();
        cardLayout->addLayout(titleRow);

        // Key label
        auto *keyLabel = new QLabel(QString::fromUtf8("API Key"));
        keyLabel->setStyleSheet(QStringLiteral(
            "font-size: 11px; color: #6b7280; font-weight: 500;"));
        cardLayout->addWidget(keyLabel);

        // Key combo + query button
        auto *keyRow = new QHBoxLayout;
        keyRow->setSpacing(8);

        sec.keyCombo = new QComboBox;
        sec.keyCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        sec.keyCombo->setFixedHeight(34);
        sec.keyCombo->setStyleSheet(QStringLiteral(
            "QComboBox {"
            "  border: 1px solid #d1d5db; border-radius: 6px;"
            "  padding: 0 12px; font-size: 12px; color: #374151; background: #ffffff;"
            "}"
            "QComboBox:focus { border-color: #6366f1; }"
            "QComboBox::drop-down { border: none; width: 24px; }"));

        sec.queryButton = new QPushButton(QString::fromUtf8("🔄 查询模型"));
        sec.queryButton->setFixedHeight(34);
        sec.queryButton->setFixedWidth(100);
        sec.queryButton->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: %1; color: white; border: none;"
            "  border-radius: 6px; font-size: 12px; font-weight: bold; padding: 0 8px;"
            "}"
            "QPushButton:hover { background: %2; }"
            "QPushButton:disabled { background: #d1d5db; color: #9ca3af; }")
            .arg(accent)
            .arg(accent)); // hover same — slight opacity handled by Qt

        const AiTool capTool = tool;
        connect(sec.queryButton, &QPushButton::clicked,
                this, [this, capTool]() { onQueryModels(capTool); });

        keyRow->addWidget(sec.keyCombo);
        keyRow->addWidget(sec.queryButton);
        cardLayout->addLayout(keyRow);

        // Loading label
        sec.loadingLabel = new QLabel(QString::fromUtf8("⏳ 查询中..."));
        sec.loadingLabel->setStyleSheet(QStringLiteral(
            "font-size: 11px; color: #6b7280;"));
        sec.loadingLabel->setVisible(false);
        cardLayout->addWidget(sec.loadingLabel);

        // Model label
        auto *modelLabel = new QLabel(QString::fromUtf8("模型"));
        modelLabel->setStyleSheet(QStringLiteral(
            "font-size: 11px; color: #6b7280; font-weight: 500;"));
        cardLayout->addWidget(modelLabel);

        // Model combo
        sec.modelCombo = new QComboBox;
        sec.modelCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        sec.modelCombo->setFixedHeight(34);
        sec.modelCombo->addItem(QString::fromUtf8("— 请先查询模型 —"));
        sec.modelCombo->setStyleSheet(QStringLiteral(
            "QComboBox {"
            "  border: 1px solid #d1d5db; border-radius: 6px;"
            "  padding: 0 12px; font-size: 12px; color: #374151; background: #ffffff;"
            "}"
            "QComboBox:focus { border-color: #6366f1; }"
            "QComboBox::drop-down { border: none; width: 24px; }"));
        cardLayout->addWidget(sec.modelCombo);

        scrollLayout->addWidget(card);
        m_sections.append(sec);
    }

    scrollLayout->addStretch();
    scrollArea->setWidget(scrollContent);
    outerLayout->addWidget(scrollArea, 1);

    return page;
}

// ---------------------------------------------------------------------------
// buildPage3 — confirmation summary
// ---------------------------------------------------------------------------

QWidget *ConnectWizardDialog::buildPage3()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(32, 40, 32, 32);
    layout->setSpacing(16);

    auto *iconLabel = new QLabel(QString::fromUtf8("✅"));
    iconLabel->setAlignment(Qt::AlignCenter);
    iconLabel->setStyleSheet(QStringLiteral("font-size: 48px;"));

    auto *headLabel = new QLabel(QString::fromUtf8("确认配置摘要"));
    headLabel->setAlignment(Qt::AlignCenter);
    headLabel->setStyleSheet(QStringLiteral(
        "font-size: 18px; font-weight: bold; color: #111827;"));

    auto *summaryCard = new QFrame;
    summaryCard->setStyleSheet(QStringLiteral(
        "QFrame {"
        "  background: #f9fafb;"
        "  border: 1px solid #e5e7eb;"
        "  border-radius: 8px;"
        "}"));
    auto *summaryInner = new QVBoxLayout(summaryCard);
    summaryInner->setContentsMargins(16, 16, 16, 16);

    m_summaryLabel = new QLabel;
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_summaryLabel->setTextFormat(Qt::RichText);
    m_summaryLabel->setStyleSheet(QStringLiteral(
        "font-size: 13px; color: #374151; background: transparent;"));
    summaryInner->addWidget(m_summaryLabel);

    layout->addStretch();
    layout->addWidget(iconLabel);
    layout->addWidget(headLabel);
    layout->addSpacing(8);
    layout->addWidget(summaryCard);
    layout->addStretch();

    return page;
}

// ---------------------------------------------------------------------------
// refreshPage3
// ---------------------------------------------------------------------------

void ConnectWizardDialog::refreshPage3()
{
    const QString name = m_nameEdit->text().trimmed();
    QString html;
    html += QStringLiteral("<b>") + QString::fromUtf8("📋 档案名称：") + QStringLiteral("</b>")
            + name.toHtmlEscaped() + QStringLiteral("<br><br>");

    for (const ToolSection &sec : m_sections) {
        if (!sec.enableCheck->isChecked())
            continue;

        const QString key = currentKey(sec);
        const QString keyDisplay = key.isEmpty()
            ? QStringLiteral("<i>") + QString::fromUtf8("未选择") + QStringLiteral("</i>")
            : key.left(8) + QStringLiteral("****");

        const bool hasModel = sec.modelCombo->count() > 0
                              && sec.modelCombo->currentIndex() >= 0
                              && !sec.modelCombo->currentText().startsWith(
                                     QString::fromUtf8("—"));
        const QString model = hasModel
            ? sec.modelCombo->currentText().toHtmlEscaped()
            : QStringLiteral("<i>") + QString::fromUtf8("未选择") + QStringLiteral("</i>");

        html += QStringLiteral("<b>") + toolLabel(sec.tool).toHtmlEscaped()
                + QStringLiteral("</b><br>");
        html += QString::fromUtf8("　Key：") + keyDisplay + QStringLiteral("<br>");
        html += QString::fromUtf8("　模型：") + model + QStringLiteral("<br><br>");
    }

    m_summaryLabel->setText(html);
}

// ---------------------------------------------------------------------------
// populateKeyDropdowns
// ---------------------------------------------------------------------------

void ConnectWizardDialog::populateKeyDropdowns()
{
    for (ToolSection &sec : m_sections) {
        const QString platform = ToolManager::toolPlatform(sec.tool);
        sec.keyCombo->clear();
        sec.keyCombo->addItem(QString::fromUtf8("— 请选择 Key —"), QString());

        for (const QJsonValue &val : m_allKeys) {
            const QJsonObject obj   = val.toObject();
            const QJsonObject group = obj[QStringLiteral("group")].toObject();
            if (group[QStringLiteral("platform")].toString() != platform)
                continue;

            const QString displayName = obj[QStringLiteral("name")].toString();
            const QString keyValue    = obj[QStringLiteral("key")].toString();
            sec.keyCombo->addItem(displayName, keyValue);
        }
    }

    // Pre-select when editing
    if (m_editIndex >= 0) {
        const auto profiles = m_profileManager->allProfiles();
        if (m_editIndex < profiles.size()) {
            const auto &profile = profiles[m_editIndex];
            for (ToolSection &sec : m_sections) {
                QString existingKey;
                switch (sec.tool) {
                case AiTool::ClaudeCode: existingKey = profile.claudeKey; break;
                case AiTool::CodexCli:   existingKey = profile.codexKey;  break;
                case AiTool::GeminiCli:  existingKey = profile.geminiKey; break;
                }
                if (existingKey.isEmpty()) continue;
                for (int i = 1; i < sec.keyCombo->count(); ++i) {
                    if (sec.keyCombo->itemData(i, Qt::UserRole).toString() == existingKey) {
                        sec.keyCombo->setCurrentIndex(i);
                        break;
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void ConnectWizardDialog::onApiKeysReceived(const QJsonArray &keys)
{
    m_allKeys = keys;
    populateKeyDropdowns();
}

void ConnectWizardDialog::onQueryModels(AiTool tool)
{
    ToolSection *sec = nullptr;
    for (ToolSection &s : m_sections) {
        if (s.tool == tool) { sec = &s; break; }
    }
    if (!sec) return;

    const QString key = currentKey(*sec);
    if (key.isEmpty()) {
        QMessageBox::warning(this,
            QString::fromUtf8("提示"),
            QString::fromUtf8("请先选择一个 API Key。"));
        return;
    }

    m_waitingModels = true;
    m_queryingTool  = tool;
    setPage2Loading(tool, true);
    m_apiClient->getModels(key);
}

void ConnectWizardDialog::onModelsReceived(const QJsonArray &models)
{
    if (!m_waitingModels) return;

    ToolSection *sec = nullptr;
    for (ToolSection &s : m_sections) {
        if (s.tool == m_queryingTool) { sec = &s; break; }
    }

    m_waitingModels = false;

    if (!sec) return;

    sec->modelCombo->clear();
    if (models.isEmpty()) {
        sec->modelCombo->addItem(QString::fromUtf8("— 无可用模型 —"));
    } else {
        for (const QJsonValue &val : models) {
            QString modelId;
            if (val.isObject())
                modelId = val.toObject()[QStringLiteral("id")].toString();
            if (modelId.isEmpty())
                modelId = val.toString();
            if (!modelId.isEmpty())
                sec->modelCombo->addItem(modelId);
        }
    }

    setPage2Loading(m_queryingTool, false);
}

void ConnectWizardDialog::onRequestFailed(const QString &error)
{
    if (!m_waitingModels) return;

    m_waitingModels = false;

    ToolSection *sec = nullptr;
    for (ToolSection &s : m_sections) {
        if (s.tool == m_queryingTool) { sec = &s; break; }
    }
    if (!sec) return;

    sec->queryButton->setEnabled(true);
    sec->loadingLabel->setText(
        QString::fromUtf8("❌ 查询失败：") + error);
    sec->loadingLabel->setVisible(true);
}

// ---------------------------------------------------------------------------
// 类型切换
// ---------------------------------------------------------------------------

void ConnectWizardDialog::onTypeChanged(int id)
{
    m_selectedType = static_cast<ProfileType>(id);
    updateSectionVisibility();
}

void ConnectWizardDialog::updateSectionVisibility()
{
    const QList<AiTool> visible = toolsForType(m_selectedType);
    for (ToolSection &s : m_sections) {
        if (s.card) {
            s.card->setVisible(visible.contains(s.tool));
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void ConnectWizardDialog::setPage2Loading(AiTool tool, bool loading)
{
    for (ToolSection &s : m_sections) {
        if (s.tool != tool) continue;
        s.loadingLabel->setText(QString::fromUtf8("⏳ 查询中..."));
        s.loadingLabel->setVisible(loading);
        s.queryButton->setEnabled(!loading);
        break;
    }
}

void ConnectWizardDialog::updateNavButtons()
{
    const int idx = m_stack->currentIndex();
    m_backBtn->setEnabled(idx > 0);
    m_nextBtn->setText(idx == 2
                       ? QString::fromUtf8("✅ 完成")
                       : QString::fromUtf8("下一步 →"));
    m_stepLabel->setText(
        QString::fromUtf8("第 %1 步 / 共 3 步").arg(idx + 1));
}

QString ConnectWizardDialog::currentKey(const ToolSection &s) const
{
    return s.keyCombo->currentData(Qt::UserRole).toString();
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

void ConnectWizardDialog::goNext()
{
    const int idx = m_stack->currentIndex();

    if (idx == 0) {
        if (m_nameEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this,
                QString::fromUtf8("提示"),
                QString::fromUtf8("档案名称不能为空，请输入一个名称。"));
            m_nameEdit->setFocus();
            return;
        }
        m_stack->setCurrentIndex(1);
        updateSectionVisibility();  // 按当前类型显隐工具 section
        updateNavButtons();

    } else if (idx == 1) {
        refreshPage3();
        m_stack->setCurrentIndex(2);
        updateNavButtons();

    } else {
        finish();
    }
}

void ConnectWizardDialog::goBack()
{
    const int idx = m_stack->currentIndex();
    if (idx > 0) {
        m_stack->setCurrentIndex(idx - 1);
        updateNavButtons();
    }
}

void ConnectWizardDialog::finish()
{
    const QString name = m_nameEdit->text().trimmed();
    int profileIdx;

    if (m_editIndex == -1) {
        profileIdx = m_profileManager->addProfile(name, m_selectedType);
    } else {
        profileIdx = m_editIndex;
        m_profileManager->renameProfile(profileIdx, name);
        m_profileManager->setProfileType(profileIdx, m_selectedType);
    }

    for (const ToolSection &sec : m_sections) {
        if (!sec.enableCheck->isChecked()) continue;
        if (sec.card && !sec.card->isVisible()) continue;  // 类型过滤掉的工具跳过

        const QString key = currentKey(sec);
        if (key.isEmpty()) continue;

        const bool hasModel = sec.modelCombo->count() > 0
                              && sec.modelCombo->currentIndex() >= 0
                              && !sec.modelCombo->currentText().startsWith(
                                     QString::fromUtf8("—"));
        const QString model = hasModel ? sec.modelCombo->currentText() : QString();

        m_profileManager->saveToolConfig(profileIdx, sec.tool, key, model);
    }

    m_resultIndex = profileIdx;
    accept();
}
