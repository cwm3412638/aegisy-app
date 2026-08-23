#include "api_keys_dialog.h"
#include "companion_config_projection.h"
#include "companion_key_management_projection.h"
#include "companion_model_projection.h"

#include <QApplication>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>

#include <iostream>

class ApiKeysDialogTestAccess
{
public:
    static void install(ApiKeysDialog &dialog,
                        const QJsonObject &configuration,
                        const QJsonObject &management)
    {
        dialog.m_accountIdentity = configuration.value(
            QStringLiteral("account_identity")).toString();
        dialog.m_configurationProjectionSha256 = configuration.value(
            QStringLiteral("projection_sha256")).toString();
        dialog.m_managementRequestId = QStringLiteral("dialog-management-fixture");
        dialog.onManagementReceived(dialog.m_managementRequestId, management);
    }

    static void toggle(ApiKeysDialog &dialog) { dialog.onToggleStatusClicked(); }
    static void test(ApiKeysDialog &dialog) { dialog.onTestKeyClicked(); }
    static void refresh(ApiKeysDialog &dialog) { dialog.loadApiKeys(); }

    static void setPendingManagement(ApiKeysDialog &dialog,
                                     const QString &requestId)
    {
        dialog.m_managementRequestId = requestId;
    }

    static void deliverManagement(ApiKeysDialog &dialog,
                                  const QString &requestId,
                                  const QJsonObject &projection)
    {
        dialog.onManagementReceived(requestId, projection);
    }

    static void setPendingTest(ApiKeysDialog &dialog,
                               const QString &requestId,
                               const QString &keyIdentity)
    {
        dialog.m_testRequestId = requestId;
        dialog.m_testKeyIdentity = keyIdentity;
    }

    static void deliverModel(ApiKeysDialog &dialog,
                             const QString &requestId,
                             const QString &keyIdentity,
                             const QJsonObject &projection)
    {
        dialog.onCompanionModelsReceived(requestId, keyIdentity, projection);
    }

    static QString status(const ApiKeysDialog &dialog)
    {
        return dialog.m_statusLabel->text();
    }

    static bool hasPendingManagement(const ApiKeysDialog &dialog)
    {
        return !dialog.m_managementRequestId.isEmpty();
    }

    static bool hasPendingTest(const ApiKeysDialog &dialog)
    {
        return !dialog.m_testRequestId.isEmpty()
            || !dialog.m_testKeyIdentity.isEmpty();
    }
};

namespace {

bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

QString handle(const QString &prefix, QChar value)
{
    return prefix + QString(64, value);
}

QJsonObject managementProjection(const QJsonObject &configuration,
                                 const QString &keyIdentity)
{
    const QString groupHandle = handle(
        QStringLiteral("website-group-management:opaque:"), QLatin1Char('1'));
    const QJsonArray groups{QJsonObject{
        {QStringLiteral("group_handle"), groupHandle},
        {QStringLiteral("display_name"), QStringLiteral("Codex")},
        {QStringLiteral("platform"), QStringLiteral("openai")},
        {QStringLiteral("create_handle"), handle(
            QStringLiteral("website-group-create:opaque:"), QLatin1Char('2'))}
    }};
    const QHash<QString, QJsonObject> metadata{{keyIdentity, QJsonObject{
        {QStringLiteral("update_handle"), handle(
            QStringLiteral("website-key-update:opaque:"), QLatin1Char('3'))},
        {QStringLiteral("delete_handle"), handle(
            QStringLiteral("website-key-delete:opaque:"), QLatin1Char('4'))},
        {QStringLiteral("test_handle"), handle(
            QStringLiteral("website-key-test:opaque:"), QLatin1Char('5'))},
        {QStringLiteral("group_handle"), groupHandle},
        {QStringLiteral("quota"), 1000.0},
        {QStringLiteral("quota_used"), 125.0},
        {QStringLiteral("created_at"), QStringLiteral("2026-08-01T00:00:00Z")},
        {QStringLiteral("expires_at"), QJsonValue::Null}
    }}};
    return CompanionKeyManagementProjection::fromConfiguration(
        configuration, metadata, groups);
}

QString visibleTableData(QTableWidget *table)
{
    QString result;
    for (int row = 0; row < table->rowCount(); ++row) {
        for (int column = 0; column < table->columnCount(); ++column) {
            const QTableWidgetItem *item = table->item(row, column);
            if (!item) continue;
            result += item->text() + QLatin1Char('\n') + item->toolTip();
            for (int role = Qt::UserRole; role < Qt::UserRole + 16; ++role) {
                result += item->data(role).toString();
            }
        }
    }
    return result;
}

bool allDisabled(const QList<QPushButton *> &buttons)
{
    for (const QPushButton *button : buttons) {
        if (!button || button->isEnabled()) return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    const QString rawId = QStringLiteral("raw-dialog-key-id-42");
    const QString credential = QStringLiteral("sk-dialog-secret-sentinel");
    const QString displayName = QStringLiteral("<b>Managed & literal</b>");
    const QJsonArray rawKeys{QJsonObject{
        {QStringLiteral("id"), rawId},
        {QStringLiteral("key"), credential},
        {QStringLiteral("name"), displayName},
        {QStringLiteral("status"), QStringLiteral("active")},
        {QStringLiteral("group"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("Codex")},
            {QStringLiteral("platform"), QStringLiteral("openai")}
        }}
    }};
    const QString account = QStringLiteral("website-account-session:sha256:")
        + QString(64, QLatin1Char('a'));
    const QJsonObject configuration = CompanionConfigProjection::fromWebsiteApiKeys(
        rawKeys, account, QStringLiteral("https://www.aegisy.cc"), 1770000000000LL);
    const QString keyIdentity = configuration.value(QStringLiteral("keys"))
        .toArray().at(0).toObject().value(QStringLiteral("key_identity")).toString();
    const QJsonObject management = managementProjection(configuration, keyIdentity);
    if (!require(CompanionKeyManagementProjection::validate(management),
                 "management fixture is invalid")) return 1;

    ApiClient client;
    ApiKeysDialog dialog(&client);
    ApiKeysDialogTestAccess::install(dialog, configuration, management);
    dialog.show();
    application.processEvents();

    auto *table = dialog.findChild<QTableWidget *>(QStringLiteral("apiKeysTable"));
    auto *status = dialog.findChild<QLabel *>(QStringLiteral("apiKeysStatusLabel"));
    auto *total = dialog.findChild<QLabel *>(QStringLiteral("apiKeysTotalLabel"));
    auto *refresh = dialog.findChild<QPushButton *>(QStringLiteral("apiKeysRefreshButton"));
    auto *create = dialog.findChild<QPushButton *>(QStringLiteral("apiKeysCreateButton"));
    auto *test = dialog.findChild<QPushButton *>(QStringLiteral("apiKeysTestButton"));
    auto *edit = dialog.findChild<QPushButton *>(QStringLiteral("apiKeysEditButton"));
    auto *group = dialog.findChild<QPushButton *>(QStringLiteral("apiKeysGroupButton"));
    auto *toggle = dialog.findChild<QPushButton *>(QStringLiteral("apiKeysToggleButton"));
    auto *remove = dialog.findChild<QPushButton *>(QStringLiteral("apiKeysDeleteButton"));
    const QList<QPushButton *> mutationButtons{create, test, edit, group, toggle, remove};
    if (!require(table && status && total && refresh && table->rowCount() == 1,
                 "management projection did not render")
            || !require(status->textFormat() == Qt::PlainText,
                        "status label is not plain text")
            || !require(table->item(0, 0)
                            && table->item(0, 0)->text() == displayName,
                        "safe display name was not rendered literally")) return 1;

    table->selectRow(0);
    application.processEvents();
    const QString visible = visibleTableData(table);
    const QByteArray encoded = QJsonDocument(management).toJson(QJsonDocument::Compact);
    if (!require(!visible.contains(rawId) && !visible.contains(credential),
                 "raw Key ID or credential entered the table")
            || !require(!visible.contains(QStringLiteral("website-key-update:opaque:"))
                        && !visible.contains(QStringLiteral("website-key-delete:opaque:"))
                        && !visible.contains(QStringLiteral("website-key-test:opaque:")),
                        "action handle entered table text or item data")
            || !require(!encoded.contains(credential.toUtf8())
                        && !encoded.contains(rawId.toUtf8()),
                        "management projection contains raw fixture data")
            || !require(create->isEnabled() && test->isEnabled()
                        && edit->isEnabled() && group->isEnabled()
                        && toggle->isEnabled() && remove->isEnabled(),
                        "valid management controls are disabled")) return 1;

    ApiKeysDialogTestAccess::toggle(dialog);
    application.processEvents();
    if (!require(ApiKeysDialogTestAccess::status(dialog).startsWith(
                     QStringLiteral("操作失败：")),
                 "synchronous mutation rejection was overwritten by busy text")) return 1;

    ApiKeysDialogTestAccess::install(dialog, configuration, management);
    table->selectRow(0);
    ApiKeysDialogTestAccess::test(dialog);
    application.processEvents();
    if (!require(ApiKeysDialogTestAccess::status(dialog).startsWith(
                     QStringLiteral("Key 测试失败："))
                    && !ApiKeysDialogTestAccess::hasPendingTest(dialog),
                 "synchronous test rejection was overwritten or left pending")) return 1;

    ApiKeysDialogTestAccess::install(dialog, configuration, management);
    ApiKeysDialogTestAccess::setPendingManagement(
        dialog, QStringLiteral("owned-management"));
    const QString beforeWrongManagement = ApiKeysDialogTestAccess::status(dialog);
    ApiKeysDialogTestAccess::deliverManagement(
        dialog, QStringLiteral("wrong-management"), QJsonObject());
    if (!require(ApiKeysDialogTestAccess::status(dialog) == beforeWrongManagement
                    && ApiKeysDialogTestAccess::hasPendingManagement(dialog)
                    && table->rowCount() == 1,
                 "unowned management result changed dialog state")) return 1;
    ApiKeysDialogTestAccess::deliverManagement(
        dialog, QStringLiteral("owned-management"), QJsonObject());
    if (!require(ApiKeysDialogTestAccess::status(dialog)
                    == QStringLiteral("管理投影校验失败")
                    && !ApiKeysDialogTestAccess::hasPendingManagement(dialog)
                    && table->rowCount() == 0 && total->text() == QStringLiteral("共 0 个 Key")
                    && allDisabled(mutationButtons),
                 "owned invalid management result did not fail closed")) return 1;

    const QJsonObject model = CompanionModelProjection::fromProviderResponse(
        keyIdentity, QJsonObject{{QStringLiteral("data"), QJsonArray{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("gpt-test")}}
        }}});
    if (!require(CompanionModelProjection::validate(model),
                 "model fixture is invalid")) return 1;
    ApiKeysDialogTestAccess::install(dialog, configuration, management);
    table->selectRow(0);
    ApiKeysDialogTestAccess::setPendingTest(
        dialog, QStringLiteral("owned-model"), keyIdentity);
    const QString beforeWrongModel = ApiKeysDialogTestAccess::status(dialog);
    ApiKeysDialogTestAccess::deliverModel(
        dialog, QStringLiteral("wrong-model"), keyIdentity, model);
    ApiKeysDialogTestAccess::deliverModel(
        dialog, QStringLiteral("owned-model"), QStringLiteral("wrong-key"), model);
    if (!require(ApiKeysDialogTestAccess::status(dialog) == beforeWrongModel
                    && ApiKeysDialogTestAccess::hasPendingTest(dialog),
                 "unowned model result changed dialog state")) return 1;
    ApiKeysDialogTestAccess::deliverModel(
        dialog, QStringLiteral("owned-model"), keyIdentity, QJsonObject());
    if (!require(ApiKeysDialogTestAccess::status(dialog)
                    == QStringLiteral("Key 测试结果校验失败")
                    && !ApiKeysDialogTestAccess::hasPendingTest(dialog),
                 "owned invalid model result did not terminate the test")) return 1;

    ApiKeysDialogTestAccess::setPendingTest(
        dialog, QStringLiteral("valid-model"), keyIdentity);
    ApiKeysDialogTestAccess::deliverModel(
        dialog, QStringLiteral("valid-model"), keyIdentity, model);
    if (!require(ApiKeysDialogTestAccess::status(dialog)
                    == QStringLiteral("Key 可用：1 个模型")
                    && !ApiKeysDialogTestAccess::hasPendingTest(dialog),
                 "owned valid model result did not complete")) return 1;

    ApiKeysDialogTestAccess::install(dialog, configuration, management);
    table->selectRow(0);
    ApiKeysDialogTestAccess::setPendingTest(
        dialog, QStringLiteral("old-test"), keyIdentity);
    ApiKeysDialogTestAccess::refresh(dialog);
    const QString statusAfterRefresh = ApiKeysDialogTestAccess::status(dialog);
    if (!require(table->rowCount() == 0
                    && total->text() == QStringLiteral("共 0 个 Key")
                    && allDisabled(mutationButtons)
                    && !ApiKeysDialogTestAccess::hasPendingTest(dialog),
                 "refresh retained stale Key state or controls")) return 1;
    ApiKeysDialogTestAccess::deliverModel(
        dialog, QStringLiteral("old-test"), keyIdentity, model);
    if (!require(ApiKeysDialogTestAccess::status(dialog) == statusAfterRefresh
                    && !ApiKeysDialogTestAccess::status(dialog).contains(
                        QStringLiteral("Key 可用")),
                 "late pre-refresh Key test changed dialog state")) return 1;

    return 0;
}
