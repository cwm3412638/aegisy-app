#include "extension_center_dialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
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
    return 0;
}
