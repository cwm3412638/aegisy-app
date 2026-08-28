#include "extension_center_dialog.h"

#include "app_theme.h"
#include "extension_compatibility_policy.h"

#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
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
    const ExtensionReviewLedgerStoreResult &ledger,
    QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("扩展中心"));
    resize(1120, 660);
    setMinimumSize(820, 500);
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

    m_table = new QTableWidget(0, 9, this);
    m_table->setObjectName(QStringLiteral("extensionCenterTable"));
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("名称 / ID"), QStringLiteral("类型"), QStringLiteral("版本"),
        QStringLiteral("作用域"), QStringLiteral("请求能力"), QStringLiteral("来源"),
        QStringLiteral("信任"), QStringLiteral("兼容状态"), QStringLiteral("人工复核")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 9; ++column) {
        m_table->horizontalHeader()->setSectionResizeMode(
            column, QHeaderView::ResizeToContents);
    }
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    root->addWidget(m_table, 1);

    m_reviewStatus = new QLabel(this);
    m_reviewStatus->setObjectName(QStringLiteral("extensionReviewStatus"));
    m_reviewStatus->setWordWrap(true);
    m_reviewStatus->setStyleSheet(QStringLiteral(
        "font-size:12px; color:#667085; background:#f8fafc;"
        "border:1px solid #eaecf0; border-radius:7px; padding:8px 10px;"));
    root->addWidget(m_reviewStatus);

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
    populate(records, sourceIssueCodes, ledger);
}

void ExtensionCenterDialog::setReviewSnapshot(
    const QList<ExtensionRegistryRecord> &records,
    const QStringList &sourceIssueCodes,
    const ExtensionReviewLedgerStoreResult &ledger)
{
    populate(records, sourceIssueCodes, ledger);
}

void ExtensionCenterDialog::setReviewBusy(bool busy)
{
    m_reviewBusy = busy;
    for (QPushButton *button : m_reviewButtons) {
        if (button) {
            button->setEnabled(!busy
                && button->property("extensionReviewEligible").toBool());
        }
    }
}

void ExtensionCenterDialog::showReviewError(const QString &errorCode)
{
    if (!m_reviewStatus) return;
    const QRegularExpression fixedCode(QStringLiteral("^[a-z0-9][a-z0-9-]{0,95}$"));
    m_reviewStatus->setText(fixedCode.match(errorCode).hasMatch()
        ? QStringLiteral("审核未提交：%1").arg(errorCode)
        : QStringLiteral("审核未提交：扩展复核状态不可用"));
    m_reviewStatus->setStyleSheet(QStringLiteral(
        "font-size:12px; color:#b42318; background:#fff5f5;"
        "border:1px solid #fecdca; border-radius:7px; padding:8px 10px;"));
}

void ExtensionCenterDialog::populate(
    const QList<ExtensionRegistryRecord> &records,
    const QStringList &sourceIssueCodes,
    const ExtensionReviewLedgerStoreResult &ledger)
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
    m_ledger = ledger;
    rebuildRows();
    std::sort(m_records.begin(), m_records.end(), [](const auto &left, const auto &right) {
        if (left.kind != right.kind) {
            return static_cast<int>(left.kind) < static_cast<int>(right.kind);
        }
        return left.id < right.id;
    });
    m_table->setRowCount(m_rows.size());
    m_reviewButtons.clear();
    for (int row = 0; row < m_rows.size(); ++row) {
        const ReviewRow &entry = m_rows.at(row);
        const ExtensionRegistryRecord &record = entry.record;
        const QString displayName = record.name == record.id
            ? record.id
            : QStringLiteral("%1  [%2]").arg(record.name, record.id);
        auto *name = readOnlyItem(displayName);
        name->setData(Qt::UserRole, record.id);
        name->setData(Qt::UserRole + 1, static_cast<int>(record.kind));
        m_table->setItem(row, 0, name);
        m_table->setItem(row, 1, readOnlyItem(kindLabel(record.kind)));
        m_table->setItem(row, 2, readOnlyItem(
            record.version.isEmpty() ? QStringLiteral("未知") : record.version));
        m_table->setItem(row, 3, readOnlyItem(
            record.scope.isEmpty() ? QStringLiteral("未知") : record.scope));
        m_table->setItem(row, 4, readOnlyItem(
            record.requestedCapabilities.isEmpty()
                ? QStringLiteral("无") : record.requestedCapabilities.join(
                    QStringLiteral(", "))));
        m_table->setItem(row, 5, readOnlyItem(sourceLabel(record.sourceKind)));
        m_table->setItem(row, 6, readOnlyItem(
            entry.hasRecord ? trustLabel(record.trust)
                                : QStringLiteral("来源未发现")));
        m_table->setItem(row, 7, readOnlyItem(
            compatibilityLabel(record.compatibility)));
        auto *reviewButton = new QPushButton(this);
        reviewButton->setObjectName(QStringLiteral("extensionReviewButton"));
        reviewButton->setFixedHeight(28);
        reviewButton->setCursor(Qt::PointingHandCursor);
        reviewButton->setStyleSheet(AppTheme::secondaryButtonStyle());
        const bool ledgerUsable = m_ledger.state == ExtensionReviewLedgerStoreState::Empty
            || m_ledger.state == ExtensionReviewLedgerStoreState::Ready;
        const bool revoke = entry.hasPin;
        reviewButton->setText(revoke ? QStringLiteral("撤销审核") : QStringLiteral("审核"));
        reviewButton->setToolTip(revoke
            ? QStringLiteral("撤销该扩展的人工复核证据")
            : QStringLiteral("只写入人工复核证据，不安装、启用或执行"));
        const bool reviewEligible = ledgerUsable
            && (!entry.hasRecord || record.installed);
        reviewButton->setProperty("extensionReviewEligible", reviewEligible);
        reviewButton->setEnabled(reviewEligible && !m_reviewBusy);
        connect(reviewButton, &QPushButton::clicked, this,
                [this, row]() { reviewRow(row); });
        m_reviewButtons.append(reviewButton);
        m_table->setItem(row, 8, readOnlyItem(QString()));
        m_table->setCellWidget(row, 8, reviewButton);
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
    if (m_reviewStatus) {
        m_reviewStatus->setStyleSheet(QStringLiteral(
            "font-size:12px; color:#667085; background:#f8fafc;"
            "border:1px solid #eaecf0; border-radius:7px; padding:8px 10px;"));
        switch (m_ledger.state) {
        case ExtensionReviewLedgerStoreState::Empty:
            m_reviewStatus->setText(QStringLiteral(
                "人工复核：尚未建立复核记录。审核只产生 Verified 证据，不安装、启用、更新、删除或执行扩展。"));
            break;
        case ExtensionReviewLedgerStoreState::Ready:
            m_reviewStatus->setText(QStringLiteral(
                "人工复核：已认证第 %1 代记录。审核/撤销会重新读取来源并使用 CAS，失败不会覆盖现有记录。")
                .arg(m_ledger.generation));
            break;
        default:
            m_reviewStatus->setText(QStringLiteral(
                "人工复核存储不可用，审核操作已冻结；不会把故障当成未审核。"));
            break;
        }
    }
    applyFilter();
}

void ExtensionCenterDialog::rebuildRows()
{
    m_rows.clear();
    std::sort(m_records.begin(), m_records.end(), [](const auto &left, const auto &right) {
        if (left.kind != right.kind) {
            return static_cast<int>(left.kind) < static_cast<int>(right.kind);
        }
        return left.id < right.id;
    });
    for (const ExtensionRegistryRecord &record : m_records) {
        ReviewRow row;
        row.record = record;
        row.hasRecord = true;
        for (const ExtensionReviewPin &pin : m_ledger.pins) {
            if (pin.kind == record.kind && pin.id == record.id) {
                row.pin = pin;
                row.hasPin = true;
                break;
            }
        }
        m_rows.append(row);
    }
    for (const ExtensionReviewPin &pin : m_ledger.pins) {
        bool present = false;
        for (const ExtensionRegistryRecord &record : m_records) {
            present = present || (record.kind == pin.kind && record.id == pin.id);
        }
        if (present) continue;
        ReviewRow row;
        row.pin = pin;
        row.hasPin = true;
        row.record.kind = pin.kind;
        row.record.id = pin.id;
        row.record.name = QStringLiteral("来源未发现");
        row.record.sourceIdentity = pin.sourceIdentity;
        row.record.contentIdentity = pin.contentIdentity;
        row.record.compatibility = ExtensionCompatibilityState::Unknown;
        row.record.compatibilityReason = QStringLiteral("extension-review-source-missing");
        row.record.scope = QStringLiteral("user");
        m_rows.append(row);
    }
}

bool ExtensionCenterDialog::confirmPrompt(const ExtensionReviewPrompt &prompt,
                                          ExtensionReviewAction action)
{
    if (prompt.state != ExtensionReviewPromptState::Ready) {
        showReviewError(prompt.errorCode);
        return false;
    }
    QStringList warningLabels;
    for (const ExtensionReviewWarning warning : prompt.warnings) {
        switch (warning) {
        case ExtensionReviewWarning::NameMismatchesIdentifier:
            warningLabels.append(QStringLiteral("名称与标识不一致")); break;
        case ExtensionReviewWarning::VersionUnknown:
            warningLabels.append(QStringLiteral("版本未知")); break;
        case ExtensionReviewWarning::CapabilityNotGranted:
            warningLabels.append(QStringLiteral("请求了宿主未授予的能力")); break;
        case ExtensionReviewWarning::CapabilityBeyondReadOnly:
            warningLabels.append(QStringLiteral("请求了写入或执行能力")); break;
        case ExtensionReviewWarning::CompatibilityUnresolved:
            warningLabels.append(QStringLiteral("兼容性仍需审核")); break;
        case ExtensionReviewWarning::ContentChangedSinceReview:
            warningLabels.append(QStringLiteral("内容相较上次审核已变化")); break;
        }
    }
    const QString text = QStringList{
        action == ExtensionReviewAction::Approve
            ? QStringLiteral("审核人工复核") : QStringLiteral("撤销人工复核"),
        QString(),
        QStringLiteral("名称：") + prompt.title,
        QStringLiteral("标识：") + prompt.identifier,
        QStringLiteral("类型：") + prompt.kindLabel,
        QStringLiteral("版本：") + prompt.versionLabel,
        QStringLiteral("作用域：") + prompt.scopeLabel,
        QStringLiteral("请求能力：")
            + prompt.capabilities.join(QStringLiteral(", ")),
        QStringLiteral("来源身份：") + prompt.sourceIdentity,
        QStringLiteral("内容身份：") + prompt.contentIdentity,
        QString(),
        QStringLiteral("风险提示：")
            + (warningLabels.isEmpty() ? QStringLiteral("无")
                                       : warningLabels.join(QStringLiteral("；"))),
        QString(),
        QStringLiteral("本操作只写入或撤销人工复核证据，不安装、启用、更新、删除、执行扩展，也不改变本地工具配置。确认后仍需重新读取来源并通过并发 CAS。")
    }.join(QLatin1Char('\n'));
    QMessageBox box(QMessageBox::Question, QStringLiteral("确认人工复核"), text,
                    QMessageBox::Cancel | QMessageBox::Ok, this);
    box.setTextFormat(Qt::PlainText);
    auto *check = new QCheckBox(QStringLiteral(
        "我已核对完整来源身份和内容身份，只记录复核证据"), &box);
    box.setCheckBox(check);
    box.button(QMessageBox::Ok)->setEnabled(false);
    connect(check, &QCheckBox::toggled, box.button(QMessageBox::Ok),
            &QAbstractButton::setEnabled);
    box.button(QMessageBox::Ok)->setText(QStringLiteral("确认"));
    box.button(QMessageBox::Cancel)->setText(QStringLiteral("取消"));
    return box.exec() == QMessageBox::Ok && check->isChecked();
}

bool ExtensionCenterDialog::confirmRevoke(const ExtensionReviewPin &pin)
{
    const QString text = QStringLiteral(
        "撤销人工复核\n\n类型：%1\n标识：%2\n来源身份：%3\n内容身份：%4\n\n"
        "本操作只撤销该条 Verified 证据，不安装、启用、删除或执行任何扩展。")
        .arg(kindLabel(pin.kind), pin.id, pin.sourceIdentity, pin.contentIdentity);
    QMessageBox box(QMessageBox::Question, QStringLiteral("确认撤销审核"), text,
                    QMessageBox::Cancel | QMessageBox::Ok, this);
    box.setTextFormat(Qt::PlainText);
    auto *check = new QCheckBox(QStringLiteral("我已核对要撤销的完整身份"), &box);
    box.setCheckBox(check);
    box.button(QMessageBox::Ok)->setEnabled(false);
    connect(check, &QCheckBox::toggled, box.button(QMessageBox::Ok),
            &QAbstractButton::setEnabled);
    box.button(QMessageBox::Ok)->setText(QStringLiteral("确认"));
    box.button(QMessageBox::Cancel)->setText(QStringLiteral("取消"));
    return box.exec() == QMessageBox::Ok && check->isChecked();
}

void ExtensionCenterDialog::reviewRow(int row)
{
    if (m_reviewBusy || row < 0 || row >= m_rows.size()) return;
    const ReviewRow &item = m_rows.at(row);
    ExtensionReviewRequest request;
    request.kind = item.record.kind;
    request.id = item.record.id;
    request.action = ExtensionReviewAction::Revoke;
    if (item.hasPin) {
        if (!confirmRevoke(item.pin)) return;
    } else {
        const ExtensionReviewPrompt prompt = ExtensionReviewPresentation::build(
            item.record, ExtensionCompatibilityPolicy::defaultGrantedCapabilities(),
            false, QString());
        if (!confirmPrompt(prompt, ExtensionReviewAction::Approve)) return;
        request.action = ExtensionReviewAction::Approve;
        request.reviewedSourceIdentity = prompt.reviewedSourceIdentity;
        request.reviewedContentIdentity = prompt.reviewedContentIdentity;
    }
    emit reviewRequested(request);
}

void ExtensionCenterDialog::applyFilter()
{
    const QString search = m_search->text().trimmed();
    const int kind = m_kindFilter->currentData().toInt();
    for (int row = 0; row < m_rows.size(); ++row) {
        const ExtensionRegistryRecord &record = m_rows.at(row).record;
        const bool kindMatches = kind < 0 || static_cast<int>(record.kind) == kind;
        const bool textMatches = search.isEmpty()
            || record.name.contains(search, Qt::CaseInsensitive)
            || record.id.contains(search, Qt::CaseInsensitive);
        m_table->setRowHidden(row, !kindMatches || !textMatches);
    }
}
