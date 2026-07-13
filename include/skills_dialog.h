#ifndef SKILLS_DIALOG_H
#define SKILLS_DIALOG_H

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class SkillManager;

class SkillsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SkillsDialog(SkillManager *manager, QWidget *parent = nullptr);

private slots:
    void rebuildTable();
    void rebuildCatalog();
    void onItemChanged(int row, int column);
    void onInstallUrl();
    void onImportDirectory();
    void onInstallCatalogSelected();
    void onDeleteSelected();
    void onOpenFolder();
    void onInstallPresentationRuntime();
    void updateSelection();

private:
    QString selectedSkillId() const;
    QString selectedCatalogSkillId() const;
    void filterCatalog(const QString &text);

    SkillManager *m_manager = nullptr;
    QTabWidget *m_tabs = nullptr;
    QTableWidget *m_table = nullptr;
    QTableWidget *m_catalogTable = nullptr;
    QLineEdit *m_catalogSearch = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_details = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_installCatalogButton = nullptr;
    QPushButton *m_runtimeButton = nullptr;
    bool m_rebuilding = false;
};

#endif // SKILLS_DIALOG_H
