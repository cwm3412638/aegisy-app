#ifndef DESKTOP_ENHANCEMENT_DIALOG_H
#define DESKTOP_ENHANCEMENT_DIALOG_H

#include <QDialog>
#include <QSet>

#include <atomic>
#include <memory>

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
    QStringList selectedPluginIds() const;
    void updateInstallButton();
    void showPluginDetails(int row);
    void startPluginInstallation(const QStringList &pluginIds);

    DesktopEnhancementManager *m_manager;
    QList<CodexPluginInfo> m_plugins;
    QLineEdit *m_pluginSearch;
    QTableWidget *m_pluginTable;
    QLabel *m_pluginStatus;
    QLabel *m_pluginDetails;
    QPushButton *m_installPluginButton;
    QPushButton *m_refreshPluginButton;
    QPushButton *m_computerUseButton;
    QLabel *m_historyStatus;
    QLabel *m_claudeStatus;
    QPushButton *m_localizeClaudeButton;
    QSet<QString> m_checkedPluginIds;
    std::shared_ptr<std::atomic_bool> m_pluginCancel;
    bool m_pluginRefreshRunning = false;
    bool m_pluginInstallRunning = false;
};

#endif // DESKTOP_ENHANCEMENT_DIALOG_H
