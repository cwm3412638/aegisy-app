#include "desktop_enhancement_dialog.h"

#include "app_theme.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <algorithm>

namespace {

QFrame *sectionFrame(QWidget *parent)
{
    auto *frame = new QFrame(parent);
    frame->setStyleSheet(QStringLiteral(
        "QFrame { background: white; border: 1px solid #e4e7ec; border-radius: 8px; }"
        "QLabel { border: none; background: transparent; }"));
    return frame;
}

QLabel *mutedLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color: #667085; font-size: 12px;"));
    return label;
}

} // namespace

DesktopEnhancementDialog::DesktopEnhancementDialog(DesktopEnhancementManager *manager,
                                                     QWidget *parent)
    : QDialog(parent)
    , m_manager(manager)
{
    setWindowTitle(QStringLiteral("桌面增强"));
    resize(820, 610);
    setMinimumSize(720, 520);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(14);

    auto *title = new QLabel(QStringLiteral("桌面客户端与 Codex 能力"), this);
    title->setStyleSheet(QStringLiteral("font-size: 20px; font-weight: 700; color: #101828;"));
    layout->addWidget(title);
    layout->addWidget(mutedLabel(
        QStringLiteral("使用官方插件接口、可回滚会话同步和仅驻留内存的 Claude 汉化。所有高权限操作都需要确认。"),
        this));

    auto *tabs = new QTabWidget(this);
    layout->addWidget(tabs, 1);

    auto *catalogPage = new QWidget(tabs);
    auto *catalogLayout = new QVBoxLayout(catalogPage);
    catalogLayout->setContentsMargins(12, 14, 12, 12);
    catalogLayout->setSpacing(12);

    auto *modelFrame = sectionFrame(catalogPage);
    auto *modelLayout = new QHBoxLayout(modelFrame);
    modelLayout->setContentsMargins(14, 12, 14, 12);
    auto *modelText = new QVBoxLayout;
    auto *modelTitle = new QLabel(QStringLiteral("全量模型列表"), modelFrame);
    modelTitle->setStyleSheet(QStringLiteral("font-weight: 700; color: #101828;"));
    modelText->addWidget(modelTitle);
    modelText->addWidget(mutedLabel(
        QStringLiteral("直接展示当前 Aegisy API Key 返回的全部模型，不使用桌面客户端内置白名单过滤。"),
        modelFrame));
    modelLayout->addLayout(modelText, 1);
    auto *modelsButton = new QPushButton(QStringLiteral("查看模型"), modelFrame);
    modelsButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    modelLayout->addWidget(modelsButton);
    connect(modelsButton, &QPushButton::clicked, this, &DesktopEnhancementDialog::openModelsRequested);
    catalogLayout->addWidget(modelFrame);

    auto *pluginToolbar = new QHBoxLayout;
    m_pluginSearch = new QLineEdit(catalogPage);
    m_pluginSearch->setPlaceholderText(QStringLiteral("搜索插件或市场..."));
    m_pluginSearch->setClearButtonEnabled(true);
    pluginToolbar->addWidget(m_pluginSearch, 1);
    auto *refreshButton = new QPushButton(QStringLiteral("刷新列表"), catalogPage);
    refreshButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    pluginToolbar->addWidget(refreshButton);
    catalogLayout->addLayout(pluginToolbar);

    m_pluginTable = new QTableWidget(catalogPage);
    m_pluginTable->setColumnCount(4);
    m_pluginTable->setHorizontalHeaderLabels({ QStringLiteral("插件"), QStringLiteral("市场"),
                                                QStringLiteral("版本"), QStringLiteral("状态") });
    m_pluginTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_pluginTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_pluginTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_pluginTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_pluginTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pluginTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pluginTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pluginTable->verticalHeader()->setVisible(false);
    m_pluginTable->setAlternatingRowColors(true);
    catalogLayout->addWidget(m_pluginTable, 1);

    auto *pluginFooter = new QHBoxLayout;
    m_pluginStatus = mutedLabel(QStringLiteral("正在读取 Codex 插件目录..."), catalogPage);
    pluginFooter->addWidget(m_pluginStatus, 1);
    m_installPluginButton = new QPushButton(QStringLiteral("安装所选插件"), catalogPage);
    m_installPluginButton->setEnabled(false);
    m_installPluginButton->setStyleSheet(AppTheme::primaryButtonStyle());
    pluginFooter->addWidget(m_installPluginButton);
    catalogLayout->addLayout(pluginFooter);
    tabs->addTab(catalogPage, QStringLiteral("模型与插件"));

    auto *automationPage = new QWidget(tabs);
    auto *automationLayout = new QVBoxLayout(automationPage);
    automationLayout->setContentsMargins(12, 14, 12, 12);
    automationLayout->setSpacing(14);

    auto *historyFrame = sectionFrame(automationPage);
    auto *historyLayout = new QVBoxLayout(historyFrame);
    historyLayout->setContentsMargins(16, 14, 16, 14);
    auto *historyTitle = new QLabel(QStringLiteral("同步 Codex 历史会话"), historyFrame);
    historyTitle->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 700; color: #101828;"));
    historyLayout->addWidget(historyTitle);
    historyLayout->addWidget(mutedLabel(
        QStringLiteral("将本地会话元数据同步到当前 model_provider，并修复桌面端会话索引。操作前会备份 JSONL 和 SQLite。"),
        historyFrame));
    auto *historyActions = new QHBoxLayout;
    m_historyStatus = mutedLabel(QStringLiteral("尚未同步"), historyFrame);
    historyActions->addWidget(m_historyStatus, 1);
    auto *syncButton = new QPushButton(QStringLiteral("立即同步"), historyFrame);
    syncButton->setStyleSheet(AppTheme::primaryButtonStyle());
    historyActions->addWidget(syncButton);
    historyLayout->addLayout(historyActions);
    automationLayout->addWidget(historyFrame);

    auto *computerFrame = sectionFrame(automationPage);
    auto *computerLayout = new QVBoxLayout(computerFrame);
    computerLayout->setContentsMargins(16, 14, 16, 14);
    auto *computerTitle = new QLabel(QStringLiteral("Computer Use 电脑控制"), computerFrame);
    computerTitle->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 700; color: #101828;"));
    computerLayout->addWidget(computerTitle);
    computerLayout->addWidget(mutedLabel(
        QStringLiteral("通过 Codex 官方 openai-bundled 市场安装 Computer Use 插件。实际控制仍遵循 Codex 的授权、沙箱与确认策略。"),
        computerFrame));
    auto *computerActions = new QHBoxLayout;
    computerActions->addStretch();
    m_computerUseButton = new QPushButton(QStringLiteral("安装 Computer Use"), computerFrame);
    m_computerUseButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    computerActions->addWidget(m_computerUseButton);
    computerLayout->addLayout(computerActions);
    automationLayout->addWidget(computerFrame);
    automationLayout->addStretch();
    tabs->addTab(automationPage, QStringLiteral("会话与控制"));

    auto *claudePage = new QWidget(tabs);
    auto *claudeLayout = new QVBoxLayout(claudePage);
    claudeLayout->setContentsMargins(12, 14, 12, 12);
    auto *claudeFrame = sectionFrame(claudePage);
    auto *claudeFrameLayout = new QVBoxLayout(claudeFrame);
    claudeFrameLayout->setContentsMargins(16, 14, 16, 14);
    auto *claudeTitle = new QLabel(QStringLiteral("Claude Desktop 中文界面"), claudeFrame);
    claudeTitle->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 700; color: #101828;"));
    claudeFrameLayout->addWidget(claudeTitle);
    claudeFrameLayout->addWidget(mutedLabel(
        QStringLiteral("启动 Claude 的本机调试端口并把中文词典注入内存，不修改 Claude 安装文件。关闭 Claude 后注入自动失效。"),
        claudeFrame));
    m_claudeStatus = mutedLabel(QStringLiteral("支持 Windows 与 macOS"), claudeFrame);
    claudeFrameLayout->addWidget(m_claudeStatus);
    auto *claudeActions = new QHBoxLayout;
    claudeActions->addStretch();
    m_localizeClaudeButton = new QPushButton(QStringLiteral("汉化并启动 Claude"), claudeFrame);
    m_localizeClaudeButton->setStyleSheet(AppTheme::primaryButtonStyle());
    claudeActions->addWidget(m_localizeClaudeButton);
    claudeFrameLayout->addLayout(claudeActions);
    claudeLayout->addWidget(claudeFrame);
    claudeLayout->addStretch();
    tabs->addTab(claudePage, QStringLiteral("Claude 汉化"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(refreshButton, &QPushButton::clicked, this, &DesktopEnhancementDialog::refreshPlugins);
    connect(m_pluginSearch, &QLineEdit::textChanged, this, &DesktopEnhancementDialog::filterPlugins);
    connect(m_pluginTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        const QString id = selectedPluginId();
        bool installable = false;
        for (const CodexPluginInfo &plugin : m_plugins) {
            if (plugin.id == id) installable = !plugin.installed;
        }
        m_installPluginButton->setEnabled(installable);
    });
    connect(m_installPluginButton, &QPushButton::clicked,
            this, &DesktopEnhancementDialog::installSelectedPlugin);
    connect(m_computerUseButton, &QPushButton::clicked,
            this, &DesktopEnhancementDialog::installComputerUse);
    connect(syncButton, &QPushButton::clicked, this, &DesktopEnhancementDialog::syncHistory);
    connect(m_localizeClaudeButton, &QPushButton::clicked,
            this, &DesktopEnhancementDialog::localizeClaude);
    connect(m_manager, &DesktopEnhancementManager::localizationProgress,
            this, &DesktopEnhancementDialog::onLocalizationProgress);
    connect(m_manager, &DesktopEnhancementManager::localizationFinished,
            this, &DesktopEnhancementDialog::onLocalizationFinished);

    QTimer::singleShot(0, this, &DesktopEnhancementDialog::refreshPlugins);
}

void DesktopEnhancementDialog::refreshPlugins()
{
    m_pluginStatus->setText(QStringLiteral("正在读取 Codex 官方插件列表..."));
    QString error;
    m_plugins = m_manager->listCodexPlugins(&error);
    if (!error.isEmpty()) {
        m_pluginStatus->setText(error);
        m_pluginStatus->setStyleSheet(QStringLiteral("color: #b42318; font-size: 12px;"));
    } else {
        const int installed = std::count_if(m_plugins.cbegin(), m_plugins.cend(),
            [](const CodexPluginInfo &plugin) { return plugin.installed; });
        m_pluginStatus->setText(QStringLiteral("共 %1 个插件，已安装 %2 个")
            .arg(m_plugins.size()).arg(installed));
        m_pluginStatus->setStyleSheet(QStringLiteral("color: #667085; font-size: 12px;"));
    }
    rebuildPluginTable();
}

void DesktopEnhancementDialog::filterPlugins()
{
    rebuildPluginTable();
}

void DesktopEnhancementDialog::rebuildPluginTable()
{
    const QString search = m_pluginSearch->text().trimmed();
    m_pluginTable->setRowCount(0);
    for (const CodexPluginInfo &plugin : m_plugins) {
        if (!search.isEmpty()
                && !plugin.name.contains(search, Qt::CaseInsensitive)
                && !plugin.marketplace.contains(search, Qt::CaseInsensitive)) {
            continue;
        }
        const int row = m_pluginTable->rowCount();
        m_pluginTable->insertRow(row);
        auto *name = new QTableWidgetItem(plugin.name);
        name->setData(Qt::UserRole, plugin.id);
        name->setToolTip(plugin.path);
        m_pluginTable->setItem(row, 0, name);
        m_pluginTable->setItem(row, 1, new QTableWidgetItem(plugin.marketplace));
        m_pluginTable->setItem(row, 2, new QTableWidgetItem(
            plugin.version.isEmpty() ? QStringLiteral("-") : plugin.version));
        m_pluginTable->setItem(row, 3, new QTableWidgetItem(
            plugin.installed ? (plugin.enabled ? QStringLiteral("已启用") : QStringLiteral("已安装"))
                             : QStringLiteral("可安装")));
    }
    m_installPluginButton->setEnabled(false);
}

QString DesktopEnhancementDialog::selectedPluginId() const
{
    const int row = m_pluginTable->currentRow();
    if (row < 0 || !m_pluginTable->item(row, 0)) return QString();
    return m_pluginTable->item(row, 0)->data(Qt::UserRole).toString();
}

void DesktopEnhancementDialog::installSelectedPlugin()
{
    const QString pluginId = selectedPluginId();
    if (pluginId.isEmpty()) return;
    if (QMessageBox::question(this, QStringLiteral("安装 Codex 插件"),
            QStringLiteral("将通过 Codex 官方插件命令安装：\n%1\n\n插件首次使用时仍可能请求额外授权。")
                .arg(pluginId), QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) return;

    m_installPluginButton->setEnabled(false);
    m_pluginStatus->setText(QStringLiteral("正在安装 %1...").arg(pluginId));
    QString output;
    QString error;
    if (!m_manager->installCodexPlugin(pluginId, &output, &error)) {
        QMessageBox::critical(this, QStringLiteral("安装失败"), error);
    } else {
        QMessageBox::information(this, QStringLiteral("安装完成"),
                                 QStringLiteral("插件 %1 已安装。重启 Codex 后生效。")
                                     .arg(pluginId));
    }
    refreshPlugins();
}

void DesktopEnhancementDialog::installComputerUse()
{
    for (const CodexPluginInfo &plugin : m_plugins) {
        if (plugin.id == QStringLiteral("computer-use@openai-bundled") && plugin.installed) {
            QMessageBox::information(this, QStringLiteral("Computer Use"),
                                     QStringLiteral("Computer Use 插件已经安装。"));
            return;
        }
    }
    const QString pluginId = QStringLiteral("computer-use@openai-bundled");
    if (QMessageBox::question(this, QStringLiteral("安装 Computer Use"),
            QStringLiteral("将通过 Codex 官方插件市场安装 Computer Use。\n"
                           "插件首次运行仍会按 Codex 策略请求电脑控制授权。是否继续？"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) return;
    m_computerUseButton->setEnabled(false);
    m_pluginStatus->setText(QStringLiteral("正在安装 Computer Use..."));
    QString output;
    QString error;
    if (!m_manager->installCodexPlugin(pluginId, &output, &error)) {
        QMessageBox::critical(this, QStringLiteral("安装失败"), error);
    } else {
        QMessageBox::information(this, QStringLiteral("安装完成"),
            QStringLiteral("Computer Use 已安装。重启 Codex 后即可使用。"));
    }
    m_computerUseButton->setEnabled(true);
    refreshPlugins();
}

void DesktopEnhancementDialog::syncHistory()
{
    if (QMessageBox::question(this, QStringLiteral("同步历史会话"),
            QStringLiteral("Aegisy 将备份并更新 ~/.codex 中的会话元数据和桌面索引。\n"
                           "同步时建议先关闭 Codex Desktop。是否继续？"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) return;

    m_historyStatus->setText(QStringLiteral("正在同步..."));
    QString error;
    const SessionSyncReport report = m_manager->syncCodexHistory(&error);
    if (!error.isEmpty()) {
        m_historyStatus->setText(QStringLiteral("同步失败"));
        QMessageBox::critical(this, QStringLiteral("同步失败"), error);
        return;
    }
    m_historyStatus->setText(QStringLiteral("已同步到 %1：%2 个会话文件，%3 条索引")
        .arg(report.provider).arg(report.sessionFilesChanged).arg(report.databaseRowsChanged));
    QMessageBox::information(this, QStringLiteral("同步完成"),
        QStringLiteral("已更新 %1 个会话文件和 %2 条桌面索引。\n备份位置：%3")
            .arg(report.sessionFilesChanged).arg(report.databaseRowsChanged, 0, 10)
            .arg(report.backupPath));
}

void DesktopEnhancementDialog::localizeClaude()
{
    if (QMessageBox::question(this, QStringLiteral("Claude Desktop 运行时汉化"),
            QStringLiteral("将以调试模式启动 Claude，并向其内存注入中文词典。\n"
                           "不会修改安装文件；关闭 Claude 后效果失效。是否继续？"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) return;
    m_localizeClaudeButton->setEnabled(false);
    m_manager->localizeClaudeDesktop();
}

void DesktopEnhancementDialog::onLocalizationProgress(const QString &message)
{
    m_claudeStatus->setText(message);
    m_claudeStatus->setStyleSheet(QStringLiteral("color: #175cd3; font-size: 12px;"));
}

void DesktopEnhancementDialog::onLocalizationFinished(bool success, const QString &message)
{
    m_localizeClaudeButton->setEnabled(true);
    m_claudeStatus->setText(message);
    m_claudeStatus->setStyleSheet(success
        ? QStringLiteral("color: #067647; font-size: 12px;")
        : QStringLiteral("color: #b42318; font-size: 12px;"));
    if (!success) QMessageBox::warning(this, QStringLiteral("Claude 汉化未完成"), message);
}
