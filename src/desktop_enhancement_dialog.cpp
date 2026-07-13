#include "desktop_enhancement_dialog.h"

#include "app_theme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProgressDialog>
#include <QScreen>
#include <QStyle>
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

void sizeScrollableDialog(QDialog *dialog, QWidget *parent)
{
    const QScreen *screen = parent ? parent->screen() : QGuiApplication::primaryScreen();
    const QSize available = screen ? screen->availableGeometry().size() : QSize(900, 700);
    dialog->resize(qMax(520, qMin(720, available.width() - 80)),
                   qMax(420, qMin(560, available.height() - 100)));
    dialog->setMinimumSize(500, 380);
}

bool confirmPluginInstallation(QWidget *parent,
                               const QList<CodexPluginInfo> &plugins)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("安装 Codex 插件"));
    dialog.setWindowFlags(dialog.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    sizeScrollableDialog(&dialog, parent);

    auto *root = new QVBoxLayout(&dialog);
    root->setContentsMargins(20, 18, 20, 16);
    root->setSpacing(12);

    auto *header = new QHBoxLayout;
    auto *icon = new QLabel(&dialog);
    icon->setPixmap(dialog.style()->standardIcon(QStyle::SP_MessageBoxQuestion)
                        .pixmap(36, 36));
    icon->setFixedSize(42, 42);
    icon->setAlignment(Qt::AlignCenter);
    header->addWidget(icon, 0, Qt::AlignTop);
    auto *titleColumn = new QVBoxLayout;
    auto *title = new QLabel(
        QStringLiteral("安装 %1 个已选插件？").arg(plugins.size()), &dialog);
    title->setStyleSheet(QStringLiteral(
        "font-size: 17px; font-weight: 700; color: #101828;"));
    titleColumn->addWidget(title);
    titleColumn->addWidget(mutedLabel(
        QStringLiteral("插件将按列表顺序安装，安装过程中可以停止后续任务。"), &dialog));
    header->addLayout(titleColumn, 1);
    root->addLayout(header);

    auto *table = new QTableWidget(&dialog);
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({ QStringLiteral("插件"), QStringLiteral("功能说明") });
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->verticalHeader()->setVisible(false);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setWordWrap(true);
    table->setRowCount(plugins.size());
    for (int row = 0; row < plugins.size(); ++row) {
        const CodexPluginInfo &plugin = plugins.at(row);
        auto *name = new QTableWidgetItem(plugin.name);
        name->setToolTip(plugin.id);
        table->setItem(row, 0, name);
        auto *description = new QTableWidgetItem(plugin.description);
        description->setToolTip(plugin.officialDescription.isEmpty()
            ? plugin.description : plugin.officialDescription);
        table->setItem(row, 1, description);
        table->setRowHeight(row, 46);
    }
    root->addWidget(table, 1);

    auto *notice = mutedLabel(
        QStringLiteral("部分插件首次安装或使用时会根据 Codex 策略请求额外授权。"), &dialog);
    root->addWidget(notice);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    buttons->button(QDialogButtonBox::Cancel)->setStyleSheet(AppTheme::secondaryButtonStyle());
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("开始安装"));
    buttons->button(QDialogButtonBox::Ok)->setStyleSheet(AppTheme::primaryButtonStyle());
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    root->addWidget(buttons);
    return dialog.exec() == QDialog::Accepted;
}

void showBatchInstallResult(QWidget *parent, const QString &result)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("批量安装结果"));
    dialog.setWindowFlags(dialog.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    sizeScrollableDialog(&dialog, parent);

    auto *root = new QVBoxLayout(&dialog);
    root->setContentsMargins(20, 18, 20, 16);
    root->setSpacing(12);
    auto *title = new QLabel(QStringLiteral("插件安装已结束"), &dialog);
    title->setStyleSheet(QStringLiteral(
        "font-size: 17px; font-weight: 700; color: #101828;"));
    root->addWidget(title);
    auto *details = new QPlainTextEdit(&dialog);
    details->setReadOnly(true);
    details->setPlainText(result);
    details->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    root->addWidget(details, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    buttons->button(QDialogButtonBox::Close)->setStyleSheet(AppTheme::primaryButtonStyle());
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    root->addWidget(buttons);
    dialog.exec();
}

} // namespace

DesktopEnhancementDialog::DesktopEnhancementDialog(DesktopEnhancementManager *manager,
                                                     QWidget *parent)
    : QDialog(parent)
    , m_manager(manager)
{
    setWindowTitle(QStringLiteral("桌面增强"));
    resize(980, 680);
    setMinimumSize(820, 560);

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
    auto *selectAllButton = new QPushButton(QStringLiteral("全选可安装"), catalogPage);
    selectAllButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    pluginToolbar->addWidget(selectAllButton);
    auto *clearSelectionButton = new QPushButton(QStringLiteral("清空选择"), catalogPage);
    clearSelectionButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    pluginToolbar->addWidget(clearSelectionButton);
    auto *refreshButton = new QPushButton(QStringLiteral("刷新列表"), catalogPage);
    refreshButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    pluginToolbar->addWidget(refreshButton);
    catalogLayout->addLayout(pluginToolbar);

    m_pluginTable = new QTableWidget(catalogPage);
    m_pluginTable->setColumnCount(6);
    m_pluginTable->setHorizontalHeaderLabels({ QStringLiteral("选择"), QStringLiteral("插件"),
        QStringLiteral("功能说明"), QStringLiteral("市场"), QStringLiteral("版本"),
        QStringLiteral("状态") });
    m_pluginTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_pluginTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_pluginTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_pluginTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_pluginTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_pluginTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_pluginTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pluginTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pluginTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pluginTable->verticalHeader()->setVisible(false);
    m_pluginTable->setAlternatingRowColors(true);
    catalogLayout->addWidget(m_pluginTable, 1);

    auto *detailFrame = sectionFrame(catalogPage);
    auto *detailLayout = new QVBoxLayout(detailFrame);
    detailLayout->setContentsMargins(12, 10, 12, 10);
    m_pluginDetails = mutedLabel(
        QStringLiteral("选择一行可查看插件的完整功能说明。勾选多个未安装插件后可批量安装。"),
        detailFrame);
    m_pluginDetails->setMinimumHeight(38);
    detailLayout->addWidget(m_pluginDetails);
    catalogLayout->addWidget(detailFrame);

    auto *pluginFooter = new QHBoxLayout;
    m_pluginStatus = mutedLabel(QStringLiteral("正在读取 Codex 插件目录..."), catalogPage);
    pluginFooter->addWidget(m_pluginStatus, 1);
    m_installPluginButton = new QPushButton(QStringLiteral("安装已选插件"), catalogPage);
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
    connect(selectAllButton, &QPushButton::clicked, this, [this]() {
        for (const CodexPluginInfo &plugin : m_plugins) {
            if (!plugin.installed) m_checkedPluginIds.insert(plugin.id);
        }
        rebuildPluginTable();
    });
    connect(clearSelectionButton, &QPushButton::clicked, this, [this]() {
        m_checkedPluginIds.clear();
        rebuildPluginTable();
    });
    connect(m_pluginTable, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem *item) {
        if (!item || item->column() != 0) return;
        const QString id = item->data(Qt::UserRole).toString();
        if (item->checkState() == Qt::Checked) m_checkedPluginIds.insert(id);
        else m_checkedPluginIds.remove(id);
        updateInstallButton();
    });
    connect(m_pluginTable, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) { showPluginDetails(row); });
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
    m_pluginTable->blockSignals(true);
    m_pluginTable->setRowCount(0);
    for (const CodexPluginInfo &plugin : m_plugins) {
        if (!search.isEmpty()
                && !plugin.name.contains(search, Qt::CaseInsensitive)
                && !plugin.marketplace.contains(search, Qt::CaseInsensitive)
                && !plugin.description.contains(search, Qt::CaseInsensitive)) {
            continue;
        }
        const int row = m_pluginTable->rowCount();
        m_pluginTable->insertRow(row);

        auto *check = new QTableWidgetItem;
        check->setData(Qt::UserRole, plugin.id);
        check->setTextAlignment(Qt::AlignCenter);
        if (plugin.installed) {
            check->setFlags(Qt::ItemIsEnabled);
            check->setToolTip(QStringLiteral("该插件已经安装"));
        } else {
            check->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            check->setCheckState(m_checkedPluginIds.contains(plugin.id)
                ? Qt::Checked : Qt::Unchecked);
        }
        m_pluginTable->setItem(row, 0, check);

        auto *name = new QTableWidgetItem(plugin.name);
        name->setData(Qt::UserRole, plugin.id);
        name->setToolTip(plugin.path);
        m_pluginTable->setItem(row, 1, name);
        auto *description = new QTableWidgetItem(plugin.description);
        description->setToolTip(plugin.officialDescription.isEmpty()
            ? plugin.description
            : QStringLiteral("%1\n\n官方说明：%2")
                .arg(plugin.description, plugin.officialDescription));
        m_pluginTable->setItem(row, 2, description);
        m_pluginTable->setItem(row, 3, new QTableWidgetItem(plugin.marketplace));
        m_pluginTable->setItem(row, 4, new QTableWidgetItem(
            plugin.version.isEmpty() ? QStringLiteral("-") : plugin.version));
        m_pluginTable->setItem(row, 5, new QTableWidgetItem(
            plugin.installed ? (plugin.enabled ? QStringLiteral("已启用") : QStringLiteral("已安装"))
                             : QStringLiteral("可安装")));
        m_pluginTable->setRowHeight(row, 44);
    }
    m_pluginTable->blockSignals(false);
    updateInstallButton();
}

QStringList DesktopEnhancementDialog::selectedPluginIds() const
{
    QStringList ids;
    for (const CodexPluginInfo &plugin : m_plugins) {
        if (!plugin.installed && m_checkedPluginIds.contains(plugin.id)) ids.append(plugin.id);
    }
    return ids;
}

void DesktopEnhancementDialog::updateInstallButton()
{
    const int count = selectedPluginIds().size();
    m_installPluginButton->setEnabled(count > 0);
    m_installPluginButton->setText(count > 0
        ? QStringLiteral("安装已选插件 (%1)").arg(count)
        : QStringLiteral("安装已选插件"));
}

void DesktopEnhancementDialog::showPluginDetails(int row)
{
    if (row < 0 || !m_pluginTable->item(row, 1)) return;
    const QString id = m_pluginTable->item(row, 1)->data(Qt::UserRole).toString();
    for (const CodexPluginInfo &plugin : m_plugins) {
        if (plugin.id != id) continue;
        QString text = QStringLiteral("%1：%2").arg(plugin.name, plugin.description);
        if (!plugin.officialDescription.isEmpty()
                && plugin.officialDescription != plugin.description) {
            text += QStringLiteral("\n官方说明：%1").arg(plugin.officialDescription);
        }
        m_pluginDetails->setText(text);
        return;
    }
}

void DesktopEnhancementDialog::installSelectedPlugin()
{
    const QStringList pluginIds = selectedPluginIds();
    if (pluginIds.isEmpty()) return;
    QList<CodexPluginInfo> selectedPlugins;
    for (const CodexPluginInfo &plugin : m_plugins) {
        if (pluginIds.contains(plugin.id)) selectedPlugins.append(plugin);
    }
    if (!confirmPluginInstallation(this, selectedPlugins)) return;

    m_installPluginButton->setEnabled(false);
    QProgressDialog progress(QStringLiteral("正在安装 Codex 插件..."),
                             QStringLiteral("停止后续安装"), 0, pluginIds.size(), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setValue(0);
    QStringList succeeded;
    QStringList failed;
    for (int i = 0; i < pluginIds.size(); ++i) {
        if (progress.wasCanceled()) break;
        const QString pluginId = pluginIds[i];
        progress.setLabelText(QStringLiteral("正在安装 %1 (%2/%3)...")
            .arg(pluginId).arg(i + 1).arg(pluginIds.size()));
        QApplication::processEvents();
        m_pluginStatus->setText(progress.labelText());
        QString output;
        QString error;
        if (m_manager->installCodexPlugin(pluginId, &output, &error)) {
            succeeded.append(pluginId);
            m_checkedPluginIds.remove(pluginId);
        } else {
            failed.append(QStringLiteral("%1：%2").arg(pluginId, error.left(180)));
        }
        progress.setValue(i + 1);
    }
    refreshPlugins();
    QString result = succeeded.isEmpty()
        ? QStringLiteral("没有插件安装成功。")
        : QStringLiteral("已安装 %1 个插件：\n%2")
            .arg(succeeded.size()).arg(succeeded.join(QLatin1Char('\n')));
    if (!failed.isEmpty()) {
        result += QStringLiteral("\n\n安装失败 %1 个：\n%2")
            .arg(failed.size()).arg(failed.join(QLatin1Char('\n')));
    }
    result += QStringLiteral("\n\n重启 Codex 后生效。");
    showBatchInstallResult(this, result);
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
