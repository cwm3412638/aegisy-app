#include "profile_manager.h"
#include <QSettings>

static const QString kPfx = "profiles";

ProfileManager::ProfileManager(QObject *parent) : QObject(parent)
{
    ensureDefaultProfile();
}

void ProfileManager::ensureDefaultProfile()
{
    if (count() == 0) addProfile("默认");
}

int ProfileManager::count() const
{
    return QSettings().value(kPfx + "/count", 0).toInt();
}

QList<Profile> ProfileManager::allProfiles() const
{
    QSettings s;
    const int n = s.value(kPfx + "/count", 0).toInt();
    QList<Profile> result;
    result.reserve(n);
    for (int i = 0; i < n; ++i) {
        Profile p;
        p.index       = i;
        p.name        = s.value(QString("%1/%2/name").arg(kPfx).arg(i),
                                QString("档案 %1").arg(i+1)).toString();
        p.claudeKey   = s.value(QString("%1/%2/claude_key").arg(kPfx).arg(i)).toString();
        p.claudeModel = s.value(QString("%1/%2/claude_model").arg(kPfx).arg(i)).toString();
        p.codexKey    = s.value(QString("%1/%2/codex_key").arg(kPfx).arg(i)).toString();
        p.codexModel  = s.value(QString("%1/%2/codex_model").arg(kPfx).arg(i)).toString();
        p.geminiKey   = s.value(QString("%1/%2/gemini_key").arg(kPfx).arg(i)).toString();
        p.geminiModel = s.value(QString("%1/%2/gemini_model").arg(kPfx).arg(i)).toString();
        result.append(p);
    }
    return result;
}

int ProfileManager::activeIndex() const
{
    const int n = count();
    if (n == 0) return -1;
    return qBound(0, QSettings().value(kPfx + "/active", 0).toInt(), n - 1);
}

Profile ProfileManager::activeProfile() const
{
    const QList<Profile> all = allProfiles();
    const int idx = activeIndex();
    return (idx >= 0 && idx < all.size()) ? all[idx] : Profile{};
}

int ProfileManager::addProfile(const QString &name)
{
    QSettings s;
    const int n = s.value(kPfx + "/count", 0).toInt();
    s.setValue(QString("%1/%2/name").arg(kPfx).arg(n), name);
    s.setValue(kPfx + "/count", n + 1);
    emit profilesChanged();
    return n;
}

void ProfileManager::removeProfile(int index)
{
    QSettings s;
    const int n = s.value(kPfx + "/count", 0).toInt();
    if (n <= 1) return;
    for (int i = index; i < n - 1; ++i) {
        const QString src = QString("%1/%2").arg(kPfx).arg(i + 1);
        const QString dst = QString("%1/%2").arg(kPfx).arg(i);
        for (const QString &k : {"name","claude_key","claude_model",
                                  "codex_key","codex_model",
                                  "gemini_key","gemini_model"}) {
            s.setValue(dst + "/" + k, s.value(src + "/" + k));
        }
    }
    s.remove(QString("%1/%2").arg(kPfx).arg(n - 1));
    s.setValue(kPfx + "/count", n - 1);
    const int active = s.value(kPfx + "/active", 0).toInt();
    if (active >= n - 1) s.setValue(kPfx + "/active", n - 2);
    emit profilesChanged();
}

void ProfileManager::renameProfile(int index, const QString &name)
{
    QSettings().setValue(QString("%1/%2/name").arg(kPfx).arg(index), name);
    emit profilesChanged();
}

void ProfileManager::setActiveIndex(int index)
{
    const int old = activeIndex();
    QSettings().setValue(kPfx + "/active", index);
    emit activeProfileChanged(old, index);
}

void ProfileManager::saveToolConfig(int profileIndex, AiTool tool,
                                     const QString &key, const QString &model)
{
    QSettings s;
    QString keyField, modelField;
    switch (tool) {
        case AiTool::ClaudeCode: keyField = "claude_key"; modelField = "claude_model"; break;
        case AiTool::CodexCli:   keyField = "codex_key";  modelField = "codex_model";  break;
        case AiTool::GeminiCli:  keyField = "gemini_key"; modelField = "gemini_model"; break;
        default: return;
    }
    const QString base = QString("%1/%2").arg(kPfx).arg(profileIndex);
    s.setValue(base + "/" + keyField,   key);
    s.setValue(base + "/" + modelField, model);
}

void ProfileManager::saveKey(int profileIndex, AiTool tool, const QString &key)
{
    saveToolConfig(profileIndex, tool, key, QString());
}
