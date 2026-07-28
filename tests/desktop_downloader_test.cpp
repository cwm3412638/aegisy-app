#include "desktop_downloader.h"
#include "tool_manager.h"

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
    const auto claudeProduct = DesktopDownloader::proxiedProductForTool(
        AiTool::ClaudeCode);
    const auto codexProduct = DesktopDownloader::proxiedProductForTool(
        AiTool::CodexCli);
    expect(!DesktopDownloader::proxiedProductForTool(AiTool::GeminiCli),
           "Gemini must not be routed to a Claude or ChatGPT installer");
    expect(!DesktopDownloader::proxiedProductForTool(AiTool::OpenCode),
           "OpenCode must not be routed to a Claude or ChatGPT installer");
    expect(!DesktopDownloader::proxiedProductForTool(static_cast<AiTool>(999)),
           "unknown tools must fail closed instead of selecting an installer");
    if (codexProduct) {
        expect(DesktopDownloader::requestPath(*codexProduct).startsWith(
                   QStringLiteral("/api/v1/desktop-downloads/chatgpt/")),
               "Codex proxy routing must never fall back to the public download page");
    }
    if (DesktopDownloader::platformSupported()) {
        expect(claudeProduct
                   && *claudeProduct == DesktopDownloader::Product::Claude,
               "Claude Code must launch the Claude Desktop proxy downloader");
        expect(claudePath.startsWith(
                   QStringLiteral("/api/v1/desktop-downloads/claude/")),
               "desktop download must use the authenticated proxy endpoint");
    } else {
        expect(!claudeProduct,
               "unsupported platforms must not launch the proxy downloader");
        expect(claudePath.isEmpty(),
               "unsupported platforms must not expose a download endpoint");
    }
#if defined(Q_OS_WIN)
    expect(codexProduct
               && *codexProduct == DesktopDownloader::Product::ChatGpt,
           "Codex must launch the ChatGPT proxy downloader on Windows");
    expect(DesktopDownloader::productSupported(
               DesktopDownloader::Product::ChatGpt),
           "ChatGPT for Windows must use the authenticated proxy download");
    expect(DesktopDownloader::requestPath(
               DesktopDownloader::Product::ChatGpt)
               == QStringLiteral("/api/v1/desktop-downloads/chatgpt/win-x64"),
           "ChatGPT for Windows must target the server proxy endpoint");
#elif defined(Q_OS_MAC)
    if (QSysInfo::currentCpuArchitecture().contains(QStringLiteral("arm"))) {
        expect(codexProduct
                   && *codexProduct == DesktopDownloader::Product::ChatGpt,
               "Codex must launch the ChatGPT proxy downloader on Apple Silicon");
        expect(DesktopDownloader::productSupported(
                   DesktopDownloader::Product::ChatGpt),
               "ChatGPT must be downloadable on Apple Silicon");
    } else {
        expect(!codexProduct,
               "Intel macOS must keep the unsupported official-page fallback");
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
