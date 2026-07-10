#include "profile_archive.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QTemporaryDir>

namespace {

int fail(const char *message)
{
    qCritical("%s", message);
    return 1;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid()) {
        return fail("temporary directory creation failed");
    }

    QList<ArchiveProfile> source = {
        { QStringLiteral("Work Codex"), 2, QStringLiteral("sk-secret-one"),
          QStringLiteral("gpt-test") },
        { QStringLiteral("Claude Team"), 1, QStringLiteral("sk-secret-two"),
          QStringLiteral("claude-test") },
    };
    const QString path = directory.filePath(QStringLiteral("profiles.aegisy"));
    QString error;
    if (!ProfileArchive::writeEncrypted(
            path, source, QStringLiteral("correct-horse-battery"), &error)) {
        qCritical("write failed: %s", qPrintable(error));
        return 1;
    }
    if (!QFileInfo::exists(path)) {
        return fail("archive file was not created");
    }

    QList<ArchiveProfile> restored;
    if (!ProfileArchive::readEncrypted(
            path, QStringLiteral("correct-horse-battery"), &restored, &error)) {
        qCritical("read failed: %s", qPrintable(error));
        return 1;
    }
    if (restored.size() != source.size()
            || restored[0].name != source[0].name
            || restored[0].type != source[0].type
            || restored[0].key != source[0].key
            || restored[0].model != source[0].model
            || restored[1].key != source[1].key) {
        return fail("archive round trip mismatch");
    }

    QList<ArchiveProfile> rejected;
    if (ProfileArchive::readEncrypted(
            path, QStringLiteral("wrong-password"), &rejected, &error)) {
        return fail("wrong password unexpectedly succeeded");
    }
    return 0;
}
