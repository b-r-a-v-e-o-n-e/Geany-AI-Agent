// SPDX-License-Identifier: MIT
#include "webtools.h"
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace ai {

namespace {
ToolResult fail(const QString &m) { ToolResult r; r.isError = true; r.text = QStringLiteral("Error: ") + m; r.note = r.text; return r; }

QJsonObject tool(const char *n, const char *d, const QJsonObject &p, const QJsonArray &req)
{
    return QJsonObject{{"name", n}, {"description", d}, {"input_schema", QJsonObject{{"type", "object"}, {"properties", p}, {"required", req}}}};
}
QJsonObject str(const char *d) { return QJsonObject{{"type", "string"}, {"description", d}}; }

QString decodeEntities(QString s)
{
    static const QRegularExpression num(QStringLiteral("&#(x?)([0-9a-fA-F]+);"));
    QString out; int last = 0;
    auto it = num.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next();
        bool ok = false;
        const uint cp = m.captured(2).toUInt(&ok, m.captured(1).isEmpty() ? 10 : 16);
        out += s.mid(last, m.capturedStart() - last);
        if (ok && cp > 0 && cp < 0x110000) out += QString::fromUcs4(reinterpret_cast<const char32_t *>(&cp), 1); else out += m.captured(0);
        last = m.capturedEnd();
    }
    out += s.mid(last);
    out.replace(QStringLiteral("&nbsp;"), QStringLiteral(" ")).replace(QStringLiteral("&lt;"), QStringLiteral("<")).replace(QStringLiteral("&gt;"), QStringLiteral(">"))
       .replace(QStringLiteral("&quot;"), QStringLiteral("\"")).replace(QStringLiteral("&apos;"), QStringLiteral("'")).replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    return out;
}
} // namespace

QJsonArray builtinToolDefs()
{
    QJsonArray a;
    a.append(tool("web_fetch", "Download a web page (http/https) and return it as plain text; JSON/text is returned as is. Use it to read documentation or an API reference. "
                  "Local and private network addresses are refused.",
                  QJsonObject{{"url", str("Full http(s) URL")}, {"max_chars", QJsonObject{{"type", "integer"}, {"description", "Maximum characters to return (default 20000, max 60000)"}}}},
                  QJsonArray{"url"}));
    a.append(tool("web_search", "Search the web (DuckDuckGo, no key) and return titles, URLs and snippets. Follow up with web_fetch to read a result.",
                  QJsonObject{{"query", str("Search query")}}, QJsonArray{"query"}));
    a.append(tool("current_time", "Current date and time (local and UTC), optionally in an IANA time zone such as Europe/Sofia.",
                  QJsonObject{{"timezone", str("Optional IANA time zone")}}, QJsonArray()));
    a.append(tool("memory_store", "Save a short note that is remembered in future sessions (user preferences, project facts). Overwrites the same key.",
                  QJsonObject{{"key", str("Short key, e.g. 'preferred-language'")}, {"value", str("The note")}}, QJsonArray{"key", "value"}));
    a.append(tool("memory_search", "Search the saved notes (substring of key or value). Empty query lists the most recent notes.",
                  QJsonObject{{"query", str("Text to look for")}}, QJsonArray()));
    a.append(tool("memory_forget", "Delete a saved note by key.", QJsonObject{{"key", str("Key of the note")}}, QJsonArray{"key"}));
    return a;
}

bool isBuiltinTool(const QString &n)
{
    return n == QLatin1String("web_fetch") || n == QLatin1String("web_search") || n == QLatin1String("current_time")
        || n == QLatin1String("memory_store") || n == QLatin1String("memory_search") || n == QLatin1String("memory_forget");
}

/* ── HTML helpers ────────────────────────────────────────────────────────── */

QString htmlToText(const QString &htmlIn)
{
    QString h = htmlIn;
    h.remove(QRegularExpression(QStringLiteral("<!--.*?-->"), QRegularExpression::DotMatchesEverythingOption));
    h.remove(QRegularExpression(QStringLiteral("<(script|style|noscript|svg|head)\\b.*?</\\1\\s*>"), QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption));
    h.replace(QRegularExpression(QStringLiteral("<(br|/p|/div|/li|/tr|/h[1-6]|/pre|/table|/section|/article)\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("\n"));
    h.replace(QRegularExpression(QStringLiteral("<li\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("\n• "));
    h.replace(QRegularExpression(QStringLiteral("<h[1-6]\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("\n\n"));
    h.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    h = decodeEntities(h);
    h.replace(QRegularExpression(QStringLiteral("[ \\t\\x{a0}]+")), QStringLiteral(" "));
    h.replace(QRegularExpression(QStringLiteral(" ?\\n ?")), QStringLiteral("\n"));
    h.replace(QRegularExpression(QStringLiteral("\\n{3,}")), QStringLiteral("\n\n"));
    return h.trimmed();
}

QList<SearchHit> parseDuckDuckGo(const QString &html)
{
    QList<SearchHit> hits;
    static const QRegularExpression link(QStringLiteral("<a\\b[^>]*class=\"[^\"]*result__a[^\"]*\"[^>]*href=\"([^\"]*)\"[^>]*>(.*?)</a>"),
                                         QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression link2(QStringLiteral("<a\\b[^>]*href=\"([^\"]*)\"[^>]*class=\"[^\"]*result__a[^\"]*\"[^>]*>(.*?)</a>"),
                                          QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression snip(QStringLiteral("class=\"[^\"]*result__snippet[^\"]*\"[^>]*>(.*?)</(a|div|td)>"),
                                         QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    QList<QRegularExpressionMatch> links;
    for (const QRegularExpression *re : {&link, &link2}) {
        auto it = re->globalMatch(html);
        while (it.hasNext()) links << it.next();
        if (!links.isEmpty()) break;
    }
    QList<QRegularExpressionMatch> snips;
    auto si = snip.globalMatch(html);
    while (si.hasNext()) snips << si.next();
    for (int i = 0; i < links.size() && hits.size() < 8; ++i) {
        QString href = decodeEntities(links[i].captured(1));
        if (href.startsWith(QLatin1String("//"))) href = QStringLiteral("https:") + href;
        const QUrl u(href);
        if (u.host().endsWith(QLatin1String("duckduckgo.com")) && u.path() == QLatin1String("/l/")) {
            const QString t = QUrlQuery(u).queryItemValue(QStringLiteral("uddg"), QUrl::FullyDecoded);
            if (!t.isEmpty()) href = t;
        }
        if (!href.startsWith(QLatin1String("http"))) continue;
        if (href.contains(QLatin1String("duckduckgo.com/y.js"))) continue;                 // ads
        SearchHit h;
        h.title = htmlToText(links[i].captured(2)).simplified();
        h.url = href;
        // the snippet that follows this link and precedes the next one
        const int from = links[i].capturedEnd(), to = (i + 1 < links.size()) ? links[i + 1].capturedStart() : html.size();
        for (const auto &s : snips) if (s.capturedStart() >= from && s.capturedStart() < to) { h.snippet = htmlToText(s.captured(1)).simplified(); break; }
        hits << h;
    }
    return hits;
}

bool webUrlAllowed(const QString &urlIn, QString *why)
{
    const QUrl u(urlIn.trimmed(), QUrl::StrictMode);
    if (!u.isValid() || (u.scheme() != QLatin1String("http") && u.scheme() != QLatin1String("https")) || u.host().isEmpty()) {
        if (why) *why = QStringLiteral("only valid http:// or https:// URLs are allowed");
        return false;
    }
    if (qEnvironmentVariableIsSet("GEANY_AI_WEB_ALLOW_LOCAL")) return true;
    const QString host = u.host().toLower();
    bool local = host == QLatin1String("localhost") || host.endsWith(QLatin1String(".localhost")) || host.endsWith(QLatin1String(".local")) || host.endsWith(QLatin1String(".internal"));
    QHostAddress a;
    if (a.setAddress(host)) {
        local = local || a.isLoopback() || a.isLinkLocal() || a.isMulticast() || a.isNull() || a == QHostAddress(QHostAddress::AnyIPv4) || a == QHostAddress(QHostAddress::AnyIPv6);
        if (a.protocol() == QAbstractSocket::IPv4Protocol) {
            const quint32 v = a.toIPv4Address();
            local = local || (v >> 24) == 10 || (v >> 20) == 0xAC1 || (v >> 16) == 0xC0A8 || (v >> 16) == 0xA9FE || (v >> 22) == 0x191 /*100.64/10*/;
        } else {
            const Q_IPV6ADDR b = a.toIPv6Address();
            local = local || (b[0] & 0xFE) == 0xFC;                                          // fc00::/7
        }
    }
    if (local && why) *why = QStringLiteral("local and private network addresses are refused (%1)").arg(host);
    return !local;
}

/* ── network ─────────────────────────────────────────────────────────────── */

static bool httpGet(const QString &url, int timeoutMs, const std::function<bool()> &aborted, QByteArray *body, QString *ctype, QString *error)
{
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) geany-ai-qt"));
    req.setRawHeader("Accept", "text/html,application/xhtml+xml,application/json,text/plain;q=0.9,*/*;q=0.5");
    req.setRawHeader("Accept-Language", "en");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setMaximumRedirectsAllowed(5);
    QNetworkReply *rep = nam.get(req);
    QEventLoop loop;
    bool timedOut = false, tooBig = false, stopped = false;
    QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(rep, &QNetworkReply::redirected, &loop, [&](const QUrl &to) {
        QString why;
        if (!webUrlAllowed(to.toString(), &why)) { *error = QStringLiteral("redirect refused: ") + why; stopped = true; rep->abort(); }
    });
    QObject::connect(rep, &QNetworkReply::readyRead, &loop, [&] { body->append(rep->readAll()); if (body->size() > 3 * 1024 * 1024) { tooBig = true; rep->abort(); } });
    QTimer timer; timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] { timedOut = true; rep->abort(); });
    timer.start(timeoutMs);
    QTimer poll; poll.setInterval(150);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (aborted && aborted()) { stopped = true; if (error->isEmpty()) *error = QStringLiteral("cancelled by the user"); rep->abort(); } });
    poll.start();
    loop.exec();
    body->append(rep->readAll());
    const int status = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    *ctype = rep->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
    const QNetworkReply::NetworkError ne = rep->error();
    const QString es = rep->errorString();
    rep->deleteLater();
    if (stopped) return false;
    if (tooBig) { body->truncate(3 * 1024 * 1024); return true; }                          // keep what we have
    if (timedOut) { *error = QStringLiteral("timed out"); return false; }
    if (ne != QNetworkReply::NoError && status == 0) { *error = es; return false; }
    if (status >= 400) { *error = QStringLiteral("HTTP %1").arg(status); return false; }
    return true;
}

/* ── notes (memory) ──────────────────────────────────────────────────────── */

QString memoryFile()
{
    const QByteArray o = qgetenv("GEANY_AI_MEMORY_FILE");
    if (!o.isEmpty()) return QString::fromUtf8(o);
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath(QStringLiteral("geany-ai/memory.json"));
}

static QJsonObject loadMemory()
{
    QFile f(memoryFile());
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

static bool saveMemory(const QJsonObject &o)
{
    QDir().mkpath(QFileInfo(memoryFile()).absolutePath());
    QFile f(memoryFile());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return true;
}

/* ── dispatcher ──────────────────────────────────────────────────────────── */

ToolResult runBuiltinTool(const QString &name, const QJsonObject &in, const std::function<bool()> &aborted)
{
    ToolResult r;
    if (name == QLatin1String("current_time")) {
        QTimeZone tz = QTimeZone::systemTimeZone();
        const QString want = in.value("timezone").toString().trimmed();
        if (!want.isEmpty()) {
            tz = QTimeZone(want.toUtf8());
            if (!tz.isValid()) return fail(QStringLiteral("unknown time zone: ") + want);
        }
        const QDateTime now = QDateTime::currentDateTimeUtc();
        r.text = QStringLiteral("%1 (%2)\nUTC: %3\nWeekday: %4")
                     .arg(now.toTimeZone(tz).toString(Qt::ISODate), QString::fromUtf8(tz.id()), now.toString(Qt::ISODate), QLocale::c().dayName(now.toTimeZone(tz).date().dayOfWeek()));
        r.note = QStringLiteral("current_time");
        return r;
    }
    if (name == QLatin1String("memory_store")) {
        const QString k = in.value("key").toString().trimmed(), v = in.value("value").toString();
        if (k.isEmpty() || k.size() > 80 || v.isEmpty()) return fail(QStringLiteral("give a key (max 80 characters) and a value"));
        QJsonObject m = loadMemory();
        if (m.size() >= 500 && !m.contains(k)) return fail(QStringLiteral("memory is full (500 notes); forget some first"));
        m[k] = QJsonObject{{"value", v.left(4000)}, {"time", QDateTime::currentDateTime().toString(Qt::ISODate)}};
        if (!saveMemory(m)) return fail(QStringLiteral("cannot write ") + memoryFile());
        r.text = QStringLiteral("saved note '%1'").arg(k);
        r.note = QStringLiteral("memory_store %1").arg(k);
        return r;
    }
    if (name == QLatin1String("memory_forget")) {
        const QString k = in.value("key").toString().trimmed();
        QJsonObject m = loadMemory();
        if (!m.contains(k)) return fail(QStringLiteral("no note with key '%1'").arg(k));
        m.remove(k); saveMemory(m);
        r.text = QStringLiteral("deleted note '%1'").arg(k);
        r.note = QStringLiteral("memory_forget %1").arg(k);
        return r;
    }
    if (name == QLatin1String("memory_search")) {
        const QString q = in.value("query").toString().trimmed();
        const QJsonObject m = loadMemory();
        QList<QPair<QString, QString>> rows;                                                // time, text
        for (auto it = m.begin(); it != m.end(); ++it) {
            const QJsonObject o = it.value().toObject();
            if (!q.isEmpty() && !it.key().contains(q, Qt::CaseInsensitive) && !o.value("value").toString().contains(q, Qt::CaseInsensitive)) continue;
            rows.append({o.value("time").toString(), QStringLiteral("%1: %2").arg(it.key(), o.value("value").toString())});
        }
        std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        QStringList out;
        for (int i = 0; i < rows.size() && i < 50; ++i) out << rows[i].second;
        r.text = out.isEmpty() ? QStringLiteral("no notes found") : out.join(QLatin1Char('\n'));
        r.note = QStringLiteral("memory_search '%1' (%2 notes)").arg(q).arg(out.size());
        return r;
    }
    if (name == QLatin1String("web_search")) {
        const QString q = in.value("query").toString().trimmed();
        if (q.isEmpty()) return fail(QStringLiteral("empty query"));
        QString base = qEnvironmentVariable("GEANY_AI_SEARCH_URL", QStringLiteral("https://html.duckduckgo.com/html/"));
        QUrl u(base); QUrlQuery uq; uq.addQueryItem(QStringLiteral("q"), q); u.setQuery(uq);
        QByteArray body; QString ct, e;
        if (!httpGet(u.toString(), 20000, aborted, &body, &ct, &e)) return fail(QStringLiteral("search failed: ") + e);
        const QList<SearchHit> hits = parseDuckDuckGo(QString::fromUtf8(body));
        if (hits.isEmpty()) return fail(QStringLiteral("no results (the search service may be rate limiting or changed its format). Try web_fetch on a specific documentation URL."));
        QStringList out;
        int n = 0;
        for (const SearchHit &h : hits) out << QStringLiteral("%1. %2\n   %3\n   %4").arg(++n).arg(h.title, h.url, h.snippet);
        r.text = out.join(QStringLiteral("\n\n"));
        r.note = QStringLiteral("web_search '%1' (%2 results)").arg(q.left(60)).arg(hits.size());
        return r;
    }
    if (name == QLatin1String("web_fetch")) {
        const QString url = in.value("url").toString().trimmed();
        QString why;
        if (!webUrlAllowed(url, &why)) return fail(why);
        int maxc = in.value("max_chars").toInt(20000);
        maxc = qBound(500, maxc, 60000);
        QByteArray body; QString ct, e;
        if (!httpGet(url, 25000, aborted, &body, &ct, &e)) return fail(QStringLiteral("could not fetch %1: %2").arg(url, e));
        const bool html = ct.contains(QLatin1String("html")) || (ct.isEmpty() && body.left(300).contains("<html"));
        const bool textual = html || ct.startsWith(QLatin1String("text/")) || ct.contains(QLatin1String("json")) || ct.contains(QLatin1String("xml")) || ct.contains(QLatin1String("javascript")) || ct.isEmpty();
        if (!textual) return fail(QStringLiteral("not a text page (content type %1)").arg(ct));
        if (body.left(4000).contains('\0')) return fail(QStringLiteral("binary content"));
        QString text = QString::fromUtf8(body);
        if (html) text = htmlToText(text);
        const int total = text.size();
        if (text.size() > maxc) text = text.left(maxc) + QStringLiteral("\n… [truncated: %1 of %2 characters shown; call web_fetch again with a larger max_chars or a more specific URL]").arg(maxc).arg(total);
        r.text = text;
        r.note = QStringLiteral("web_fetch %1 (%2 chars)").arg(url.left(90)).arg(qMin(total, maxc));
        return r;
    }
    return fail(QStringLiteral("unknown built-in tool ") + name);
}

} // namespace ai
