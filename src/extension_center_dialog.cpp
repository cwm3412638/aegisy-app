#include "extension_center_dialog.h"

#include "app_theme.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QString kindLabel(ExtensionKind kind)
{
    switch (kind) {
    case ExtensionKind::CodexPlugin: return QStringLiteral("Codex 插件");
    case ExtensionKind::Skill: return QStringLiteral("Skill");
    case ExtensionKind::Mcp: return QStringLiteral("MCP");
    }
    return {};
}

QString sourceLabel(ExtensionSourceKind source)
{
    switch (source) {
    case ExtensionSourceKind::BuiltIn: return QStringLiteral("内置");
    case ExtensionSourceKind::LocalDirectory: return QStringLiteral("本地目录");
    case ExtensionSourceKind::CodexCli: return QStringLiteral("Codex CLI");
    case ExtensionSourceKind::ToolConfiguration: return QStringLiteral("工具配置");
    }
    return {};
}

QString trustLabel(ExtensionTrustState trust)
{
    return trust == ExtensionTrustState::Verified
        ? QStringLiteral("已验证") : QStringLiteral("待审核");
}

QString compatibilityLabel(ExtensionCompatibilityState state)
{
    switch (state) {
    case ExtensionCompatibilityState::Compatible: return QStringLiteral("兼容");
    case ExtensionCompatibilityState::Unknown: return QStringLiteral("兼容性未知");
    case ExtensionCompatibilityState::Incompatible: return QStringLiteral("不兼容");
    }
    return {};
}

QTableWidgetItem *readOnlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable & ~Qt::ItemIsUserCheckable);
    return item;
}

} // namespace

ExtensionCenterDialog::ExtensionCenterDialog(
    const QList<ExtensionRegistryRecord> &records,
    const QStringList &sourceIssueCodes,
    QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("扩展中心"));
    resize(840, 560);
    setMinimumSize(680, 420);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(22, 20, 22, 16);
    root->setSpacing(12);
    auto *title = new QLabel(QStringLiteral("Codex 插件、Skills 与 MCP"), this);
    title->setStyleSheet(QStringLiteral(
        "font-size:17px; font-weight:700; color:#101828;"));
    root->addWidget(title);

    auto *filters = new QHBoxLayout;
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("extensionCenterSearch"));
    m_search->setPlaceholderText(QStringLiteral("搜索扩展"));
    m_kindFilter = new QComboBox(this);
    m_kindFilter->setObjectName(QStringLiteral("extensionCenterKindFilter"));
    m_kindFilter->addItem(QStringLiteral("全部类型"), -1);
    m_kindFilter->addItem(QStringLiteral("Codex 插件"),
                          static_cast<int>(ExtensionKind::CodexPlugin));
    m_kindFilter->addItem(QStringLiteral("Skills"),
                          static_cast<int>(ExtensionKind::Skill));
    m_kindFilter->addItem(QStringLiteral("MCP"),
                          static_cast<int>(ExtensionKind::Mcp));
    filters->addWidget(m_search, 1);
    filters->addWidget(m_kindFilter);
    root->addLayout(filters);

    m_table = new QTableWidget(0, 6, this);
    m_table->setObjectName(QStringLiteral("extensionCenterTable"));
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("名称"), QStringLiteral("类型"), QStringLiteral("版本"),
        QStringLiteral("来源"), QStringLiteral("信任"), QStringLiteral("兼容状态")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 6; ++column) {
        m_table->horizontalHeader()->setSectionResizeMode(
            column, QHeaderView::ResizeToContents);
    }
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    root->addWidget(m_table, 1);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("extensionCenterStatus"));
    m_status->setStyleSheet(QStringLiteral("font-size:12px; color:#667085;"));
    root->addWidget(m_status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    buttons->button(QDialogButtonBox::Close)->setStyleSheet(
        AppTheme::secondaryButtonStyle());
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(m_search, &QLineEdit::textChanged, this, &ExtensionCenterDialog::applyFilter);
    connect(m_kindFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ExtensionCenterDialog::applyFilter);
    populate(records, sourceIssueCodes);
}

void ExtensionCenterDialog::populate(
    const QList<ExtensionRegistryRecord> &records,
    const QStringList &sourceIssueCodes)
{
    ExtensionRegistryProjection projection;
    QString error;
    if (!ExtensionRegistry::build(records, &projection, &error)) {
        m_records.clear();
        m_table->setRowCount(0);
        m_status->setText(QStringLiteral("扩展清单无效"));
        return;
    }
    m_records = records;
    std::sort(m_records.begin(), m_records.end(), [](const auto &left, const auto &right) {
        if (left.kind != right.kind) {
            return static_cast<int>(left.kind) < static_cast<int>(right.kind);
        }
        return left.id < right.id;
    });
    m_table->setRowCount(m_records.size());
    for (int row = 0; row < m_records.size(); ++row) {
        const ExtensionRegistryRecord &record = m_records.at(row);
        auto *name = readOnlyItem(record.name);
        name->setData(Qt::UserRole, record.id);
        name->setData(Qt::UserRole + 1, static_cast<int>(record.kind));
        name->setData(Qt::UserRole + 2, record.sourceIdentity);
        name->setData(Qt::UserRole + 3, record.contentIdentity);
        m_table->setItem(row, 0, name);
        m_table->setItem(row, 1, readOnlyItem(kindLabel(record.kind)));
        m_table->setItem(row, 2, readOnlyItem(
            record.version.isEmpty() ? QStringLiteral("未知") : record.version));
        m_table->setItem(row, 3, readOnlyItem(sourceLabel(record.sourceKind)));
        m_table->setItem(row, 4, readOnlyItem(trustLabel(record.trust)));
        m_table->setItem(row, 5, readOnlyItem(
            compatibilityLabel(record.compatibility)));
    }
    int safeIssueCount = 0;
    const QRegularExpression fixedCode(QStringLiteral("^[a-z0-9][a-z0-9-]{0,95}$"));
    for (const QString &issue : sourceIssueCodes) {
        if (fixedCode.match(issue).hasMatch()) ++safeIssueCount;
    }
    m_status->setText(safeIssueCount == 0
        ? QStringLiteral("%1 个扩展 · 只读清单").arg(m_records.size())
        : QStringLiteral("%1 个扩展 · %2 个来源不可用")
            .arg(m_records.size()).arg(safeIssueCount));
    applyFilter();
}

void ExtensionCenterDialog::applyFilter()
{
    const QString search = m_search->text().trimmed();
    const int kind = m_kindFilter->currentData().toInt();
    for (int row = 0; row < m_records.size(); ++row) {
        const ExtensionRegistryRecord &record = m_records.at(row);
        const bool kindMatches = kind < 0 || static_cast<int>(record.kind) == kind;
        const bool textMatches = search.isEmpty()
            || record.name.contains(search, Qt::CaseInsensitive)
            || record.id.contains(search, Qt::CaseInsensitive);
        m_table->setRowHidden(row, !kindMatches || !textMatches);
    }
}
