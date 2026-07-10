#ifndef PROFILE_ARCHIVE_H
#define PROFILE_ARCHIVE_H

#include <QList>
#include <QString>

struct ArchiveProfile {
    QString name;
    int type = 0;
    QString key;
    QString model;
};

class ProfileArchive
{
public:
    static bool writeEncrypted(const QString &filePath,
                               const QList<ArchiveProfile> &profiles,
                               const QString &password,
                               QString *error);
    static bool readEncrypted(const QString &filePath,
                              const QString &password,
                              QList<ArchiveProfile> *profiles,
                              QString *error);
};

#endif // PROFILE_ARCHIVE_H
