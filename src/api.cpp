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
                      "Returns the exit code and combined output. No sudo on the local computer (use install_packages); on a remote target machine passwordless sudo works.",
                      QJsonObject{{"command", str("The shell command")}, {"cwd", str("Working directory (absolute); default: first permitted dir")},
                                  {"timeout", num("Timeout in seconds (default 120, max 1800)")}},
                      QJsonArray{"command"}));
        a.append(tool("build_project",
                      "Build the project: detects CMake / meson / cargo / go / make / npm / python and runs the right commands. On success it lists the executables; "
                      "on failure it returns ONLY the first errors with the offending source lines and hints (missing packages, moc problems) — far more useful than raw build output. "
                      "Use it after every meaningful change instead of run_command for compiling.",
                      QJsonObject{{"dir", str("Project folder (default: the first permitted dir)")}, {"command", str("Optional explicit build command instead of auto-detection")},
                                  {"timeout", num("Timeout in seconds (default 600, max 3600)")}},
                      QJsonArray()));
        a.append(tool("install_packages",
                      "Install system packages (apt-get on Debian/Ubuntu; on a remote target machine also dnf, zypper, pacman, apk…, using that distribution's package names). Use only for missing packages found with check_environment.",
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
    if (!m_ctx->remoteSummary().isEmpty()) msg += QStringLiteral("\n\n") + m_ctx->remoteSummary();
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
                              "3. If something is missing, call install_packages (package names of the system reported by check_environment) and check again.\n"
                              "4. Write the code (write_file / edit_file).\n"
                              "5. Build it with build_project (it returns only the first errors with the offending source lines), fix the FIRST error, build again; repeat until it builds; then run the program or tests with run_command.\n"
                              "6. If web_search / web_fetch are offered, use them to read current documentation instead of guessing APIs; memory_store / memory_search keep notes between sessions. If MCP tools (names starting with mcp__) are offered, use them when they fit.\n"
                              "7. Call task_complete with a summary of what was built, how it was verified and the exact path of the result.\n"
                              "Commands are non-interactive (stdin closed, no pagers, no sudo prompts). Keep commands short-running or set a timeout. "
                              "Never delete or overwrite things outside the permitted directories.\n\n"
                              "BUILD DISCIPLINE (this is what makes projects actually compile):\n"
                              "- Start with the smallest thing that builds and runs (CMakeLists.txt + one main file) and call build_project immediately. Then add ONE feature or file at a time and build after each. Never write ten files and build at the end.\n"
                              "- Keep the project small and the dependencies few. Prefer the standard library and the one framework asked for. For Qt: Qt6, find_package(Qt6 REQUIRED COMPONENTS Widgets), set(CMAKE_AUTOMOC ON), build the UI in C++ code — do NOT use .ui files, resource files or optional Qt modules unless essential.\n"
                              "- Fix the first error only; later ones are usually a consequence. Change as little as possible with edit_file; do not rewrite whole files to fix one error, and never delete working code.\n"
                              "- Read an error carefully: the file:line it reports and the lines shown are the truth. If you are unsure of an API, use web_search/web_fetch (if offered) or look at the installed headers with search_files instead of guessing.\n"
                              "- If the same problem survives three attempts, simplify: remove or stub the problematic feature, get a working build, and only then come back to it.\n"
                              "- Done means: build_project succeeded AND you ran the program (or its tests) at least once and saw correct output. Say honestly in task_complete what works and what does not.")
                 .arg(m_ctx->mode() == Mode::Ask ? QStringLiteral("the user confirms each action") : QStringLiteral("full auto, nothing is confirmed"));
        break;
    }
    return msg;
}

static QString agentRules()
{
    return QStringLiteral(
        "NON-NEGOTIABLE RULES FOR BUILDING SOFTWARE (they apply to every task, whatever else was said above):\n"
        "- Start with the SMALLEST program that builds and runs (a window or a hello-world, a few files). Call build_project right after the first files exist. "
        "Add one feature at a time and build after EACH one. Never write more than 3 files between two builds.\n"
        "- In CMakeLists.txt never list source files that do not exist yet: use file(GLOB SOURCES CONFIGURE_DEPENDS *.cpp *.h) (or src/*.cpp) so every file you create is picked up automatically. "
        "Every .cpp must #include the headers of every class it uses (Qt classes too: <QTcpSocket>, <QTimer>, <QFile> ...); declare each class in its own header and include that header.\n"
        "- When a build fails, fix ONLY the first error shown, with edit_file, then build again. Never rewrite a whole file to fix one error. "
        "Never rewrite the same file more than twice: if it still fails, remove or stub the feature that causes it.\n"
        "- After 3 failed builds in a row, SIMPLIFY (drop the feature, use a simpler API) instead of trying variations of the same idea.\n"
        "- Qt6 (not Qt5!) API: use errorOccurred (not error) on sockets, QRegularExpression (no QRegExp), Qt::SkipEmptyParts, QString/quint32 keys instead of QHostAddress in QMap/QSet, "
        "find_package(Qt6 COMPONENTS Widgets Network REQUIRED) and target_link_libraries(... Qt6::Widgets Qt6::Network). Declare every member in the header before defining it in the .cpp. "
        "Always build with build_project (it works for CMake, qmake .pro, meson, make, cargo, go, npm) rather than raw make/cmake commands.\n"
        "- Do not use generated GUI files (.ui, .qrc, resource compilers); build the interface in code. For Qt use Qt6 Widgets with CMAKE_AUTOMOC ON. "
        "Do not hand-edit XML; if you cannot validate something, avoid it.\n"
        "- Do not repeat a tool call that already gave the same result. If you are not making progress, change the approach or call task_complete and say what blocks you.\n"
        "- Do not 'verify' by re-reading files you just wrote. The build is the verification.\n"
        "- If a tool is missing (xmllint, etc.), do not try to install or work around it unless the task needs it.\n"
        "- Do not stop at 'it compiles': run the program briefly (with a timeout; for GUI programs use QT_QPA_PLATFORM=offscreen) before task_complete.\n"
        "- You HAVE internet access through your search/fetch tools (web_search, web_fetch and any MCP search or documentation tools). Use them EARLY: whenever you are unsure about an API, a package name or an error message, search first (the error text or the exact function name) and read the result; do not guess and do not retry blindly.\n");
}

QString AiSession::searchHint() const
{
    QStringList names;
    static const QRegularExpression re(QStringLiteral("search|fetch|wiki|ask_question|docs|query"), QRegularExpression::CaseInsensitiveOption);
    for (const QJsonValue &v : m_ctx->extraToolDefs()) {
        const QJsonObject o = v.toObject();
        const QString n = o.contains("name") ? o.value("name").toString() : o.value("function").toObject().value("name").toString();
        if (!n.isEmpty() && re.match(n).hasMatch() && !n.startsWith(QLatin1String("remote_")) && !n.startsWith(QLatin1String("memory_"))) names << n;
    }
    if (names.isEmpty()) return {};
    names = names.mid(0, 5);
    return QStringLiteral(" BEFORE trying another fix, LOOK IT UP on the internet with one of your search tools (%1): search for the exact error message or the exact Qt/library function name and read the answer or the API page. Do not guess an API.")
        .arg(names.join(QStringLiteral(", ")));
}

QString AiSession::systemText() const
{
    return m_s.systemPrompt + QStringLiteral("\n\n") + agentRules();
}

void AiSession::start()
{
    m_running = true;
    if (m_cancelled) { finishCancelled(); return; }
    m_turnCap = m_ctx->canRun() ? kMaxTurnsAgent : kMaxTurnsManual;
    m_messages = QJsonArray();
    if (m_openai) m_messages.append(QJsonObject{{"role", "system"}, {"content", systemText()}});
    m_messages.append(QJsonObject{{"role", "user"}, {"content", initialMessage()}});
    nextTurn();
}

void AiSession::repairHistory()
{
    if (m_messages.isEmpty()) return;
    if (!m_openai) {
        const QJsonObject last = m_messages.last().toObject();
        if (last.value("role").toString() != QLatin1String("assistant")) return;
        QJsonArray results;
        for (const QJsonValue &v : last.value("content").toArray()) {
            const QJsonObject b = v.toObject();
            if (b.value("type").toString() == QLatin1String("tool_use"))
                results.append(QJsonObject{{"type", "tool_result"}, {"tool_use_id", b.value("id")},
                                           {"content", "(interrupted before this tool ran)"}, {"is_error", true}});
        }
        if (!results.isEmpty()) m_messages.append(QJsonObject{{"role", "user"}, {"content", results}});
        return;
    }
    // OpenAI: every tool call of the last assistant message needs a tool message
    int ai = -1;
    for (int i = m_messages.size() - 1; i >= 0; --i)
        if (m_messages[i].toObject().value("role").toString() == QLatin1String("assistant")) { ai = i; break; }
    if (ai < 0) return;
    QStringList need;
    for (const QJsonValue &v : m_messages[ai].toObject().value("tool_calls").toArray()) need << v.toObject().value("id").toString();
    for (int i = ai + 1; i < m_messages.size(); ++i) need.removeAll(m_messages[i].toObject().value("tool_call_id").toString());
    for (const QString &id : need)
        m_messages.append(QJsonObject{{"role", "tool"}, {"tool_call_id", id}, {"content", "(interrupted before this tool ran)"}});
}

void AiSession::compactHistory()
{
    // Long conversations: shorten old tool output and old file contents so the context stays usable.
    if (QJsonDocument(m_messages).toJson(QJsonDocument::Compact).size() < 400000) return;
    auto shrink = [](const QString &s, int n) { return s.size() > n ? s.left(n) + QStringLiteral("\n… [shortened to save context]") : s; };
    QJsonArray out;
    const int keep = 8;
    for (int i = 0; i < m_messages.size(); ++i) {
        QJsonObject m = m_messages[i].toObject();
        if (i < m_messages.size() - keep) {
            if (m.value("role").toString() == QLatin1String("tool")) {
                m["content"] = shrink(m.value("content").toString(), 600);
            } else if (m.value("content").isArray()) {
                QJsonArray blocks;
                for (const QJsonValue &bv : m.value("content").toArray()) {
                    QJsonObject b = bv.toObject();
                    if (b.value("type").toString() == QLatin1String("tool_result") && b.value("content").isString())
                        b["content"] = shrink(b.value("content").toString(), 600);
                    if (b.value("type").toString() == QLatin1String("tool_use")) {
                        QJsonObject in = b.value("input").toObject();
                        for (const char *k : {"content", "old_str", "new_str"})
                            if (in.value(QLatin1String(k)).isString()) in[QLatin1String(k)] = shrink(in.value(QLatin1String(k)).toString(), 300);
                        b["input"] = in;
                    }
                    blocks.append(b);
                }
                m["content"] = blocks;
            }
            if (m.contains("tool_calls")) {
                QJsonArray calls;
                for (const QJsonValue &cv : m.value("tool_calls").toArray()) {
                    QJsonObject c = cv.toObject();
                    QJsonObject fn = c.value("function").toObject();
                    if (fn.value("arguments").toString().size() > 1500) fn["arguments"] = QStringLiteral("{\"note\":\"arguments shortened to save context\"}");
                    c["function"] = fn;
                    calls.append(c);
                }
                m["tool_calls"] = calls;
            }
        }
        out.append(m);
    }
    m_messages = out;
}

void AiSession::appendUser(const QString &text)
{
    if (!m_openai && !m_messages.isEmpty()) {
        QJsonObject last = m_messages.last().toObject();
        if (last.value("role").toString() == QLatin1String("user") && last.value("content").isArray()) {
            QJsonArray blocks = last.value("content").toArray();      // tool results first, then our text
            blocks.append(QJsonObject{{"type", "text"}, {"text", text}});
            last["content"] = blocks;
            m_messages[m_messages.size() - 1] = last;
            return;
        }
    }
    m_messages.append(QJsonObject{{"role", "user"}, {"content", text}});
}

void AiSession::sendFollowUp(const QString &text)
{
    if (m_running) return;
    m_cancelled = false; m_done = false; m_running = true; m_stopped = false;
    m_turn = 0; m_retries = 0; m_failStreak = 0; m_quiet = 0; m_nextCheckpoint = 30;
    m_sigs.clear(); m_failHashes.clear();
    m_turnCap = m_ctx->canRun() ? kMaxTurnsAgent : kMaxTurnsManual;
    m_ctx->resetAbort();
    repairHistory();
    QString msg;
    for (const QString &n : std::as_const(m_notes)) msg += QStringLiteral("[Note] ") + n + QLatin1Char('\n');
    m_notes.clear();
    msg += text;
    appendUser(msg);
    nextTurn();
}

void AiSession::resume(const QJsonArray &history, const QString &text)
{
    m_messages = history;
    if (m_openai && !m_messages.isEmpty() && m_messages[0].toObject().value("role").toString() == QLatin1String("system"))
        m_messages[0] = QJsonObject{{"role", "system"}, {"content", systemText()}};      // current system prompt
    m_running = false;
    if (m_cancelled) { m_running = true; finishCancelled(); return; }
    sendFollowUp(text);
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
    compactHistory();
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
        body["tools"]      = openAiToolDefinitions(m_ctx->mode(), m_ctx->extraToolDefs());
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
        body["system"]     = systemText();
        body["tools"]      = toolDefinitions(m_ctx->mode(), m_ctx->extraToolDefs());
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
    if (complete) {
        m_messages.append(QJsonObject{{"role", "user"}, {"content", results}});     // keeps the history valid for follow-ups
        finish(summary.isEmpty() ? TR("Done.") : summary);
        return;
    }

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

    if (m_ctx->isBuiltinTool(name))
        return m_ctx->callBuiltin(name, input);
    if (m_ctx->isMcpTool(name))
        return m_ctx->callMcp(name, input);
    if (name == QLatin1String("remote_hosts")) return m_ctx->remoteHostsInfo();
    if (name == QLatin1String("remote_run")) {
        if (!needStr("host") || !needStr("command")) return bad(QStringLiteral("missing string parameter 'host' or 'command'"));
        return m_ctx->remoteRun(input.value("host").toString(), input.value("command").toString(), input.value("cwd").toString(), input.value("timeout").toInt(300));
    }
    if (name == QLatin1String("remote_upload")) {
        if (!needStr("host")) return bad(QStringLiteral("missing string parameter 'host'"));
        return m_ctx->remoteUpload(input.value("host").toString(), input.value("local_path").toString(), input.value("remote_path").toString(), strList(input.value("exclude")));
    }
    if (name == QLatin1String("remote_download")) {
        if (!needStr("host") || !needStr("remote_path") || !needStr("local_dir")) return bad(QStringLiteral("missing parameter 'host', 'remote_path' or 'local_dir'"));
        return m_ctx->remoteDownload(input.value("host").toString(), input.value("remote_path").toString(), input.value("local_dir").toString(), strList(input.value("exclude")));
    }
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
        if (m_ctx->canRun() && m_unbuilt.size() >= 5 && !m_unbuilt.contains(input.value("path").toString()))
            return bad(QStringLiteral("REFUSED: you already wrote or changed %1 files without building. Call build_project NOW with what exists (CMakeLists.txt must list only existing files - use file(GLOB ...)), fix the first error, "
                                      "and only then add more files. (This rule exists because big un-built code almost never compiles.)").arg(m_unbuilt.size()));
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
        ToolResult r = m_ctx->runCommand(input.value("command").toString(), input.value("cwd").toString(),
                                         input.value("timeout").toInt(120));
        r.text = condenseCompilerOutput(r.text);
        return r;
    }
    if (name == QLatin1String("build_project"))
        return m_ctx->buildProject(input.value("dir").toString(), input.value("command").toString(), input.value("timeout").toInt(600));
    if (name == QLatin1String("install_packages"))
    {
        ToolResult r = m_ctx->installPackages(strList(input.value("packages")), input.value("update").toBool());
        if (r.isError && r.text.contains(QLatin1String("Unable to locate package"))) {
            QString h = QStringLiteral("\nHINT: that package name does not exist - do not guess names. Find the real one with run_command `apt-cache search --names-only <word>`.");
            if (r.text.contains(QLatin1String("qt6")) || r.text.contains(QLatin1String("qt5")))
                h += QStringLiteral(" On Debian/Ubuntu there are no libqt6widgets6-dev / libqt6-network-dev packages: ALL Qt6 core/gui/widgets/network headers are in `qt6-base-dev` "
                                    "(check first: `dpkg -s qt6-base-dev` or `pkg-config --modules-exist Qt6Widgets Qt6Network`; it is often already installed). Qt Designer is qt6-tools-dev, multimedia is qt6-multimedia-dev.");
            r.text += h;
        }
        return r;
    }
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
    if (name == QLatin1String("run_command")) {
        static const QRegularExpression buildCmd(QStringLiteral("\\b(cmake|make|gmake|ninja|cargo|qmake6?|meson|g\\+\\+|gcc|clang\\+*|go build|npm run build)\\b"));
        if (buildCmd.match(input.value("command").toString()).hasMatch()) m_unbuilt.clear();
    }
    if (name == QLatin1String("build_project")) {
        m_unbuilt.clear();
        m_buildState = res.isError ? 2 : 1;
        if (!res.isError) m_everBuilt = true;
        if (!res.isError) { m_pathWrites.clear(); m_sameBuildErr = 0; m_failedBuilds = 0; m_lastBuildErr.clear(); }
        else {
            QString first;
            for (const QString &l : res.text.split(QLatin1Char('\n')))
                if (!l.trimmed().startsWith(QLatin1String("---")) && l.contains(QLatin1String("error"), Qt::CaseInsensitive)) { first = l.trimmed().left(160); break; }
            if (!first.isEmpty() && first == m_lastBuildErr) ++m_sameBuildErr; else { m_sameBuildErr = 1; m_lastBuildErr = first; }
            ++m_failedBuilds;
            if (m_failedBuilds >= 3 && m_failedBuilds % 3 == 0 && m_ctx->isBuiltinTool(QStringLiteral("web_search")) && !first.isEmpty()) {
                // The model does not look things up by itself: do it for it and hand over the results.
                QString msg = first;
                const int e = msg.indexOf(QLatin1String("error: "));
                if (e >= 0) msg = msg.mid(e + 7);
                const QString q = QStringLiteral("Qt6 C++ ") + msg.left(120);
                const ToolResult sr = m_ctx->callBuiltin(QStringLiteral("web_search"), QJsonObject{{"query", q}});
                if (!sr.isError && !sr.text.trimmed().isEmpty()) {
                    Q_EMIT progress(QStringLiteral("🔎 ") + q);
                    m_pendingNote = QStringLiteral("Automatic note: %1 builds in a row failed. I searched the web for you (query: \"%2\"). Read these results, open the most relevant one with web_fetch if needed, "
                        "and apply what they say instead of guessing:\n%3\nIf this does not help, simplify: remove or stub the failing feature.")
                        .arg(m_failedBuilds).arg(q, sr.text.left(2500));
                }
            }
            if (m_pendingNote.isEmpty() && m_failedBuilds >= 3 && m_failedBuilds % 3 == 0) {
                const QString s = searchHint();
                m_pendingNote = QStringLiteral("Automatic note: %1 builds in a row failed.").arg(m_failedBuilds) + s
                    + QStringLiteral(" Fix only the FIRST error; most of the others are consequences (usually a missing #include or a missing declaration in a header). If it keeps failing, simplify.");
            }
            if (!first.isEmpty() && first != m_lastHinted && m_pendingNote.isEmpty()
                && (first.contains(QLatin1String("is not a member of")) || first.contains(QLatin1String("no matching function"))
                    || first.contains(QLatin1String("has no member named")) || first.contains(QLatin1String("undefined reference")))) {
                const QString s = searchHint();
                if (!s.isEmpty()) { m_lastHinted = first; m_pendingNote = QStringLiteral("Automatic note: the first error (%1) looks like a wrong or unknown API call.").arg(first) + s; }
            }
            if (m_sameBuildErr >= 2 && m_pendingNote.isEmpty())
                m_pendingNote = QStringLiteral("Automatic note: the build has failed %1 times with the same first error (%2). Your fixes are not working. "
                    "Do NOT try another variation blindly.").arg(m_sameBuildErr).arg(first) + searchHint()
                    + QStringLiteral(" If that does not help, remove or stub the code that triggers it (or replace it with something simpler), build again, and only then continue.");
        }
    }
    if ((name == QLatin1String("write_file") || name == QLatin1String("edit_file")) && !res.isError) {
        const QString p = input.value("path").toString();
        m_unbuilt.insert(p);
        const int n = ++m_pathWrites[p];
        if (n >= 3 && m_pendingNote.isEmpty())
            m_pendingNote = QStringLiteral("Automatic note: you have modified %1 %2 times without a successful build. Stop rewriting it. "
                "Run build_project, read the FIRST error only and fix exactly that line; if it is a feature that keeps breaking, delete or stub it.").arg(p).arg(n) + searchHint();
    }
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
    if (m_everBuilt && !m_checkpointExtended && m_turn >= m_nextCheckpoint) {       // working code exists and the run is progressing: do not interrupt at turn 30
        m_checkpointExtended = true; m_nextCheckpoint += 30;
    }
    const QString reason = stuckReason();
    if (m_quiet > 0) --m_quiet;
    if (!m_pendingNote.isEmpty()) {
        const QString n = m_pendingNote; m_pendingNote.clear();
        if (!reason.isEmpty()) resetStuckState();        // the automatic note is the course correction: no dialog this round
        for (auto it = m_pathWrites.begin(); it != m_pathWrites.end(); ++it) it.value() = 0;
        m_sameBuildErr = 0;
        Q_EMIT progress(QStringLiteral("↻ ") + TR("Reminder sent to the model: get a minimal version building first."));
        return n;
    }
    if (reason.isEmpty() && m_ctx->canRun() && m_turn >= 10 && m_turn % 10 == 0 && m_turn != m_lastNudge && m_buildState != 1) {
        m_lastNudge = m_turn;        // automatic, silent course correction: no dialog, the model just gets a reminder
        Q_EMIT progress(QStringLiteral("↻ ") + TR("Reminder sent to the model: get a minimal version building first."));
        return QStringLiteral("Automatic reminder (turn %1): the project %2. Stop exploring. Make the smallest working version first: write only what is needed for a minimal build, "
                              "call build_project, fix only the FIRST error, and build again. If a feature keeps failing, remove or stub it and come back later.").arg(m_turn).arg(m_buildState == 2 ? QStringLiteral("still does not build") : QStringLiteral("has not been built successfully yet")) + searchHint();
    }
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
