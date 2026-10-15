// SPDX-License-Identifier: MIT
#include "api.h"
#include "i18n.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

namespace ai {

static const int kMaxTurnsManual = 40;
static const int kMaxTurnsAgent  = 80;
static const int kMaxRetries = 3;

QString modelsEndpoint(const QString &kind, const QString &urlIn)
{
    QString u = urlIn.trimmed();
    if (kind == QLatin1String("openai")) {
        u = AiSession::localEndpoint(u);                       // .../chat/completions
        u.chop(int(QStringLiteral("/chat/completions").size()));
        return u + QStringLiteral("/models");
    }
    if (u.isEmpty()) u = QString::fromLatin1(kDefaultApiUrl);
    while (u.endsWith(QLatin1Char('/'))) u.chop(1);
    if (u.endsWith(QLatin1String("/messages"))) u.chop(int(QStringLiteral("/messages").size()));
    else if (!u.endsWith(QLatin1String("/v1")))  u += QStringLiteral("/v1");
    return u + QStringLiteral("/models?limit=100");
}

QStringList parseModelList(const QByteArray &json)
{
    QStringList out;
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    auto add = [&](QString id) {
        if (id.startsWith(QLatin1String("models/"))) id = id.mid(7);     // Google Gemini
        if (!id.isEmpty() && !out.contains(id)) out << id;
    };
    for (const QJsonValue &v : o.value("data").toArray()) {
        const QJsonObject m = v.toObject();
        add(m.value("id").toString());
    }
    if (out.isEmpty()) {
        for (const QJsonValue &v : o.value("models").toArray()) {       // llama.cpp / Ollama native shape
            const QJsonObject m = v.toObject();
            add(m.contains("model") ? m.value("model").toString() : m.value("name").toString());
        }
    }
    return out;
}

AiSession::AiSession(const Settings &s, ToolContext *ctx, const QString &instruction,
                     const QString &hints, const QString &exclusions, QObject *parent)
    : QObject(parent), m_s(s), m_ctx(ctx), m_instruction(instruction), m_hints(hints), m_exclusions(exclusions)
{
    m_openai = m_s.isOpenAi();
}

AiSession::~AiSession()
{
    if (m_ctx) m_ctx->abort();
    if (m_reply) { m_reply->disconnect(this); m_reply->abort(); }
    delete m_ctx;
}

QJsonArray AiSession::toolDefinitions(Mode mode, const QJsonArray &extra)
{
    auto str = [](const char *desc) { return QJsonObject{{"type", "string"}, {"description", desc}}; };
    auto num = [](const char *desc) { return QJsonObject{{"type", "integer"}, {"description", desc}}; };
    auto boolean = [](const char *desc) { return QJsonObject{{"type", "boolean"}, {"description", desc}}; };
    auto strArr = [](const char *desc) {
        return QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}}, {"description", desc}};
    };
    auto tool = [](const char *name, const char *desc, const QJsonObject &props, const QJsonArray &req) {
        return QJsonObject{{"name", name}, {"description", desc},
                           {"input_schema", QJsonObject{{"type", "object"}, {"properties", props}, {"required", req}}}};
    };
    const bool run = (mode != Mode::Manual);
    QJsonArray a;
    a.append(tool("list_dir", "List the contents of a directory.",
                  QJsonObject{{"path", str("Absolute path to the directory")}}, QJsonArray{"path"}));
    a.append(tool("read_file", "Read the contents of a UTF-8 text file (max 2 MB).",
                  QJsonObject{{"path", str("Absolute path to the file")}}, QJsonArray{"path"}));
    a.append(tool("search_files", "Search file contents with a regular expression (like grep -rn). Max 200 matches; skips .git and binary files.",
                  QJsonObject{{"path", str("Absolute directory to search")}, {"pattern", str("Regular expression")},
                              {"glob", str("Optional file name filter, e.g. *.cpp")}},
                  QJsonArray{"path", "pattern"}));
    a.append(tool("write_file",
                  run ? "Write the COMPLETE new content of a file (creates it and its directories if missing)."
                      : "Stage a write of the COMPLETE new content of a file (creates it if missing). The user reviews the diff before anything is written to disk.",
                  QJsonObject{{"path", str("Absolute path to the file")}, {"content", str("Complete new file content")}},
                  QJsonArray{"path", "content"}));
    a.append(tool("edit_file",
                  "Replace an exact piece of text in an existing file. old_str must match exactly once (or set replace_all). Prefer this over write_file for small changes.",
                  QJsonObject{{"path", str("Absolute path to the file")}, {"old_str", str("Exact text to replace")},
                              {"new_str", str("Replacement text")}, {"replace_all", boolean("Replace every occurrence")}},
                  QJsonArray{"path", "old_str", "new_str"}));
    a.append(tool("check_environment",
                  "Read-only check of the system: OS, package manager, install privileges, and whether the given commands, "
                  "Debian packages and pkg-config modules are available (with versions). With no arguments checks common build tools. Always call this before compiling.",
                  QJsonObject{{"commands", strArr("Executables, e.g. g++, cmake")},
                              {"packages", strArr("Debian packages, e.g. qt6-base-dev")},
                              {"pkgconfig", strArr("pkg-config modules, e.g. gtk+-3.0")}},
                  QJsonArray()));
    if (run) {
        a.append(tool("run_command",
                      "Run a shell command with /bin/bash -c in a permitted directory (compile, run tests, git, …). Non-interactive: stdin is closed. "
                      "Returns the exit code and combined output. No sudo (use install_packages).",
                      QJsonObject{{"command", str("The shell command")}, {"cwd", str("Working directory (absolute); default: first permitted dir")},
                                  {"timeout", num("Timeout in seconds (default 120, max 1800)")}},
                      QJsonArray{"command"}));
        a.append(tool("install_packages",
                      "Install Debian/Ubuntu packages with apt-get (asks for administrator rights through the system). Use only for missing packages found with check_environment.",
                      QJsonObject{{"packages", strArr("Package names")}, {"update", boolean("Run apt-get update first")}},
                      QJsonArray{"packages"}));
    }
    if (run) for (const QJsonValue &v : extra) a.append(v);
    a.append(tool("task_complete", "Signal that the task is complete.",
                  QJsonObject{{"summary", str("What was done, what was verified, and anything the user still has to do")}}, QJsonArray{"summary"}));
    return a;
}

QJsonArray AiSession::openAiToolDefinitions(Mode mode, const QJsonArray &extra)
{
    QJsonArray out;
    for (const QJsonValue &v : toolDefinitions(mode, extra)) {
        const QJsonObject t = v.toObject();
        out.append(QJsonObject{{"type", "function"},
                               {"function", QJsonObject{{"name", t.value("name")},
                                                        {"description", t.value("description")},
                                                        {"parameters", t.value("input_schema")}}}});
    }
    return out;
}

QString AiSession::localEndpoint(const QString &urlIn)
{
    QString u = urlIn.trimmed();
    if (u.isEmpty()) u = QStringLiteral("http://127.0.0.1:8080");
    while (u.endsWith(QLatin1Char('/'))) u.chop(1);
    if (u.endsWith(QLatin1String("/chat/completions"))) return u;
    const QUrl q(u);
    if (q.path().isEmpty() || q.path() == QLatin1String("/")) return u + QStringLiteral("/v1/chat/completions");
    return u + QStringLiteral("/chat/completions");
}

QString AiSession::initialMessage() const
{
    QString msg = QStringLiteral("Task: ") + m_instruction;
    if (!m_hints.trimmed().isEmpty())
        msg += QStringLiteral("\n\nHints: ") + m_hints.trimmed();
    if (!m_ctx->exclusions().isEmpty())
        msg += QStringLiteral("\n\nExclusions (do not read or modify):\n  ") + m_ctx->exclusions().join(QStringLiteral("\n  "));
    msg += QStringLiteral("\n\nPermitted directories:\n  ") + m_ctx->permittedDirs().join(QStringLiteral("\n  "));
    msg += QStringLiteral("\n\nSystem: ") + m_ctx->osSummary();
    switch (m_ctx->mode()) {
    case Mode::Manual:
        msg += QStringLiteral("\nMode: MANUAL. You cannot run commands or install packages. Explore with list_dir/read_file/search_files, "
                              "stage changes with write_file/edit_file (the user reviews them), use check_environment if useful, "
                              "then call task_complete. If something must be compiled or installed, tell the user the exact commands in your summary.");
        break;
    case Mode::Ask:
    case Mode::Auto:
        msg += QStringLiteral("\nMode: AGENT (%1). Work autonomously:\n"
                              "1. Explore the project (list_dir, read_file, search_files).\n"
                              "2. Call check_environment for everything the task needs (compilers, build tools, -dev packages, pkg-config modules).\n"
                              "3. If something is missing, call install_packages (Debian/Ubuntu names) and check again.\n"
                              "4. Write the code (write_file / edit_file).\n"
                              "5. Build it with run_command, read the errors, fix them and repeat until it builds; then run tests or the program if sensible.\n"
                              "6. If MCP tools (names starting with mcp__) are offered, use them when they fit the task.\n"
                              "7. Call task_complete with a summary of what was built, how it was verified and the exact path of the result.\n"
                              "Commands are non-interactive (stdin closed, no sudo, no pagers). Keep commands short-running or set a timeout. "
                              "Never delete or overwrite things outside the permitted directories.")
                 .arg(m_ctx->mode() == Mode::Ask ? QStringLiteral("the user confirms each action") : QStringLiteral("full auto, nothing is confirmed"));
        break;
    }
    return msg;
}

void AiSession::start()
{
    m_running = true;
    if (m_cancelled) { finishCancelled(); return; }
    m_turnCap = m_ctx->canRun() ? kMaxTurnsAgent : kMaxTurnsManual;
    m_messages = QJsonArray();
    if (m_openai) m_messages.append(QJsonObject{{"role", "system"}, {"content", m_s.systemPrompt}});
    m_messages.append(QJsonObject{{"role", "user"}, {"content", initialMessage()}});
    nextTurn();
}

void AiSession::cancel()
{
    if (m_cancelled) return;
    m_cancelled = true;
    if (m_ctx) m_ctx->abort();              // kills a running command
    if (!m_running) return;                 // cancelled before start(): start() reports it
    if (m_reply) m_reply->abort();          // onReply() then reports the cancellation
    else if (!m_inTool) finishCancelled();  // e.g. while waiting to retry; during a tool the loop checks m_cancelled
}

void AiSession::finishCancelled()
{
    if (m_done) return;
    m_done = true; m_running = false;
    ApiResult r;
    r.cancelled = true;
    r.inputTokens = m_in; r.outputTokens = m_out;
    r.applied = m_ctx->applied();
    r.backupDir = m_ctx->backupDir();
    r.mode = m_ctx->mode();
    Q_EMIT finished(r);
}

void AiSession::nextTurn()
{
    if (m_turn >= m_turnCap) {
        if (onStuck) {
            QString text;
            const StuckAction a = onStuck(TR("The turn limit (%1) was reached").arg(m_turnCap), &text);
            if (a == StuckAction::Stop) { m_stopped = true; finish(TR("Stopped by you at the turn limit.")); return; }
            m_turnCap += 40;
            if (a == StuckAction::Guidance && !text.trimmed().isEmpty())
                m_messages.append(QJsonObject{{"role", "user"}, {"content", QStringLiteral("Note from the user: ") + text.trimmed()}});
            else if (a == StuckAction::DifferentApproach)
                m_messages.append(QJsonObject{{"role", "user"}, {"content", QStringLiteral("Note from the user: you have used many turns. Step back and try a different, simpler approach, or call task_complete and explain what blocks you.")}});
            resetStuckState();
        } else { fail(TR("Exceeded the maximum number of turns without completing.")); return; }
    }
    ++m_turn;
    m_retries = 0;
    post();
}

void AiSession::post()
{
    QJsonObject body;
    QNetworkRequest req;
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    const QString model = m_s.effectiveModel();
    if (m_openai) {
        req.setUrl(QUrl(localEndpoint(m_s.effectiveUrl())));
        if (!m_s.apiKey().isEmpty())
            req.setRawHeader("Authorization", "Bearer " + m_s.apiKey().toUtf8());
        body["model"]      = model;
        body["max_tokens"] = m_s.maxTokens;
        body["tools"]      = openAiToolDefinitions(m_ctx->mode(), m_ctx->mcpToolDefs());
        body["messages"]   = m_messages;
        body["stream"]     = false;
        // Local models can take minutes before the first byte of a non-streamed reply.
        req.setTransferTimeout(m_s.provider == QLatin1String("local") ? 60 * 60 * 1000 : 10 * 60 * 1000);
    } else {
        req.setUrl(QUrl(m_s.effectiveUrl()));
        req.setRawHeader("x-api-key", m_s.apiKey().toUtf8());
        req.setRawHeader("anthropic-version", "2023-06-01");
        body["model"]      = model;
        body["max_tokens"] = m_s.maxTokens;
        body["system"]     = m_s.systemPrompt;
        body["tools"]      = toolDefinitions(m_ctx->mode(), m_ctx->mcpToolDefs());
        body["messages"]   = m_messages;
        req.setTransferTimeout(5 * 60 * 1000);
    }

    Q_EMIT progress(TR("Turn %1: waiting for the model…").arg(m_turn));
    m_reply = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::finished, this, &AiSession::onReply);
}

void AiSession::onReply()
{
    QNetworkReply *r = m_reply;
    m_reply = nullptr;
    if (!r) return;

    const int status        = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray data   = r->readAll();
    const QString netErr    = r->errorString();
    const bool hadNetErr    = (status == 0 && r->error() != QNetworkReply::NoError);
    r->deleteLater();
    r->disconnect(this);

    if (m_cancelled) { finishCancelled(); return; }
    if (hadNetErr) { fail(TR("Network error: %1").arg(netErr)); return; }

    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
    const QJsonObject obj = doc.object();

    if (status >= 400 || obj.value("type").toString() == QLatin1String("error")) {
        const QJsonValue ev = obj.value("error");
        QString msg = ev.isObject() ? ev.toObject().value("message").toString() : ev.toString();
        if (msg.isEmpty()) msg = QString::fromUtf8(data.left(300));
        const bool retryable = status == 429 || status == 500 || status == 502 || status == 503 || status == 529;
        if (retryable && m_retries < kMaxRetries) {
            ++m_retries;
            const int secs = 2 * m_retries;
            Q_EMIT progress(TR("Temporary API error (HTTP %1), retrying in %2 s…").arg(status).arg(secs));
            QTimer::singleShot(secs * 1000, this, [this] { if (!m_cancelled && !m_done) post(); });
            return;
        }
        fail(TR("API error (HTTP %1): %2").arg(status).arg(msg));
        return;
    }
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        fail(TR("Invalid response from the API: %1").arg(pe.error != QJsonParseError::NoError ? pe.errorString()
                                                                                              : QStringLiteral("not an object")));
        return;
    }
    if (m_openai) handleOpenAiResponse(obj);
    else          handleResponse(obj);
}

void AiSession::handleResponse(const QJsonObject &resp)
{
    const QJsonObject usage = resp.value("usage").toObject();
    m_in  += usage.value("input_tokens").toInteger();
    m_out += usage.value("output_tokens").toInteger();

    const QJsonArray content = resp.value("content").toArray();
    const QString stop = resp.value("stop_reason").toString();

    // Keep the assistant turn exactly as returned (needed for tool_use ids).
    m_messages.append(QJsonObject{{"role", "assistant"}, {"content", content}});

    QString text;
    QList<QJsonObject> uses;
    for (const QJsonValue &v : content) {
        const QJsonObject b = v.toObject();
        const QString type = b.value("type").toString();
        if (type == QLatin1String("text")) text += b.value("text").toString();
        else if (type == QLatin1String("tool_use")) uses << b;
    }
    if (!text.trimmed().isEmpty()) {
        QString line = text.trimmed().split(QLatin1Char('\n')).first();
        if (line.size() > 200) line = line.left(200) + QStringLiteral("…");
        Q_EMIT progress(QStringLiteral("💬 ") + line);
    }

    if (stop == QLatin1String("max_tokens")) { fail(TR("The response was cut off (max_tokens reached). Increase \"Max output tokens\" in the settings or ask for smaller changes.")); return; }
    if (uses.isEmpty()) {
        if (stop == QLatin1String("refusal") && text.trimmed().isEmpty()) { fail(TR("The model declined to continue.")); return; }
        finish(text.trimmed().isEmpty() ? TR("Done.") : text.trimmed());
        return;
    }

    QJsonArray results;
    bool complete = false;
    QString summary;
    for (const QJsonObject &u : uses) {
        const QString name = u.value("name").toString();
        const QString id   = u.value("id").toString();
        const QJsonObject input = u.value("input").toObject();

        QJsonObject tr{{"type", "tool_result"}, {"tool_use_id", id}};
        if (name == QLatin1String("task_complete")) {
            complete = true;
            summary = input.value("summary").toString();
            tr["content"] = QStringLiteral("ok");
        } else {
            const ToolResult res = execute(name, input);
            if (m_cancelled) { finishCancelled(); return; }
            recordCall(name, input, res);
            Q_EMIT progress(QStringLiteral("🔧 ") + (res.note.isEmpty() ? name : res.note));
            tr["content"] = res.text;
            if (res.isError) tr["is_error"] = true;
        }
        results.append(tr);
    }
    if (complete) { finish(summary.isEmpty() ? TR("Done.") : summary); return; }

    bool stopNow = false;
    const QString note = stuckCheck(&stopNow);
    if (m_cancelled) { finishCancelled(); return; }
    if (stopNow) { finish(TR("Stopped by you at a checkpoint.")); return; }
    if (!note.isEmpty()) results.append(QJsonObject{{"type", "text"}, {"text", note}});

    m_messages.append(QJsonObject{{"role", "user"}, {"content", results}});
    nextTurn();
}

void AiSession::handleOpenAiResponse(const QJsonObject &resp)
{
    const QJsonObject usage = resp.value("usage").toObject();
    m_in  += usage.value("prompt_tokens").toInteger();
    m_out += usage.value("completion_tokens").toInteger();

    const QJsonArray choices = resp.value("choices").toArray();
    if (choices.isEmpty()) { fail(TR("Invalid response from the API: %1").arg(QStringLiteral("no choices"))); return; }
    const QJsonObject ch  = choices.first().toObject();
    const QJsonObject msg = ch.value("message").toObject();
    const QString finishReason = ch.value("finish_reason").toString();
    const QString text    = msg.value("content").isString() ? msg.value("content").toString() : QString();

    struct Call { QString id, name; QJsonObject input; bool badArgs = false; };
    QList<Call> calls;
    QJsonArray cleanCalls;                       // normalised copy for the history
    const QJsonArray rawCalls = msg.value("tool_calls").toArray();
    int idx = 0;
    for (const QJsonValue &v : rawCalls) {
        const QJsonObject tc = v.toObject();
        const QJsonObject fn = tc.value("function").toObject();
        Call c;
        c.id   = tc.value("id").toString();
        if (c.id.isEmpty()) c.id = QStringLiteral("call_%1_%2").arg(m_turn).arg(idx);
        c.name = fn.value("name").toString();
        QString argStr;
        const QJsonValue av = fn.value("arguments");
        if (av.isObject()) {                      // some servers (e.g. Ollama) return an object
            c.input = av.toObject();
            argStr = QString::fromUtf8(QJsonDocument(c.input).toJson(QJsonDocument::Compact));
        } else {
            argStr = av.toString();
            QJsonParseError pe;
            const QJsonDocument d = QJsonDocument::fromJson(argStr.toUtf8(), &pe);
            if (pe.error == QJsonParseError::NoError && d.isObject()) c.input = d.object();
            else c.badArgs = true;
        }
        cleanCalls.append(QJsonObject{{"id", c.id}, {"type", "function"},
                                      {"function", QJsonObject{{"name", c.name}, {"arguments", argStr.isEmpty() ? QStringLiteral("{}") : argStr}}}});
        calls << c;
        ++idx;
    }

    QJsonObject assistant{{"role", "assistant"}, {"content", text}};
    if (!cleanCalls.isEmpty()) assistant["tool_calls"] = cleanCalls;
    m_messages.append(assistant);

    if (!text.trimmed().isEmpty()) {
        QString line = text.trimmed().split(QLatin1Char('\n')).first();
        if (line.size() > 200) line = line.left(200) + QStringLiteral("…");
        Q_EMIT progress(QStringLiteral("💬 ") + line);
    }
    if (finishReason == QLatin1String("length")) {
        fail(TR("The response was cut off (max_tokens reached). Increase \"Max output tokens\" in the settings or ask for smaller changes."));
        return;
    }
    if (calls.isEmpty()) { finish(text.trimmed().isEmpty() ? TR("Done.") : text.trimmed()); return; }

    bool complete = false;
    QString summary;
    for (const Call &c : calls) {
        QString out;
        if (c.name == QLatin1String("task_complete") && !c.badArgs) {
            complete = true;
            summary = c.input.value("summary").toString();
            out = QStringLiteral("ok");
        } else if (c.badArgs) {
            out = QStringLiteral("Error: tool arguments are not valid JSON; call the tool again with a valid JSON object.");
            Q_EMIT progress(QStringLiteral("🔧 ") + out);
        } else {
            const ToolResult res = execute(c.name, c.input);
            if (m_cancelled) { finishCancelled(); return; }
            recordCall(c.name, c.input, res);
            Q_EMIT progress(QStringLiteral("🔧 ") + (res.note.isEmpty() ? c.name : res.note));
            out = res.text;
        }
        m_messages.append(QJsonObject{{"role", "tool"}, {"tool_call_id", c.id}, {"content", out}});
    }
    if (complete) { finish(summary.isEmpty() ? TR("Done.") : summary); return; }

    bool stopNow = false;
    const QString note = stuckCheck(&stopNow);
    if (m_cancelled) { finishCancelled(); return; }
    if (stopNow) { finish(TR("Stopped by you at a checkpoint.")); return; }
    if (!note.isEmpty()) m_messages.append(QJsonObject{{"role", "user"}, {"content", note}});
    nextTurn();
}

static QStringList strList(const QJsonValue &v)
{
    QStringList out;
    if (v.isArray()) { for (const QJsonValue &x : v.toArray()) out << x.toString(); }
    else if (v.isString()) { out = v.toString().split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts); }
    return out;
}

ToolResult AiSession::execute(const QString &name, const QJsonObject &input)
{
    struct Guard { bool &f; Guard(bool &x) : f(x) { f = true; } ~Guard() { f = false; } } guard(m_inTool);
    auto bad = [](const QString &m) { ToolResult r; r.isError = true; r.text = QStringLiteral("Error: ") + m; r.note = r.text; return r; };
    auto needStr = [&](const char *k) { return input.value(QLatin1String(k)).isString(); };

    if (m_ctx->isMcpTool(name))
        return m_ctx->callMcp(name, input);
    if (name == QLatin1String("read_file"))
        return m_ctx->readFile(input.value("path").toString());
    if (name == QLatin1String("list_dir"))
        return m_ctx->listDir(input.value("path").toString());
    if (name == QLatin1String("search_files")) {
        if (!needStr("pattern")) return bad(QStringLiteral("missing string parameter 'pattern'"));
        return m_ctx->searchFiles(input.value("path").toString(), input.value("pattern").toString(), input.value("glob").toString());
    }
    if (name == QLatin1String("write_file")) {
        if (!needStr("content")) return bad(QStringLiteral("missing string parameter 'content'"));
        return m_ctx->writeFile(input.value("path").toString(), input.value("content").toString());
    }
    if (name == QLatin1String("edit_file")) {
        if (!needStr("old_str") || !needStr("new_str")) return bad(QStringLiteral("missing string parameter 'old_str' or 'new_str'"));
        return m_ctx->editFile(input.value("path").toString(), input.value("old_str").toString(),
                               input.value("new_str").toString(), input.value("replace_all").toBool());
    }
    if (name == QLatin1String("check_environment"))
        return m_ctx->checkEnvironment(strList(input.value("commands")), strList(input.value("packages")), strList(input.value("pkgconfig")));
    if (name == QLatin1String("run_command")) {
        if (!needStr("command")) return bad(QStringLiteral("missing string parameter 'command'"));
        return m_ctx->runCommand(input.value("command").toString(), input.value("cwd").toString(),
                                 input.value("timeout").toInt(120));
    }
    if (name == QLatin1String("install_packages"))
        return m_ctx->installPackages(strList(input.value("packages")), input.value("update").toBool());
    return bad(QStringLiteral("unknown tool: ") + name);
}

void AiSession::resetStuckState()
{
    m_sigs.clear(); m_failHashes.clear();
    m_failStreak = 0;
    m_quiet = 4;
    m_nextCheckpoint = m_turn + 30;
}

void AiSession::recordCall(const QString &name, const QJsonObject &input, const ToolResult &res)
{
    // A repeated call only counts as a loop if it also produced the same result:
    // running the same build command again after an edit is normal.
    const QString sig = name + QLatin1Char('|') + QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact))
                      + QLatin1Char('|') + res.text.left(300);
    m_sigs << sig;
    while (m_sigs.size() > 10) m_sigs.removeFirst();
    if (res.isError) {
        ++m_failStreak;
        m_failHashes << res.text.left(300);
        while (m_failHashes.size() > 8) m_failHashes.removeFirst();
    } else {
        m_failStreak = 0;
        m_failHashes.clear();
    }
}

QString AiSession::stuckReason() const
{
    if (m_quiet > 0) return {};
    for (const QString &s : m_sigs) {
        const int n = m_sigs.count(s);
        if (n >= 3) return TR("The agent repeated the same tool call with the same result %1 times").arg(n);
    }
    for (const QString &s : m_failHashes) {
        const int n = m_failHashes.count(s);
        if (n >= 3) return TR("The same error appeared %1 times").arg(n);
    }
    if (m_failStreak >= 6) return TR("%1 tool calls in a row failed").arg(m_failStreak);
    if (m_turn >= m_nextCheckpoint) return TR("Long run: %1 turns so far").arg(m_turn);
    return {};
}

QString AiSession::stuckCheck(bool *stop)
{
    *stop = false;
    const QString reason = stuckReason();
    if (m_quiet > 0) --m_quiet;
    if (reason.isEmpty() || !onStuck) return {};
    Q_EMIT progress(QStringLiteral("⚠ ") + reason);
    QString text;
    const StuckAction a = onStuck(reason, &text);
    resetStuckState();
    switch (a) {
    case StuckAction::Continue:
        return {};
    case StuckAction::DifferentApproach:
        return QStringLiteral("Note from the user: you appear to be stuck (%1). Do not repeat the same approach. Step back, re-read the "
                              "relevant code and the errors, and try a fundamentally different solution. If it is truly blocked, call "
                              "task_complete and explain what is missing.").arg(reason);
    case StuckAction::Guidance:
        return text.trimmed().isEmpty() ? QString() : QStringLiteral("Note from the user: ") + text.trimmed();
    case StuckAction::Stop:
        *stop = true; m_stopped = true;
        return {};
    }
    return {};
}

void AiSession::fail(const QString &msg)
{
    if (m_done) return;
    m_done = true; m_running = false;
    ApiResult r;
    r.error = msg;
    r.inputTokens = m_in; r.outputTokens = m_out;
    r.writes = m_ctx->staged();
    r.applied = m_ctx->applied();
    r.backupDir = m_ctx->backupDir();
    r.mode = m_ctx->mode();
    Q_EMIT finished(r);
}

void AiSession::finish(const QString &summary)
{
    if (m_done) return;
    m_done = true; m_running = false;
    ApiResult r;
    r.summary = summary;
    r.stoppedByUser = m_stopped;
    r.inputTokens = m_in; r.outputTokens = m_out;
    r.writes = m_ctx->staged();
    r.applied = m_ctx->applied();
    r.backupDir = m_ctx->backupDir();
    r.mode = m_ctx->mode();
    Q_EMIT finished(r);
}

} // namespace ai
