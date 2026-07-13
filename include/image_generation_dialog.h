#ifndef IMAGE_GENERATION_DIALOG_H
#define IMAGE_GENERATION_DIALOG_H

#include <QDialog>
#include <QImage>
#include <QJsonArray>

class ApiClient;
class QComboBox;
class QLabel;
class QPushButton;
class QTextEdit;
class QResizeEvent;

class ImageGenerationDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ImageGenerationDialog(ApiClient *apiClient, QWidget *parent = nullptr);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void reject() override;

private slots:
    void onApiKeysReceived(const QJsonArray &keys);
    void onRequestFailed(const QString &error);
    void onGroupChanged(int index);
    void onGenerateClicked();
    void onImageGenerated(const QByteArray &imageData,
                          const QString &outputFormat,
                          const QString &revisedPrompt);
    void onImageGenerationFailed(const QString &error);
    void onSaveClicked();

private:
    void setupUi();
    void populateGroups();
    void populateKeys();
    void setGenerating(bool generating);
    void updatePreview();
    QString selectedApiKey() const;

    ApiClient *m_apiClient;
    QComboBox *m_groupCombo = nullptr;
    QComboBox *m_keyCombo = nullptr;
    QComboBox *m_modelCombo = nullptr;
    QComboBox *m_sizeCombo = nullptr;
    QComboBox *m_qualityCombo = nullptr;
    QComboBox *m_formatCombo = nullptr;
    QTextEdit *m_promptEdit = nullptr;
    QLabel *m_previewLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_generateButton = nullptr;
    QPushButton *m_saveButton = nullptr;

    QJsonArray m_allKeys;
    QImage m_generatedImage;
    QByteArray m_generatedBytes;
    QString m_generatedFormat;
    bool m_generating = false;
};

#endif // IMAGE_GENERATION_DIALOG_H
