#ifndef SKILL_MANAGER_H
#define SKILL_MANAGER_H

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QStringList>

class QNetworkAccessManager;
class QUrl;

struct SkillInfo
{
    QString id;
    QString name;
    QString version;
    QString description;
    QString executor;
    QString source;
    QString path;
    QString requiredGroup;
    QStringList triggers;
    QStringList permissions;
    bool enabled = false;
    bool trusted = false;
    bool builtin = false;
    bool compatible = true;
};

struct SkillCatalogInfo
{
    QString id;
    QString directory;
    QString skillName;
    QString name;
    QString category;
    QString description;
    QString source;
    QStringList requirements;
    QStringList triggers;
    QString instructions;
};

class SkillManager : public QObject
{
    Q_OBJECT

public:
    explicit SkillManager(QObject *parent = nullptr,
                          const QString &skillsRoot = QString());

    QList<SkillInfo> skills() const;
    QList<SkillCatalogInfo> catalogSkills() const;
    SkillInfo skill(const QString &id) const;
    SkillInfo matchSkill(const QString &text) const;
    QString skillInstructions(const QString &id) const;
    QString skillsRoot() const;

    void refresh();
    bool setEnabled(const QString &id, bool enabled, QString *error = nullptr);
    bool removeSkill(const QString &id, QString *error = nullptr);
    bool installFromDirectory(const QString &sourceDirectory, QString *error = nullptr);
    bool installFromUrl(const QString &url, QString *error = nullptr);
    bool installCatalogSkill(const QString &id, QString *error = nullptr);

    bool presentationRuntimeReady() const;
    bool installPresentationRuntime(QString *error = nullptr);
    bool executePresentation(const QJsonObject &plan,
                             const QString &outputPath,
                             QString *error = nullptr) const;

signals:
    void skillsChanged();

private:
    void ensureBuiltInSkills();
    bool installBuiltInSkill(const QString &directoryName,
                             const QStringList &resourceFiles);
    bool installResourceSkill(const QString &resourcePrefix,
                              const QString &directoryName,
                              const QStringList &resourceFiles,
                              QString *error = nullptr);
    SkillInfo loadSkill(const QString &directory) const;
    bool writeManifest(const QString &directory,
                       const QJsonObject &manifest,
                       QString *error = nullptr) const;
    bool copyDirectory(const QString &source,
                       const QString &destination,
                       QString *error,
                       qint64 *totalSize) const;
    QByteArray download(const QUrl &url, bool required, QString *error) const;
    QString normalizedPackageUrl(const QString &url) const;
    QString safeSkillId(const QString &value) const;
    QString presentationPython() const;

    QString m_skillsRoot;
    QList<SkillInfo> m_skills;
    QNetworkAccessManager *m_networkManager = nullptr;
};

#endif // SKILL_MANAGER_H
