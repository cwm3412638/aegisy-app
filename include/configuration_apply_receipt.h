#ifndef CONFIGURATION_APPLY_RECEIPT_H
#define CONFIGURATION_APPLY_RECEIPT_H

#include <QString>

enum class AiTool {
    ClaudeCode,
    CodexCli,
    GeminiCli,
    OpenCode,
};

struct ConfigurationApplyReceipt {
    AiTool tool = AiTool::CodexCli;
    QString backupId;
    QString backupManifestIdentity;
    QString sourceFilesIdentity;
    QString candidateFilesIdentity;
    QString appliedFilesIdentity;
    bool gatewayMode = false;

    bool isPrepared() const
    {
        return !backupId.isEmpty() && !backupManifestIdentity.isEmpty()
            && !sourceFilesIdentity.isEmpty() && !candidateFilesIdentity.isEmpty()
            && appliedFilesIdentity.isEmpty();
    }
};

#endif // CONFIGURATION_APPLY_RECEIPT_H
