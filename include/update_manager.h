#ifndef UPDATE_MANAGER_H
#define UPDATE_MANAGER_H

#include <QObject>

class UpdateManager : public QObject
{
    Q_OBJECT

public:
    explicit UpdateManager(QObject *parent = nullptr);
    ~UpdateManager() override;

    bool isSupported() const;
    bool automaticallyChecksForUpdates() const;

public slots:
    void checkForUpdates();
    void setAutomaticallyChecksForUpdates(bool enabled);

signals:
    void automaticChecksChanged(bool enabled);

private:
    void *m_platformUpdater = nullptr;
};

#endif // UPDATE_MANAGER_H
