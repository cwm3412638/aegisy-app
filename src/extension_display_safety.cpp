#include "extension_display_safety.h"

#include <QRegularExpression>
#include <QSet>

bool ExtensionDisplaySafety::safeDisplayText(const QString &value, int maximum)
{
    if (value.isEmpty() || value.size() > maximum) return false;
    for (const QChar character : value) {
        const QChar::Category category = character.category();
        if (character.isNull() || category == QChar::Other_Control
                || category == QChar::Other_Format
                || category == QChar::Other_Surrogate
                || category == QChar::Other_PrivateUse
                || category == QChar::Other_NotAssigned) {
            return false;
        }
        // 双向控制与零宽字符即使不属于格式类，也必须显式排除。
        const ushort code = character.unicode();
        if (code == 0x7f || (code >= 0x2028 && code <= 0x202f)
                || (code >= 0x2066 && code <= 0x2069)
                || (code >= 0x200b && code <= 0x200f) || code == 0xfeff) {
            return false;
        }
    }
    // 前后空白会让两个不同的名称在表格里看起来一致。
    return value.trimmed() == value;
}

bool ExtensionDisplaySafety::hashIdentity(const QString &value,
                                          const QString &prefix)
{
    return QRegularExpression(QStringLiteral("^%1[0-9a-f]{64}$")
        .arg(QRegularExpression::escape(prefix))).match(value).hasMatch();
}

bool ExtensionDisplaySafety::validId(const QString &value)
{
    return QRegularExpression(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,127}$"))
        .match(value).hasMatch();
}

QString ExtensionDisplaySafety::fingerprint(const QString &identity)
{
    const QString hex = identity.section(QLatin1Char(':'), -1);
    if (hex.size() < 20) return hex;
    return hex.left(8) + QStringLiteral("…") + hex.right(8);
}

QString ExtensionDisplaySafety::kindLabel(ExtensionKind kind)
{
    switch (kind) {
    case ExtensionKind::CodexPlugin: return QStringLiteral("Codex 插件");
    case ExtensionKind::Skill: return QStringLiteral("Skill");
    case ExtensionKind::Mcp: return QStringLiteral("MCP 服务器");
    }
    return QString();
}

bool ExtensionDisplaySafety::beyondReadOnly(const QString &capability)
{
    static const QSet<QString> denied{
        QStringLiteral("process"), QStringLiteral("command-execution"),
        QStringLiteral("git-mutation"), QStringLiteral("filesystem-write")};
    return denied.contains(capability) || capability.contains(QStringLiteral("write"))
        || capability.contains(QStringLiteral("exec"))
        || capability.contains(QStringLiteral("mutation"))
        || capability.contains(QStringLiteral("delete"));
}

bool ExtensionDisplaySafety::nameAgreesWithIdentifier(const QString &name,
                                                     const QString &id)
{
    const QString foldedName = name.toCaseFolded()
        .remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
    const QString foldedId = id.toCaseFolded()
        .remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
    if (foldedName.isEmpty() || foldedId.isEmpty()) return false;
    return foldedId.contains(foldedName) || foldedName.contains(foldedId);
}
