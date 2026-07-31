#pragma once

#include <QString>

namespace CanonicalPathPolicy {

enum class Flavor {
    Posix,
    Windows,
};

inline Flavor nativeFlavor()
{
#ifdef Q_OS_WIN
    return Flavor::Windows;
#else
    return Flavor::Posix;
#endif
}

inline QString normalized(const QString &path, Flavor flavor)
{
    QString result = path;
    if (flavor == Flavor::Windows) {
        result.replace(QLatin1Char('\\'), QLatin1Char('/'));
    }

    while (result.size() > 1 && result.endsWith(QLatin1Char('/'))) {
        const bool windowsDriveRoot = flavor == Flavor::Windows
            && result.size() == 3 && result.at(0).isLetter()
            && result.at(1) == QLatin1Char(':')
            && result.at(2) == QLatin1Char('/');
        if (windowsDriveRoot) break;
        result.chop(1);
    }
    return result;
}

inline Qt::CaseSensitivity caseSensitivity(Flavor flavor)
{
    return flavor == Flavor::Windows
        ? Qt::CaseInsensitive : Qt::CaseSensitive;
}

inline bool equals(const QString &left, const QString &right, Flavor flavor)
{
    const QString normalizedLeft = normalized(left, flavor);
    const QString normalizedRight = normalized(right, flavor);
    return !normalizedLeft.isEmpty() && !normalizedRight.isEmpty()
        && normalizedLeft.compare(normalizedRight,
                                  caseSensitivity(flavor)) == 0;
}

inline bool isStrictDescendant(const QString &parent, const QString &path,
                               Flavor flavor)
{
    const QString normalizedParent = normalized(parent, flavor);
    const QString normalizedPath = normalized(path, flavor);
    if (normalizedParent.isEmpty() || normalizedPath.isEmpty()
        || normalizedParent.compare(normalizedPath,
                                    caseSensitivity(flavor)) == 0) {
        return false;
    }
    QString prefix = normalizedParent;
    if (!prefix.endsWith(QLatin1Char('/'))) prefix.append(QLatin1Char('/'));
    return normalizedPath.startsWith(prefix, caseSensitivity(flavor));
}

} // namespace CanonicalPathPolicy
