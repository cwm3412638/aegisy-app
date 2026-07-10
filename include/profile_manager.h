#ifndef PROFILE_MANAGER_H
#define PROFILE_MANAGER_H

#include <QList>
#include <QObject>
#include <QString>

#include "tool_manager.h"

// ProfileType 只表示一个具体工具。筛选栏的“全部”使用独立的 UI id，
// 不再作为可持久化的档案类型。
enum class ProfileType {
    Claude = 1,
    Codex  = 2,
    Gemini = 3,
};

inline QList<ProfileType> allProfileTypes()
{
    return { ProfileType::Claude, ProfileType::Codex, ProfileType::Gemini };
}

inline bool isValidProfileType(ProfileType type)
{
    return type == ProfileType::Claude
        || type == ProfileType::Codex
        || type == ProfileType::Gemini;
}

inline AiTool toolForType(ProfileType type)
{
    switch (type) {
    case ProfileType::Claude: return AiTool::ClaudeCode;
    case ProfileType::Gemini: return AiTool::GeminiCli;
    case ProfileType::Codex:  return AiTool::CodexCli;
    }
    return AiTool::CodexCli;
}

inline ProfileType profileTypeForTool(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return ProfileType::Claude;
    case AiTool::GeminiCli:  return ProfileType::Gemini;
    case AiTool::CodexCli:   return ProfileType::Codex;
    }
    return ProfileType::Codex;
}

inline QString profileTypeName(ProfileType type)
{
    switch (type) {
    case ProfileType::Claude: return QStringLiteral("Claude");
    case ProfileType::Codex:  return QStringLiteral("Codex");
    case ProfileType::Gemini: return QStringLiteral("Gemini");
    }
    return QStringLiteral("Codex");
}

// 一个档案只绑定一个工具，并只保存这一工具的 Key 与模型。
struct Profile {
    int         index = -1;
    QString     name;
    ProfileType type  = ProfileType::Codex;
    QString     key;
    QString     model;

    AiTool tool() const { return toolForType(type); }

    QString keyFor(AiTool requestedTool) const
    {
        return requestedTool == tool() ? key : QString();
    }

    QString modelFor(AiTool requestedTool) const
    {
        return requestedTool == tool() ? model : QString();
    }

    int configuredCount() const { return key.isEmpty() ? 0 : 1; }
    bool hasAnyKey() const { return !key.isEmpty(); }
};

// 档案管理器：持久化到 QSettings。
// 新布局：
//   profiles/schema_version      = 2
//   profiles/count               = N
//   profiles/active              = 0
//   profiles/<i>/name            = "工作 Codex"
//   profiles/<i>/type            = 2
//   profiles/<i>/key             = "sk-..."
//   profiles/<i>/model           = "gpt-5"
class ProfileManager : public QObject
{
    Q_OBJECT

public:
    explicit ProfileManager(QObject *parent = nullptr);

    QList<Profile> allProfiles() const;
    int     activeIndex() const;
    Profile activeProfile() const;
    int     count() const;

    int addProfile(const QString &name, ProfileType type,
                   const QString &key = QString(),
                   const QString &model = QString());
    void updateProfile(int index, const QString &name, ProfileType type,
                       const QString &key, const QString &model);
    void removeProfile(int index);
    void setActiveIndex(int index);

signals:
    void profilesChanged();
    void activeProfileChanged(int oldIndex, int newIndex);

private:
    void migrateLegacyProfiles();
    void ensureDefaultProfile();
};

#endif // PROFILE_MANAGER_H
