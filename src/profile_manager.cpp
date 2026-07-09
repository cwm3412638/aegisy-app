#include "profile_manager.h"
#include <QSettings>
#include <QList>

static const QString kPfx = "profiles";

// 工具枚举 → QSettings key 片段
static QString tk(AiTool tool) {
    switch (tool) {
        case AiTool::ClaudeCode: return "claude";
        case AiTool::CodexCli:  return "codex";
        case AiTool::GeminiCli: return "gemini";
        default:                return "unknown";
    }
}

ProfileManager::ProfileManager(QObject *parent) : QObject(parent)
{
    ensureDefaultProfile();
}

void ProfileManager::ensureDefaultProfile()
{
    if (count() == 0) {
        addProfile("默认");
    }
}

int ProfileManager::count() const
{
    return QSettings().value(kPfx + "/count", 0).toInt();
}

QList<Profile> ProfileManager::allProfiles() const
{
    QSettings settings;
    const int n = settings.value(kPfx + "/count", 0).toInt();
    QList<Profile> result;
    result.reserve(n);

    for (int i = 0; i < n; ++i) {
        Profile p;
        p.index     = i;
        p.name      = settings.value(QString("%1/%2/name").arg(kPfx).arg(i),
                                     QString("档案 %1").arg(i + 1)).toString();
        p.claudeKey = settings.value(QString("%1/%2/claude").arg(kPfx).arg(i)).toString();
        p.codexKey  = settings.value(QString("%1/%2/codex").arg(kPfx).arg(i)).toString();
        p.geminiKey = settings.value(QString("%1/%2/gemini").arg(kPfx).arg(i)).toString();
        result.append(p);
    }
    return result;
}

int ProfileManager::activeIndex() const
{
    const int n = count();
    if (n == 0) return -1;
    const int idx = QSettings().value(kPfx + "/active", 0).toInt();
    return qBound(0, idx, n - 1);
}

Profile ProfileManager::activeProfile() const
{
    const QList<Profile> all = allProfiles();
    const int idx = activeIndex();
    if (idx >= 0 && idx < all.size()) return all[idx];
    return Profile{};
}

int ProfileManager::addProfile(const QString &name)
{
    QSettings settings;
    const int n = settings.value(kPfx + "/count", 0).toInt();
    settings.setValue(QString("%1/%2/name").arg(kPfx).arg(n), name);
    settings.setValue(kPfx + "/count", n + 1);
    emit profilesChanged();
    return n;
}

void ProfileManager::removeProfile(int index)
{
    QSettings settings;
    const int n = settings.value(kPfx + "/count", 0).toInt();
    if (n <= 1) return;  // 不能删最后一个档案

    // 将 index 之后的档案整体前移一位
    for (int i = index; i < n - 1; ++i) {
        const QString src = QString("%1/%2").arg(kPfx).arg(i + 1);
        const QString dst = QString("%1/%2").arg(kPfx).arg(i);
        settings.setValue(dst + "/name",   settings.value(src + "/name"));
        settings.setValue(dst + "/claude", settings.value(src + "/claude"));
        settings.setValue(dst + "/codex",  settings.value(src + "/codex"));
        settings.setValue(dst + "/gemini", settings.value(src + "/gemini"));
    }

    // 清除最后一个槽位（已前移，原数据废弃）
    settings.remove(QString("%1/%2").arg(kPfx).arg(n - 1));
    settings.setValue(kPfx + "/count", n - 1);

    // 修正 active index，防止越界
    const int active = QSettings().value(kPfx + "/active", 0).toInt();
    if (active >= n - 1) {
        settings.setValue(kPfx + "/active", n - 2);
    }

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

void ProfileManager::saveKey(int profileIndex, AiTool tool, const QString &key)
{
    QSettings().setValue(
        QString("%1/%2/%3").arg(kPfx).arg(profileIndex).arg(tk(tool)),
        key);
}
