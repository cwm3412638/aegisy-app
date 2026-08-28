#ifndef EXTENSION_CENTER_DIALOG_H
#define EXTENSION_CENTER_DIALOG_H

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
        QWidget *parent = nullptr);

    void setReviewSnapshot(const QList<ExtensionRegistryRecord> &records,
                           const QStringList &sourceIssueCodes,
                           const ExtensionReviewLedgerStoreResult &ledger);
    void setReviewBusy(bool busy);
    void showReviewError(const QString &errorCode);

signals:
    void reviewRequested(const ExtensionReviewRequest &request);

private slots:
    void applyFilter();
    void reviewRow(int row);

private:
    void populate(const QList<ExtensionRegistryRecord> &records,
                  const QStringList &sourceIssueCodes,
                  const ExtensionReviewLedgerStoreResult &ledger);
    bool confirmPrompt(const ExtensionReviewPrompt &prompt,
                       ExtensionReviewAction action);
    bool confirmRevoke(const ExtensionReviewPin &pin);
    void rebuildRows();

    struct ReviewRow {
        ExtensionRegistryRecord record;
        ExtensionReviewPin pin;
        bool hasRecord = false;
        bool hasPin = false;
    };

    QList<ExtensionRegistryRecord> m_records;
    QLineEdit *m_search = nullptr;
    QComboBox *m_kindFilter = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_reviewStatus = nullptr;
    QList<QPushButton *> m_reviewButtons;
    ExtensionReviewLedgerStoreResult m_ledger;
    QList<ReviewRow> m_rows;
    bool m_reviewBusy = false;
};

#endif // EXTENSION_CENTER_DIALOG_H
