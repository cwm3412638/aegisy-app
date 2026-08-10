#pragma once

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace aegisy::test {

enum class FailureCode {
    QT_STDERR_CHANNEL_PROBE,
    AWB_ASSERTION,
    AWB_DATA_ROOT,
    AWB_AAP_HANDSHAKE,
    AWB_DURABLE_STORE,
    AWB_COMPOSER_READY,
    AWB_TIMELINE_TURN,
    AWB_MUTATION_ACK,
    AWB_SNAPSHOT_SAVE,
    MONACO_ASSERTION,
    MONACO_HOST_CONTROL,
    MONACO_SPLIT_BLANK,
    MONACO_SNAPSHOT_SAVE,
    MONACO_SPLIT_RESTORE,
    QT_D3D11_INITIALIZATION,
    WEBENGINE_GLES2_CONTEXT_CREATE,
    WEBENGINE_GLES3_CONTEXT_CREATE,
    WEBENGINE_CONTEXT_FATAL,
};

inline constexpr char kFailurePrefix[] = "AEGISY_TEST_FAILURE: ";
inline constexpr char kLocalDetailPrefix[] = "AEGISY_TEST_DETAIL: ";
inline constexpr std::size_t kMaxFailureCodeBytes = 32;
inline constexpr std::size_t kMaxMessageBytes = 768;
inline constexpr char kFailureChannelSelfTestArgument[] =
    "--failure-channel-self-test";
inline constexpr int kFailureChannelSelfTestExitCode = 86;
inline constexpr int kFailureChannelSelfTestInternalFailureExitCode = 87;

inline constexpr const char *failureCodeText(FailureCode code) noexcept
{
    switch (code) {
    case FailureCode::QT_STDERR_CHANNEL_PROBE: return "QT_STDERR_CHANNEL_PROBE";
    case FailureCode::AWB_ASSERTION: return "AWB_ASSERTION";
    case FailureCode::AWB_DATA_ROOT: return "AWB_DATA_ROOT";
    case FailureCode::AWB_AAP_HANDSHAKE: return "AWB_AAP_HANDSHAKE";
    case FailureCode::AWB_DURABLE_STORE: return "AWB_DURABLE_STORE";
    case FailureCode::AWB_COMPOSER_READY: return "AWB_COMPOSER_READY";
    case FailureCode::AWB_TIMELINE_TURN: return "AWB_TIMELINE_TURN";
    case FailureCode::AWB_MUTATION_ACK: return "AWB_MUTATION_ACK";
    case FailureCode::AWB_SNAPSHOT_SAVE: return "AWB_SNAPSHOT_SAVE";
    case FailureCode::MONACO_ASSERTION: return "MONACO_ASSERTION";
    case FailureCode::MONACO_HOST_CONTROL: return "MONACO_HOST_CONTROL";
    case FailureCode::MONACO_SPLIT_BLANK: return "MONACO_SPLIT_BLANK";
    case FailureCode::MONACO_SNAPSHOT_SAVE: return "MONACO_SNAPSHOT_SAVE";
    case FailureCode::MONACO_SPLIT_RESTORE: return "MONACO_SPLIT_RESTORE";
    case FailureCode::QT_D3D11_INITIALIZATION:
        return "QT_D3D11_INITIALIZATION";
    case FailureCode::WEBENGINE_GLES2_CONTEXT_CREATE:
        return "WEBENGINE_GLES2_CONTEXT_CREATE";
    case FailureCode::WEBENGINE_GLES3_CONTEXT_CREATE:
        return "WEBENGINE_GLES3_CONTEXT_CREATE";
    case FailureCode::WEBENGINE_CONTEXT_FATAL: return "WEBENGINE_CONTEXT_FATAL";
    }
    return nullptr;
}

inline bool localDiagnosticsEnabled() noexcept
{
    const char *value = std::getenv("AEGISY_TEST_LOCAL_DIAGNOSTICS");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

namespace failure_channel_detail {

using NativeWrite = bool (*)(void *, const char *, std::size_t,
                             std::size_t *) noexcept;
using FallbackWrite = bool (*)(void *, const char *, std::size_t) noexcept;

struct Writers {
    NativeWrite native = nullptr;
    void *nativeContext = nullptr;
    FallbackWrite fallback = nullptr;
    void *fallbackContext = nullptr;
};

inline bool writeWithFallback(const char *line, std::size_t length,
                              const Writers &writers) noexcept
{
    if (line == nullptr || length == 0) return false;

    if (writers.native != nullptr) {
        std::size_t offset = 0;
        while (offset < length) {
            std::size_t written = 0;
            const std::size_t remaining = length - offset;
            const bool succeeded = writers.native(
                writers.nativeContext, line + offset, remaining, &written);
            if (written > remaining) return false;
            offset += written;
            if (!succeeded || written == 0) {
                // Once native stderr accepted a byte, never split the line across
                // a second output channel.
                if (offset != 0) return false;
                break;
            }
        }
        if (offset == length) return true;
    }

    return writers.fallback != nullptr
        && writers.fallback(writers.fallbackContext, line, length);
}

#ifdef _WIN32
inline bool writeWindowsHandle(void *context, const char *bytes,
                               std::size_t length,
                               std::size_t *writtenBytes) noexcept
{
    if (context == nullptr || bytes == nullptr || writtenBytes == nullptr
            || length > static_cast<std::size_t>(MAXDWORD)) {
        return false;
    }
    DWORD written = 0;
    const HANDLE handle = *static_cast<HANDLE *>(context);
    const BOOL result = WriteFile(handle, bytes, static_cast<DWORD>(length),
                                  &written, nullptr);
    *writtenBytes = static_cast<std::size_t>(written);
    return result != FALSE;
}
#endif

inline bool writeCrtStderr(void *, const char *bytes,
                           std::size_t length) noexcept
{
    if (bytes == nullptr || length == 0) return false;
    const std::size_t written = std::fwrite(bytes, 1, length, stderr);
    const int flushResult = std::fflush(stderr);
    return written == length && flushResult == 0;
}

inline bool writeStderrLine(const char *line, std::size_t length) noexcept
{
    Writers writers;
#ifdef _WIN32
    HANDLE errorHandle = GetStdHandle(STD_ERROR_HANDLE);
    if (errorHandle != nullptr && errorHandle != INVALID_HANDLE_VALUE) {
        writers.native = &writeWindowsHandle;
        writers.nativeContext = &errorHandle;
    }
#endif
    writers.fallback = &writeCrtStderr;
    return writeWithFallback(line, length, writers);
}

inline std::size_t buildFailureLine(FailureCode code, char *line,
                                    std::size_t capacity) noexcept
{
    const char *codeText = failureCodeText(code);
    if (codeText == nullptr || line == nullptr) return 0;
    const std::size_t prefixLength = sizeof(kFailurePrefix) - 1;
    const std::size_t codeLength = std::strlen(codeText);
    const std::size_t length = prefixLength + codeLength + 1;
    if (codeLength == 0 || codeLength > kMaxFailureCodeBytes || length > capacity) {
        return 0;
    }
    std::memcpy(line, kFailurePrefix, prefixLength);
    std::memcpy(line + prefixLength, codeText, codeLength);
    line[length - 1] = '\n';
    return length;
}

inline std::size_t buildLocalDetailLine(const char *message, char *line,
                                        std::size_t capacity) noexcept
{
    if (message == nullptr || line == nullptr) return 0;
    const std::size_t prefixLength = sizeof(kLocalDetailPrefix) - 1;
    if (capacity < prefixLength + 1) return 0;
    std::memcpy(line, kLocalDetailPrefix, prefixLength);
    std::size_t length = prefixLength;
    for (std::size_t index = 0;
         index < kMaxMessageBytes && message[index] != '\0'; ++index) {
        if (length + 1 >= capacity) break;
        const auto byte = static_cast<unsigned char>(message[index]);
        line[length++] = byte < 0x20 || byte > 0x7e
            ? '?'
            : static_cast<char>(byte);
    }
    if (length >= capacity) return 0;
    line[length++] = '\n';
    return length;
}

struct InjectedWriterContext {
    char nativeOutput[256]{};
    std::size_t nativeLength = 0;
    char fallbackOutput[256]{};
    std::size_t fallbackLength = 0;
    std::size_t nativeCalls = 0;
    std::size_t maximumChunk = static_cast<std::size_t>(-1);
    std::size_t failOnCall = static_cast<std::size_t>(-1);
    std::size_t bytesWrittenOnFailure = 0;
    bool fallbackCalled = false;
};

inline bool injectedNativeWrite(void *opaque, const char *bytes,
                                std::size_t length,
                                std::size_t *writtenBytes) noexcept
{
    auto *context = static_cast<InjectedWriterContext *>(opaque);
    if (context == nullptr || bytes == nullptr || writtenBytes == nullptr) return false;
    const std::size_t call = context->nativeCalls++;
    *writtenBytes = 0;
    const bool failing = call == context->failOnCall;
    const std::size_t requested = failing
        ? context->bytesWrittenOnFailure : context->maximumChunk;
    const std::size_t amount = length < requested ? length : requested;
    if (amount == 0 || amount > sizeof(context->nativeOutput) - context->nativeLength) {
        return !failing && amount != 0;
    }
    std::memcpy(context->nativeOutput + context->nativeLength, bytes, amount);
    context->nativeLength += amount;
    *writtenBytes = amount;
    return !failing;
}

inline bool injectedFallbackWrite(void *opaque, const char *bytes,
                                  std::size_t length) noexcept
{
    auto *context = static_cast<InjectedWriterContext *>(opaque);
    if (context == nullptr || bytes == nullptr
            || length > sizeof(context->fallbackOutput)) {
        return false;
    }
    context->fallbackCalled = true;
    std::memcpy(context->fallbackOutput, bytes, length);
    context->fallbackLength = length;
    return true;
}

inline bool runWriterSelfTests() noexcept
{
    struct CodeExpectation {
        FailureCode code;
        const char *text;
    };
    constexpr CodeExpectation codeExpectations[] = {
        {FailureCode::QT_STDERR_CHANNEL_PROBE, "QT_STDERR_CHANNEL_PROBE"},
        {FailureCode::AWB_ASSERTION, "AWB_ASSERTION"},
        {FailureCode::AWB_DATA_ROOT, "AWB_DATA_ROOT"},
        {FailureCode::AWB_AAP_HANDSHAKE, "AWB_AAP_HANDSHAKE"},
        {FailureCode::AWB_DURABLE_STORE, "AWB_DURABLE_STORE"},
        {FailureCode::AWB_COMPOSER_READY, "AWB_COMPOSER_READY"},
        {FailureCode::AWB_TIMELINE_TURN, "AWB_TIMELINE_TURN"},
        {FailureCode::AWB_MUTATION_ACK, "AWB_MUTATION_ACK"},
        {FailureCode::AWB_SNAPSHOT_SAVE, "AWB_SNAPSHOT_SAVE"},
        {FailureCode::MONACO_ASSERTION, "MONACO_ASSERTION"},
        {FailureCode::MONACO_HOST_CONTROL, "MONACO_HOST_CONTROL"},
        {FailureCode::MONACO_SPLIT_BLANK, "MONACO_SPLIT_BLANK"},
        {FailureCode::MONACO_SNAPSHOT_SAVE, "MONACO_SNAPSHOT_SAVE"},
        {FailureCode::MONACO_SPLIT_RESTORE, "MONACO_SPLIT_RESTORE"},
        {FailureCode::QT_D3D11_INITIALIZATION,
         "QT_D3D11_INITIALIZATION"},
        {FailureCode::WEBENGINE_GLES2_CONTEXT_CREATE,
         "WEBENGINE_GLES2_CONTEXT_CREATE"},
        {FailureCode::WEBENGINE_GLES3_CONTEXT_CREATE,
         "WEBENGINE_GLES3_CONTEXT_CREATE"},
        {FailureCode::WEBENGINE_CONTEXT_FATAL, "WEBENGINE_CONTEXT_FATAL"},
    };
    for (const CodeExpectation &expectation : codeExpectations) {
        if (std::strcmp(failureCodeText(expectation.code), expectation.text) != 0) {
            return false;
        }
    }

    char line[sizeof(kFailurePrefix) - 1 + kMaxFailureCodeBytes + 1]{};
    const std::size_t length = buildFailureLine(FailureCode::AWB_ASSERTION,
                                                line, sizeof(line));
    constexpr char expectedLine[] = "AEGISY_TEST_FAILURE: AWB_ASSERTION\n";
    if (length != sizeof(expectedLine) - 1
            || std::memcmp(line, expectedLine, length) != 0) {
        return false;
    }

    InjectedWriterContext partial;
    partial.maximumChunk = 7;
    Writers partialWriters{&injectedNativeWrite, &partial,
                           &injectedFallbackWrite, &partial};
    if (!writeWithFallback(line, length, partialWriters)
            || partial.nativeCalls < 2 || partial.fallbackCalled
            || partial.nativeLength != length
            || std::memcmp(partial.nativeOutput, line, length) != 0) {
        return false;
    }

    InjectedWriterContext initialFailure;
    initialFailure.failOnCall = 0;
    Writers initialFailureWriters{&injectedNativeWrite, &initialFailure,
                                  &injectedFallbackWrite, &initialFailure};
    if (!writeWithFallback(line, length, initialFailureWriters)
            || !initialFailure.fallbackCalled || initialFailure.nativeLength != 0
            || initialFailure.fallbackLength != length
            || std::memcmp(initialFailure.fallbackOutput, line, length) != 0) {
        return false;
    }

    InjectedWriterContext partialFailure;
    partialFailure.maximumChunk = 7;
    partialFailure.failOnCall = 1;
    Writers partialFailureWriters{&injectedNativeWrite, &partialFailure,
                                  &injectedFallbackWrite, &partialFailure};
    if (writeWithFallback(line, length, partialFailureWriters)
            || partialFailure.fallbackCalled || partialFailure.nativeLength != 7) {
        return false;
    }

    InjectedWriterContext failedAfterWriting;
    failedAfterWriting.failOnCall = 0;
    failedAfterWriting.bytesWrittenOnFailure = 5;
    Writers failedAfterWritingWriters{&injectedNativeWrite, &failedAfterWriting,
                                      &injectedFallbackWrite, &failedAfterWriting};
    if (writeWithFallback(line, length, failedAfterWritingWriters)
            || failedAfterWriting.fallbackCalled
            || failedAfterWriting.nativeLength != 5) {
        return false;
    }

    char detailInput[kMaxMessageBytes + 2]{};
    std::memset(detailInput, 'a', sizeof(detailInput) - 1);
    detailInput[1] = '\n';
    detailInput[2] = static_cast<char>(0x80);
    char detailLine[sizeof(kLocalDetailPrefix) - 1 + kMaxMessageBytes + 1]{};
    const std::size_t detailLength = buildLocalDetailLine(
        detailInput, detailLine, sizeof(detailLine));
    const std::size_t detailPrefixLength = sizeof(kLocalDetailPrefix) - 1;
    if (detailLength != detailPrefixLength + kMaxMessageBytes + 1
            || detailLine[detailPrefixLength] != 'a'
            || detailLine[detailPrefixLength + 1] != '?'
            || detailLine[detailPrefixLength + 2] != '?'
            || detailLine[detailLength - 1] != '\n') {
        return false;
    }

    char smallDetail[sizeof(kLocalDetailPrefix) - 1 + 8];
    std::memset(smallDetail, '#', sizeof(smallDetail));
    const std::size_t smallCapacity = detailPrefixLength + 4;
    const std::size_t smallLength = buildLocalDetailLine(
        "abcdef", smallDetail, smallCapacity);
    if (smallLength != smallCapacity || smallDetail[smallLength - 1] != '\n'
            || smallDetail[smallCapacity] != '#') {
        return false;
    }
    return true;
}

} // namespace failure_channel_detail

inline bool isFailureChannelSelfTest(int argc, char *argv[]) noexcept
{
    return argc == 2 && argv != nullptr && argv[1] != nullptr
        && std::strcmp(argv[1], kFailureChannelSelfTestArgument) == 0;
}

inline bool reportFailure(FailureCode code) noexcept
{
    char line[sizeof(kFailurePrefix) - 1 + kMaxFailureCodeBytes + 1]{};
    const std::size_t length = failure_channel_detail::buildFailureLine(
        code, line, sizeof(line));
    return length != 0 && failure_channel_detail::writeStderrLine(line, length);
}

inline bool reportLocalDiagnostic(const char *message) noexcept
{
    if (!localDiagnosticsEnabled() || message == nullptr) return true;
    char line[sizeof(kLocalDetailPrefix) - 1 + kMaxMessageBytes + 1]{};
    const std::size_t length = failure_channel_detail::buildLocalDetailLine(
        message, line, sizeof(line));
    return length != 0 && failure_channel_detail::writeStderrLine(line, length);
}

inline int runFailureChannelSelfTest() noexcept
{
    if (!failure_channel_detail::runWriterSelfTests()) {
        reportFailure(FailureCode::QT_STDERR_CHANNEL_PROBE);
        return kFailureChannelSelfTestInternalFailureExitCode;
    }
    if (!reportFailure(FailureCode::QT_STDERR_CHANNEL_PROBE)) {
        return kFailureChannelSelfTestInternalFailureExitCode;
    }
    return kFailureChannelSelfTestExitCode;
}

} // namespace aegisy::test
