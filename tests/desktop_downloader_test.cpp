#include "desktop_downloader.h"

#include <QCoreApplication>
#include <QDebug>
#include <QSysInfo>

#include <iostream>

namespace {

int failures = 0;

void expect(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);

    const QString claudePath = DesktopDownloader::requestPath(
        DesktopDownloader::Product::Claude);
    if (DesktopDownloader::platformSupported()) {
        expect(claudePath.startsWith(
                   QStringLiteral("/api/v1/desktop-downloads/claude/")),
               "desktop download must use the authenticated proxy endpoint");
    } else {
        expect(claudePath.isEmpty(),
               "unsupported platforms must not expose a download endpoint");
    }
#if defined(Q_OS_WIN)
    expect(DesktopDownloader::productSupported(
               DesktopDownloader::Product::ChatGpt),
           "ChatGPT for Windows must use the authenticated proxy download");
    expect(DesktopDownloader::requestPath(
               DesktopDownloader::Product::ChatGpt)
               == QStringLiteral("/api/v1/desktop-downloads/chatgpt/win-x64"),
           "ChatGPT for Windows must target the server proxy endpoint");
#elif defined(Q_OS_MAC)
    if (QSysInfo::currentCpuArchitecture().contains(QStringLiteral("arm"))) {
        expect(DesktopDownloader::productSupported(
                   DesktopDownloader::Product::ChatGpt),
               "ChatGPT must be downloadable on Apple Silicon");
    } else {
        expect(!DesktopDownloader::productSupported(
                   DesktopDownloader::Product::ChatGpt),
               "ChatGPT must not be offered on Intel macOS");
    }
#endif

    QString error;
    expect(!DesktopDownloader::validateInstallerParts(
               QStringLiteral("text/html"), QStringLiteral("dmg"),
               QByteArrayLiteral("<!doctype html>"),
               QByteArray(512, '\0'), 512, &error),
           "HTML responses must be rejected");
    expect(!error.isEmpty(), "rejected payloads must explain the failure");

#if defined(Q_OS_MAC)
    QByteArray dmgFooter(512, '\0');
    dmgFooter.replace(0, 4, QByteArrayLiteral("koly"));
    expect(DesktopDownloader::validateInstallerParts(
               QStringLiteral("application/octet-stream"), QStringLiteral("dmg"),
               QByteArrayLiteral("binary"),
               dmgFooter, 1024, &error),
           "a DMG footer should pass validation on macOS");
    expect(DesktopDownloader::validateInstallerParts(
               QStringLiteral("application/octet-stream"), QStringLiteral("pkg"),
               QByteArrayLiteral("xar!binary"), QByteArray(), 1024, &error),
           "a XAR package header should pass validation on macOS");
    dmgFooter.replace(0, 4, QByteArrayLiteral("nope"));
    expect(!DesktopDownloader::validateInstallerParts(
               QStringLiteral("application/octet-stream"), QStringLiteral("dmg"),
               QByteArrayLiteral("binary"),
               dmgFooter, 1024, &error),
           "an invalid DMG footer must be rejected");
#elif defined(Q_OS_WIN)
    expect(DesktopDownloader::validateInstallerParts(
               QStringLiteral("application/octet-stream"), QStringLiteral("exe"),
               QByteArrayLiteral("MZbinary"),
               QByteArray(), 1024, &error),
           "an MZ header should pass validation on Windows");
    expect(DesktopDownloader::validateInstallerParts(
               QStringLiteral("application/msix"), QStringLiteral("msix"),
               QByteArrayLiteral("PK\x03\x04" "binary"), QByteArray(), 1024, &error),
           "an MSIX ZIP header should pass validation on Windows");
    expect(!DesktopDownloader::validateInstallerParts(
               QStringLiteral("application/octet-stream"), QStringLiteral("exe"),
               QByteArrayLiteral("binary"),
               QByteArray(), 1024, &error),
           "an invalid executable header must be rejected");
#endif

    return failures == 0 ? 0 : 1;
}
