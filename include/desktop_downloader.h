#ifndef DESKTOP_DOWNLOADER_H
#define DESKTOP_DOWNLOADER_H

#include <QDialog>
#include <QByteArray>
#include <QString>
#include <QStyle>

class QNetworkAccessManager;
class QNetworkReply;
class QProgressBar;
class QLabel;
class QPushButton;
class QTemporaryFile;

// 通过 aegisy.cc 服务器代理下载桌面客户端安装包（国内可直连）。
// 服务端约定：
//   GET /api/v1/desktop-downloads/{product}/{platform}
//     product  = claude | chatgpt
//     platform = mac-arm64 | mac-x64 | win-x64
//     鉴权     = Authorization: Bearer <登录 token>
//   服务器代拉官方安装包并流式回传（不得 302 跳回官方 CDN），
//   必须返回 X-Aegisy-Download-Mode: proxy、X-Aegisy-Installer-Format，
//   并建议返回 Content-Length 与 Content-Disposition: filename。客户端拒绝
//   重定向、没有代理标识的响应和无法验证的安装包格式。
class DesktopDownloader : public QDialog
{
    Q_OBJECT

public:
    enum class Product { Claude, ChatGpt };

    DesktopDownloader(Product product,
                      const QString &appName,
                      const QString &authToken,
                      const QString &baseUrl,
                      QWidget *parent = nullptr);

    // 当前平台是否受支持（有对应安装包）。Linux/未知平台返回 false。
    static bool platformSupported();
    static bool productSupported(Product product);
    static QString requestPath(Product product);
    static bool validateInstallerParts(const QString &contentType,
                                       const QString &installerFormat,
                                       const QByteArray &firstBytes,
                                       const QByteArray &lastBytes,
                                       qint64 size,
                                       QString *error = nullptr);

private slots:
    void startDownload();
    void onReadyRead();
    void onProgress(qint64 received, qint64 total);
    void onFinished();
    void cancelDownload();
    void openDownloadedFile();
    void openDownloadFolder();
    void openOfficialDownloadPage();

private:
    static QString productSlug(Product product);
    static QString platformSlug();     // mac-arm64 / mac-x64 / win-x64 / 空
    static QString defaultFileName(Product product);
    QString officialDownloadUrl() const;
    QString responseFileName() const;
    QString uniqueSavePath(const QString &directory, const QString &fileName) const;
    bool prepareTemporaryFile(QString *error);
    bool finalizeTemporaryFile(QString *error);
    void setVisualState(const QString &title, const QString &detail,
                        const QString &badge, const QString &badgeStyle,
                        QStyle::StandardPixmap icon);
    void configureActions(const QString &primaryText,
                          void (DesktopDownloader::*primarySlot)(),
                          const QString &secondaryText = QString(),
                          void (DesktopDownloader::*secondarySlot)() = nullptr);
    void fail(const QString &message);
    void cleanupReply();
    void cleanupTemporaryFile();

    Product m_product;
    QString m_appName;
    QString m_authToken;
    QString m_baseUrl;

    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_reply = nullptr;
    QTemporaryFile *m_file = nullptr;
    QString m_savePath;
    QByteArray m_firstBytes;
    QByteArray m_lastBytes;
    QString m_streamError;
    qint64 m_receivedBytes = 0;

    QLabel *m_stateIcon = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_sourceBadge = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QPushButton *m_secondaryButton = nullptr;
    QPushButton *m_actionButton = nullptr;
    bool m_finished = false;
};

#endif // DESKTOP_DOWNLOADER_H
