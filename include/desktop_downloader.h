#ifndef DESKTOP_DOWNLOADER_H
#define DESKTOP_DOWNLOADER_H

#include <QDialog>
#include <QByteArray>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QProgressBar;
class QLabel;
class QPushButton;
class QFile;

// 通过 aegisy.cc 服务器代理下载桌面客户端安装包（国内可直连）。
// 服务端约定：
//   GET /downloads/{product}/{platform}
//     product  = claude | chatgpt
//     platform = mac-arm64 | mac-x64 | win-x64
//     鉴权     = Authorization: Bearer <登录 token>
//   服务器代拉官方安装包并流式回传（不得 302 跳回官方 CDN），
//   建议返回 Content-Length 与 Content-Disposition:filename。
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

private slots:
    void startDownload();
    void onProgress(qint64 received, qint64 total);
    void onFinished();
    void openDownloadedFile();
    void openOfficialDownloadPage();

private:
    static QString productSlug(Product product);
    static QString platformSlug();     // mac-arm64 / mac-x64 / win-x64 / 空
    static QString defaultFileName(Product product);
    QString officialDownloadUrl() const;
    bool validateInstallerPayload(const QByteArray &payload, QString *error) const;
    void fail(const QString &message);
    void cleanupReply();

    Product m_product;
    QString m_appName;
    QString m_authToken;
    QString m_baseUrl;

    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_reply = nullptr;
    QFile *m_file = nullptr;
    QString m_savePath;

    QLabel *m_statusLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QPushButton *m_actionButton = nullptr;   // 取消 / 关闭
    bool m_finished = false;
};

#endif // DESKTOP_DOWNLOADER_H
