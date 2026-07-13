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
#include <QProgressDialog>
#include <QPushButton>
#include <QTableWidget>
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

    m_table = new QTableWidget(this);
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
    root->addWidget(m_table, 1);

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
    connect(folderButton, &QPushButton::clicked, this, &SkillsDialog::onOpenFolder);
    connect(m_runtimeButton, &QPushButton::clicked,
            this, &SkillsDialog::onInstallPresentationRuntime);
    connect(m_deleteButton, &QPushButton::clicked, this, &SkillsDialog::onDeleteSelected);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_table, &QTableWidget::cellChanged, this, &SkillsDialog::onItemChanged);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &SkillsDialog::updateSelection);
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
        else if (!skill.trusted) status = QStringLiteral("未授权执行");
        else if (skill.executor == QStringLiteral("presentation")
                 && !m_manager->presentationRuntimeReady()) status = QStringLiteral("缺少运行时");
        else status = QStringLiteral("可用");
        m_table->setItem(row, 6, new QTableWidgetItem(status));
        m_table->setRowHeight(row, 46);
    }
    m_rebuilding = false;
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
        QStringLiteral("Skill 目录、SKILL.md 或 INSTALL.md 的 HTTPS 地址："),
        QLineEdit::Normal,
        QStringLiteral("https://apikey.fun/install/skill/image-gen/INSTALL.md"),
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
    QProgressDialog progress(QStringLiteral("正在安装 python-pptx，可能需要几分钟..."),
                             QString(), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setCancelButton(nullptr);
    progress.show();
    QApplication::processEvents();
    QString error;
    const bool ok = m_manager->installPresentationRuntime(&error);
    progress.close();
    if (!ok) QMessageBox::warning(this, QStringLiteral("安装失败"), error);
    else QMessageBox::information(this, QStringLiteral("安装完成"),
                                  QStringLiteral("PPT Skill 运行环境已准备完成。"));
    rebuildTable();
}

void SkillsDialog::updateSelection()
{
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
