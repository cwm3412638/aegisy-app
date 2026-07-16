#include "skills_dialog.h"

#include "app_theme.h"
#include "skill_manager.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFrame>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTabWidget>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <algorithm>

SkillsDialog::SkillsDialog(SkillManager *manager, QWidget *parent)
    : QDialog(parent)
    , m_manager(manager)
{
    setWindowTitle(QStringLiteral("Skills 管理"));
    setMinimumSize(860, 560);
    resize(1060, 680);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 16);
    root->setSpacing(12);
    auto *title = new QLabel(QStringLiteral("Skills 管理"), this);
    title->setStyleSheet(QStringLiteral("font-size: 20px; font-weight: 700; color: #101828;"));
    root->addWidget(title);
    auto *description = new QLabel(
        QStringLiteral("安装完整 Skill 目录并管理自动调用。第三方脚本默认不会执行，只有受信任的内置执行器可以自动运行。"),
        this);
    description->setWordWrap(true);
    description->setStyleSheet(QStringLiteral("font-size: 12px; color: #667085;"));
    root->addWidget(description);

    auto *toolbar = new QHBoxLayout;
    auto *urlButton = new QPushButton(QStringLiteral("从 URL 安装"), this);
    urlButton->setStyleSheet(AppTheme::primaryButtonStyle());
    toolbar->addWidget(urlButton);
    auto *directoryButton = new QPushButton(QStringLiteral("导入本地目录"), this);
    directoryButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    toolbar->addWidget(directoryButton);
    auto *folderButton = new QPushButton(QStringLiteral("打开 Skills 目录"), this);
    folderButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    toolbar->addWidget(folderButton);
    toolbar->addStretch();
    m_runtimeButton = new QPushButton(this);
    m_runtimeButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    toolbar->addWidget(m_runtimeButton);
    root->addLayout(toolbar);

    m_tabs = new QTabWidget(this);
    // 不用 documentMode，让全局 QTabWidget::pane 圆角样式生效

    m_table = new QTableWidget(m_tabs);
    m_table->setColumnCount(7);
    m_table->setHorizontalHeaderLabels({ QStringLiteral("启用"), QStringLiteral("Skill"),
        QStringLiteral("功能说明"), QStringLiteral("执行器"), QStringLiteral("来源"),
        QStringLiteral("权限"), QStringLiteral("状态") });
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tabs->addTab(m_table, QStringLiteral("已安装"));

    auto *catalogPage = new QWidget(m_tabs);
    auto *catalogLayout = new QVBoxLayout(catalogPage);
    catalogLayout->setContentsMargins(8, 8, 8, 8);
    catalogLayout->setSpacing(8);
    m_catalogSearch = new QLineEdit(catalogPage);
    m_catalogSearch->setPlaceholderText(QStringLiteral("搜索 Skill、分类或用途"));
    m_catalogSearch->setClearButtonEnabled(true);
    catalogLayout->addWidget(m_catalogSearch);
    m_catalogTable = new QTableWidget(catalogPage);
    m_catalogTable->setColumnCount(6);
    m_catalogTable->setHorizontalHeaderLabels({ QStringLiteral("Skill"), QStringLiteral("分类"),
        QStringLiteral("功能说明"), QStringLiteral("环境依赖"), QStringLiteral("来源"),
        QStringLiteral("状态") });
    m_catalogTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_catalogTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_catalogTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_catalogTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_catalogTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_catalogTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_catalogTable->verticalHeader()->setVisible(false);
    m_catalogTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_catalogTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_catalogTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    catalogLayout->addWidget(m_catalogTable, 1);
    m_tabs->addTab(catalogPage, QStringLiteral("可安装"));
    root->addWidget(m_tabs, 1);

    auto *detailFrame = new QFrame(this);
    detailFrame->setObjectName(QStringLiteral("skillDetails"));
    detailFrame->setStyleSheet(QStringLiteral(
        "QFrame#skillDetails { background: white; border: 1px solid #e4e7ec; border-radius: 8px; }"));
    auto *detailLayout = new QVBoxLayout(detailFrame);
    detailLayout->setContentsMargins(12, 10, 12, 10);
    m_details = new QLabel(QStringLiteral("选择一个 Skill 查看详情。"), detailFrame);
    m_details->setWordWrap(true);
    m_details->setStyleSheet(QStringLiteral("font-size: 12px; color: #475467; background: transparent;"));
    detailLayout->addWidget(m_details);
    root->addWidget(detailFrame);

    auto *footer = new QHBoxLayout;
    m_status = new QLabel(this);
    m_status->setStyleSheet(QStringLiteral("font-size: 12px; color: #667085;"));
    footer->addWidget(m_status, 1);
    m_installCatalogButton = new QPushButton(QStringLiteral("安装所选 Skill"), this);
    m_installCatalogButton->setEnabled(false);
    m_installCatalogButton->setStyleSheet(AppTheme::primaryButtonStyle());
    footer->addWidget(m_installCatalogButton);
    m_deleteButton = new QPushButton(QStringLiteral("删除"), this);
    m_deleteButton->setEnabled(false);
    m_deleteButton->setStyleSheet(AppTheme::dangerButtonStyle());
    footer->addWidget(m_deleteButton);
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), this);
    closeButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    footer->addWidget(closeButton);
    root->addLayout(footer);

    connect(urlButton, &QPushButton::clicked, this, &SkillsDialog::onInstallUrl);
    connect(directoryButton, &QPushButton::clicked, this, &SkillsDialog::onImportDirectory);
    connect(m_installCatalogButton, &QPushButton::clicked,
            this, &SkillsDialog::onInstallCatalogSelected);
    connect(folderButton, &QPushButton::clicked, this, &SkillsDialog::onOpenFolder);
    connect(m_runtimeButton, &QPushButton::clicked,
            this, &SkillsDialog::onInstallPresentationRuntime);
    connect(m_deleteButton, &QPushButton::clicked, this, &SkillsDialog::onDeleteSelected);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_table, &QTableWidget::cellChanged, this, &SkillsDialog::onItemChanged);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &SkillsDialog::updateSelection);
    connect(m_catalogTable, &QTableWidget::itemSelectionChanged,
            this, &SkillsDialog::updateSelection);
    connect(m_catalogSearch, &QLineEdit::textChanged,
            this, &SkillsDialog::filterCatalog);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) { updateSelection(); });
    connect(m_manager, &SkillManager::skillsChanged, this, &SkillsDialog::rebuildTable);
    rebuildTable();
}

void SkillsDialog::rebuildTable()
{
    m_rebuilding = true;
    const QList<SkillInfo> skills = m_manager->skills();
    m_table->setRowCount(skills.size());
    for (int row = 0; row < skills.size(); ++row) {
        const SkillInfo &skill = skills.at(row);
        auto *enabled = new QTableWidgetItem;
        enabled->setData(Qt::UserRole, skill.id);
        enabled->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        enabled->setCheckState(skill.enabled ? Qt::Checked : Qt::Unchecked);
        enabled->setTextAlignment(Qt::AlignCenter);
        m_table->setItem(row, 0, enabled);
        auto *name = new QTableWidgetItem(skill.name);
        name->setData(Qt::UserRole, skill.id);
        m_table->setItem(row, 1, name);
        m_table->setItem(row, 2, new QTableWidgetItem(skill.description));
        m_table->setItem(row, 3, new QTableWidgetItem(
            skill.executor == QStringLiteral("instruction") ? QStringLiteral("仅指令")
            : skill.executor == QStringLiteral("image") ? QStringLiteral("GPT Image")
            : QStringLiteral("PPT 本地运行时")));
        m_table->setItem(row, 4, new QTableWidgetItem(skill.source));
        m_table->setItem(row, 5, new QTableWidgetItem(
            skill.permissions.isEmpty() ? QStringLiteral("无") : skill.permissions.join(QStringLiteral("、"))));
        QString status;
        if (!skill.compatible) status = QStringLiteral("不兼容");
        else if (skill.executor == QStringLiteral("instruction")) status = QStringLiteral("仅指令");
        else if (!skill.trusted) status = QStringLiteral("未授权执行");
        else if (skill.executor == QStringLiteral("presentation")
                 && !m_manager->presentationRuntimeReady()) status = QStringLiteral("缺少运行时");
        else status = QStringLiteral("可用");
        m_table->setItem(row, 6, new QTableWidgetItem(status));
        m_table->setRowHeight(row, 44);
    }
    m_rebuilding = false;
    rebuildCatalog();
    m_runtimeButton->setText(m_manager->presentationRuntimeReady()
        ? QStringLiteral("PPT 运行环境已安装") : QStringLiteral("安装 PPT 运行环境"));
    m_runtimeButton->setEnabled(!m_manager->presentationRuntimeReady());
    m_status->setText(QStringLiteral("共 %1 个 Skills，启用 %2 个")
        .arg(skills.size())
        .arg(std::count_if(skills.cbegin(), skills.cend(),
                           [](const SkillInfo &skill) { return skill.enabled; })));
    updateSelection();
}

void SkillsDialog::onItemChanged(int row, int column)
{
    if (m_rebuilding || column != 0 || !m_table->item(row, 0)) return;
    QString error;
    const QString id = m_table->item(row, 0)->data(Qt::UserRole).toString();
    if (!m_manager->setEnabled(id, m_table->item(row, 0)->checkState() == Qt::Checked, &error)) {
        QMessageBox::warning(this, QStringLiteral("无法更新 Skill"), error);
        rebuildTable();
    }
}

void SkillsDialog::onInstallUrl()
{
    bool accepted = false;
    const QString url = QInputDialog::getText(
        this, QStringLiteral("从 URL 安装 Skill"),
        QStringLiteral("Aegisy Skill 目录、SKILL.md 或 INSTALL.md 地址：\n"
                       "例如 https://www.aegisy.cc/skills/<skill-name>/SKILL.md"),
        QLineEdit::Normal, QString(),
        &accepted).trimmed();
    if (!accepted || url.isEmpty()) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const bool ok = m_manager->installFromUrl(url, &error);
    QApplication::restoreOverrideCursor();
    if (!ok) QMessageBox::warning(this, QStringLiteral("安装失败"), error);
    else QMessageBox::information(this, QStringLiteral("安装完成"),
        QStringLiteral("Skill 已完整保存。第三方脚本默认禁用，需要审核后才能配置执行器。"));
}

void SkillsDialog::rebuildCatalog()
{
    const QList<SkillCatalogInfo> catalog = m_manager->catalogSkills();
    m_catalogTable->setRowCount(catalog.size());
    for (int row = 0; row < catalog.size(); ++row) {
        const SkillCatalogInfo &entry = catalog.at(row);
        auto *name = new QTableWidgetItem(entry.name);
        name->setData(Qt::UserRole, entry.id);
        m_catalogTable->setItem(row, 0, name);
        m_catalogTable->setItem(row, 1, new QTableWidgetItem(entry.category));
        m_catalogTable->setItem(row, 2, new QTableWidgetItem(entry.description));
        m_catalogTable->setItem(row, 3, new QTableWidgetItem(
            entry.requirements.join(QStringLiteral("、"))));
        m_catalogTable->setItem(row, 4, new QTableWidgetItem(entry.source));
        const bool installed = !m_manager->skill(entry.id).id.isEmpty();
        m_catalogTable->setItem(row, 5, new QTableWidgetItem(
            installed ? QStringLiteral("已安装") : QStringLiteral("可安装")));
        m_catalogTable->setRowHeight(row, 44);
    }
    filterCatalog(m_catalogSearch ? m_catalogSearch->text() : QString());
}

void SkillsDialog::filterCatalog(const QString &text)
{
    const QString query = text.trimmed();
    for (int row = 0; row < m_catalogTable->rowCount(); ++row) {
        QString searchable;
        for (int column = 0; column < m_catalogTable->columnCount(); ++column) {
            if (const QTableWidgetItem *item = m_catalogTable->item(row, column)) {
                searchable += item->text() + QLatin1Char(' ');
            }
        }
        m_catalogTable->setRowHidden(
            row, !query.isEmpty() && !searchable.contains(query, Qt::CaseInsensitive));
    }
}

QString SkillsDialog::selectedCatalogSkillId() const
{
    const int row = m_catalogTable->currentRow();
    return row >= 0 && m_catalogTable->item(row, 0)
        ? m_catalogTable->item(row, 0)->data(Qt::UserRole).toString() : QString();
}

void SkillsDialog::onInstallCatalogSelected()
{
    const QString id = selectedCatalogSkillId();
    if (id.isEmpty()) return;
    const QList<SkillCatalogInfo> catalog = m_manager->catalogSkills();
    const auto found = std::find_if(catalog.cbegin(), catalog.cend(),
        [&](const SkillCatalogInfo &entry) { return entry.id == id; });
    if (found == catalog.cend()) return;
    const QString dependencies = found->requirements.isEmpty()
        ? QStringLiteral("无额外依赖") : found->requirements.join(QStringLiteral("、"));
    if (QMessageBox::question(this, QStringLiteral("安装 Skill"),
        QStringLiteral("安装 %1？\n\n运行此工作流可能需要：%2")
            .arg(found->name, dependencies),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes) return;
    QString error;
    if (!m_manager->installCatalogSkill(id, &error)) {
        QMessageBox::warning(this, QStringLiteral("安装失败"), error);
        return;
    }
    QMessageBox::information(this, QStringLiteral("安装完成"),
                             QStringLiteral("%1 已加入本地 Skills。").arg(found->name));
}

void SkillsDialog::onImportDirectory()
{
    const QString directory = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择包含 SKILL.md 的目录"));
    if (directory.isEmpty()) return;
    QString error;
    if (!m_manager->installFromDirectory(directory, &error)) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), error);
    }
}

QString SkillsDialog::selectedSkillId() const
{
    const int row = m_table->currentRow();
    return row >= 0 && m_table->item(row, 1)
        ? m_table->item(row, 1)->data(Qt::UserRole).toString() : QString();
}

void SkillsDialog::onDeleteSelected()
{
    const QString id = selectedSkillId();
    if (id.isEmpty()) return;
    const SkillInfo skill = m_manager->skill(id);
    if (QMessageBox::question(this, QStringLiteral("删除 Skill"),
        QStringLiteral("确定删除 %1？").arg(skill.name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    QString error;
    if (!m_manager->removeSkill(id, &error)) {
        QMessageBox::warning(this, QStringLiteral("删除失败"), error);
    }
}

void SkillsDialog::onOpenFolder()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_manager->skillsRoot()));
}

void SkillsDialog::onInstallPresentationRuntime()
{
    if (QMessageBox::question(this, QStringLiteral("安装 PPT 运行环境"),
        QStringLiteral("将在 Aegisy 应用数据目录创建独立 Python 环境并从 PyPI 安装 python-pptx。是否继续？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    m_runtimeButton->setEnabled(false);
    m_runtimeButton->setText(QStringLiteral("正在安装..."));
    m_status->setText(QStringLiteral("正在后台安装 python-pptx，可继续浏览其他 Skills。"));

    QPointer<SkillsDialog> dialog(this);
    const QString skillsRoot = m_manager->skillsRoot();
    QThread *worker = QThread::create([dialog, skillsRoot]() {
        SkillManager manager(nullptr, skillsRoot);
        QString error;
        const bool ok = manager.installPresentationRuntime(&error);
        if (!dialog) return;
        QMetaObject::invokeMethod(dialog, [dialog, ok, error]() {
            if (!dialog) return;
            if (!ok) {
                QMessageBox::warning(dialog, QStringLiteral("安装失败"), error);
            } else {
                QMessageBox::information(
                    dialog, QStringLiteral("安装完成"),
                    QStringLiteral("PPT Skill 运行环境已准备完成。"));
            }
            dialog->rebuildTable();
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void SkillsDialog::updateSelection()
{
    const bool catalogTab = m_tabs && m_tabs->currentIndex() == 1;
    m_installCatalogButton->setVisible(catalogTab);
    m_deleteButton->setVisible(!catalogTab);
    if (catalogTab) {
        const QString id = selectedCatalogSkillId();
        const QList<SkillCatalogInfo> catalog = m_manager->catalogSkills();
        const int installedCount = std::count_if(
            catalog.cbegin(), catalog.cend(), [this](const SkillCatalogInfo &entry) {
                return !m_manager->skill(entry.id).id.isEmpty();
            });
        m_status->setText(QStringLiteral("精选目录共 %1 个 Skills，已安装 %2 个")
            .arg(catalog.size()).arg(installedCount));
        const auto found = std::find_if(catalog.cbegin(), catalog.cend(),
            [&](const SkillCatalogInfo &entry) { return entry.id == id; });
        const bool installed = !id.isEmpty() && !m_manager->skill(id).id.isEmpty();
        m_installCatalogButton->setEnabled(found != catalog.cend() && !installed);
        if (found == catalog.cend()) {
            m_details->setText(QStringLiteral("选择一个 Skill 查看详情。"));
            return;
        }
        m_details->setText(QStringLiteral(
            "%1\n分类：%2\n环境依赖：%3\n安装方式：Aegisy 内置技能包")
            .arg(found->description, found->category,
                 found->requirements.isEmpty() ? QStringLiteral("无")
                                               : found->requirements.join(QStringLiteral("、"))));
        return;
    }

    const QList<SkillInfo> installedSkills = m_manager->skills();
    m_status->setText(QStringLiteral("共 %1 个 Skills，启用 %2 个")
        .arg(installedSkills.size())
        .arg(std::count_if(installedSkills.cbegin(), installedSkills.cend(),
                           [](const SkillInfo &skill) { return skill.enabled; })));
    const SkillInfo skill = m_manager->skill(selectedSkillId());
    m_deleteButton->setEnabled(!skill.id.isEmpty() && !skill.builtin);
    if (skill.id.isEmpty()) {
        m_details->setText(QStringLiteral("选择一个 Skill 查看详情。"));
        return;
    }
    m_details->setText(QStringLiteral("%1\n触发词：%2\n目录：%3")
        .arg(skill.description,
             skill.triggers.isEmpty() ? QStringLiteral("由宿主客户端决定") : skill.triggers.join(QStringLiteral("、")),
             skill.path));
}
