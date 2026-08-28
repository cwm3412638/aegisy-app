#include "extension_center_dialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QCheckBox>
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

bool expect(bool condition, const char *message)
{
    if (!condition) QTextStream(stderr) << message << Qt::endl;
    return condition;
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
    int closeButtons = 0;
    for (QPushButton *button : buttons) {
        reviewButtons += button->objectName() == QStringLiteral("extensionReviewButton");
        closeButtons += button->text() == QStringLiteral("关闭");
    }
    if (!expect(reviewButtons == 3 && closeButtons == 1,
                "extension center review controls are incomplete")) return 1;
    const QList<QPushButton *> reviewControls = dialog.findChildren<QPushButton *>(
        QStringLiteral("extensionReviewButton"));
    if (!expect(!reviewControls.isEmpty() && !reviewControls.first()->isEnabled(),
                "invalid review ledger did not freeze review actions")) return 1;
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
    bool promptOk = false;
    QObject::connect(&reviewDialog, &ExtensionCenterDialog::reviewRequested,
                     [&emitted](const ExtensionReviewRequest &) { ++emitted; });
    QTimer promptInspector;
    promptInspector.setInterval(1);
    QObject::connect(&promptInspector, &QTimer::timeout, [&]() {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box) return;
        const QCheckBox *check = box->checkBox();
        const QAbstractButton *okButton = box->button(QMessageBox::Ok);
        promptOk = box->textFormat() == Qt::PlainText && check && !check->isChecked()
            && okButton && !okButton->isEnabled()
            && box->text().contains(QStringLiteral("来源身份："))
            && box->text().contains(QStringLiteral("内容身份："));
        box->reject();
        promptInspector.stop();
    });
    promptInspector.start();
    enabledReviews.at(1)->click();
    if (!expect(promptOk, "review confirmation is not exact/plain/default-deny")
            || !expect(emitted == 0, "cancelled review emitted a request")) return 1;
    return 0;
}
