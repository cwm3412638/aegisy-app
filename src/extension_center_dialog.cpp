#include "extension_center_dialog.h"

#include "app_theme.h"
#include "extension_compatibility_policy.h"
#include "extension_display_safety.h"

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

    m_table = new QTableWidget(0, 12, this);
    m_table->setObjectName(QStringLiteral("extensionCenterTable"));
    // 最后一列不叫"删除"：这个动作只收回两份账本里的记录，磁盘上的内容一个字节都不动。
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("名称 / ID"), QStringLiteral("类型"), QStringLiteral("版本"),
        QStringLiteral("作用域"), QStringLiteral("请求能力"), QStringLiteral("来源"),
        QStringLiteral("信任"), QStringLiteral("兼容状态"), QStringLiteral("人工复核"),
        QStringLiteral("启用授权"), QStringLiteral("收回记录"),
        QStringLiteral("检查更新")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 11; ++column) {
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

    m_removalStatus = new QLabel(this);
    m_removalStatus->setObjectName(QStringLiteral("extensionRemovalStatus"));
    m_removalStatus->setWordWrap(true);
    m_removalStatus->setStyleSheet(QStringLiteral(
        "font-size:12px; color:#667085; background:#f8fafc;"
        "border:1px solid #eaecf0; border-radius:7px; padding:8px 10px;"));
    root->addWidget(m_removalStatus);

    // 披露区。它与上面的表格分开，因为它回答的是另一个问题：上面是"已经在这台机器上的
    // 扩展"，这里是"这个包里有什么"。放进同一张表会让一个尚未导入的包看起来已经在列。
    m_importButton = new QPushButton(QStringLiteral("披露扩展包内容"), this);
    m_importButton->setObjectName(QStringLiteral("extensionImportDiscloseButton"));
    m_importButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    // 按钮不叫"导入"：它只读出包里有什么。叫导入会让人以为点完之后磁盘上多了一份内容。
    m_importButton->setToolTip(QStringLiteral(
        "读出一个扩展包里的每一个组件及其请求的能力；不导入、不安装、不写入磁盘"));
    connect(m_importButton, &QPushButton::clicked, this, [this]() {
        if (m_importBusy) return;
        emit bundleDisclosureRequested();
    });
    root->addWidget(m_importButton);

    m_importTable = new QTableWidget(0, 5, this);
    m_importTable->setObjectName(QStringLiteral("extensionImportTable"));
    m_importTable->setHorizontalHeaderLabels({
        QStringLiteral("组件 / ID"), QStringLiteral("类型"),
        QStringLiteral("声明类型"), QStringLiteral("请求能力"),
        QStringLiteral("内容摘要")});
    m_importTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 5; ++column) {
        m_importTable->horizontalHeader()->setSectionResizeMode(
            column, QHeaderView::ResizeToContents);
    }
    m_importTable->verticalHeader()->hide();
    m_importTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_importTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_importTable->setMaximumHeight(150);
    root->addWidget(m_importTable);

    m_importStatus = new QLabel(this);
    m_importStatus->setObjectName(QStringLiteral("extensionImportStatus"));
    m_importStatus->setWordWrap(true);
    m_importStatus->setStyleSheet(QStringLiteral(
        "font-size:12px; color:#667085; background:#f8fafc;"
        "border:1px solid #eaecf0; border-radius:7px; padding:8px 10px;"));
    m_importStatus->setText(QStringLiteral(
        "尚未披露任何扩展包。披露只读出包里的内容，不导入、不安装、不写入磁盘。"));
    root->addWidget(m_importStatus);

    // 更新区。它与披露区分开，因为它回答的是另一个问题：披露问"这个包里有什么"，更新问
    // "这个包能不能替换已经在列的那一份"。当前这个问题的答案永远是不能，而屏幕必须说清楚
    // 是缺什么，不能只把动作灰掉——只灰掉按钮会让人以为是自己这个包有问题而反复重做包。
    m_updateTable = new QTableWidget(0, 3, this);
    m_updateTable->setObjectName(QStringLiteral("extensionUpdateTable"));
    m_updateTable->setHorizontalHeaderLabels({
        QStringLiteral("证据项"), QStringLiteral("结论"), QStringLiteral("说明")});
    m_updateTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    for (int column = 0; column < 2; ++column) {
        m_updateTable->horizontalHeader()->setSectionResizeMode(
            column, QHeaderView::ResizeToContents);
    }
    m_updateTable->verticalHeader()->hide();
    m_updateTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_updateTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_updateTable->setMaximumHeight(150);
    root->addWidget(m_updateTable);

    m_updateStatus = new QLabel(this);
    m_updateStatus->setObjectName(QStringLiteral("extensionUpdateStatus"));
    m_updateStatus->setWordWrap(true);
    m_updateStatus->setStyleSheet(QStringLiteral(
        "font-size:12px; color:#667085; background:#f8fafc;"
        "border:1px solid #eaecf0; border-radius:7px; padding:8px 10px;"));
    m_updateStatus->setText(QStringLiteral(
        "尚未检查任何更新。检查只读出候选包并列出证据，不替换当前生效的版本，也不授予执行权。"));
    root->addWidget(m_updateStatus);

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

void ExtensionCenterDialog::setRemovalBusy(bool busy)
{
    m_removalBusy = busy;
    for (QPushButton *button : m_removalButtons) {
        if (button) {
            button->setEnabled(!busy
                && button->property("extensionRemovalEligible").toBool());
        }
    }
}

void ExtensionCenterDialog::setRemovalSnapshot(
    const QList<ExtensionRegistryRecord> &records,
    const QStringList &sourceIssueCodes,
    const ExtensionReviewLedgerStoreResult &ledger,
    const ExtensionEnablementLedgerStoreResult &grants)
{
    // 移除确实读过并写过两份账本，因此它的刷新替换两者。这与复核/授权刷新只带自己那一半
    // 相反，而那条规则的理由是"没读过的账本不能被报成空的"——这里两份都读过。
    populate(records, sourceIssueCodes, ledger, grants);
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

void ExtensionCenterDialog::showRemovalError(const QString &errorCode)
{
    if (!m_removalStatus) return;
    const QRegularExpression fixedCode(QStringLiteral("^[a-z0-9][a-z0-9-]{0,95}$"));
    m_removalStatus->setText(fixedCode.match(errorCode).hasMatch()
        ? QStringLiteral("记录未完全收回：%1").arg(errorCode)
        : QStringLiteral("记录未完全收回：扩展记录状态不可用"));
    m_removalStatus->setStyleSheet(QStringLiteral(
        "font-size:12px; color:#b42318; background:#fff5f5;"
        "border:1px solid #fecdca; border-radius:7px; padding:8px 10px;"));
}

void ExtensionCenterDialog::setImportBusy(bool busy)
{
    m_importBusy = busy;
    if (m_importButton) m_importButton->setEnabled(!busy);
}

void ExtensionCenterDialog::setImportDisclosure(
    const ExtensionImportDisclosure &disclosure)
{
    if (!m_importTable || !m_importStatus) return;
    // 每一次披露都完整替换上一次的组件列表。留着上一次的行会让一次失败的读取看起来在描述
    // 这一次选的那个包，而屏幕上那些组件属于另一个包。
    m_importTable->setRowCount(0);

    QStringList lines;
    lines.append(ExtensionImportPresentation::stateLabel(disclosure.state));
    // 这两句必须始终在场，而不是只在成功时出现：一次被拒绝的披露同样什么都没导入，而人
    // 需要知道自己不必去清理任何东西。
    if (!disclosure.importsBundle && !disclosure.writesToDisk) {
        lines.append(QStringLiteral(
            "本次披露没有导入、安装或启用任何内容，也没有向磁盘写入任何字节。"));
    }
    if (!disclosure.title.isEmpty()) {
        lines.append(QStringLiteral("包：%1（%2）")
                         .arg(disclosure.title, disclosure.identifier));
    }
    if (!disclosure.versionLabel.isEmpty()) {
        lines.append(QStringLiteral("版本：%1").arg(disclosure.versionLabel));
    }
    if (!disclosure.sourceFingerprint.isEmpty()) {
        lines.append(QStringLiteral("来源指纹：%1").arg(disclosure.sourceFingerprint));
    }
    if (!disclosure.contentFingerprint.isEmpty()) {
        lines.append(QStringLiteral("内容指纹：%1").arg(disclosure.contentFingerprint));
    }
    if (disclosure.anyBeyondReadOnly) {
        lines.append(QStringLiteral(
            "包内至少一个组件请求了写入或执行能力；这些能力当前不会被授予。"));
    }
    // 诊断只能是固定代码。把读取层返回的任意文本直接贴到界面上，等于让包里的内容决定
    // 屏幕上写着什么。
    const QRegularExpression fixedCode(QStringLiteral("^[a-z0-9][a-z0-9-]{0,95}$"));
    if (!disclosure.errorCode.isEmpty()) {
        lines.append(fixedCode.match(disclosure.errorCode).hasMatch()
            ? QStringLiteral("诊断：%1").arg(disclosure.errorCode)
            : QStringLiteral("诊断：扩展包状态不可用"));
    }
    m_importStatus->setText(lines.join(QStringLiteral("\n")));
    const bool alarming =
        disclosure.state == ExtensionImportDisclosureState::FailedClosed
        || disclosure.state == ExtensionImportDisclosureState::Unpresentable
        || disclosure.state == ExtensionImportDisclosureState::Unreadable;
    m_importStatus->setStyleSheet(alarming
        ? QStringLiteral("font-size:12px; color:#b42318; background:#fff5f5;"
                         "border:1px solid #fecdca; border-radius:7px; padding:8px 10px;")
        : QStringLiteral("font-size:12px; color:#667085; background:#f8fafc;"
                         "border:1px solid #eaecf0; border-radius:7px; padding:8px 10px;"));

    // 失败关闭仍然列出全部组件，包括那个不支持的组件：隐藏证据会让没人能判断这个包到底
    // 想做什么。能力逐行逐组件列出，这里不做任何整包汇总。
    for (const ExtensionComponentPreview &item : disclosure.components) {
        const int row = m_importTable->rowCount();
        m_importTable->insertRow(row);
        const QString name = item.displayName.isEmpty()
            ? item.identifier : item.displayName;
        m_importTable->setItem(row, 0, readOnlyItem(
            QStringLiteral("%1\n%2").arg(name, item.identifier)));
        m_importTable->setItem(row, 1, readOnlyItem(
            item.unsupported
                ? QStringLiteral("%1（不支持，导入失败关闭）").arg(item.kindLabel)
                : item.kindLabel));
        m_importTable->setItem(row, 2, readOnlyItem(item.declaredType));
        m_importTable->setItem(row, 3, readOnlyItem(
            item.capabilities.isEmpty()
                ? QStringLiteral("未请求任何能力")
                : (item.beyondReadOnly
                       ? QStringLiteral("%1（越出只读边界）")
                             .arg(item.capabilities.join(QStringLiteral("、")))
                       : item.capabilities.join(QStringLiteral("、")))));
        m_importTable->setItem(row, 4, readOnlyItem(item.contentFingerprint));
    }
}

void ExtensionCenterDialog::setUpdateBusy(bool busy)
{
    m_updateBusy = busy;
    for (QPushButton *button : m_updateButtons) {
        if (button) button->setEnabled(!busy);
    }
}

void ExtensionCenterDialog::setUpdatePlan(const ExtensionUpdatePlan &plan)
{
    if (!m_updateTable || !m_updateStatus) return;
    // 每一次检查都完整替换上一次的证据表。留着上一次的行会让一次失败的检查看起来在描述
    // 这一次选的那个候选包，而屏幕上那些证据属于另一份内容。
    m_updateTable->setRowCount(0);

    QStringList lines;
    lines.append(ExtensionUpdatePresentation::stateLabel(plan.state));
    // 这三句必须始终在场，而不是只在被拒绝时出现：即使有一天判定通过，暂存也仍然不替换、
    // 不授权，而"更新已暂存"很容易被读成"新版本正在运行"。
    if (plan.stagesOnly && !plan.replacesActiveVersion && !plan.grantsExecution) {
        lines.append(QStringLiteral(
            "本次检查没有替换当前生效的版本，没有授予任何执行权，也没有向磁盘写入任何字节。"));
    }
    if (!plan.title.isEmpty()) {
        lines.append(QStringLiteral("扩展：%1（%2）")
                         .arg(plan.title, plan.identifier));
    }
    if (!plan.activeVersionLabel.isEmpty()
            || !plan.candidateVersionLabel.isEmpty()) {
        lines.append(QStringLiteral("当前版本：%1 → 候选版本：%2")
                         .arg(plan.activeVersionLabel.isEmpty()
                                  ? QStringLiteral("版本不可展示")
                                  : plan.activeVersionLabel,
                              plan.candidateVersionLabel.isEmpty()
                                  ? QStringLiteral("尚无候选")
                                  : plan.candidateVersionLabel));
    }
    // 两份指纹都列出：人要能看出这确实是两份不同的内容，而不是同一份内容换了个版本号。
    if (!plan.activeFingerprint.isEmpty()) {
        lines.append(QStringLiteral("当前内容指纹：%1").arg(plan.activeFingerprint));
    }
    if (!plan.candidateFingerprint.isEmpty()) {
        lines.append(QStringLiteral("候选内容指纹：%1").arg(plan.candidateFingerprint));
    }
    // 降级必须被说出来，不能只靠两个版本号让人自己比：降级会重新引入已经被修复过的内容。
    if (plan.downgrade) {
        lines.append(QStringLiteral(
            "候选版本低于当前版本；降级会重新引入当前版本里已经被修复的内容。"));
    }
    // 这一句是这一屏存在的理由：问题不在这个包上，人不必反复重做包。
    if (plan.anyUnverifiable) {
        lines.append(QStringLiteral(
            "有证据项在这台机器上没有任何人能核查；这不是这个包的问题，重做包不会让它变成"
            "可核查。"));
    }
    const QRegularExpression fixedCode(QStringLiteral("^[a-z0-9][a-z0-9-]{0,95}$"));
    if (!plan.errorCode.isEmpty()) {
        lines.append(fixedCode.match(plan.errorCode).hasMatch()
            ? QStringLiteral("诊断：%1").arg(plan.errorCode)
            : QStringLiteral("诊断：候选包状态不可用"));
    }
    m_updateStatus->setText(lines.join(QStringLiteral("\n")));
    const bool alarming = plan.state == ExtensionUpdatePlanState::Blocked
        || plan.state == ExtensionUpdatePlanState::Unpresentable;
    m_updateStatus->setStyleSheet(alarming
        ? QStringLiteral("font-size:12px; color:#b42318; background:#fff5f5;"
                         "border:1px solid #fecdca; border-radius:7px; padding:8px 10px;")
        : QStringLiteral("font-size:12px; color:#667085; background:#f8fafc;"
                         "border:1px solid #eaecf0; border-radius:7px; padding:8px 10px;"));

    // 逐项列出证据，齐备的项也列出：人有权看到这次更新凭什么成立，也有权看到它凭什么不
    // 成立。"没有人能核查"与"核查失败"在这里必须是两句不同的话。
    for (const ExtensionUpdateEvidenceLine &item : plan.evidence) {
        const int row = m_updateTable->rowCount();
        m_updateTable->insertRow(row);
        m_updateTable->setItem(row, 0, readOnlyItem(item.label));
        m_updateTable->setItem(row, 1, readOnlyItem(
            item.established
                ? QStringLiteral("已确立")
                : (item.unverifiable
                       ? QStringLiteral("无人可核查")
                       : QStringLiteral("核查未通过"))));
        m_updateTable->setItem(row, 2, readOnlyItem(
            item.diagnostic.isEmpty()
                ? QString()
                : (fixedCode.match(item.diagnostic).hasMatch()
                       ? item.diagnostic
                       : QStringLiteral("说明不可展示"))));
    }
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
    m_removalButtons.clear();
    m_updateButtons.clear();
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

        auto *removalButton = new QPushButton(this);
        removalButton->setObjectName(QStringLiteral("extensionRemovalButton"));
        removalButton->setFixedHeight(28);
        removalButton->setCursor(Qt::PointingHandCursor);
        removalButton->setStyleSheet(AppTheme::secondaryButtonStyle());
        removalButton->setText(QStringLiteral("收回记录"));
        const ExtensionRemovalPlan plan = removalPlanFor(row);
        const bool planReady = plan.state == ExtensionRemovalPlanState::Ready;
        // 移除动作不设门禁：内容漂移、复核被撤回、来源已消失的目标都必须仍然可以被收回，
        // 否则一个被篡改的扩展将永远留着一份已认证的授权。可点击性只取决于三件事：两份
        // 账本都读得出来（控制器在任一份不可读时拒绝，而在授权未知的情况下声称已收回授权
        // 是这条路径最不该做的事），判定层认这次移除，以及确实有东西可以收回。
        const bool hasSomethingToWithdraw = entry.hasPin || entry.hasGrant;
        const bool removalEligible = ledgerUsable && grantLedgerUsable
            && planReady && hasSomethingToWithdraw;
        removalButton->setToolTip(
            !ledgerUsable || !grantLedgerUsable
                ? QStringLiteral("账本不可读，无法确认收回结果")
                : (!planReady
                    ? QStringLiteral("目标无法安全展示，不能收回")
                    : (hasSomethingToWithdraw
                        ? QStringLiteral(
                            "收回该扩展的启用授权与人工复核记录；不删除磁盘上的任何内容")
                        : QStringLiteral("没有可收回的记录"))));
        removalButton->setProperty("extensionRemovalEligible", removalEligible);
        removalButton->setEnabled(removalEligible && !m_removalBusy);
        connect(removalButton, &QPushButton::clicked, this,
                [this, row]() { removalRow(row); });
        m_removalButtons.append(removalButton);
        m_table->setItem(row, 10, readOnlyItem(QString()));
        m_table->setCellWidget(row, 10, removalButton);

        auto *updateButton = new QPushButton(this);
        updateButton->setObjectName(QStringLiteral("extensionUpdateButton"));
        updateButton->setFixedHeight(28);
        updateButton->setCursor(Qt::PointingHandCursor);
        updateButton->setStyleSheet(AppTheme::secondaryButtonStyle());
        updateButton->setText(QStringLiteral("检查更新"));
        // 检查更新不设门禁：它只读出候选包并列出证据，不改动任何记录，也不写盘。把它灰掉
        // 恰恰是这一屏要避免的那件事——人会以为是自己这个包有问题而反复重做包，而真正缺的
        // 是这台机器上没有签名权威。谁不能被更新，由证据表逐项说清楚。
        updateButton->setToolTip(QStringLiteral(
            "读出一份候选包并逐项列出证据；不替换当前版本、不授予执行权、不写入磁盘"));
        updateButton->setEnabled(!m_updateBusy);
        connect(updateButton, &QPushButton::clicked, this,
                [this, row]() { updateRow(row); });
        m_updateButtons.append(updateButton);
        m_table->setItem(row, 11, readOnlyItem(QString()));
        m_table->setCellWidget(row, 11, updateButton);
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
    if (m_removalStatus) {
        m_removalStatus->setStyleSheet(QStringLiteral(
            "font-size:12px; color:#667085; background:#f8fafc;"
            "border:1px solid #eaecf0; border-radius:7px; padding:8px 10px;"));
        const bool bothUsable = ledgerUsable && grantLedgerUsable;
        m_removalStatus->setText(bothUsable
            ? QStringLiteral(
                "收回记录：先收回启用授权，再收回人工复核记录；不可变身份被保留。"
                "本操作不删除磁盘上的任何内容——内容仍在原处，重新复核并授权后会重新可用。")
            : QStringLiteral(
                "复核或授权存储不可用，收回记录已冻结；不会在授权状态未知时声称已经收回。"));
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

ExtensionRemovalPlan ExtensionCenterDialog::removalPlanFor(int row) const
{
    if (row < 0 || row >= m_rows.size()) {
        ExtensionRemovalPlan rejected;
        rejected.errorCode = QStringLiteral("extension-removal-row-absent");
        return rejected;
    }
    const ReviewRow &entry = m_rows.at(row);
    // 来源已消失时把记录指针留空：移除仍然进行，呈现层会据此说明目标已不存在。这次移除
    // 是否成立由呈现层转述判定层的结论，这里不另判一遍。
    return ExtensionLifecyclePresentation::buildRemoval(
        entry.record.kind, entry.record.id,
        entry.hasRecord ? &entry.record : nullptr,
        entry.hasPin, entry.hasGrant);
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

bool ExtensionCenterDialog::confirmRemoval(const ExtensionRemovalPlan &plan)
{
    if (plan.state != ExtensionRemovalPlanState::Ready) {
        showRemovalError(plan.errorCode);
        return false;
    }
    QStringList withdrawn;
    if (plan.withdrawsGrant) withdrawn.append(QStringLiteral("启用授权"));
    if (plan.withdrawsReview) withdrawn.append(QStringLiteral("人工复核记录"));
    const QString text = QStringList{
        QStringLiteral("收回扩展记录"),
        QString(),
        QStringLiteral("名称：") + plan.title,
        QStringLiteral("标识：") + plan.identifier,
        QStringLiteral("类型：") + plan.kindLabel,
        plan.targetAbsent
            ? QStringLiteral("来源状态：已消失，收回的是一份不再存在的目标留下的记录")
            : QStringLiteral("来源状态：仍在清单中"),
        QStringLiteral("来源身份：")
            + (plan.sourceIdentity.isEmpty() ? QStringLiteral("不可用")
                                             : plan.sourceIdentity),
        QStringLiteral("内容身份：")
            + (plan.contentIdentity.isEmpty() ? QStringLiteral("不可用")
                                              : plan.contentIdentity),
        QString(),
        QStringLiteral("本次收回：")
            + (withdrawn.isEmpty() ? QStringLiteral("没有可收回的记录")
                                   : withdrawn.join(QStringLiteral("、"))),
        QStringLiteral("保留身份：")
            + (plan.retainsIdentity
                ? (plan.retainedIdentity.isEmpty() ? QStringLiteral("是")
                                                   : plan.retainedIdentity)
                : QStringLiteral("否")),
        QString(),
        // 这一句是这个对话框存在的理由。把它说成一次删除会让人以为磁盘上那份内容已经
        // 消失，于是停止清理，而内容还在原处，重新复核并授权后会重新可用。
        plan.removesSourceContent
            ? QStringLiteral("本操作会删除磁盘上的内容。")
            : QStringLiteral(
                "本操作不删除磁盘上的任何内容：只收回上面列出的记录。内容仍留在原处，"
                "重新经过人工复核并重新授权后会重新可用。"),
        QStringLiteral(
            "先收回启用授权，再收回人工复核记录；任何中间失败都会停在\"没有授权、"
            "复核记录尚存\"上，并且会被明确报告为未完全收回。")
    }.join(QLatin1Char('\n'));
    QMessageBox box(QMessageBox::Question, QStringLiteral("确认收回扩展记录"), text,
                    QMessageBox::Cancel | QMessageBox::Ok, this);
    box.setTextFormat(Qt::PlainText);
    auto *check = new QCheckBox(QStringLiteral(
        "我已核对要收回记录的完整身份，并知道磁盘内容不会被删除"), &box);
    box.setCheckBox(check);
    box.button(QMessageBox::Ok)->setEnabled(false);
    connect(check, &QCheckBox::toggled, box.button(QMessageBox::Ok),
            &QAbstractButton::setEnabled);
    box.button(QMessageBox::Ok)->setText(QStringLiteral("确认"));
    box.button(QMessageBox::Cancel)->setText(QStringLiteral("取消"));
    return box.exec() == QMessageBox::Ok && check->isChecked();
}

void ExtensionCenterDialog::removalRow(int row)
{
    if (m_removalBusy || row < 0 || row >= m_rows.size()) return;
    const ExtensionRemovalPlan plan = removalPlanFor(row);
    if (!confirmRemoval(plan)) return;
    // 只发 (kind, id)：被收回的内容摘要可能已经不可读，而收回必须仍然能够完成。绑定摘要
    // 会让一个被篡改的扩展永远留着一份已认证的授权。
    emit removalRequested(m_rows.at(row).record.kind, m_rows.at(row).record.id);
}

void ExtensionCenterDialog::updateRow(int row)
{
    if (m_updateBusy || row < 0 || row >= m_rows.size()) return;
    // 检查更新不需要确认对话框：它不改动任何东西。要求确认会训练人对确认框视而不见，
    // 而真正需要确认的是复核、授权与收回。
    emit updatePlanRequested(m_rows.at(row).record.kind, m_rows.at(row).record.id);
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
