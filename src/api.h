// SPDX-License-Identifier: MIT
#pragma once
#include "settings.h"
#include "tools.h"
#include <QHash>
#include <QSet>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <functional>

class QNetworkReply;

namespace ai {

struct ApiResult {
    QString            summary;
    QString            error;       // empty on success
    bool               cancelled = false;
    QList<StagedWrite> writes;        // Manual mode: staged for review
    QList<AppliedWrite> applied;      // Ask / Auto: already on disk
    QString            backupDir;
    Mode               mode = Mode::Manual;
    bool               stoppedByUser = false;   // stopped at a "stuck" checkpoint: take over manually
    qint64             inputTokens = 0;
    qint64             outputTokens = 0;
};

/** What the user chose when the agent seems to be going in circles. */
enum class StuckAction { Continue, DifferentApproach, Guidance, Stop };

/** URL of the "list models" endpoint for an API kind ("anthropic" | "openai") and its URL. */
QString modelsEndpoint(const QString &kind, const QString &url);

/** Extracts model ids from a /models response (OpenAI "data[].id", llama.cpp "models[].model", Ollama). */
QStringList parseModelList(const QByteArray &json);

/**
 * One "Ask AI Agent" task: runs the tool-use conversation with the Messages API
 * asynchronously on the Qt event loop (no threads).
 */
class AiSession : public QObject {
    Q_OBJECT
public:
    AiSession(const Settings &s, ToolContext *ctx, const QString &instruction,
              const QString &hints, const QString &exclusions, QObject *parent = nullptr);
    ~AiSession() override;

    /** Called (synchronously, may show a dialog) when the agent loops or runs long. Optional. */
    std::function<StuckAction(const QString &reason, QString *guidance)> onStuck;
    bool cancelled() const { return m_cancelled; }
    bool idle() const { return !m_running; }
    ToolContext *context() const { return m_ctx; }
    QString kind() const { return m_openai ? QStringLiteral("openai") : QStringLiteral("anthropic"); }
    QJsonArray history() const { return m_messages; }

    /** Next instruction in the same conversation (the model remembers everything so far). */
    void sendFollowUp(const QString &text);
    /** Continue a saved conversation (history must be of the same API kind). */
    void resume(const QJsonArray &history, const QString &text);
    /** Text for the model that is sent in front of the next instruction (e.g. "the user undid all changes"). */
    void addNote(const QString &note) { m_notes << note; }

    void start();
    void cancel();
    bool running() const { return m_running; }

    static QJsonArray toolDefinitions(Mode mode = Mode::Manual, const QJsonArray &extra = QJsonArray());       // Anthropic format
    static QJsonArray openAiToolDefinitions(Mode mode = Mode::Manual, const QJsonArray &extra = QJsonArray()); // OpenAI function-calling format
    /** No path -> /v1/chat/completions; any path (…/v1, …/v1beta/openai) -> + /chat/completions. */
    static QString    localEndpoint(const QString &url);

Q_SIGNALS:
    void progress(const QString &line);
    void finished(const ai::ApiResult &result);

private:
    QString initialMessage() const;
    void    nextTurn();
    void    post();
    void    onReply();
    void    handleResponse(const QJsonObject &resp);
    void    handleOpenAiResponse(const QJsonObject &resp);
    void    fail(const QString &msg);
    void    finish(const QString &summary);
    void    finishCancelled();
    ToolResult execute(const QString &name, const QJsonObject &input);
    void    recordCall(const QString &name, const QJsonObject &input, const ToolResult &res);
    QString stuckReason() const;
    QString stuckCheck(bool *stop);
    QString systemText() const;
    QString searchHint() const;
    void    resetStuckState();
    void    repairHistory();
    void    compactHistory();
    void    appendUser(const QString &text);

    Settings               m_s;
    ToolContext           *m_ctx;       // owned
    QString                m_instruction, m_hints, m_exclusions;
    QNetworkAccessManager  m_nam;
    QPointer<QNetworkReply> m_reply;
    QJsonArray             m_messages;
    int                    m_turn = 0;
    int                    m_retries = 0;
    bool                   m_openai = false;
    bool                   m_running = false;
    bool                   m_cancelled = false;
    bool                   m_done = false;
    bool                   m_inTool = false;
    bool                   m_stopped = false;
    QStringList            m_sigs, m_failHashes;
    int                    m_failStreak = 0;
    int                    m_buildState = 0;          // 0 never built, 1 last build ok, 2 last build failed
    int                    m_lastNudge = 0;
    QHash<QString,int>     m_pathWrites;               // writes per path since the last successful build
    QString                m_lastBuildErr, m_lastHinted;
    int                    m_sameBuildErr = 0;
    int                    m_failedBuilds = 0;         // consecutive failed builds
    QString                m_pendingNote;
    QSet<QString>          m_unbuilt;                  // files written since the last build
    int                    m_quiet = 0;
    int                    m_nextCheckpoint = 30;
    bool                   m_everBuilt = false, m_checkpointExtended = false;
    int                    m_turnCap = 0;
    QStringList            m_notes;
    qint64                 m_in = 0, m_out = 0;
};

} // namespace ai

Q_DECLARE_METATYPE(ai::ApiResult)
