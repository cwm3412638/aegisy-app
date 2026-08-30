#ifndef EXTENSION_CENTER_DIALOG_H
#define EXTENSION_CENTER_DIALOG_H

#include "extension_enablement_ledger_store.h"
#include "extension_enablement_presentation.h"
#include "extension_enablement_workflow.h"
#include "extension_registry.h"
#include "extension_review_ledger_store.h"
#include "extension_review_presentation.h"
#include "extension_review_workflow.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QTableWidget;
class QAbstractButton;
class QPushButton;

class ExtensionCenterDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ExtensionCenterDialog(
        const QList<ExtensionRegistryRecord> &records,
        const QStringList &sourceIssueCodes = {},
        const ExtensionReviewLedgerStoreResult &ledger = {},
        const ExtensionEnablementLedgerStoreResult &grants = {},
        QWidget *parent = nullptr);

    void setReviewSnapshot(const QList<ExtensionRegistryRecord> &records,
                           const QStringList &sourceIssueCodes,
                           const ExtensionReviewLedgerStoreResult &ledger);
    void setReviewBusy(bool busy);
    void showReviewError(const QString &errorCode);

    void setEnablementSnapshot(const QList<ExtensionRegistryRecord> &records,
                               const QStringList &sourceIssueCodes,
                               const ExtensionEnablementLedgerStoreResult &grants);
    void setEnablementBusy(bool busy);
    void showEnablementError(const QString &errorCode);

signals:
    void reviewRequested(const ExtensionReviewRequest &request);
    void enablementRequested(const ExtensionEnablementRequest &request);

private slots:
    void applyFilter();
    void reviewRow(int row);
    void enablementRow(int row);

private:
    void populate(const QList<ExtensionRegistryRecord> &records,
                  const QStringList &sourceIssueCodes,
                  const ExtensionReviewLedgerStoreResult &ledger,
                  const ExtensionEnablementLedgerStoreResult &grants);
    bool confirmPrompt(const ExtensionReviewPrompt &prompt,
                       ExtensionReviewAction action);
    bool confirmRevoke(const ExtensionReviewPin &pin);
    // 授权确认与复核确认问的是两个不同的问题：复核问"有人看过这份内容吗"，授权问"你要让
    // 这份内容运行吗"。因此它有独立的确认文本，并且必须把"这次授权当前不会让任何东西
    // 运行"写在人能看到的地方。
    bool confirmEnablementPrompt(const ExtensionEnablementPrompt &prompt);
    bool confirmEnablementRevocation(const ExtensionRevocationPrompt &prompt);
    // 授权动作的可点击性只能来自呈现层的判定，不能由这里另算一遍：另算一遍必然会与那
    // 一层漂移，而漂移的方向是给一份没人复核过的内容提供授权按钮，那份授权会以已认证的
    // 形式留在账本里，等复核出现的那一刻自动生效。
    ExtensionEnablementPrompt enablementPromptFor(int row) const;
    void rebuildRows();

    struct ReviewRow {
        ExtensionRegistryRecord record;
        ExtensionReviewPin pin;
        ExtensionEnablementGrant grant;
        bool hasRecord = false;
        bool hasPin = false;
        bool hasGrant = false;
    };

    QList<ExtensionRegistryRecord> m_records;
    QLineEdit *m_search = nullptr;
    QComboBox *m_kindFilter = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_reviewStatus = nullptr;
    QLabel *m_enablementStatus = nullptr;
    QList<QPushButton *> m_reviewButtons;
    QList<QPushButton *> m_enablementButtons;
    ExtensionReviewLedgerStoreResult m_ledger;
    ExtensionEnablementLedgerStoreResult m_grants;
    QList<ReviewRow> m_rows;
    bool m_reviewBusy = false;
    bool m_enablementBusy = false;
};

#endif // EXTENSION_CENTER_DIALOG_H
