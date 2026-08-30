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

// Blocked 的原因必须逐项可分辨。把"没人复核过"显示成"当前主机装不下"会让人以为换台
// 机器就能运行一份从未被人看过的内容。
QString blockReasonLabel(ExtensionEnablementBlockReason reason)
{
    switch (reason) {
    case ExtensionEnablementBlockReason::NotInstalled:
        return QStringLiteral("未安装，不能授权");
    case ExtensionEnablementBlockReason::TrustMissing:
        return QStringLiteral("未经人工复核，不能授权");
    case ExtensionEnablementBlockReason::CompatibilityMissing:
        return QStringLiteral("兼容性未确立，不能授权");
    case ExtensionEnablementBlockReason::None:
        break;
    }
    return QStringLiteral("不能授权");
}

QString enablementWarningLabel(ExtensionEnablementWarning warning)
{
    switch (warning) {
    case ExtensionEnablementWarning::NameMismatchesIdentifier:
        return QStringLiteral("名称与标识不一致");
    case ExtensionEnablementWarning::VersionUnknown:
        return QStringLiteral("版本未知");
    case ExtensionEnablementWarning::CapabilityNotGranted:
        return QStringLiteral("请求了宿主未授予的能力");
    case ExtensionEnablementWarning::CapabilityBeyondReadOnly:
        return QStringLiteral("请求了写入或执行能力");
    case ExtensionEnablementWarning::ContentChangedSinceGrant:
        return QStringLiteral("内容相较上次授权已变化");
    case ExtensionEnablementWarning::AlreadyGranted:
        return QStringLiteral("已存在等效授权，本次不改变任何状态");
    case ExtensionEnablementWarning::GrantDoesNotExecuteYet:
        return QStringLiteral(
            "本次授权当前不会让任何内容运行：权限、审批、沙箱与恢复门禁尚未完成");
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
    const ExtensionEnablementLedgerStoreResult &grants,
    QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("扩展中心"));
    resize(1220, 680);
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

    m_table = new QTableWidget(0, 10, this);
    m_table->setObjectName(QStringLiteral("extensionCenterTable"));
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("名称 / ID"), QStringLiteral("类型"), QStringLiteral("版本"),
        QStringLiteral("作用域"), QStringLiteral("请求能力"), QStringLiteral("来源"),
        QStringLiteral("信任"), QStringLiteral("兼容状态"), QStringLiteral("人工复核"),
        QStringLiteral("启用授权")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 10; ++column) {
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

    m_enablementStatus = new QLabel(this);
    m_enablementStatus->setObjectName(QStringLiteral("extensionEnablementStatus"));
    m_enablementStatus->setWordWrap(true);
    m_enablementStatus->setStyleSheet(QStringLiteral(
        "font-size:12px; color:#667085; background:#f8fafc;"
        "border:1px solid #eaecf0; border-radius:7px; padding:8px 10px;"));
    root->addWidget(m_enablementStatus);

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
    populate(records, sourceIssueCodes, ledger, grants);
}

void ExtensionCenterDialog::setReviewSnapshot(
    const QList<ExtensionRegistryRecord> &records,
    const QStringList &sourceIssueCodes,
    const ExtensionReviewLedgerStoreResult &ledger)
{
    // 复核操作不读取授权账本，因此不能借这条路径改写授权集合：把它当成空集合会让界面
    // 显示"这些扩展没有被授权过"，而实际情况是这次操作根本没有读过授权。
    populate(records, sourceIssueCodes, ledger, m_grants);
}

void ExtensionCenterDialog::setEnablementSnapshot(
    const QList<ExtensionRegistryRecord> &records,
    const QStringList &sourceIssueCodes,
    const ExtensionEnablementLedgerStoreResult &grants)
{
    // 对称地：授权操作不读取复核账本，因此保留现有复核记录。
    populate(records, sourceIssueCodes, m_ledger, grants);
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

void ExtensionCenterDialog::setEnablementBusy(bool busy)
{
    m_enablementBusy = busy;
    for (QPushButton *button : m_enablementButtons) {
        if (button) {
            button->setEnabled(!busy
                && button->property("extensionEnablementEligible").toBool());
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

void ExtensionCenterDialog::showEnablementError(const QString &errorCode)
{
    if (!m_enablementStatus) return;
    // 诊断只能是固定代码。把后端返回的任意文本直接贴到界面上，等于让来源文本决定屏幕上
    // 写着什么。
    const QRegularExpression fixedCode(QStringLiteral("^[a-z0-9][a-z0-9-]{0,95}$"));
    m_enablementStatus->setText(fixedCode.match(errorCode).hasMatch()
        ? QStringLiteral("启用授权未提交：%1").arg(errorCode)
        : QStringLiteral("启用授权未提交：扩展授权状态不可用"));
    m_enablementStatus->setStyleSheet(QStringLiteral(
        "font-size:12px; color:#b42318; background:#fff5f5;"
        "border:1px solid #fecdca; border-radius:7px; padding:8px 10px;"));
}

void ExtensionCenterDialog::populate(
    const QList<ExtensionRegistryRecord> &records,
    const QStringList &sourceIssueCodes,
    const ExtensionReviewLedgerStoreResult &ledger,
    const ExtensionEnablementLedgerStoreResult &grants)
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
    m_grants = grants;
    // 读不出来的授权账本不提供任何授权：一份不完整的集合会让界面显示"这些扩展没有被授权
    // 过"，而实际情况是当前授权未知。反过来，把读不出来的账本里残留的那几条显示出来，会
    // 让界面提供一个撤销动作去收回一条其实无法确认存在的授权。
    if (m_grants.state != ExtensionEnablementLedgerStoreState::Empty
            && m_grants.state != ExtensionEnablementLedgerStoreState::Ready) {
        m_grants.grants.clear();
    }
    rebuildRows();
    std::sort(m_records.begin(), m_records.end(), [](const auto &left, const auto &right) {
        if (left.kind != right.kind) {
            return static_cast<int>(left.kind) < static_cast<int>(right.kind);
        }
        return left.id < right.id;
    });
    m_table->setRowCount(m_rows.size());
    m_reviewButtons.clear();
    m_enablementButtons.clear();
    const bool ledgerUsable = m_ledger.state == ExtensionReviewLedgerStoreState::Empty
        || m_ledger.state == ExtensionReviewLedgerStoreState::Ready;
    // 授权账本读不出来时冻结全部授权动作，包括撤销：在授权集合未知的情况下提交一份"完整
    // 集合"会静默撤销读不出来的那些授权。撤销方向虽然安全，但它把一次篡改表述成用户主动
    // 停用。
    const bool grantLedgerUsable =
        m_grants.state == ExtensionEnablementLedgerStoreState::Empty
        || m_grants.state == ExtensionEnablementLedgerStoreState::Ready;
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

        auto *enablementButton = new QPushButton(this);
        enablementButton->setObjectName(QStringLiteral("extensionEnablementButton"));
        enablementButton->setFixedHeight(28);
        enablementButton->setCursor(Qt::PointingHandCursor);
        enablementButton->setStyleSheet(AppTheme::secondaryButtonStyle());
        if (entry.hasGrant) {
            // 撤销永远可用（只要授权集合读得出来）：内容漂移、复核被撤回、来源消失的扩展
            // 都必须仍然可以收回授权，否则一个被篡改的扩展将永远无法被撤销。
            enablementButton->setText(QStringLiteral("撤销授权"));
            enablementButton->setToolTip(
                QStringLiteral("收回该扩展的启用授权，不删除、不停用、不执行任何内容"));
            enablementButton->setProperty("extensionEnablementEligible",
                                          grantLedgerUsable);
        } else {
            // 授权动作的可点击性只取自呈现层的判定。未复核、不兼容或未安装的扩展不得出现
            // 可点击的授权动作：那份授权会以已认证的形式留在账本里，等门禁出现的那一刻
            // 自动生效，也就是在为未来的内容预先授权。
            const ExtensionEnablementPrompt prompt = enablementPromptFor(row);
            const bool ready =
                prompt.state == ExtensionEnablementPromptState::Ready;
            enablementButton->setText(QStringLiteral("授权启用"));
            enablementButton->setToolTip(ready
                ? QStringLiteral("只记录启用授权；当前不会让任何内容运行")
                : (prompt.state == ExtensionEnablementPromptState::Blocked
                    ? blockReasonLabel(prompt.blockReason)
                    : QStringLiteral("内容无法安全展示，不能授权")));
            enablementButton->setProperty("extensionEnablementEligible",
                                          grantLedgerUsable && ready);
        }
        enablementButton->setEnabled(
            enablementButton->property("extensionEnablementEligible").toBool()
            && !m_enablementBusy);
        connect(enablementButton, &QPushButton::clicked, this,
                [this, row]() { enablementRow(row); });
        m_enablementButtons.append(enablementButton);
        m_table->setItem(row, 9, readOnlyItem(QString()));
        m_table->setCellWidget(row, 9, enablementButton);
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
    if (m_enablementStatus) {
        m_enablementStatus->setStyleSheet(QStringLiteral(
            "font-size:12px; color:#667085; background:#f8fafc;"
            "border:1px solid #eaecf0; border-radius:7px; padding:8px 10px;"));
        switch (m_grants.state) {
        case ExtensionEnablementLedgerStoreState::Empty:
            m_enablementStatus->setText(QStringLiteral(
                "启用授权：尚未授权任何扩展。授权只记录\"你要求运行这份内容\"，当前不会让任何内容运行——"
                "权限、审批、沙箱与恢复门禁尚未完成。"));
            break;
        case ExtensionEnablementLedgerStoreState::Ready:
            m_enablementStatus->setText(QStringLiteral(
                "启用授权：已认证第 %1 代记录。授权绑定确切内容摘要，内容变化后不会延续；"
                "当前仍然不会让任何内容运行。")
                .arg(m_grants.generation));
            break;
        default:
            m_enablementStatus->setText(QStringLiteral(
                "启用授权存储不可用，授权与撤销操作已冻结；不会把故障当成未授权。"));
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
        for (const ExtensionEnablementGrant &grant : m_grants.grants) {
            if (grant.kind == record.kind && grant.id == record.id) {
                row.grant = grant;
                row.hasGrant = true;
                break;
            }
        }
        m_rows.append(row);
    }
    // 来源已消失但仍留有复核记录或启用授权的目标必须各自出现一行，否则那条记录无法被
    // 撤销：一个被删掉来源的扩展会永久留着一份已认证的授权。两类残留合并到同一行，
    // 因为它们指向同一个 (kind, id)。
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
        for (const ExtensionEnablementGrant &grant : m_grants.grants) {
            if (grant.kind == pin.kind && grant.id == pin.id) {
                row.grant = grant;
                row.hasGrant = true;
                break;
            }
        }
        m_rows.append(row);
    }
    for (const ExtensionEnablementGrant &grant : m_grants.grants) {
        bool present = false;
        for (const ReviewRow &row : m_rows) {
            present = present || (row.record.kind == grant.kind
                                  && row.record.id == grant.id);
        }
        if (present) continue;
        ReviewRow row;
        row.grant = grant;
        row.hasGrant = true;
        row.record.kind = grant.kind;
        row.record.id = grant.id;
        row.record.name = QStringLiteral("来源未发现");
        row.record.sourceIdentity = grant.sourceIdentity;
        row.record.contentIdentity = grant.contentIdentity;
        row.record.compatibility = ExtensionCompatibilityState::Unknown;
        row.record.compatibilityReason =
            QStringLiteral("extension-enablement-source-missing");
        row.record.scope = QStringLiteral("user");
        m_rows.append(row);
    }
}

ExtensionEnablementPrompt ExtensionCenterDialog::enablementPromptFor(int row) const
{
    if (row < 0 || row >= m_rows.size()) {
        ExtensionEnablementPrompt rejected;
        rejected.errorCode = QStringLiteral("extension-enablement-row-absent");
        return rejected;
    }
    const ReviewRow &entry = m_rows.at(row);
    if (!entry.hasRecord) {
        // 来源已消失：没有可授权的内容。授权一个不存在的目标等于预先授权将来出现的内容。
        ExtensionEnablementPrompt rejected;
        rejected.errorCode = QStringLiteral("extension-enablement-source-missing");
        return rejected;
    }
    return ExtensionEnablementPresentation::build(
        entry.record, ExtensionCompatibilityPolicy::defaultGrantedCapabilities(),
        entry.hasGrant, entry.hasGrant ? entry.grant.contentIdentity : QString());
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

bool ExtensionCenterDialog::confirmEnablementPrompt(
    const ExtensionEnablementPrompt &prompt)
{
    // Ready 之外的状态一律不提问。Blocked 是可以安全展示的，但它不该带出一个授权动作：
    // 界面在这里只能说明原因。
    if (prompt.state != ExtensionEnablementPromptState::Ready) {
        showEnablementError(prompt.state == ExtensionEnablementPromptState::Blocked
            ? QStringLiteral("extension-enablement-blocked")
            : prompt.errorCode);
        return false;
    }
    QStringList warningLabels;
    for (const ExtensionEnablementWarning warning : prompt.warnings) {
        warningLabels.append(enablementWarningLabel(warning));
    }
    const QString text = QStringList{
        QStringLiteral("授权启用扩展"),
        QString(),
        QStringLiteral("名称：") + prompt.title,
        QStringLiteral("标识：") + prompt.identifier,
        QStringLiteral("类型：") + prompt.kindLabel,
        QStringLiteral("版本：") + prompt.versionLabel,
        QStringLiteral("作用域：") + prompt.scopeLabel,
        QStringLiteral("请求能力：")
            + (prompt.capabilities.isEmpty() ? QStringLiteral("无")
                : prompt.capabilities.join(QStringLiteral(", "))),
        QStringLiteral("来源身份：") + prompt.sourceIdentity,
        QStringLiteral("内容身份：") + prompt.contentIdentity,
        QString(),
        QStringLiteral("风险提示：")
            + (warningLabels.isEmpty() ? QStringLiteral("无")
                                       : warningLabels.join(QStringLiteral("；"))),
        QString(),
        QStringLiteral(
            "被授权的是上面这份确切内容，不是这个名字：渲染之后内容一旦变化，本次授权会失败"
            "而不会套用到新内容上。本操作只记录启用授权，不安装、更新、删除或执行任何扩展，"
            "当前也不会让任何内容运行。")
    }.join(QLatin1Char('\n'));
    QMessageBox box(QMessageBox::Question, QStringLiteral("确认启用授权"), text,
                    QMessageBox::Cancel | QMessageBox::Ok, this);
    box.setTextFormat(Qt::PlainText);
    auto *check = new QCheckBox(QStringLiteral(
        "我已核对完整来源身份和内容身份，要求运行这份内容"), &box);
    box.setCheckBox(check);
    box.button(QMessageBox::Ok)->setEnabled(false);
    connect(check, &QCheckBox::toggled, box.button(QMessageBox::Ok),
            &QAbstractButton::setEnabled);
    box.button(QMessageBox::Ok)->setText(QStringLiteral("确认"));
    box.button(QMessageBox::Cancel)->setText(QStringLiteral("取消"));
    return box.exec() == QMessageBox::Ok && check->isChecked();
}

bool ExtensionCenterDialog::confirmEnablementRevocation(
    const ExtensionRevocationPrompt &prompt)
{
    if (prompt.state != ExtensionRevocationPromptState::Ready) {
        showEnablementError(prompt.errorCode);
        return false;
    }
    const QString text = QStringList{
        QStringLiteral("撤销启用授权"),
        QString(),
        QStringLiteral("名称：") + prompt.title,
        QStringLiteral("标识：") + prompt.identifier,
        QStringLiteral("类型：") + prompt.kindLabel,
        prompt.targetAbsent
            ? QStringLiteral("来源状态：已消失，撤销的是一份不再存在的目标")
            : QStringLiteral("来源状态：仍在清单中"),
        QString(),
        QStringLiteral(
            "本操作只收回该条启用授权，不删除、不停用、不更新、不执行任何内容。"
            "人工复核证据保持不变。")
    }.join(QLatin1Char('\n'));
    QMessageBox box(QMessageBox::Question, QStringLiteral("确认撤销启用授权"), text,
                    QMessageBox::Cancel | QMessageBox::Ok, this);
    box.setTextFormat(Qt::PlainText);
    auto *check = new QCheckBox(QStringLiteral("我已核对要撤销授权的完整身份"), &box);
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

void ExtensionCenterDialog::enablementRow(int row)
{
    if (m_enablementBusy || row < 0 || row >= m_rows.size()) return;
    const ReviewRow &item = m_rows.at(row);
    ExtensionEnablementRequest request;
    request.kind = item.record.kind;
    request.id = item.record.id;
    if (item.hasGrant) {
        // 撤销按 (kind, id) 进行，不带摘要：内容已经变化、来源已经消失的授权同样必须
        // 可以被收回。
        request.action = ExtensionEnablementAction::Disable;
        const ExtensionRevocationPrompt prompt =
            ExtensionEnablementPresentation::buildRevocation(
                item.record.kind, item.record.id,
                item.hasRecord ? &item.record : nullptr);
        if (!confirmEnablementRevocation(prompt)) return;
    } else {
        const ExtensionEnablementPrompt prompt = enablementPromptFor(row);
        if (!confirmEnablementPrompt(prompt)) return;
        request.action = ExtensionEnablementAction::Enable;
        // 回传的摘要就是屏幕上展示过的摘要，因此渲染之后发生的漂移会让授予失败，而不是
        // 把这个决定悄悄套用到新内容上。
        request.reviewedSourceIdentity = prompt.reviewedSourceIdentity;
        request.reviewedContentIdentity = prompt.reviewedContentIdentity;
    }
    emit enablementRequested(request);
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
