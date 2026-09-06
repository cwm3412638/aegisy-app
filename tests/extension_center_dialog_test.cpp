#include "extension_center_dialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaMethod>
#include <QPixmap>
#include <QPushButton>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>

namespace {

ExtensionRegistryRecord record(ExtensionKind kind, const QString &id, QChar fill)
{
    ExtensionRegistryRecord value;
    value.kind = kind;
    value.id = id;
    value.name = id;
    value.version = QStringLiteral("1.0.0");
    value.sourceKind = kind == ExtensionKind::CodexPlugin
        ? ExtensionSourceKind::CodexCli
        : (kind == ExtensionKind::Mcp
            ? ExtensionSourceKind::ToolConfiguration
            : ExtensionSourceKind::LocalDirectory);
    value.sourceIdentity = QStringLiteral("extension-source:sha256:") + QString(64, fill);
    value.contentIdentity = QStringLiteral("extension-content:sha256:") + QString(64, fill);
    value.compatibilityReason = QStringLiteral("compatibility-unverified");
    value.scope = QStringLiteral("user");
    value.installed = true;
    return value;
}

// 只有已安装、已复核且兼容的记录才允许出现可点击的授权动作。
ExtensionRegistryRecord grantable(ExtensionKind kind, const QString &id, QChar fill)
{
    ExtensionRegistryRecord value = record(kind, id, fill);
    value.trust = ExtensionTrustState::Verified;
    value.compatibility = ExtensionCompatibilityState::Compatible;
    value.compatibilityReason.clear();
    return value;
}

bool expect(bool condition, const char *message)
{
    if (!condition) QTextStream(stderr) << message << Qt::endl;
    return condition;
}

// 打开下一个模态确认框，检查它是不是纯文本、默认拒绝、并且展示了完整身份，然后取消它。
struct PromptInspection {
    bool seen = false;
    bool plainDefaultDeny = false;
    QString text;
};

void inspectNextPrompt(PromptInspection *result, QPushButton *button)
{
    QTimer inspector;
    inspector.setInterval(1);
    QObject::connect(&inspector, &QTimer::timeout, [result, &inspector]() {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box) return;
        const QCheckBox *check = box->checkBox();
        const QAbstractButton *okButton = box->button(QMessageBox::Ok);
        result->seen = true;
        result->plainDefaultDeny = box->textFormat() == Qt::PlainText
            && check && !check->isChecked()
            && okButton && !okButton->isEnabled();
        result->text = box->text();
        box->reject();
        inspector.stop();
    });
    inspector.start();
    button->click();
}

ExtensionComponentPreview component(ExtensionComponentKind kind,
                                    const QString &id,
                                    const QString &declaredType,
                                    const QStringList &capabilities,
                                    bool beyondReadOnly,
                                    bool unsupported)
{
    ExtensionComponentPreview item;
    item.kind = kind;
    item.identifier = id;
    item.displayName = id;
    item.kindLabel = ExtensionImportPreviewBuilder::componentKindLabel(kind);
    item.capabilities = capabilities;
    item.beyondReadOnly = beyondReadOnly;
    item.unsupported = unsupported;
    item.declaredType = declaredType;
    item.contentFingerprint = QStringLiteral("aabbccdd");
    return item;
}

// 备份浏览夹具：id 用暂存域语法内的真实形状；完整条目带规范化时间戳与重算格式的
// 清单身份，损坏条目没有时间戳（清单结构校验失败时没有可信时间可读）。
ExtensionStagingBackupListEntry backupEntry(const QString &subject,
                                            const QString &backupId,
                                            bool intact)
{
    ExtensionStagingBackupListEntry entry;
    entry.backupId = backupId;
    entry.subject = subject;
    entry.manifestIdentity =
        QStringLiteral("extension-staging-backup-manifest:sha256:")
        + QString(64, intact ? QLatin1Char('f') : QLatin1Char('e'));
    if (intact) {
        entry.createdAt = QDateTime::fromString(
            QStringLiteral("2026-09-05T10:20:30.000Z"), Qt::ISODateWithMs);
        entry.verification =
            ExtensionStagingBackupEntryVerification::ListedIntact;
    } else {
        entry.verification =
            ExtensionStagingBackupEntryVerification::ListedCorrupt;
        entry.verificationIssue = QStringLiteral(
            "extension-staging-inventory-entry-manifest-invalid");
    }
    return entry;
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    ExtensionCenterDialog dialog({
        record(ExtensionKind::CodexPlugin, QStringLiteral("plugin.one"), QLatin1Char('a')),
        record(ExtensionKind::Skill, QStringLiteral("skill.one"), QLatin1Char('b')),
        record(ExtensionKind::Mcp, QStringLiteral("mcp.one"), QLatin1Char('c')),
    }, {QStringLiteral("mcp-source-unavailable")});
    auto *table = dialog.findChild<QTableWidget *>(QStringLiteral("extensionCenterTable"));
    auto *search = dialog.findChild<QLineEdit *>(QStringLiteral("extensionCenterSearch"));
    auto *filter = dialog.findChild<QComboBox *>(QStringLiteral("extensionCenterKindFilter"));
    if (!expect(table && search && filter && table->rowCount() == 3,
                "extension center did not render three sources")) return 1;
    for (int row = 0; row < table->rowCount(); ++row) {
        const QTableWidgetItem *item = table->item(row, 0);
        QString serialized;
        for (int column = 0; column < table->columnCount(); ++column) {
            const QTableWidgetItem *cell = table->item(row, column);
            if (!expect(cell && !(cell->flags() & Qt::ItemIsEditable)
                            && !(cell->flags() & Qt::ItemIsUserCheckable),
                        "extension center exposed an editable table item")) return 1;
            serialized += cell->text();
            for (int role = Qt::UserRole; role <= Qt::UserRole + 12; ++role) {
                serialized += cell->data(role).toString();
            }
        }
        if (!expect(!serialized.contains(QStringLiteral("/Users/"))
                        && !serialized.contains(QStringLiteral("token="))
                        && !serialized.contains(QStringLiteral("command"))
                        && !serialized.contains(QStringLiteral("authority")),
                    "extension center item roles contain unsafe source data")) return 1;
    }
    const QList<QPushButton *> buttons = dialog.findChildren<QPushButton *>();
    int reviewButtons = 0;
    int enablementButtons = 0;
    int closeButtons = 0;
    for (QPushButton *button : buttons) {
        reviewButtons += button->objectName() == QStringLiteral("extensionReviewButton");
        enablementButtons +=
            button->objectName() == QStringLiteral("extensionEnablementButton");
        closeButtons += button->text() == QStringLiteral("关闭");
    }
    if (!expect(reviewButtons == 3 && enablementButtons == 3 && closeButtons == 1,
                "extension center review controls are incomplete")) return 1;
    const QList<QPushButton *> reviewControls = dialog.findChildren<QPushButton *>(
        QStringLiteral("extensionReviewButton"));
    if (!expect(!reviewControls.isEmpty() && !reviewControls.first()->isEnabled(),
                "invalid review ledger did not freeze review actions")) return 1;
    // 授权账本默认是 Invalid，因此每一个授权动作都必须冻结：把故障当成"未授权"会让界面
    // 邀请人在授权状态未知的情况下提交一份"完整集合"。
    const QList<QPushButton *> grantControls = dialog.findChildren<QPushButton *>(
        QStringLiteral("extensionEnablementButton"));
    for (QPushButton *control : grantControls) {
        if (!expect(!control->isEnabled(),
                    "invalid grant ledger did not freeze enablement actions")) return 1;
    }
    search->setText(QStringLiteral("skill.one"));
    QCoreApplication::processEvents();
    if (!expect(!table->isRowHidden(1) && table->isRowHidden(0)
                    && table->isRowHidden(2),
                "extension center search did not isolate the Skill")) return 1;
    search->clear();
    filter->setCurrentIndex(filter->findData(static_cast<int>(ExtensionKind::Mcp)));
    QCoreApplication::processEvents();
    if (!expect(!table->isRowHidden(2) && table->isRowHidden(0)
                    && table->isRowHidden(1),
                "extension center kind filter did not isolate MCP")) return 1;

    ExtensionReviewLedgerStoreResult readyLedger;
    readyLedger.state = ExtensionReviewLedgerStoreState::Ready;
    readyLedger.generation = 1;
    ExtensionReviewPin stalePin;
    stalePin.kind = ExtensionKind::Skill;
    stalePin.id = QStringLiteral("missing.skill");
    stalePin.sourceIdentity = QStringLiteral("extension-source:sha256:")
        + QString(64, QLatin1Char('d'));
    stalePin.contentIdentity = QStringLiteral("extension-content:sha256:")
        + QString(64, QLatin1Char('e'));
    readyLedger.pins = {stalePin};
    ExtensionCenterDialog reviewDialog({
        record(ExtensionKind::CodexPlugin, QStringLiteral("plugin.one"), QLatin1Char('a')),
        record(ExtensionKind::Skill, QStringLiteral("skill.one"), QLatin1Char('b')),
        record(ExtensionKind::Mcp, QStringLiteral("mcp.one"), QLatin1Char('c')),
    }, {}, readyLedger);
    const QString screenshotPath = QString::fromLocal8Bit(
        qgetenv("AEGISY_EXTENSION_CENTER_SCREENSHOT"));
    if (!screenshotPath.isEmpty()) {
        reviewDialog.show();
        QCoreApplication::processEvents();
        if (!reviewDialog.grab().save(screenshotPath)) {
            return expect(false, "extension center screenshot could not be saved") ? 0 : 1;
        }
        reviewDialog.hide();
    }
    auto *reviewTable = reviewDialog.findChild<QTableWidget *>(
        QStringLiteral("extensionCenterTable"));
    const QList<QPushButton *> enabledReviews = reviewDialog.findChildren<QPushButton *>(
        QStringLiteral("extensionReviewButton"));
    if (!expect(reviewTable && reviewTable->rowCount() == 4
                    && enabledReviews.size() == 4
                    && enabledReviews.last()->text() == QStringLiteral("撤销审核")
                    && enabledReviews.last()->isEnabled(),
                "stale review pin has no revocation row")) return 1;

    int emitted = 0;
    QObject::connect(&reviewDialog, &ExtensionCenterDialog::reviewRequested,
                     [&emitted](const ExtensionReviewRequest &) { ++emitted; });
    PromptInspection reviewPrompt;
    inspectNextPrompt(&reviewPrompt, enabledReviews.at(1));
    if (!expect(reviewPrompt.seen && reviewPrompt.plainDefaultDeny
                    && reviewPrompt.text.contains(QStringLiteral("来源身份："))
                    && reviewPrompt.text.contains(QStringLiteral("内容身份：")),
                "review confirmation is not exact/plain/default-deny")
            || !expect(emitted == 0, "cancelled review emitted a request")) return 1;

    // ---- 启用授权 ----
    // 未复核的记录在授权账本可用时仍然不得出现可点击的授权动作：那份授权会以已认证的
    // 形式留在账本里，等复核出现的那一刻自动生效。
    ExtensionEnablementLedgerStoreResult emptyGrants;
    emptyGrants.state = ExtensionEnablementLedgerStoreState::Empty;
    ExtensionCenterDialog grantDialog({
        record(ExtensionKind::CodexPlugin, QStringLiteral("plugin.one"), QLatin1Char('a')),
        grantable(ExtensionKind::Skill, QStringLiteral("skill.one"), QLatin1Char('b')),
        record(ExtensionKind::Mcp, QStringLiteral("mcp.one"), QLatin1Char('c')),
    }, {}, readyLedger, emptyGrants);
    const QList<QPushButton *> grants = grantDialog.findChildren<QPushButton *>(
        QStringLiteral("extensionEnablementButton"));
    if (!expect(grants.size() == 4, "grant column is missing a row")) return 1;
    if (!expect(!grants.at(0)->isEnabled() && !grants.at(2)->isEnabled()
                    && !grants.at(3)->isEnabled(),
                "an unreviewed extension offered a clickable grant action")) return 1;
    if (!expect(grants.at(1)->isEnabled()
                    && grants.at(1)->text() == QStringLiteral("授权启用"),
                "a reviewed, compatible, installed extension has no grant action")) {
        return 1;
    }
    // 被冻结的按钮必须说明冻结的确切原因：把"没人复核过"显示成"当前主机装不下"会让人
    // 以为换台机器就能运行一份从未被人看过的内容。
    if (!expect(grants.at(0)->toolTip().contains(QStringLiteral("未经人工复核")),
                "a blocked grant action does not name the missing gate")) return 1;

    // 三道门禁齐备、且账本里已有一条授权，但授权账本读不出来：授权与撤销都必须冻结。
    // 授权集合未知时提交一份"完整集合"会静默撤销读不出来的那些授权，把一次篡改表述成
    // 用户主动停用，而下一次授予还会以错误的代号提交。
    ExtensionReviewLedgerStoreResult pinlessLedger;
    pinlessLedger.state = ExtensionReviewLedgerStoreState::Ready;
    pinlessLedger.generation = 1;
    ExtensionEnablementLedgerStoreResult unreadableGrants;
    unreadableGrants.state = ExtensionEnablementLedgerStoreState::Unavailable;
    unreadableGrants.errorCode = QStringLiteral("extension-enablement-store-locked");
    ExtensionEnablementGrant unreadableGrant;
    unreadableGrant.kind = ExtensionKind::Skill;
    unreadableGrant.id = QStringLiteral("skill.one");
    unreadableGrant.sourceIdentity = QStringLiteral("extension-source:sha256:")
        + QString(64, QLatin1Char('b'));
    unreadableGrant.contentIdentity = QStringLiteral("extension-content:sha256:")
        + QString(64, QLatin1Char('b'));
    unreadableGrants.grants = {unreadableGrant};
    ExtensionCenterDialog frozenDialog({
        grantable(ExtensionKind::Skill, QStringLiteral("skill.one"), QLatin1Char('b')),
        grantable(ExtensionKind::Mcp, QStringLiteral("mcp.one"), QLatin1Char('c')),
    }, {}, pinlessLedger, unreadableGrants);
    const QList<QPushButton *> frozen = frozenDialog.findChildren<QPushButton *>(
        QStringLiteral("extensionEnablementButton"));
    if (!expect(frozen.size() == 2,
                "the frozen fixture did not render both rows")) return 1;
    for (QPushButton *control : frozen) {
        if (!expect(!control->isEnabled(),
                    "an unreadable grant ledger left grant actions clickable")) return 1;
    }
    // 读不出来的账本同样不得被当成空集合：它带来的授权不进入视图，因此不会有"撤销"动作
    // 声称账本里有一条可以收回的授权。
    for (QPushButton *control : frozen) {
        if (!expect(control->text() != QStringLiteral("撤销授权"),
                    "an unreadable grant ledger was treated as a readable one")) return 1;
    }

    int granted = 0;
    ExtensionEnablementRequest lastRequest;
    QObject::connect(&grantDialog, &ExtensionCenterDialog::enablementRequested,
                     [&](const ExtensionEnablementRequest &request) {
        ++granted;
        lastRequest = request;
    });
    PromptInspection grantPrompt;
    inspectNextPrompt(&grantPrompt, grants.at(1));
    if (!expect(grantPrompt.seen && grantPrompt.plainDefaultDeny,
                "grant confirmation is not exact/plain/default-deny")) return 1;
    if (!expect(grantPrompt.text.contains(QStringLiteral("来源身份："))
                    && grantPrompt.text.contains(
                        QStringLiteral("extension-content:sha256:")
                        + QString(64, QLatin1Char('b'))),
                "grant confirmation does not show the exact content identity")) return 1;
    // 授权当前不会让任何内容运行。这一句必须出现在人能看到的地方，否则人会以为自己刚刚
    // 开启了执行。
    if (!expect(grantPrompt.text.contains(QStringLiteral("不会让任何内容运行")),
                "grant confirmation claims the grant starts execution")) return 1;
    if (!expect(granted == 0, "cancelled grant emitted a request")) return 1;

    // 勾选并确认后发出的请求必须携带屏幕上展示过的那两个完整摘要，并且绑定类型与标识。
    // 不携带摘要的授权就不再绑定"人看到的那份内容"：渲染之后内容一旦变化，规划层就无从
    // 察觉，那个决定会被套用到新内容上。
    QTimer accepter;
    accepter.setInterval(1);
    QObject::connect(&accepter, &QTimer::timeout, [&accepter]() {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box) return;
        if (QCheckBox *check = box->checkBox()) check->setChecked(true);
        // exec() 的返回值要与 QMessageBox::Ok 比较，因此必须点那个按钮而不是 accept()。
        if (QAbstractButton *ok = box->button(QMessageBox::Ok)) ok->click();
        accepter.stop();
    });
    accepter.start();
    grants.at(1)->click();
    if (!expect(granted == 1, "an accepted grant emitted no request")) return 1;
    if (!expect(lastRequest.action == ExtensionEnablementAction::Enable
                    && lastRequest.kind == ExtensionKind::Skill
                    && lastRequest.id == QStringLiteral("skill.one"),
                "the grant request is not bound to the confirmed kind and id")) return 1;
    if (!expect(lastRequest.reviewedContentIdentity
                        == QStringLiteral("extension-content:sha256:")
                            + QString(64, QLatin1Char('b'))
                    && lastRequest.reviewedSourceIdentity
                        == QStringLiteral("extension-source:sha256:")
                            + QString(64, QLatin1Char('b')),
                "the grant request does not echo the rendered identities")) return 1;

    // 撤销永远可用（只要授权集合读得出来），包括来源已消失的授权：否则一个被篡改的扩展
    // 将永远无法被撤销。
    ExtensionEnablementLedgerStoreResult readyGrants;
    readyGrants.state = ExtensionEnablementLedgerStoreState::Ready;
    readyGrants.generation = 3;
    ExtensionEnablementGrant staleGrant;
    staleGrant.kind = ExtensionKind::Mcp;
    staleGrant.id = QStringLiteral("missing.mcp");
    staleGrant.sourceIdentity = QStringLiteral("extension-source:sha256:")
        + QString(64, QLatin1Char('f'));
    staleGrant.contentIdentity = QStringLiteral("extension-content:sha256:")
        + QString(64, QLatin1Char('0'));
    // 内容已经漂移的授权：摘要与当前记录不一致，撤销仍然必须可用。
    ExtensionEnablementGrant driftedGrant;
    driftedGrant.kind = ExtensionKind::Skill;
    driftedGrant.id = QStringLiteral("skill.one");
    driftedGrant.sourceIdentity = QStringLiteral("extension-source:sha256:")
        + QString(64, QLatin1Char('9'));
    driftedGrant.contentIdentity = QStringLiteral("extension-content:sha256:")
        + QString(64, QLatin1Char('8'));
    readyGrants.grants = {staleGrant, driftedGrant};
    ExtensionCenterDialog revokeDialog({
        grantable(ExtensionKind::Skill, QStringLiteral("skill.one"), QLatin1Char('b')),
    }, {}, readyLedger, readyGrants);
    auto *revokeTable = revokeDialog.findChild<QTableWidget *>(
        QStringLiteral("extensionCenterTable"));
    const QList<QPushButton *> revokes = revokeDialog.findChildren<QPushButton *>(
        QStringLiteral("extensionEnablementButton"));
    // 三行：已授权的 skill.one、只留复核记录的 missing.skill、只留授权的 missing.mcp。
    if (!expect(revokeTable && revokeTable->rowCount() == 3 && revokes.size() == 3,
                "stale grants and pins do not each get a row")) return 1;
    int revokeActions = 0;
    for (QPushButton *control : revokes) {
        if (control->text() != QStringLiteral("撤销授权")) continue;
        ++revokeActions;
        if (!expect(control->isEnabled(),
                    "a revocation action was frozen on a readable grant ledger")) {
            return 1;
        }
    }
    if (!expect(revokeActions == 2,
                "drifted or absent grants lost their revocation action")) return 1;

    int revoked = 0;
    ExtensionEnablementRequest revokeRequest;
    QObject::connect(&revokeDialog, &ExtensionCenterDialog::enablementRequested,
                     [&](const ExtensionEnablementRequest &request) {
        ++revoked;
        revokeRequest = request;
    });
    QPushButton *absentRevoke = nullptr;
    for (QPushButton *control : revokes) {
        if (control->text() == QStringLiteral("撤销授权")
                && control->toolTip().contains(QStringLiteral("收回"))) {
            absentRevoke = control;
        }
    }
    if (!expect(absentRevoke != nullptr, "no revocation control was rendered")) return 1;
    PromptInspection revokePrompt;
    inspectNextPrompt(&revokePrompt, absentRevoke);
    if (!expect(revokePrompt.seen && revokePrompt.plainDefaultDeny,
                "grant revocation is not exact/plain/default-deny")) return 1;
    if (!expect(revokePrompt.text.contains(QStringLiteral("人工复核证据保持不变")),
                "grant revocation does not say the review evidence survives")) return 1;
    if (!expect(revoked == 0, "cancelled revocation emitted a request")) return 1;

    // 一次复核操作回推的快照不带授权账本，因此它不得把授权集合改写成空集合：那会让界面
    // 显示"这些扩展没有被授权过"，而实际情况是这次操作根本没有读过授权。
    revokeDialog.setReviewSnapshot({
        grantable(ExtensionKind::Skill, QStringLiteral("skill.one"), QLatin1Char('b')),
    }, {}, readyLedger);
    // 被替换掉的单元格控件只是排队等待删除，不排空这些事件就会把上一轮的按钮一起数进来。
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const QList<QPushButton *> afterReview = revokeDialog.findChildren<QPushButton *>(
        QStringLiteral("extensionEnablementButton"));
    int survivingRevokes = 0;
    for (QPushButton *control : afterReview) {
        survivingRevokes += control->text() == QStringLiteral("撤销授权");
    }
    if (!expect(survivingRevokes == 2,
                "a review refresh erased the enablement grants from the view")) return 1;

    // 授权操作进行中时全部授权动作冻结，而复核动作不受影响：两者写的是两份互相独立的
    // 账本。
    revokeDialog.setEnablementBusy(true);
    for (QPushButton *control : afterReview) {
        if (!expect(!control->isEnabled(),
                    "a busy grant operation left enablement actions clickable")) return 1;
    }
    revokeDialog.setEnablementBusy(false);
    int reenabled = 0;
    for (QPushButton *control : afterReview) {
        reenabled += control->isEnabled();
    }
    if (!expect(reenabled >= 2,
                "clearing the busy flag did not restore eligible grant actions")) return 1;

    // 诊断只能是固定代码。后端返回的任意文本不得决定屏幕上写着什么。
    revokeDialog.showEnablementError(QStringLiteral("extension-enablement-content-drift"));
    auto *grantStatus = revokeDialog.findChild<QLabel *>(
        QStringLiteral("extensionEnablementStatus"));
    if (!expect(grantStatus
                    && grantStatus->text().contains(
                        QStringLiteral("extension-enablement-content-drift")),
                "a fixed grant diagnostic was not surfaced")) return 1;
    revokeDialog.showEnablementError(QStringLiteral("<b>rm -rf /Users/someone</b>"));
    if (!expect(grantStatus && !grantStatus->text().contains(QStringLiteral("/Users/"))
                    && !grantStatus->text().contains(QStringLiteral("<b>")),
                "an unfixed grant diagnostic reached the screen verbatim")) return 1;

    // ---- 收回记录 ----
    // 两份账本默认都是 Invalid：收回必须全部冻结。控制器在任一份不可读时拒绝，而在授权
    // 状态未知的情况下声称已经收回授权是这条路径最不该做的事。
    const QList<QPushButton *> defaultRemovals = dialog.findChildren<QPushButton *>(
        QStringLiteral("extensionRemovalButton"));
    if (!expect(defaultRemovals.size() == 3,
                "removal column is missing a row")) return 1;
    for (QPushButton *control : defaultRemovals) {
        if (!expect(!control->isEnabled(),
                    "unreadable ledgers left removal actions clickable")) return 1;
        // 这一列不叫"删除"：这个动作一个字节的磁盘内容都不动。
        if (!expect(!control->text().contains(QStringLiteral("删除")),
                    "the removal action calls itself a deletion")) return 1;
    }

    // 没有任何记录可收回的行必须冻结，而只留一条复核记录的行必须可点击：收回的对象是账本
    // 里的记录，不是屏幕上那一项内容。
    const QList<QPushButton *> grantRemovals = grantDialog.findChildren<QPushButton *>(
        QStringLiteral("extensionRemovalButton"));
    if (!expect(grantRemovals.size() == 4,
                "the grant fixture did not render a removal action per row")) return 1;
    for (int index = 0; index < 3; ++index) {
        if (!expect(!grantRemovals.at(index)->isEnabled(),
                    "a target with no records offered a removal action")) return 1;
        if (!expect(grantRemovals.at(index)->toolTip().contains(
                        QStringLiteral("没有可收回的记录")),
                    "a frozen removal action does not name the reason")) return 1;
    }
    if (!expect(grantRemovals.at(3)->isEnabled(),
                "a stale review pin lost its removal action")) return 1;

    // 授权账本读不出来时收回冻结，即使复核账本可读：控制器会拒绝，界面不该先邀请人点击。
    const QList<QPushButton *> frozenRemovals = frozenDialog.findChildren<QPushButton *>(
        QStringLiteral("extensionRemovalButton"));
    if (!expect(frozenRemovals.size() == 2,
                "the frozen fixture did not render both removal actions")) return 1;
    for (QPushButton *control : frozenRemovals) {
        if (!expect(!control->isEnabled(),
                    "an unreadable grant ledger left removal clickable")) return 1;
        if (!expect(control->toolTip().contains(QStringLiteral("账本不可读")),
                    "a frozen removal action does not name the unreadable ledger")) {
            return 1;
        }
    }

    // 收回没有门禁：内容已漂移的授权、来源已消失的目标都必须仍然可以被收回，否则一个被
    // 篡改或被删掉来源的扩展会永远留着一份已认证的授权。
    const QList<QPushButton *> removals = revokeDialog.findChildren<QPushButton *>(
        QStringLiteral("extensionRemovalButton"));
    if (!expect(removals.size() == 3,
                "drifted and absent targets do not each get a removal action")) return 1;
    for (QPushButton *control : removals) {
        if (!expect(control->isEnabled(),
                    "a removal action was gated on drift or source presence")) return 1;
    }
    // 收回进行中时全部收回动作冻结：当前记录状态与失败原因都还没有被人确认，继续允许点击
    // 只会在同一个未知状态上再叠一次写入。
    revokeDialog.setRemovalBusy(true);
    for (QPushButton *control : removals) {
        if (!expect(!control->isEnabled(),
                    "a busy removal operation left removal actions clickable")) return 1;
    }
    revokeDialog.setRemovalBusy(false);
    int restored = 0;
    for (QPushButton *control : removals) {
        restored += control->isEnabled();
    }
    if (!expect(restored == 3,
                "clearing the busy flag did not restore eligible removal actions")) {
        return 1;
    }

    // 授权账本可读且账本里确实有一条授权，但复核账本读不出来：收回仍然必须冻结。控制器
    // 在任一份账本不可读时拒绝，而在复核状态未知的情况下先邀请人点击，只会让人以为记录
    // 已经被收回。这一半与另一半不对称：授权集合是可读的，因此撤销授权仍然可用。
    ExtensionReviewLedgerStoreResult unreadableLedger;
    unreadableLedger.state = ExtensionReviewLedgerStoreState::Unavailable;
    unreadableLedger.errorCode = QStringLiteral("extension-review-store-locked");
    ExtensionCenterDialog halfFrozenDialog({
        grantable(ExtensionKind::Skill, QStringLiteral("skill.one"), QLatin1Char('b')),
    }, {}, unreadableLedger, readyGrants);
    const QList<QPushButton *> halfFrozenRemovals =
        halfFrozenDialog.findChildren<QPushButton *>(
            QStringLiteral("extensionRemovalButton"));
    if (!expect(halfFrozenRemovals.size() == 2,
                "the half-frozen fixture did not render both rows")) return 1;
    for (QPushButton *control : halfFrozenRemovals) {
        if (!expect(!control->isEnabled(),
                    "an unreadable review ledger left removal clickable")) return 1;
    }
    int halfFrozenRevokes = 0;
    for (QPushButton *control : halfFrozenDialog.findChildren<QPushButton *>(
             QStringLiteral("extensionEnablementButton"))) {
        if (control->text() != QStringLiteral("撤销授权")) continue;
        ++halfFrozenRevokes;
        if (!expect(control->isEnabled(),
                    "a readable grant ledger lost its revocation action")) return 1;
    }
    if (!expect(halfFrozenRevokes == 2,
                "an unreadable review ledger froze grant revocation too")) return 1;

    // 对称的另一半：复核账本可读且留有一条复核记录，但授权账本读不出来。收回同样必须
    // 冻结——在授权状态未知的情况下声称已经收回授权，是这条路径最不该做的事。
    ExtensionCenterDialog grantFrozenDialog({
        grantable(ExtensionKind::Skill, QStringLiteral("skill.one"), QLatin1Char('b')),
    }, {}, readyLedger, unreadableGrants);
    const QList<QPushButton *> grantFrozenRemovals =
        grantFrozenDialog.findChildren<QPushButton *>(
            QStringLiteral("extensionRemovalButton"));
    // 两行：skill.one 与只留复核记录的 missing.skill。
    if (!expect(grantFrozenRemovals.size() == 2,
                "the grant-frozen fixture did not render both rows")) return 1;
    for (QPushButton *control : grantFrozenRemovals) {
        if (!expect(!control->isEnabled(),
                    "an unreadable grant ledger left removal clickable")) return 1;
    }

    // 判定层拒绝的目标不得出现可点击的收回动作，而它仍然必须显示出来：一条标识不合法的
    // 残留授权需要被看见，否则没人知道账本里有它。
    ExtensionEnablementLedgerStoreResult malformedGrants;
    malformedGrants.state = ExtensionEnablementLedgerStoreState::Ready;
    malformedGrants.generation = 2;
    ExtensionEnablementGrant malformedGrant;
    malformedGrant.kind = ExtensionKind::Skill;
    malformedGrant.id = QStringLiteral("../escape");
    malformedGrant.sourceIdentity = QStringLiteral("extension-source:sha256:")
        + QString(64, QLatin1Char('7'));
    malformedGrant.contentIdentity = QStringLiteral("extension-content:sha256:")
        + QString(64, QLatin1Char('6'));
    malformedGrants.grants = {malformedGrant};
    ExtensionCenterDialog malformedDialog({}, {}, pinlessLedger, malformedGrants);
    const QList<QPushButton *> malformedRemovals =
        malformedDialog.findChildren<QPushButton *>(
            QStringLiteral("extensionRemovalButton"));
    if (!expect(malformedRemovals.size() == 1,
                "a malformed residual grant lost its row")) return 1;
    if (!expect(!malformedRemovals.first()->isEnabled(),
                "a target the policy rejects offered a removal action")) return 1;
    if (!expect(malformedRemovals.first()->toolTip().contains(
                    QStringLiteral("无法安全展示")),
                "a rejected removal action does not name the reason")) return 1;

    int removalsRequested = 0;
    ExtensionKind removedKind = ExtensionKind::CodexPlugin;
    QString removedId;
    QObject::connect(&revokeDialog, &ExtensionCenterDialog::removalRequested,
                     [&](ExtensionKind kind, const QString &id) {
        ++removalsRequested;
        removedKind = kind;
        removedId = id;
    });
    PromptInspection removalPrompt;
    inspectNextPrompt(&removalPrompt, removals.at(0));
    if (!expect(removalPrompt.seen && removalPrompt.plainDefaultDeny,
                "removal confirmation is not exact/plain/default-deny")) return 1;
    // 这一句是这个对话框存在的理由。写"删除扩展"会让人以为磁盘上那份内容已经消失，于是
    // 停止清理，而内容还在原处，重新复核并授权后会重新可用。
    if (!expect(removalPrompt.text.contains(QStringLiteral("不删除磁盘上的任何内容")),
                "removal confirmation does not say the disk content survives")) return 1;
    if (!expect(removalPrompt.text.contains(QStringLiteral("本次收回："))
                    && removalPrompt.text.contains(QStringLiteral("启用授权")),
                "removal confirmation does not name which halves it withdraws")) return 1;
    if (!expect(removalPrompt.text.contains(QStringLiteral("保留身份：")),
                "removal confirmation does not state the identity is retained")) return 1;
    if (!expect(removalsRequested == 0,
                "cancelled removal emitted a request")) return 1;

    QTimer removalAccepter;
    removalAccepter.setInterval(1);
    QObject::connect(&removalAccepter, &QTimer::timeout, [&removalAccepter]() {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box) return;
        if (QCheckBox *check = box->checkBox()) check->setChecked(true);
        if (QAbstractButton *ok = box->button(QMessageBox::Ok)) ok->click();
        removalAccepter.stop();
    });
    removalAccepter.start();
    // 点的是第三行（来源已消失、只留一条授权的 missing.mcp），不是第一行：请求必须绑定
    // 被确认的那一行，否则一次收回会落到屏幕上另一个目标上。
    removals.at(2)->click();
    if (!expect(removalsRequested == 1,
                "an accepted removal emitted no request")) return 1;
    if (!expect(removedKind == ExtensionKind::Mcp
                    && removedId == QStringLiteral("missing.mcp"),
                "the removal request is not bound to the confirmed kind and id")) return 1;

    // 收回确实读过并写过两份账本，因此它的回执替换两者。空的授权集合必须真的清空视图里的
    // 授权，否则一次成功的收回之后屏幕上还留着一条已经不存在的授权。
    ExtensionEnablementLedgerStoreResult clearedGrants;
    clearedGrants.state = ExtensionEnablementLedgerStoreState::Empty;
    revokeDialog.setRemovalSnapshot({
        grantable(ExtensionKind::Skill, QStringLiteral("skill.one"), QLatin1Char('b')),
    }, {}, pinlessLedger, clearedGrants);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const QList<QPushButton *> afterRemoval = revokeDialog.findChildren<QPushButton *>(
        QStringLiteral("extensionEnablementButton"));
    int lingering = 0;
    for (QPushButton *control : afterRemoval) {
        lingering += control->text() == QStringLiteral("撤销授权");
    }
    if (!expect(lingering == 0,
                "a removal refresh left withdrawn grants on the screen")) return 1;
    if (!expect(revokeTable->rowCount() == 1,
                "a removal refresh did not drop the rows whose records are gone")) {
        return 1;
    }
    const QList<QPushButton *> settledRemovals = revokeDialog.findChildren<QPushButton *>(
        QStringLiteral("extensionRemovalButton"));
    if (!expect(settledRemovals.size() == 1 && !settledRemovals.first()->isEnabled(),
                "a fully withdrawn target still offers a removal action")) return 1;

    // 部分完成必须可分辨，而诊断只能是固定代码。
    revokeDialog.showRemovalError(QStringLiteral("extension-removal-incomplete"));
    auto *removalStatus = revokeDialog.findChild<QLabel *>(
        QStringLiteral("extensionRemovalStatus"));
    if (!expect(removalStatus
                    && removalStatus->text().contains(
                        QStringLiteral("extension-removal-incomplete")),
                "a fixed removal diagnostic was not surfaced")) return 1;
    revokeDialog.showRemovalError(QStringLiteral("<b>rm -rf /Users/someone</b>"));
    if (!expect(removalStatus
                    && !removalStatus->text().contains(QStringLiteral("/Users/"))
                    && !removalStatus->text().contains(QStringLiteral("<b>")),
                "an unfixed removal diagnostic reached the screen verbatim")) return 1;

    // 披露区回答的是另一个问题：上面的表格是"已经在这台机器上的扩展"，披露区是"这个包里
    // 有什么"。两者混在一张表里会让一个尚未导入的包看起来已经在列。
    auto *importTable = revokeDialog.findChild<QTableWidget *>(
        QStringLiteral("extensionImportTable"));
    auto *importStatus = revokeDialog.findChild<QLabel *>(
        QStringLiteral("extensionImportStatus"));
    auto *discloseButton = revokeDialog.findChild<QPushButton *>(
        QStringLiteral("extensionImportDiscloseButton"));
    if (!expect(importTable && importStatus && discloseButton,
                "the extension center offers no bundle disclosure surface")) {
        return 1;
    }
    // 按钮不能自称导入：叫导入会让人以为点完之后磁盘上多了一份内容。
    if (!expect(!discloseButton->text().contains(QStringLiteral("导入"))
                    && discloseButton->toolTip().contains(
                        QStringLiteral("不导入")),
                "the disclosure action calls itself an import")) return 1;
    if (!expect(importTable->rowCount() == 0
                    && importStatus->text().contains(QStringLiteral("尚未披露")),
                "the disclosure surface claims a bundle before one was read")) {
        return 1;
    }

    // 一次完整的披露：逐组件列出，能力逐行归属到它自己的组件。
    ExtensionImportDisclosure ready;
    ready.state = ExtensionImportDisclosureState::Ready;
    ready.title = QStringLiteral("Acme Bundle");
    ready.identifier = QStringLiteral("acme.bundle");
    ready.versionLabel = QStringLiteral("1.2.0");
    ready.sourceFingerprint = QStringLiteral("11223344");
    ready.contentFingerprint = QStringLiteral("55667788");
    ready.anyBeyondReadOnly = true;
    ready.components = {
        component(ExtensionComponentKind::Skill, QStringLiteral("acme.reader"),
                  QStringLiteral("skill"), {QStringLiteral("filesystem-write")},
                  true, false),
        component(ExtensionComponentKind::Asset, QStringLiteral("acme.notes"),
                  QStringLiteral("asset"), {}, false, false),
    };
    revokeDialog.setImportDisclosure(ready);
    if (!expect(importTable->rowCount() == 2,
                "a disclosed bundle did not list every component")) return 1;
    // 披露必须自己说清楚它没有导入任何东西，否则人会以为磁盘上已经多了一份内容并据此
    // 往下走，比如去清理一个从未被写入的目录。
    if (!expect(importStatus->text().contains(QStringLiteral("没有导入"))
                    && importStatus->text().contains(QStringLiteral("没有向磁盘写入")),
                "a disclosure does not say it imported and wrote nothing")) return 1;
    if (!expect(importStatus->text().contains(QStringLiteral("acme.bundle"))
                    && importStatus->text().contains(QStringLiteral("55667788")),
                "a disclosure hides which bundle and which content it describes")) {
        return 1;
    }
    if (!expect(importStatus->text().contains(QStringLiteral("写入或执行")),
                "a bundle requesting writes is not called out")) return 1;
    // 能力归属到组件而不是整包：两个组件各自请求"写文件"与"连网"时，汇总看起来与一个组件
    // 同时请求两者完全一样，而后者才是真正危险的组合。
    QString readerCapabilities;
    QString notesCapabilities;
    for (int row = 0; row < importTable->rowCount(); ++row) {
        QTableWidgetItem *name = importTable->item(row, 0);
        QTableWidgetItem *capability = importTable->item(row, 3);
        if (!name || !capability) continue;
        if (name->text().contains(QStringLiteral("acme.reader"))) {
            readerCapabilities = capability->text();
        }
        if (name->text().contains(QStringLiteral("acme.notes"))) {
            notesCapabilities = capability->text();
        }
    }
    if (!expect(readerCapabilities.contains(QStringLiteral("filesystem-write")),
                "a component's own capability is missing from its row")) return 1;
    if (!expect(!notesCapabilities.contains(QStringLiteral("filesystem-write")),
                "a component was shown another component's capability")) return 1;

    // 失败关闭仍然列出全部组件，包括那个不支持的组件：隐藏证据会让没人能判断这个包到底
    // 想做什么。而屏幕上必须能看出正是它让导入失败关闭的。
    ExtensionImportDisclosure failedClosed;
    failedClosed.state = ExtensionImportDisclosureState::FailedClosed;
    failedClosed.title = QStringLiteral("Future Bundle");
    failedClosed.identifier = QStringLiteral("acme.future");
    failedClosed.errorCode = QStringLiteral("extension-import-unsupported-component");
    failedClosed.anyBeyondReadOnly = true;
    failedClosed.components = {
        component(ExtensionComponentKind::Unsupported,
                  QStringLiteral("acme.quantum"), QStringLiteral("quantum-agent"),
                  {QStringLiteral("command-execution")}, true, true),
    };
    revokeDialog.setImportDisclosure(failedClosed);
    if (!expect(importTable->rowCount() == 1,
                "failing closed discarded the component evidence")) return 1;
    QTableWidgetItem *unsupportedKind = importTable->item(0, 1);
    QTableWidgetItem *declared = importTable->item(0, 2);
    if (!expect(unsupportedKind
                    && unsupportedKind->text().contains(QStringLiteral("失败关闭")),
                "the unsupported component is not shown as the reason")) return 1;
    if (!expect(declared
                    && declared->text() == QStringLiteral("quantum-agent"),
                "the declared type of an unsupported component was discarded")) return 1;
    if (!expect(importStatus->text().contains(
                    QStringLiteral("extension-import-unsupported-component")),
                "a failed-closed disclosure carries no visible diagnostic")) return 1;
    if (!expect(importStatus->text().contains(QStringLiteral("没有导入")),
                "a failed-closed disclosure does not say it imported nothing")) return 1;

    // 每一次披露完整替换上一次：留着上一次的组件会让一次失败的读取看起来在描述这一次
    // 选的那个包，而屏幕上那些组件属于另一个包。
    ExtensionImportDisclosure unreadable;
    unreadable.state = ExtensionImportDisclosureState::Unreadable;
    unreadable.errorCode = QStringLiteral("extension-bundle-manifest-unreadable");
    revokeDialog.setImportDisclosure(unreadable);
    if (!expect(importTable->rowCount() == 0,
                "a refused disclosure left the previous bundle's components on screen")) {
        return 1;
    }
    if (!expect(!importStatus->text().contains(QStringLiteral("Future Bundle")),
                "a refused disclosure still names the previous bundle")) return 1;
    // 读不出来与畸形要求人做不同的事：一个去看权限，一个去修包。
    if (!expect(importStatus->text().contains(QStringLiteral("读不出来")),
                "an unreadable bundle is not distinguished from a malformed one")) {
        return 1;
    }
    ExtensionImportDisclosure malformed;
    malformed.state = ExtensionImportDisclosureState::Unpresentable;
    malformed.errorCode = QStringLiteral("extension-bundle-manifest-fields-invalid");
    revokeDialog.setImportDisclosure(malformed);
    const QString malformedText = importStatus->text();
    revokeDialog.setImportDisclosure(unreadable);
    if (!expect(malformedText != importStatus->text(),
                "unreadable and malformed bundles read identically on screen")) return 1;

    // 目录不存在不是错误：还没有包可以披露。这条路径不带诊断。
    ExtensionImportDisclosure absent;
    absent.state = ExtensionImportDisclosureState::Absent;
    revokeDialog.setImportDisclosure(absent);
    if (!expect(!importStatus->text().contains(QStringLiteral("诊断")),
                "an absent bundle is reported as if something went wrong")) return 1;

    // 诊断只能是固定代码。把包里的任意文本直接贴到界面上，等于让包的内容决定屏幕上写着
    // 什么，而这个包正是还没有被任何人复核过的东西。
    ExtensionImportDisclosure spoofed;
    spoofed.state = ExtensionImportDisclosureState::Unpresentable;
    spoofed.errorCode = QStringLiteral("<b>rm -rf /Users/someone</b>");
    revokeDialog.setImportDisclosure(spoofed);
    if (!expect(!importStatus->text().contains(QStringLiteral("/Users/"))
                    && !importStatus->text().contains(QStringLiteral("<b>")),
                "an unfixed bundle diagnostic reached the screen verbatim")) return 1;

    // 披露进行中不能再发一次请求：屏幕上只能显示一份披露，而后到的那一份必须是最后被选
    // 的那个包。
    int disclosuresRequested = 0;
    QObject::connect(&revokeDialog,
                     &ExtensionCenterDialog::bundleDisclosureRequested,
                     [&disclosuresRequested]() { ++disclosuresRequested; });
    revokeDialog.setImportBusy(true);
    if (!expect(!discloseButton->isEnabled(),
                "a disclosure in flight still offers the action")) return 1;
    discloseButton->click();
    if (!expect(disclosuresRequested == 0,
                "a disclosure was requested while one was already in flight")) return 1;
    // 禁用按钮与处理器自身的拒绝是两道独立的防线。只测点击等于只测了前一道：任何绕过
    // 按钮直接触发这个信号的路径都会发出第二次请求，而屏幕上只能显示一份披露。
    QMetaObject::invokeMethod(discloseButton, "clicked");
    if (!expect(disclosuresRequested == 0,
                "the disclosure handler itself does not refuse while busy")) return 1;
    revokeDialog.setImportBusy(false);
    if (!expect(discloseButton->isEnabled(),
                "a settled disclosure never re-enables the action")) return 1;
    discloseButton->click();
    if (!expect(disclosuresRequested == 1,
                "the disclosure action emits no request")) return 1;

    // 更新区。这一屏存在的理由是：当前没有任何一次更新可以成立，而这件事必须被说清楚，
    // 不能被一个灰掉的按钮代替——只灰掉按钮会让人以为是自己这个包有问题，于是反复重做包，
    // 而真正缺的是这台机器上根本没有装签名权威。
    auto *updateTable = revokeDialog.findChild<QTableWidget *>(
        QStringLiteral("extensionUpdateTable"));
    auto *updateStatus = revokeDialog.findChild<QLabel *>(
        QStringLiteral("extensionUpdateStatus"));
    if (!expect(updateTable && updateStatus,
                "the extension center has no update surface")) return 1;

    ExtensionUpdatePlan blocked;
    blocked.state = ExtensionUpdatePlanState::Blocked;
    blocked.title = QStringLiteral("Acme Skill");
    blocked.identifier = QStringLiteral("acme.skill");
    blocked.activeVersionLabel = QStringLiteral("1.0.0");
    blocked.candidateVersionLabel = QStringLiteral("2.0.0");
    blocked.activeFingerprint = QStringLiteral("aaaaaaaaaaaa");
    blocked.candidateFingerprint = QStringLiteral("bbbbbbbbbbbb");
    blocked.evidenceIncomplete = true;
    blocked.anyUnverifiable = true;
    ExtensionUpdateEvidenceLine signature;
    signature.label = QStringLiteral("签名");
    signature.unverifiable = true;
    signature.diagnostic =
        QStringLiteral("extension-update-signature-authority-absent");
    ExtensionUpdateEvidenceLine manifest;
    manifest.label = QStringLiteral("清单");
    manifest.established = true;
    ExtensionUpdateEvidenceLine compatibility;
    compatibility.label = QStringLiteral("兼容性");
    compatibility.diagnostic = QStringLiteral("extension-update-incompatible");
    blocked.evidence = {signature, manifest, compatibility};
    blocked.errorCode = QStringLiteral("extension-update-signature-invalid");
    revokeDialog.setUpdatePlan(blocked);
    if (!expect(updateTable->rowCount() == 3,
                "the update surface does not list every evidence item")) return 1;
    // "没有人能核查"与"核查失败"必须是屏幕上两句不同的话：一个把人送去装签名权威，一个把人
    // 送去修包。并成一句"证据不足"会让人反复重做一个本来没问题的包。
    const QTableWidgetItem *signatureVerdict = updateTable->item(0, 1);
    const QTableWidgetItem *manifestVerdict = updateTable->item(1, 1);
    const QTableWidgetItem *compatibilityVerdict = updateTable->item(2, 1);
    if (!expect(signatureVerdict && manifestVerdict && compatibilityVerdict,
                "the update evidence table has empty verdict cells")) return 1;
    if (!expect(signatureVerdict->text() != compatibilityVerdict->text(),
                "an unverifiable item reads identically to a failed check")) return 1;
    if (!expect(manifestVerdict->text() != signatureVerdict->text()
                    && manifestVerdict->text() != compatibilityVerdict->text(),
                "an established item reads like an unestablished one")) return 1;
    if (!expect(updateTable->item(1, 2) && updateTable->item(1, 2)->text().isEmpty(),
                "an established evidence item still shows a diagnostic")) return 1;
    // 这一句是这一屏的核心：问题不在这个包上。
    if (!expect(updateStatus->text().contains(QStringLiteral("不是这个包的问题")),
                "the update surface blames the bundle for an absent authority")) {
        return 1;
    }
    // 暂存不是启用，而这三件事在每一条路径上都必须被说出来。
    if (!expect(updateStatus->text().contains(QStringLiteral("没有替换当前生效的版本"))
                    && updateStatus->text().contains(QStringLiteral("没有授予")),
                "the update surface does not say it changed nothing")) return 1;
    if (!expect(updateStatus->text().contains(
                    QStringLiteral("当前版本：1.0.0 → 候选版本：2.0.0")),
                "the update surface does not show which version is in effect")) {
        return 1;
    }
    if (!expect(updateStatus->text().contains(QStringLiteral("aaaaaaaaaaaa"))
                    && updateStatus->text().contains(QStringLiteral("bbbbbbbbbbbb")),
                "the update surface does not show both content fingerprints")) return 1;

    // 降级必须被说出来：两个版本号并排放着不会让人注意到方向，而降级会重新引入已经被修复
    // 过的内容。
    ExtensionUpdatePlan downgrade = blocked;
    downgrade.downgrade = true;
    downgrade.candidateVersionLabel = QStringLiteral("0.9.0");
    revokeDialog.setUpdatePlan(downgrade);
    if (!expect(updateStatus->text().contains(QStringLiteral("降级")),
                "a downgrade is not stated on the update surface")) return 1;
    revokeDialog.setUpdatePlan(blocked);
    if (!expect(!updateStatus->text().contains(QStringLiteral("降级")),
                "a non-downgrade still warns about a downgrade")) return 1;

    // 每一次检查完整替换上一次的证据表：留着上一次的证据会让一次失败的检查看起来在描述
    // 这一次选的那个候选包。
    ExtensionUpdatePlan absentCandidate;
    absentCandidate.state = ExtensionUpdatePlanState::NoCandidate;
    absentCandidate.title = QStringLiteral("Acme Skill");
    absentCandidate.identifier = QStringLiteral("acme.skill");
    absentCandidate.activeVersionLabel = QStringLiteral("1.0.0");
    revokeDialog.setUpdatePlan(absentCandidate);
    if (!expect(updateTable->rowCount() == 0,
                "a settled check left the previous candidate's evidence on screen")) {
        return 1;
    }
    if (!expect(!updateStatus->text().contains(QStringLiteral("诊断")),
                "an absent candidate is reported as if something went wrong")) return 1;

    // 诊断只能是固定代码，逐项说明也一样：候选包正是还没有被任何人复核过的东西，让它的
    // 内容决定屏幕上写着什么等于把界面交给它。
    ExtensionUpdatePlan spoofedPlan;
    spoofedPlan.state = ExtensionUpdatePlanState::Blocked;
    spoofedPlan.errorCode = QStringLiteral("<b>rm -rf /Users/someone</b>");
    ExtensionUpdateEvidenceLine spoofedLine;
    spoofedLine.label = QStringLiteral("签名");
    spoofedLine.unverifiable = true;
    spoofedLine.diagnostic = QStringLiteral("run <b>curl /Users/someone</b>");
    spoofedPlan.evidence = {spoofedLine};
    revokeDialog.setUpdatePlan(spoofedPlan);
    QString updateSerialized = updateStatus->text();
    for (int row = 0; row < updateTable->rowCount(); ++row) {
        for (int column = 0; column < updateTable->columnCount(); ++column) {
            const QTableWidgetItem *cell = updateTable->item(row, column);
            if (cell) updateSerialized += cell->text();
        }
    }
    if (!expect(!updateSerialized.contains(QStringLiteral("/Users/"))
                    && !updateSerialized.contains(QStringLiteral("<b>")),
                "an unfixed candidate diagnostic reached the screen verbatim")) return 1;

    // 检查更新不设门禁：它只读候选包并列出证据，不改动任何记录。把它灰掉恰恰是这一屏要
    // 避免的那件事。上面那三个记录里没有一条是可复核、可授权或可收回的，而检查更新必须
    // 对它们全部可用。
    const QList<QPushButton *> updateControls =
        revokeDialog.findChildren<QPushButton *>(
            QStringLiteral("extensionUpdateButton"));
    if (!expect(!updateControls.isEmpty()
                    && updateControls.size() == revokeTable->rowCount(),
                "the update action is missing from some rows")) return 1;
    for (QPushButton *control : updateControls) {
        if (!expect(control->isEnabled(),
                    "a read-only update check was gated behind a ledger")) return 1;
    }

    // 检查进行中不能再发一次请求：屏幕上只能显示一份计划，而后到的那一份必须是最后被选的
    // 那个候选包。禁用按钮与处理器自身的拒绝是两道独立的防线。
    int updatesRequested = 0;
    QObject::connect(&revokeDialog,
                     &ExtensionCenterDialog::updatePlanRequested,
                     [&updatesRequested](ExtensionKind, const QString &) {
        ++updatesRequested;
    });
    revokeDialog.setUpdateBusy(true);
    if (!expect(!updateControls.first()->isEnabled(),
                "a check in flight still offers the action")) return 1;
    updateControls.first()->click();
    if (!expect(updatesRequested == 0,
                "a check was requested while one was already in flight")) return 1;
    QMetaObject::invokeMethod(updateControls.first(), "clicked");
    if (!expect(updatesRequested == 0,
                "the update handler itself does not refuse while busy")) return 1;
    revokeDialog.setUpdateBusy(false);
    if (!expect(updateControls.first()->isEnabled(),
                "a settled check never re-enables the action")) return 1;
    updateControls.first()->click();
    if (!expect(updatesRequested == 1,
                "the update action emits no request")) return 1;

    // ---- 暂存备份浏览与恢复入口 ----
    ExtensionCenterDialog backupDialog({
        record(ExtensionKind::Mcp, QStringLiteral("mcp.one"), QLatin1Char('c')),
    }, {}, readyLedger, emptyGrants);
    auto *backupTable = backupDialog.findChild<QTableWidget *>(
        QStringLiteral("extensionBackupTable"));
    auto *backupStatus = backupDialog.findChild<QLabel *>(
        QStringLiteral("extensionBackupStatus"));
    auto *restoreStatus = backupDialog.findChild<QLabel *>(
        QStringLiteral("extensionRestoreStatus"));
    if (!expect(backupTable && backupStatus && restoreStatus
                    && backupTable->columnCount() == 6
                    && backupTable->rowCount() == 0,
                "the backup browsing surface is missing")) return 1;
    // 初始状态必须明确区别于"没有备份"：读取尚未发生时不能说成空。
    if (!expect(backupStatus->text().contains(QStringLiteral("尚未读取"))
                    && !backupStatus->text().contains(QStringLiteral("为空")),
                "an unread backup listing already reads as empty")) return 1;

    // 全主体清点：两个主体的三份完整备份全部渲染；mcp: 主体必须带整文件语义说明。
    ExtensionStagingBackupListResult listing;
    listing.state = ExtensionStagingBackupListState::Ready;
    listing.entries = {
        backupEntry(QStringLiteral("mcp:claude-settings"),
                    QStringLiteral("ext_20260905_102030_aabbccdd"), true),
        backupEntry(QStringLiteral("skill:alpha"),
                    QStringLiteral("ext_20260904_102030_aabbccdd"), true),
        backupEntry(QStringLiteral("skill:alpha"),
                    QStringLiteral("ext_20260903_102030_aabbccdd"), false),
        backupEntry(QString(),
                    QStringLiteral("ext_20260902_102030_aabbccdd"), false),
    };
    backupDialog.setBackupListing(listing, /*restoreDestinationResolved=*/true);
    if (!expect(backupTable->rowCount() == 4,
                "the backup surface did not render every subject's backups")) {
        return 1;
    }
    QString backupSerialized;
    for (int row = 0; row < backupTable->rowCount(); ++row) {
        for (int column = 0; column < backupTable->columnCount(); ++column) {
            // "操作"列只有按钮、没有文本 item。
            if (column == 5) {
                if (!expect(!backupTable->item(row, column),
                            "the action column grew a text item")) return 1;
                continue;
            }
            const QTableWidgetItem *cell = backupTable->item(row, column);
            if (!expect(cell && !(cell->flags() & Qt::ItemIsEditable),
                        "the backup surface exposed an editable item")) return 1;
            backupSerialized += cell->text();
            if (cell) backupSerialized += cell->toolTip();
        }
    }
    if (!expect(backupSerialized.contains(QStringLiteral("mcp:claude-settings"))
                    && backupSerialized.contains(QStringLiteral("skill:alpha"))
                    && backupSerialized.contains(
                        QStringLiteral("ext_20260905_102030_aabbccdd")),
                "the backup surface lost a subject or a backup id")) return 1;
    // mcp: 主体的备份单元是整个共享设置文件；按单个服务器描述会是 dishonest 的暗示。
    if (!expect(backupTable->item(0, 4)->text().contains(
                    QStringLiteral("整个共享设置文件")),
                "an mcp: backup does not state the whole-file semantics")) return 1;
    // 损坏备份可见并标注，绝不隐藏；无法归类主体的损坏条目显示占位而不是空白。
    if (!expect(backupTable->item(2, 3)->text() == QStringLiteral("损坏")
                    && backupTable->item(3, 3)->text() == QStringLiteral("损坏")
                    && backupTable->item(2, 4)->text().contains(
                        QStringLiteral("结构损坏"))
                    && backupTable->item(3, 0)->text().contains(
                        QStringLiteral("主体无法归类")),
                "a corrupt backup is hidden or unlabeled")) return 1;
    if (!expect(backupTable->item(0, 3)->text() == QStringLiteral("完整"),
                "an intact backup is not labeled intact")) return 1;
    // 创建时间本地化渲染；损坏条目没有可信时间戳时必须明说。
    if (!expect(!backupTable->item(0, 2)->text().isEmpty()
                    && backupTable->item(0, 2)->text()
                        != QStringLiteral("时间未知")
                    && backupTable->item(2, 2)->text()
                        == QStringLiteral("时间未知"),
                "the backup surface does not render honest timestamps")) return 1;
    // 清单身份从字节重算而来，供审计指认"是哪一份"。
    if (!expect(backupTable->item(0, 1)->toolTip().contains(
                    QStringLiteral("extension-staging-backup-manifest:sha256:")),
                "the recomputed manifest identity is not auditable")) return 1;
    if (!expect(backupStatus->text().contains(QStringLiteral("共 4 份"))
                    && backupStatus->text().contains(
                        QStringLiteral("其中 2 份结构损坏")),
                "the backup surface miscounts corrupt backups")) return 1;
    // 作用域说明必须明说哪一类行有恢复入口、其余为什么没有：一句都没有的话，人会
    // 以为界面漏渲染了按钮。
    if (!expect(backupStatus->text().contains(
                    QStringLiteral("mcp:claude-settings"))
                    && backupStatus->text().contains(
                        QStringLiteral("恢复前会先捕获当前状态")),
                "the backup surface does not state the restore scope honestly")) {
        return 1;
    }

    // 恢复入口缺席而非禁用：唯一合格行（mcp:claude-settings 完整）有且仅有一个恢复
    // 按钮，其余行连按钮都不渲染。
    const QList<QPushButton *> restoreButtons =
        backupDialog.findChildren<QPushButton *>(
            QStringLiteral("extensionBackupRestoreButton"));
    if (!expect(restoreButtons.size() == 1
                    && restoreButtons.first()->text()
                        == QStringLiteral("恢复")
                    && restoreButtons.first()->isEnabled(),
                "the eligible row did not get exactly one restore button")) {
        return 1;
    }
    for (int row = 0; row < backupTable->rowCount(); ++row) {
        for (int column = 0; column < backupTable->columnCount(); ++column) {
            QWidget *cell = backupTable->cellWidget(row, column);
            if (row == 0 && column == 5) {
                if (!expect(cell == restoreButtons.first(),
                            "the restore button is not on the eligible row")) {
                    return 1;
                }
            } else if (!expect(!cell,
                               "an ineligible row grew an action widget")) {
                return 1;
            }
        }
    }
    // 点击发射的只应该是 (backupId, subject)：资格复核在编排器，按钮在场不是信任输入。
    QString requestedBackupId;
    QString requestedSubject;
    QObject::connect(&backupDialog, &ExtensionCenterDialog::restoreRequested,
                     [&requestedBackupId, &requestedSubject](
                         const QString &backupId, const QString &subject) {
        requestedBackupId = backupId;
        requestedSubject = subject;
    });
    restoreButtons.first()->click();
    if (!expect(requestedBackupId
                    == QStringLiteral("ext_20260905_102030_aabbccdd")
                    && requestedSubject
                        == QStringLiteral("mcp:claude-settings"),
                "the restore button did not emit the exact backup identity")) {
        return 1;
    }
    // 忙碌时入口冻结。
    backupDialog.setRestoreBusy(true);
    if (!expect(!restoreButtons.first()->isEnabled(),
                "a restore in flight still offers the action")) return 1;
    backupDialog.setRestoreBusy(false);
    if (!expect(restoreButtons.first()->isEnabled(),
                "a settled restore never re-enables the action")) return 1;

    // 目标不可解析时连合格行也没有按钮：恢复不可能生效的地方不得出现恢复入口。
    backupDialog.setBackupListing(listing, /*restoreDestinationResolved=*/false);
    if (!expect(backupDialog.findChildren<QPushButton *>(
                    QStringLiteral("extensionBackupRestoreButton")).isEmpty(),
                "an unresolvable destination still offered restore")) return 1;
    backupDialog.setBackupListing(listing, /*restoreDestinationResolved=*/true);
    if (!expect(backupDialog.findChildren<QPushButton *>(
                    QStringLiteral("extensionBackupRestoreButton")).size() == 1,
                "a resolved destination lost the restore button")) return 1;

    // 每一次清单完整替换上一次：旧行一个不留。
    ExtensionStagingBackupListResult shorter;
    shorter.state = ExtensionStagingBackupListState::Ready;
    shorter.entries = {
        backupEntry(QStringLiteral("skill:beta"),
                    QStringLiteral("ext_20260901_102030_aabbccdd"), true),
    };
    backupDialog.setBackupListing(shorter, /*restoreDestinationResolved=*/true);
    if (!expect(backupTable->rowCount() == 1
                    && backupTable->item(0, 1)->text()
                        == QStringLiteral("ext_20260901_102030_aabbccdd")
                    && backupStatus->text().contains(QStringLiteral("共 1 份"))
                    && !backupStatus->text().contains(QStringLiteral("其中"))
                    && backupDialog.findChildren<QPushButton *>(
                           QStringLiteral("extensionBackupRestoreButton"))
                           .isEmpty(),
                "a stale backup listing survived a refresh")) return 1;

    // 读取中：清空旧行并明说正在读取，绝不留下一份过期答案。
    backupDialog.setBackupBusy(true);
    if (!expect(backupTable->rowCount() == 0
                    && backupStatus->text().contains(QStringLiteral("正在读取")),
                "a reload left the previous listing on screen")) return 1;
    backupDialog.setBackupBusy(false);

    // 退化存储冻结成明确的非空消息：Invalid 与 Unavailable 都绝不能说成空清单。
    ExtensionStagingBackupListResult degraded;
    degraded.state = ExtensionStagingBackupListState::Invalid;
    degraded.issue = QStringLiteral("extension-staging-inventory-store-shape-invalid");
    backupDialog.setBackupListing(degraded, /*restoreDestinationResolved=*/true);
    if (!expect(backupTable->rowCount() == 0
                    && backupStatus->text().contains(QStringLiteral("浏览已冻结"))
                    && backupStatus->text().contains(
                        QStringLiteral("extension-staging-inventory-store-shape-invalid"))
                    && !backupStatus->text().contains(QStringLiteral("为空")),
                "a degraded backup store was disguised as an empty listing")) {
        return 1;
    }
    ExtensionStagingBackupListResult unavailable;
    unavailable.state = ExtensionStagingBackupListState::Unavailable;
    unavailable.issue = QStringLiteral("extension-staging-inventory-busy");
    backupDialog.setBackupListing(unavailable, /*restoreDestinationResolved=*/true);
    if (!expect(backupStatus->text().contains(QStringLiteral("浏览已冻结"))
                    && backupStatus->text().contains(QStringLiteral("暂不可用"))
                    && backupStatus->text().contains(
                        QStringLiteral("这不是空清单")),
                "an unavailable backup store was disguised as an empty listing")) {
        return 1;
    }

    // 真空与退化必须长得完全不同。
    ExtensionStagingBackupListResult genuinelyEmpty;
    genuinelyEmpty.state = ExtensionStagingBackupListState::Empty;
    backupDialog.setBackupListing(genuinelyEmpty, /*restoreDestinationResolved=*/true);
    if (!expect(backupStatus->text().contains(QStringLiteral("为空"))
                    && backupStatus->text().contains(
                        QStringLiteral("确认一份备份都没有"))
                    && !backupStatus->text().contains(QStringLiteral("冻结")),
                "a genuinely empty backup domain reads like a failure")) return 1;

    // 读取失败同样是退化：固定代码之外的文本绝不原样上屏。
    backupDialog.showBackupError(QStringLiteral("<b>rm -rf /Users/someone</b>"));
    if (!expect(backupStatus->text().contains(QStringLiteral("浏览已冻结"))
                    && !backupStatus->text().contains(QStringLiteral("<b>"))
                    && !backupStatus->text().contains(QStringLiteral("/Users/")),
                "an unfixed backup diagnostic reached the screen verbatim")) return 1;

    // 删除、裁剪、立即捕获的入口仍然不存在——连灰掉的都没有（grant-button 先例）。
    // 恢复入口只以 extensionBackupRestoreButton 的封闭形状存在。
    const QList<QPushButton *> backupDialogButtons =
        backupDialog.findChildren<QPushButton *>();
    for (QPushButton *button : backupDialogButtons) {
        if (!expect(!button->text().contains(QStringLiteral("删除"))
                        && !button->text().contains(QStringLiteral("捕获"))
                        && !button->text().contains(QStringLiteral("裁剪"))
                        && (button->objectName().contains(
                                QStringLiteral("Backup"), Qt::CaseInsensitive)
                                == (button->objectName()
                                    == QStringLiteral(
                                        "extensionBackupRestoreButton"))),
                    "a delete/prune/capture affordance exists on the dialog")) {
            return 1;
        }
    }
    // 元对象扫描：恰有 restoreRequested 一个备份动作信号，没有删除/裁剪/捕获信号。
    int restoreSignalCount = 0;
    const QMetaObject *backupMeta = backupDialog.metaObject();
    for (int index = backupMeta->methodOffset();
            index < backupMeta->methodCount(); ++index) {
        const QMetaMethod method = backupMeta->method(index);
        if (method.methodType() != QMetaMethod::Signal) continue;
        const QString name = QString::fromLatin1(method.name());
        if (name == QStringLiteral("restoreRequested")) ++restoreSignalCount;
        if (!expect(!name.contains(QStringLiteral("prune"), Qt::CaseInsensitive)
                        && !name.contains(QStringLiteral("capture"),
                                          Qt::CaseInsensitive)
                        && !name.contains(QStringLiteral("deleteBackup"),
                                          Qt::CaseInsensitive),
                    "the backup surface grew a delete/prune/capture signal")) {
            return 1;
        }
    }
    if (!expect(restoreSignalCount == 1,
                "the restoreRequested signal is missing or duplicated")) {
        return 1;
    }

    // ---- 恢复批准对话与结果报告 ----
    // 手工构造一份 Ready 准备结果：askRestoreDecision 只消费呈现层字段。
    ExtensionStagingRestorePreparation preparation;
    preparation.ok = true;
    preparation.subject = QStringLiteral("mcp:claude-settings");
    preparation.backupId = QStringLiteral("ext_20260905_102030_aabbccdd");
    preparation.destinationRoot = QStringLiteral("/tmp/restore-dest");
    preparation.preRestoreBackupId =
        QStringLiteral("ext_20260906_102030_00000001");
    ExtensionStagingRestorePrompt &prompt = preparation.prompt;
    prompt.state = ExtensionStagingRestorePromptState::Ready;
    prompt.approvable = true;
    prompt.subject = preparation.subject;
    prompt.backupId = preparation.backupId;
    prompt.createdAtLabel = QStringLiteral("2026-09-05T10:20:30.000Z");
    prompt.destinationRoot = preparation.destinationRoot;
    prompt.planIdentity = QStringLiteral("extension-staging-restore-plan:sha256:")
        + QString(64, QLatin1Char('a'));
    prompt.planFingerprint = QStringLiteral("aaaaaaaa…aaaaaaaa");
    prompt.treeIdentity = QStringLiteral("mcp-backup-content:sha256:")
        + QString(64, QLatin1Char('b'));
    prompt.treeFingerprint = QStringLiteral("bbbbbbbb…bbbbbbbb");
    prompt.directoryCount = 0;
    prompt.fileWriteCount = 1;
    prompt.alreadyInPlaceCount = 0;
    prompt.totalBytes = 12;
    ExtensionStagingRestoreEntryRow entryRow;
    entryRow.directory = false;
    entryRow.relativePath = QStringLiteral("settings.json");
    entryRow.byteCount = 12;
    entryRow.sha256 = QString(64, QLatin1Char('c'));
    prompt.entries.append(entryRow);
    prompt.identityBindingNote = QStringLiteral(
        "计划指纹绑定完整计划：以上指纹覆盖全部 1 条操作，包括因截断而未列出的条目");
    prompt.warnings = {ExtensionStagingRestoreWarning::SharedSettingsFileRestore};
    prompt.sharedFileOverwriteNote = QStringLiteral(
        "此恢复覆盖整个共享设置文件，包括其中其他服务器的配置，而不只是该服务器自己的"
        "条目");
    prompt.echoedPlanIdentity = prompt.planIdentity;
    prompt.echoedTreeIdentity = prompt.treeIdentity;

    PromptInspection restorePrompt;
    QTimer inspector;
    inspector.setInterval(1);
    QObject::connect(&inspector, &QTimer::timeout,
                     [&restorePrompt, &inspector]() {
        auto *box = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        if (!box) return;
        const QCheckBox *check = box->checkBox();
        const QAbstractButton *okButton = box->button(QMessageBox::Ok);
        restorePrompt.seen = true;
        restorePrompt.plainDefaultDeny = box->textFormat() == Qt::PlainText
            && check && !check->isChecked()
            && okButton && !okButton->isEnabled();
        restorePrompt.text = box->text();
        box->reject();
        inspector.stop();
    });
    inspector.start();
    ExtensionStagingRestoreApprovalAcknowledgement ack;
    const bool approved = backupDialog.askRestoreDecision(preparation, &ack);
    if (!expect(restorePrompt.seen && restorePrompt.plainDefaultDeny,
                "the restore prompt is not plain-text default-deny")) return 1;
    if (!expect(!approved
                    && ack.decision
                        == ExtensionStagingRestoreApprovalDecision::Decline,
                "a rejected restore prompt did not come back as Decline")) {
        return 1;
    }
    // 披露完整性：完整身份、警告、共享文件说明、执行前备份行、固定执行披露都在屏上。
    if (!expect(restorePrompt.text.contains(prompt.planIdentity)
                    && restorePrompt.text.contains(prompt.treeIdentity)
                    && restorePrompt.text.contains(prompt.backupId)
                    && restorePrompt.text.contains(
                        QStringLiteral("/tmp/restore-dest")),
                "the restore prompt lost the full identities")) return 1;
    if (!expect(restorePrompt.text.contains(
                    QStringLiteral("共享设置文件恢复"))
                    && restorePrompt.text.contains(
                        QStringLiteral("此恢复覆盖整个共享设置文件")),
                "the restore prompt lost the shared-file disclosure")) return 1;
    if (!expect(restorePrompt.text.contains(QStringLiteral("执行前备份"))
                    && restorePrompt.text.contains(
                        QStringLiteral("ext_20260906_102030_00000001"))
                    && restorePrompt.text.contains(
                        QStringLiteral("确认后，本应用将"))
                    && restorePrompt.text.contains(
                        QStringLiteral("覆盖现有的 settings.json")),
                "the restore prompt lost the pre-restore or execution "
                "disclosure")) return 1;
    // 目标原本不存在时如实说"没有恢复前备份"。
    preparation.preRestoreCaptureSkipped = true;
    preparation.preRestoreBackupId.clear();
    PromptInspection skippedPrompt;
    QTimer skippedInspector;
    skippedInspector.setInterval(1);
    QObject::connect(&skippedInspector, &QTimer::timeout,
                     [&skippedPrompt, &skippedInspector]() {
        auto *box = qobject_cast<QMessageBox *>(
            QApplication::activeModalWidget());
        if (!box) return;
        skippedPrompt.seen = true;
        skippedPrompt.text = box->text();
        box->reject();
        skippedInspector.stop();
    });
    skippedInspector.start();
    backupDialog.askRestoreDecision(preparation, &ack);
    if (!expect(skippedPrompt.seen
                    && skippedPrompt.text.contains(
                        QStringLiteral("没有可捕获的当前状态")),
                "a missing-target restore does not say there is no pre-restore "
                "backup")) {
        return 1;
    }
    preparation.preRestoreCaptureSkipped = false;
    preparation.preRestoreBackupId =
        QStringLiteral("ext_20260906_102030_00000001");

    // 错误与拒绝报告：逐阶段文案，固定代码门控。
    backupDialog.showRestoreError(QStringLiteral("capture"),
        QStringLiteral("extension-staging-capture-store-create-failed"));
    if (!expect(restoreStatus->text().contains(
                    QStringLiteral("恢复前捕获当前状态失败"))
                    && restoreStatus->text().contains(
                        QStringLiteral(
                            "extension-staging-capture-store-create-failed")),
                "a capture failure was not reported with its stage text")) {
        return 1;
    }
    backupDialog.showRestoreError(QStringLiteral("listing"),
                                  QStringLiteral("<b>not-a-code</b>"));
    if (!expect(!restoreStatus->text().contains(QStringLiteral("<b>")),
                "an unfixed restore diagnostic reached the screen verbatim")) {
        return 1;
    }
    backupDialog.showRestoreRefusal(QStringLiteral(
        "extension-staging-restore-destination-conflict"));
    if (!expect(restoreStatus->text().contains(QStringLiteral("不一致"))
                    && restoreStatus->text().contains(
                        QStringLiteral("捕获为一份新备份")),
                "a destination conflict does not mention the pre-restore "
                "capture")) {
        return 1;
    }

    // 结果报告：declined 如实已记录；记录失败冻结；Complete 零写入与真实写入各一句；
    // Partial 必须说"混合状态"并指名恢复前备份。
    ExtensionStagingRestoreOutcome outcome;
    outcome.decisionRecorded = true;
    outcome.decision = ExtensionStagingRestoreAuditDecision::Declined;
    backupDialog.showRestoreResult(outcome, preparation);
    if (!expect(restoreStatus->text().contains(QStringLiteral("已取消恢复"))
                    && restoreStatus->text().contains(
                        QStringLiteral("审计链")),
                "a declined restore was not reported as recorded")) return 1;

    outcome = ExtensionStagingRestoreOutcome{};
    outcome.decisionRecorded = false;
    outcome.errorCode = QStringLiteral("extension-restore-flow-ledger-degraded");
    backupDialog.showRestoreResult(outcome, preparation);
    if (!expect(restoreStatus->text().contains(QStringLiteral("审计链"))
                    && restoreStatus->text().contains(QStringLiteral("冻结"))
                    && restoreStatus->text().contains(
                        QStringLiteral("未写入任何内容")),
                "a recording failure was not reported as frozen")) return 1;

    outcome = ExtensionStagingRestoreOutcome{};
    outcome.decisionRecorded = true;
    outcome.decision = ExtensionStagingRestoreAuditDecision::Approved;
    outcome.executed = true;
    outcome.execution.state = ExtensionStagingRestoreExecutionState::Complete;
    outcome.execution.skippedVerifiedCount = 1;
    backupDialog.showRestoreResult(outcome, preparation);
    if (!expect(restoreStatus->text().contains(QStringLiteral("逐字节一致"))
                    && restoreStatus->text().contains(
                        QStringLiteral("无需写入")),
                "a zero-write completion was not reported honestly")) return 1;

    outcome.execution.state = ExtensionStagingRestoreExecutionState::Partial;
    outcome.execution.doneCount = 0;
    outcome.execution.skippedVerifiedCount = 0;
    outcome.execution.failureIndex = 0;
    outcome.execution.errorCode =
        QStringLiteral("extension-restore-execution-write-failed");
    backupDialog.showRestoreResult(outcome, preparation);
    if (!expect(restoreStatus->text().contains(QStringLiteral("混合状态"))
                    && restoreStatus->text().contains(
                        QStringLiteral("ext_20260906_102030_00000001")),
                "a partial restore does not name the mixed state and the "
                "rollback path")) {
        return 1;
    }

    return 0;
}
