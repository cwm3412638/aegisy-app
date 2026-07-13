#ifndef DESKTOP_ENHANCEMENT_DIALOG_H
#define DESKTOP_ENHANCEMENT_DIALOG_H

#include <QDialog>

#include "desktop_enhancement_manager.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

class DesktopEnhancementDialog : public QDialog
{
    Q_OBJECT

public:
    explicit DesktopEnhancementDialog(DesktopEnhancementManager *manager,
                                      QWidget *parent = nullptr);

signals:
    void openModelsRequested();

private slots:
    void refreshPlugins();
    void filterPlugins();
    void installSelectedPlugin();
    void installComputerUse();
    void syncHistory();
    void localizeClaude();
    void onLocalizationProgress(const QString &message);
    void onLocalizationFinished(bool success, const QString &message);

private:
    void rebuildPluginTable();
    QString selectedPluginId() const;

    DesktopEnhancementManager *m_manager;
    QList<CodexPluginInfo> m_plugins;
    QLineEdit *m_pluginSearch;
    QTableWidget *m_pluginTable;
    QLabel *m_pluginStatus;
    QPushButton *m_installPluginButton;
    QPushButton *m_computerUseButton;
    QLabel *m_historyStatus;
    QLabel *m_claudeStatus;
    QPushButton *m_localizeClaudeButton;
};

#endif // DESKTOP_ENHANCEMENT_DIALOG_H
