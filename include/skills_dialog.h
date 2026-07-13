#ifndef SKILLS_DIALOG_H
#define SKILLS_DIALOG_H

#include <QDialog>

class QLabel;
class QPushButton;
class QTableWidget;
class SkillManager;

class SkillsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SkillsDialog(SkillManager *manager, QWidget *parent = nullptr);

private slots:
    void rebuildTable();
    void onItemChanged(int row, int column);
    void onInstallUrl();
    void onImportDirectory();
    void onDeleteSelected();
    void onOpenFolder();
    void onInstallPresentationRuntime();
    void updateSelection();

private:
    QString selectedSkillId() const;

    SkillManager *m_manager = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_details = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_runtimeButton = nullptr;
    bool m_rebuilding = false;
};

#endif // SKILLS_DIALOG_H
