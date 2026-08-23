#include "extension_center_dialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTextStream>

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
    if (!expect(buttons.size() == 1
                    && buttons.first()->text() == QStringLiteral("关闭"),
                "read-only extension center exposed a mutation action")) return 1;
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
    return 0;
}
