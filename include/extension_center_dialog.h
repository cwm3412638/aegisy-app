#ifndef EXTENSION_CENTER_DIALOG_H
#define EXTENSION_CENTER_DIALOG_H

#include "extension_registry.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QTableWidget;

class ExtensionCenterDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ExtensionCenterDialog(
        const QList<ExtensionRegistryRecord> &records,
        const QStringList &sourceIssueCodes = {},
        QWidget *parent = nullptr);

private slots:
    void applyFilter();

private:
    void populate(const QList<ExtensionRegistryRecord> &records,
                  const QStringList &sourceIssueCodes);

    QList<ExtensionRegistryRecord> m_records;
    QLineEdit *m_search = nullptr;
    QComboBox *m_kindFilter = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
};

#endif // EXTENSION_CENTER_DIALOG_H
