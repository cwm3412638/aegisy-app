#ifndef ENV_MANAGER_DIALOG_H
#define ENV_MANAGER_DIALOG_H

#include <QDialog>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include "config_manager.h"

class EnvManagerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit EnvManagerDialog(ConfigManager *configManager, QWidget *parent = nullptr);

signals:
    void environmentSwitched(const QString &envId);

private slots:
    void onAddClicked();
    void onEditClicked();
    void onDeleteClicked();
    void onActivateClicked();
    void onEnvSelectionChanged();
    void onEnvDoubleClicked(QListWidgetItem *item);

private:
    void setupUi();
    void loadEnvironments();
    void updateEnvList();
    Environment getSelectedEnvironment() const;
    void showEnvEditor(const Environment &env = Environment(), bool isEdit = false);

    ConfigManager *m_configManager;

    // UI Elements
    QListWidget *m_envList;
    QPushButton *m_addButton;
    QPushButton *m_editButton;
    QPushButton *m_deleteButton;
    QPushButton *m_activateButton;
    QPushButton *m_closeButton;

    QLabel *m_detailsLabel;
    QLabel *m_statusLabel;

    QList<Environment> m_environments;
};

// 环境编辑对话框
class EnvEditorDialog : public QDialog
{
    Q_OBJECT

public:
    explicit EnvEditorDialog(const Environment &env, bool isEdit, QWidget *parent = nullptr);

    Environment getEnvironment() const;

private:
    void setupUi();

    QLineEdit *m_nameEdit;
    QLineEdit *m_apiKeyEdit;
    QLineEdit *m_baseUrlEdit;

    QCheckBox *m_claudeCheckBox;
    QCheckBox *m_cursorCheckBox;
    QCheckBox *m_continueCheckBox;

    Environment m_environment;
    bool m_isEdit;
};

#endif // ENV_MANAGER_DIALOG_H
