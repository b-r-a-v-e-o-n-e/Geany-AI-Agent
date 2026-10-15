// SPDX-License-Identifier: MIT
#include "mcp.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <csignal>
#include <unistd.h>

namespace ai {

static const char *kProtocol = "2025-03-26";

static QString sanitize(const QString &s)
{
    QString o = s;
    o.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")), QStringLiteral("_"));
    return o;
}

QString McpManager::exposedName(const QString &server, const QString &tool)
{
    QString n = QStringLiteral("mcp__") + sanitize(server) + QStringLiteral("__") + sanitize(tool);
    if (n.size() > 64) n = n.left(57) + QLatin1Char('_') + QString::number(qHash(n) % 0xFFFFFF, 16).rightJustified(6, QLatin1Char('0'));
    return n;
}

McpClient::McpClient(const McpServerCfg &cfg) : m_cfg(cfg) {}
McpClient::~McpClient() { stop(); }

void McpClient::stop()
{
    if (m_proc) {
        if (m_proc->state() != QProcess::NotRunning) {
            m_proc->closeWriteChannel();
            const qint64 pid = m_proc->processId();
            if (pid > 0) ::kill(-static_cast<pid_t>(pid), SIGTERM);
            if (!m_proc->waitForFinished(700) && pid > 0) {
                ::kill(-static_cast<pid_t>(pid), SIGKILL);
                m_proc->waitForFinished(500);
            }
        }
        m_proc.reset();
    }
    m_nam.reset();
    m_started = false;
}

void McpClient::handleMessage(const QJsonObject &msg)
{
    const bool hasId = msg.contains("id");
    if (hasId && (msg.contains("result") || msg.contains("error"))) {
        m_responses[msg.value("id").toInt(-1)] = msg;
        return;
    }
    if (hasId && msg.contains("method") && m_proc) {            // request from the server: we support nothing but ping
        QJsonObject r{{"jsonrpc", "2.0"}, {"id", msg.value("id")}};
        if (msg.value("method").toString() == QLatin1String("ping")) r["result"] = QJsonObject();
        else r["error"] = QJsonObject{{"code", -32601}, {"message", "Method not found"}};
        m_proc->write(QJsonDocument(r).toJson(QJsonDocument::Compact) + "\n");
    }
    // notifications are ignored
}

void McpClient::pump(const QByteArray &data)
{
    m_buf += data;
    int nl;
    while ((nl = m_buf.indexOf('\n')) >= 0) {
        const QByteArray line = m_buf.left(nl).trimmed();
        m_buf.remove(0, nl + 1);
        if (line.isEmpty()) continue;
        const QJsonDocument d = QJsonDocument::fromJson(line);
        if (d.isObject()) handleMessage(d.object());
    }
}

bool McpClient::sendHttp(const QJsonObject &msg, int id, QString *error, int timeoutMs)
{
    QNetworkRequest req{QUrl(m_cfg.target.trimmed())};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Accept", "application/json, text/event-stream");
    req.setRawHeader("MCP-Protocol-Version", kProtocol);
    if (!m_session.isEmpty()) req.setRawHeader("Mcp-Session-Id", m_session.toUtf8());
    const QString h = m_cfg.header.trimmed();
    const int colon = h.indexOf(QLatin1Char(':'));
    if (colon > 0) req.setRawHeader(h.left(colon).trimmed().toUtf8(), h.mid(colon + 1).trimmed().toUtf8());
    req.setTransferTimeout(timeoutMs);

    QNetworkReply *r = m_nam->post(req, QJsonDocument(msg).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (m_abort) { r->abort(); } });
    poll.start(100);
    QObject::connect(r, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    poll.stop();

    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = r->readAll();
    const QString ctype = r->header(QNetworkRequest::ContentTypeHeader).toString();
    if (r->hasRawHeader("Mcp-Session-Id")) m_session = QString::fromUtf8(r->rawHeader("Mcp-Session-Id"));
    const QString netErr = r->errorString();
    const bool failed = (status == 0 && r->error() != QNetworkReply::NoError);
    r->deleteLater();
    if (m_abort) { *error = QStringLiteral("cancelled"); return false; }
    if (failed) { *error = netErr; return false; }
    if (status == 202 || (status >= 200 && status < 300 && body.trimmed().isEmpty())) return true;
    if (status >= 400) { *error = QStringLiteral("HTTP %1 %2").arg(status).arg(QString::fromUtf8(body.left(200))); return false; }

    if (ctype.contains(QLatin1String("text/event-stream"))) {
        const QList<QByteArray> lines = body.split('\n');
        QByteArray data;
        auto flush = [&] {
            if (data.isEmpty()) return;
            const QJsonDocument d = QJsonDocument::fromJson(data);
            if (d.isObject()) handleMessage(d.object());
            else if (d.isArray()) for (const QJsonValue &v : d.array()) handleMessage(v.toObject());
            data.clear();
        };
        for (QByteArray l : lines) {
            if (l.endsWith('\r')) l.chop(1);
            if (l.isEmpty()) { flush(); continue; }
            if (l.startsWith("data:")) { if (!data.isEmpty()) data += '\n'; data += l.mid(5).trimmed(); }
        }
        flush();
    } else {
        const QJsonDocument d = QJsonDocument::fromJson(body);
        if (d.isObject()) handleMessage(d.object());
        else if (d.isArray()) for (const QJsonValue &v : d.array()) handleMessage(v.toObject());
    }
    if (id >= 0 && !m_responses.contains(id)) { *error = QStringLiteral("no response in the HTTP reply"); return false; }
    return true;
}

bool McpClient::request(const QString &method, const QJsonObject &params, QJsonObject *result, QString *error, int timeoutMs)
{
    const int id = m_nextId++;
    QJsonObject msg{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.isEmpty()) msg["params"] = params;
    m_responses.remove(id);

    if (m_cfg.isHttp()) {
        if (!sendHttp(msg, id, error, timeoutMs)) return false;
    } else {
        if (!m_proc || m_proc->state() != QProcess::Running) {
            *error = QStringLiteral("server is not running") + (m_stderr.isEmpty() ? QString() : QStringLiteral(": ") + m_stderr.trimmed().right(300));
            return false;
        }
        m_proc->write(QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n");
        QEventLoop loop;
        QTimer timeout, poll;
        timeout.setSingleShot(true);
        bool timedOut = false;
        QObject::connect(&timeout, &QTimer::timeout, &loop, [&] { timedOut = true; loop.quit(); });
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (m_abort || m_responses.contains(id)) loop.quit(); });
        QObject::connect(m_proc.get(), &QProcess::readyReadStandardOutput, &loop, [&] {
            pump(m_proc->readAllStandardOutput());
            if (m_responses.contains(id)) loop.quit();
        });
        QObject::connect(m_proc.get(), &QProcess::finished, &loop, [&] { loop.quit(); });
        timeout.start(timeoutMs);
        poll.start(100);
        if (!m_responses.contains(id) && !m_abort) loop.exec();
        if (!m_responses.contains(id)) {
            pump(m_proc->readAllStandardOutput());
        }
        if (!m_responses.contains(id)) {
            if (m_abort) *error = QStringLiteral("cancelled");
            else if (timedOut) *error = QStringLiteral("timed out after %1 s").arg(timeoutMs / 1000);
            else *error = QStringLiteral("server exited") + (m_stderr.isEmpty() ? QString() : QStringLiteral(": ") + m_stderr.trimmed().right(300));
            return false;
        }
    }
    const QJsonObject resp = m_responses.take(id);
    if (resp.contains("error")) {
        const QJsonObject e = resp.value("error").toObject();
        *error = e.value("message").toString(QStringLiteral("JSON-RPC error"));
        return false;
    }
    *result = resp.value("result").toObject();
    return true;
}

void McpClient::notify(const QString &method)
{
    const QJsonObject msg{{"jsonrpc", "2.0"}, {"method", method}};
    if (m_cfg.isHttp()) {
        QString e;
        sendHttp(msg, -1, &e, 10000);
    } else if (m_proc) {
        m_proc->write(QJsonDocument(msg).toJson(QJsonDocument::Compact) + "\n");
    }
}

bool McpClient::start(QString *error)
{
    stop();
    m_abort = false;
    m_buf.clear(); m_responses.clear(); m_tools.clear(); m_session.clear(); m_stderr.clear();

    if (m_cfg.isHttp()) {
        m_nam = std::make_unique<QNetworkAccessManager>();
    } else {
        QStringList parts = QProcess::splitCommand(m_cfg.target.trimmed());
        if (parts.isEmpty()) { *error = QStringLiteral("empty command"); return false; }
        const QString prog = parts.takeFirst();
        m_proc = std::make_unique<QProcess>();
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const QString &kv : m_cfg.env.split(QRegularExpression(QStringLiteral("[;\\n]")), Qt::SkipEmptyParts)) {
            const int eq = kv.indexOf(QLatin1Char('='));
            if (eq > 0) env.insert(kv.left(eq).trimmed(), kv.mid(eq + 1).trimmed());
        }
        m_proc->setProcessEnvironment(env);
        m_proc->setProcessChannelMode(QProcess::SeparateChannels);
        m_proc->setChildProcessModifier([] { ::setsid(); });
        QObject::connect(m_proc.get(), &QProcess::readyReadStandardError, [this] {
            m_stderr += QString::fromUtf8(m_proc->readAllStandardError());
            if (m_stderr.size() > 4000) m_stderr = m_stderr.right(2000);
        });
        m_proc->start(prog, parts);
        if (!m_proc->waitForStarted(5000)) { *error = QStringLiteral("cannot start %1: %2").arg(prog, m_proc->errorString()); m_proc.reset(); return false; }
    }

    QJsonObject res;
    const QJsonObject init{{"protocolVersion", kProtocol}, {"capabilities", QJsonObject()},
                           {"clientInfo", QJsonObject{{"name", "geany-ai-qt"}, {"version", GEANY_AI_VERSION}}}};
    if (!request(QStringLiteral("initialize"), init, &res, error, m_cfg.isHttp() ? 20000 : 60000)) { stop(); return false; }
    notify(QStringLiteral("notifications/initialized"));

    QString cursor;
    for (int page = 0; page < 20; ++page) {
        QJsonObject params;
        if (!cursor.isEmpty()) params["cursor"] = cursor;
        if (!request(QStringLiteral("tools/list"), params, &res, error, 20000)) { stop(); return false; }
        for (const QJsonValue &v : res.value("tools").toArray()) {
            const QJsonObject o = v.toObject();
            McpTool t;
            t.server = m_cfg.name;
            t.name = o.value("name").toString();
            if (t.name.isEmpty()) continue;
            t.description = o.value("description").toString();
            t.schema = o.value("inputSchema").toObject();
            if (t.schema.value("type").toString() != QLatin1String("object")) t.schema["type"] = "object";
            if (!t.schema.contains("properties")) t.schema["properties"] = QJsonObject();
            t.exposed = McpManager::exposedName(m_cfg.name, t.name);
            m_tools << t;
        }
        cursor = res.value("nextCursor").toString();
        if (cursor.isEmpty()) break;
    }
    m_started = true;
    return true;
}

bool McpClient::call(const QString &tool, const QJsonObject &args, int timeoutSec, QString *text, bool *isError, QString *error)
{
    if (!m_started) { *error = QStringLiteral("server is not connected"); return false; }
    QJsonObject res;
    if (!request(QStringLiteral("tools/call"), QJsonObject{{"name", tool}, {"arguments", args}}, &res, error, timeoutSec * 1000)) return false;
    QStringList parts;
    for (const QJsonValue &v : res.value("content").toArray()) {
        const QJsonObject c = v.toObject();
        const QString type = c.value("type").toString();
        if (type == QLatin1String("text")) parts << c.value("text").toString();
        else if (type == QLatin1String("resource")) {
            const QJsonObject r = c.value("resource").toObject();
            parts << (r.contains("text") ? r.value("text").toString() : QStringLiteral("[resource %1]").arg(r.value("uri").toString()));
        } else parts << QStringLiteral("[%1 content omitted]").arg(type);
    }
    if (parts.isEmpty() && res.contains("structuredContent"))
        parts << QString::fromUtf8(QJsonDocument(res.value("structuredContent").toObject()).toJson(QJsonDocument::Compact));
    *text = parts.join(QLatin1Char('\n'));
    *isError = res.value("isError").toBool();
    return true;
}

/* ── manager ─────────────────────────────────────────────────────────────── */

McpManager::~McpManager() { m_byName.clear(); m_clients.clear(); }

void McpManager::startAll(const QList<McpServerCfg> &servers, const std::function<void(const QString &)> &log)
{
    for (const McpServerCfg &cfg : servers) {
        if (!cfg.enabled || cfg.name.trimmed().isEmpty()) continue;
        if (log) log(QStringLiteral("MCP: starting \"%1\"%2").arg(cfg.name, cfg.isHttp() ? QString() : QStringLiteral(" (the first start may download packages)")));
        auto c = std::make_shared<McpClient>(cfg);
        QString err;
        if (!c->start(&err)) { if (log) log(QStringLiteral("MCP: \"%1\" failed: %2").arg(cfg.name, err)); continue; }
        int added = 0;
        for (McpTool t : c->tools()) {
            QString base = t.exposed;
            for (int i = 2; m_byName.contains(t.exposed); ++i) t.exposed = base.left(60) + QLatin1Char('_') + QString::number(i);
            m_byName.insert(t.exposed, {c.get(), t});
            ++added;
        }
        m_clients << c;
        if (log) log(QStringLiteral("MCP: \"%1\" ready, %2 tool(s)").arg(cfg.name).arg(added));
    }
}

QJsonArray McpManager::toolDefs() const
{
    QJsonArray a;
    for (auto it = m_byName.constBegin(); it != m_byName.constEnd(); ++it) {
        const McpTool &t = it.value().second;
        QString d = QStringLiteral("[MCP server %1] %2").arg(t.server, t.description);
        if (d.size() > 1000) d = d.left(1000) + QStringLiteral("…");
        a.append(QJsonObject{{"name", t.exposed}, {"description", d}, {"input_schema", t.schema}});
    }
    return a;
}

bool McpManager::call(const QString &exposed, const QJsonObject &args, int timeoutSec,
                      QString *text, bool *isError, QString *error, QString *label)
{
    auto it = m_byName.constFind(exposed);
    if (it == m_byName.constEnd()) { *error = QStringLiteral("unknown MCP tool"); return false; }
    if (label) *label = QStringLiteral("%1 / %2").arg(it.value().second.server, it.value().second.name);
    return it.value().first->call(it.value().second.name, args, timeoutSec, text, isError, error);
}

void McpManager::abort() { for (auto &c : m_clients) c->abort(); }

} // namespace ai
