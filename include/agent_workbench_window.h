#ifndef AGENT_WORKBENCH_WINDOW_H
#define AGENT_WORKBENCH_WINDOW_H

#include <QMainWindow>

class QWebEngineView;
class QWebEnginePage;
class QWebEngineProfile;
class QWebChannel;
class TimelineAPI;

class AgentWorkbenchWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit AgentWorkbenchWindow(QWidget *parent = nullptr);
    ~AgentWorkbenchWindow() override;

private:
    void setupUi();
    void setupMenuBar();
    void setupWebChannel();
    void loadWorkbenchBundle();
    void executeCommand(const QString &cmd);

    QWebEngineProfile *m_profile;
    QWebEnginePage *m_page;
    QWebEngineView *m_view;
    QWebChannel *m_channel;
    TimelineAPI *m_timelineAPI;
};

#endif // AGENT_WORKBENCH_WINDOW_H
