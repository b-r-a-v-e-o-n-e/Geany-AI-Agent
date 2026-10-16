// SPDX-License-Identifier: MIT
#include "tools.h"
#include "webtools.h"
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QRandomGenerator>
#include <QStringDecoder>
#include <QSysInfo>
#include <QTimer>
#include <algorithm>
#include <csignal>
#include <unistd.h>

namespace ai {

/* ── path handling ───────────────────────────────────────────────────────── */

bool ToolContext::isUnder(const QString &path, const QString &dir)
{
    if (dir == QLatin1String("/")) return true;
    return path == dir || path.startsWith(dir + QLatin1Char('/'));
}

// Canonical path of a possibly non-existing file: resolve the longest existing
// ancestor (follows symlinks) and re-append the missing tail.
QString ToolContext::canonicalize(const QString &path)
{
    QString abs = QDir::cleanPath(path);
    QStringList tail;
    QString existing = abs;
    while (!QFileInfo::exists(existing) && existing != QLatin1String("/") && !existing.isEmpty()) {
        QFileInfo fi(existing);
        tail.prepend(fi.fileName());
        existing = fi.path();
    }
    QString canon = QFileInfo(existing).canonicalFilePath();
    if (canon.isEmpty()) canon = existing;
    if (!tail.isEmpty())
        canon = (canon == QLatin1String("/") ? QString() : canon) + QLatin1Char('/') + tail.join(QLatin1Char('/'));
    return QDir::cleanPath(canon);
}

ToolContext::ToolContext(const QStringList &dirs, const QString &exclusions, Mode mode) : m_mode(mode), m_exclRaw(exclusions)
{
    for (const QString &d : dirs) {
        const QString c = canonicalize(d);
        if (!c.isEmpty() && !m_dirs.contains(c)) m_dirs << c;
    }
    const QStringList parts = exclusions.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (QString e : parts) {
        e = e.trimmed();
        if (e.isEmpty()) continue;
        if (QDir::isAbsolutePath(e)) {
            m_excl << canonicalize(e);
        } else {
            for (const QString &d : m_dirs)
                m_excl << canonicalize(d + QLatin1Char('/') + e);
        }
    }
    m_excl.removeDuplicates();
}

ToolContext::~ToolContext()
{
    for (const QString &n : std::as_const(m_usedHosts))
        for (const RemoteHost &h : std::as_const(m_hosts))
            if (h.name == n) sshCloseConnection(h);
}

bool ToolContext::resolve(const QString &path, QString *resolved, QString *why) const
{
    if (path.isEmpty() || !QDir::isAbsolutePath(path)) {
        if (why) *why = QStringLiteral("path must be an absolute path: ") + path;
        return false;
    }
    const QString c = m_remote ? QDir::cleanPath(path) : canonicalize(path);      // remote: lexical only
    for (const QString &e : m_excl) {
        if (isUnder(c, e)) {
            if (why) *why = QStringLiteral("path is excluded by the user: ") + c;
            return false;
        }
    }
    bool ok = false;
    for (const QString &d : m_dirs)
        if (isUnder(c, d)) { ok = true; break; }
    if (!ok) {
        if (why) *why = QStringLiteral("path not permitted (outside the permitted directories): ") + c;
        return false;
    }
    if (resolved) *resolved = c;
    return true;
}

static ToolResult err(const QString &msg)
{
    ToolResult r;
    r.text = QStringLiteral("Error: ") + msg;
    r.isError = true;
    r.note = r.text;
    return r;
}

QString ToolContext::osSummary() const
{
    if (m_remote) return QStringLiteral("REMOTE machine '%1': %2").arg(m_rhost.name, m_ros);
    QString os = QSysInfo::prettyProductName();
    return QStringLiteral("%1, %2").arg(os, QSysInfo::currentCpuArchitecture());
}

/* ── reading ─────────────────────────────────────────────────────────────── */

bool ToolContext::readText(const QString &p, QString *text, QString *error, bool preferStaged)
{
    QByteArray data;
    bool fromStaged = false;
    if (preferStaged) {
        for (const StagedWrite &s : std::as_const(m_staged))
            if (s.path == p) { data = s.content; fromStaged = true; break; }
    }
    if (!fromStaged && m_remote) {
        int st = 0;
        if (!remoteRead(p, kMaxReadBytes, &data, &st, error)) return false;
        if (st == 0) { *error = QStringLiteral("file does not exist: ") + p; return false; }
        if (st == 2) { *error = QStringLiteral("not a regular file: ") + p; return false; }
        if (st == 3) { *error = QStringLiteral("file too large (limit %1 bytes): %2").arg(kMaxReadBytes).arg(p); return false; }
    } else if (!fromStaged) {
        QFileInfo fi(p);
        if (!fi.exists()) { *error = QStringLiteral("file does not exist: ") + p; return false; }
        if (!fi.isFile()) { *error = QStringLiteral("not a regular file: ") + p; return false; }
        if (fi.size() > kMaxReadBytes) {
            *error = QStringLiteral("file too large (%1 bytes, limit %2): %3").arg(fi.size()).arg(kMaxReadBytes).arg(p);
            return false;
        }
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) { *error = QStringLiteral("cannot open: ") + f.errorString(); return false; }
        data = f.readAll();
    }
    if (data.left(8000).contains('\0')) { *error = QStringLiteral("binary file, refusing to read: ") + p; return false; }
    // Stateless: a truncated multibyte sequence at the end of the file is an error, too.
    QStringDecoder dec(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    *text = dec(data);
    if (dec.hasError()) {
        *error = QStringLiteral("file is not valid UTF-8, refusing to read (would be corrupted on write): ") + p;
        return false;
    }
    return true;
}

ToolResult ToolContext::readFile(const QString &path)
{
    QString p, why, text, e;
    if (!resolve(path, &p, &why)) return err(why);
    if (!readText(p, &text, &e)) return err(e);
    ToolResult r;
    r.text = text;
    r.note = QStringLiteral("read_file %1 (%2 bytes)").arg(p).arg(text.toUtf8().size());
    return r;
}

/* ── writing ─────────────────────────────────────────────────────────────── */

bool ToolContext::ask(const ConfirmRequest &req)
{
    if (!needConfirm()) return true;
    if (!confirm) return false;
    const Decision d = confirm(req);
    if (d == Decision::AllowAll) m_allowAll = true;
    return d != Decision::Deny;
}

void ToolContext::backup(const QString &path, const QByteArray &original)
{
    if (m_backupDir.isEmpty()) {
        m_backupDir = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
                          .filePath(QStringLiteral("geany-ai/backups/") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
        QDir().mkpath(m_backupDir);
    }
    QString rel = path;
    rel.replace(QLatin1Char('/'), QLatin1Char('%'));
    QFile bf(QDir(m_backupDir).filePath(rel));
    if (bf.open(QIODevice::WriteOnly | QIODevice::Truncate)) bf.write(original);
}

ToolResult ToolContext::commit(const QString &p, QByteArray bytes)
{
    if (!m_remote && QFileInfo(p).isDir()) return err(QStringLiteral("path is a directory: ") + p);

    // Original: previously staged write wins in Manual mode, otherwise what is on disk now.
    QByteArray original;
    bool existed = false;
    StagedWrite *sw = nullptr;
    if (m_mode == Mode::Manual)
        for (StagedWrite &s : m_staged) if (s.path == p) { sw = &s; break; }

    if (m_remote) {
        int st = 0; QString e;
        if (!remoteRead(p, kMaxReadBytes * 8, &original, &st, &e)) return err(e);
        if (st == 2) return err(QStringLiteral("path is a directory: ") + p);
        if (st == 3) return err(QStringLiteral("existing file is too large to replace: ") + p);
        existed = (st == 1);
    } else if (sw) {
        original = sw->original; existed = sw->existed;
    } else if (QFileInfo::exists(p)) {
        if (QFileInfo(p).size() > kMaxReadBytes * 8)
            return err(QStringLiteral("existing file is too large to replace: ") + p);
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) return err(QStringLiteral("cannot open existing file: ") + f.errorString());
        original = f.readAll();
        existed = true;
    }
    // Preserve CRLF line endings of the original if the model emitted LF only.
    if (existed && original.contains("\r\n") && !bytes.contains("\r\n"))
        bytes.replace("\n", "\r\n");

    ToolResult r;
    if (m_mode == Mode::Manual) {
        if (!sw) {
            StagedWrite n;
            n.path = p; n.original = original; n.existed = existed;
            m_staged.append(n);
            sw = &m_staged.last();
        }
        sw->content = bytes;
        r.text = QStringLiteral("staged; the user will review the change before it is applied");
        r.note = QStringLiteral("write_file %1 (%2 bytes, staged)").arg(p).arg(bytes.size());
        return r;
    }

    // Ask / Auto: write to disk now (needed so that builds see the files).
    ConfirmRequest req;
    req.kind = QStringLiteral("write");
    req.title = existed ? QStringLiteral("Modify this file?") : QStringLiteral("Create this file?");
    req.path = p; req.before = original; req.after = bytes; req.existed = existed;
    req.editorDirty = !m_remote && isEditorDirty && isEditorDirty(p);
    req.target = targetName();
    if (!ask(req)) return err(QStringLiteral("the user denied writing ") + p);

    if (existed) {
        bool already = false;
        for (const AppliedWrite &a : std::as_const(m_applied)) if (a.path == p) { already = true; break; }
        if (!already) backup(p, original);
    }
    if (m_remote) {
        QString e;
        if (!remoteWrite(p, bytes, &e)) return err(e);
    } else {
        QDir().mkpath(QFileInfo(p).absolutePath());
        QFile f(p);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(bytes) != bytes.size())
            return err(QStringLiteral("cannot write %1: %2").arg(p, f.errorString()));
        f.close();
    }

    bool found = false;
    for (AppliedWrite &a : m_applied) if (a.path == p) { a.content = bytes; found = true; break; }
    if (!found) m_applied.append({p, original, existed, bytes});
    if (onFileWritten) onFileWritten(AppliedWrite{p, original, existed, bytes});

    r.text = existed ? QStringLiteral("file updated on disk") : QStringLiteral("file created on disk");
    r.note = QStringLiteral("write_file %1 (%2 bytes, %3)").arg(p).arg(bytes.size()).arg(existed ? "updated" : "created");
    return r;
}

/** Models sometimes "abbreviate" a file. A file with such a marker would silently destroy code, so it is refused. */
static QString placeholderLine(const QString &content)
{
    static const QRegularExpression re(
        QStringLiteral("^\\s*(?://|#|/\\*|--|<!--)?\\s*(?:\\.{3}|…)\\s*(?:\\*/|-->)?\\s*$"
                       "|(?://|#|/\\*|--|<!--).*\\b(?:rest of (?:the )?(?:code|file|implementation|function|class)|remains? (?:the )?same|(?:code|file|implementation|function|class|body)s? (?:\\w+ )?unchanged|existing code|same as before|omitted for brevity|code here)\\b"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    const auto m = re.match(content);
    return m.hasMatch() ? m.captured(0).trimmed().left(120) : QString();
}

ToolResult ToolContext::writeFile(const QString &path, const QString &content)
{
    QString p, why;
    if (!resolve(path, &p, &why)) return err(why);
    const QString ph = placeholderLine(content);
    if (!ph.isEmpty())
        return err(QStringLiteral("the content contains a placeholder (\"%1\"): write_file replaces the WHOLE file, so every line must be real code. "
                                  "Write the complete file, or use edit_file to change only a part.").arg(ph));
    return commit(p, content.toUtf8());
}

ToolResult ToolContext::editFile(const QString &path, const QString &oldStr, const QString &newStr, bool replaceAll)
{
    QString p, why, text, e;
    if (!resolve(path, &p, &why)) return err(why);
    if (oldStr.isEmpty()) return err(QStringLiteral("old_str must not be empty (use write_file to create a file)"));
    if (!readText(p, &text, &e)) return err(e);
    const bool crlf = text.contains(QStringLiteral("\r\n"));
    if (crlf) text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QString o = oldStr; o.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QString n = newStr; n.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));

    const int count = text.count(o);
    if (count == 0) {
        // Show what the file really contains near the first line of old_str, so the model can retry without re-reading everything.
        QString hint;
        const QStringList ol = o.split(QLatin1Char('\n'));
        QString key;
        for (const QString &l : ol) if (!l.trimmed().isEmpty()) { key = l.trimmed(); break; }
        if (!key.isEmpty()) {
            const QStringList fl = text.split(QLatin1Char('\n'));
            for (int i = 0; i < fl.size(); ++i)
                if (fl[i].contains(key)) {
                    hint = QStringLiteral("\nThe first line of old_str appears at line %1. The file really contains:\n").arg(i + 1);
                    for (int j = qMax(0, i - 2); j < qMin(int(fl.size()), i + 8); ++j) hint += QStringLiteral("%1| %2\n").arg(j + 1).arg(fl[j].left(200));
                    break;
                }
        }
        if (hint.isEmpty()) hint = QStringLiteral("\nNo line of the file contains the first line of old_str: the file is different from what you remember (a previous edit or write_file changed it). Use read_file to see the current content.");
        return err(QStringLiteral("old_str was not found in %1 (it must match exactly, including whitespace).").arg(p) + hint);
    }
    if (count > 1 && !replaceAll)
        return err(QStringLiteral("old_str matches %1 places in %2; add more surrounding context or set replace_all").arg(count).arg(p));
    text.replace(o, n);
    ToolResult r = commit(p, text.toUtf8());
    if (!r.isError) r.note = QStringLiteral("edit_file %1 (%2 replacement%3)").arg(p).arg(count).arg(count == 1 ? "" : "s");
    return r;
}

bool revertApplied(const QList<AppliedWrite> &applied, QStringList *errors)
{
    bool ok = true;
    for (int i = applied.size() - 1; i >= 0; --i) {
        const AppliedWrite &a = applied[i];
        if (a.existed) {
            QFile f(a.path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(a.original) != a.original.size()) {
                ok = false; if (errors) *errors << a.path + QStringLiteral(": ") + f.errorString();
            }
        } else if (!QFile::remove(a.path) && QFileInfo::exists(a.path)) {
            ok = false; if (errors) *errors << a.path + QStringLiteral(": cannot remove");
        }
    }
    return ok;
}

/* ── list / search ───────────────────────────────────────────────────────── */

ToolResult ToolContext::listDir(const QString &path)
{
    QString p, why;
    if (!resolve(path, &p, &why)) return err(why);
    if (m_remote) return remoteList(p);
    QFileInfo fi(p);
    if (!fi.exists()) return err(QStringLiteral("directory does not exist: ") + p);
    if (!fi.isDir())  return err(QStringLiteral("not a directory: ") + p);

    QDir dir(p);
    const QFileInfoList list = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                                                 QDir::DirsFirst | QDir::Name);
    QJsonArray arr;
    int n = 0;
    for (const QFileInfo &e : list) {
        if (!resolve(e.absoluteFilePath(), nullptr, nullptr)) continue;   // hide excluded / escaping entries
        if (++n > 1000) break;
        QJsonObject o;
        o["name"] = e.fileName();
        o["type"] = e.isSymLink() ? "symlink" : (e.isDir() ? "dir" : "file");
        if (e.isFile()) o["size"] = e.size();
        arr.append(o);
    }
    ToolResult r;
    r.text = QString::fromUtf8(QJsonDocument(QJsonObject{{"path", p}, {"entries", arr}}).toJson(QJsonDocument::Compact));
    r.note = QStringLiteral("list_dir %1 (%2 entries)").arg(p).arg(arr.size());
    return r;
}

ToolResult ToolContext::searchFiles(const QString &path, const QString &pattern, const QString &glob)
{
    QString p, why;
    if (!resolve(path, &p, &why)) return err(why);
    if (m_remote) return remoteSearch(p, pattern, glob);
    if (!QFileInfo(p).isDir()) return err(QStringLiteral("not a directory: ") + p);
    QRegularExpression re(pattern);
    if (!re.isValid()) return err(QStringLiteral("invalid regular expression: ") + re.errorString());
    QRegularExpression globRe;
    if (!glob.trimmed().isEmpty())
        globRe = QRegularExpression(QRegularExpression::wildcardToRegularExpression(glob.trimmed()));

    const int kMaxMatches = 200, kMaxFiles = 8000;
    int files = 0, matches = 0;
    QStringList out;
    QDirIterator it(p, QDir::Files | QDir::NoSymLinks | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext() && matches < kMaxMatches && files < kMaxFiles) {
        const QString f = it.next();
        if (f.contains(QStringLiteral("/.git/"))) continue;
        if (globRe.isValid() && !glob.trimmed().isEmpty() && !globRe.match(QFileInfo(f).fileName()).hasMatch()) continue;
        if (!resolve(f, nullptr, nullptr)) continue;
        ++files;
        QFile file(f);
        if (file.size() > 1024 * 1024 || !file.open(QIODevice::ReadOnly)) continue;
        const QByteArray data = file.readAll();
        if (data.left(4000).contains('\0')) continue;
        const QStringList lines = QString::fromUtf8(data).split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size() && matches < kMaxMatches; ++i) {
            if (re.match(lines[i]).hasMatch()) {
                QString l = lines[i].trimmed();
                if (l.size() > 200) l = l.left(200) + QStringLiteral("…");
                out << QStringLiteral("%1:%2: %3").arg(f).arg(i + 1).arg(l);
                ++matches;
            }
        }
    }
    ToolResult r;
    r.text = out.isEmpty() ? QStringLiteral("no matches (%1 files searched)").arg(files) : out.join(QLatin1Char('\n'));
    if (matches >= kMaxMatches) r.text += QStringLiteral("\n… result truncated at %1 matches").arg(kMaxMatches);
    r.note = QStringLiteral("search_files /%1/ in %2 (%3 matches)").arg(pattern, p).arg(matches);
    return r;
}

/* ── processes ───────────────────────────────────────────────────────────── */

void ToolContext::abort()
{
    m_aborted = true;
    if (m_mcp) m_mcp->abort();
    if (m_proc && m_proc->state() != QProcess::NotRunning && m_proc->processId() > 0)
        ::kill(-static_cast<pid_t>(m_proc->processId()), SIGKILL);
}

bool ToolContext::commandBlocked(const QString &cmd, QString *why)
{
    struct Rule { const char *re; const char *msg; };
    static const Rule rules[] = {
        {R"(\bsudo\b|\bpkexec\b|\bsu\s+-?\w*\s*(-c|$))", "sudo/su/pkexec are not available here (no password prompt); use the install_packages tool for system packages"},
        {R"(\brm\s+(-[a-zA-Z]+\s+)*-[a-zA-Z]*[rR][a-zA-Z]*\s+(-[a-zA-Z-]+\s+)*(/|/\*|~/?|~/\*|\$HOME/?|\$\{HOME\}/?)(\s|$|;|&|\|))", "recursive deletion of /, $HOME or ~"},
        {R"(\bmkfs(\.\w+)?\b)", "creating filesystems"},
        {R"(\bdd\b[^|;&]*\bof=/dev/)", "writing to block devices"},
        {R"(>\s*/dev/(sd|nvme|hd|vd))", "writing to block devices"},
        {R"(:\(\)\s*\{\s*:\s*\|\s*:\s*&\s*\}\s*;\s*:)", "fork bomb"},
        {R"(\b(shutdown|poweroff|reboot|halt)\b)", "power control"},
        {R"(\bchmod\s+-R\s+[0-7]+\s+/(\s|$))", "recursive chmod on /"},
        {R"((curl|wget)[^|;&]*\|\s*(sudo\s+)?(ba|z|da)?sh\b)", "piping downloads into a shell"},
    };
    for (const Rule &r : rules) {
        if (QRegularExpression(QString::fromLatin1(r.re)).match(cmd).hasMatch()) {
            if (why) *why = QString::fromLatin1(r.msg);
            return true;
        }
    }
    return false;
}

static void killGroup(QProcess &p)
{
    if (p.state() != QProcess::NotRunning && p.processId() > 0)
        ::kill(-static_cast<pid_t>(p.processId()), SIGKILL);
}

ToolResult ToolContext::execProcess(const QString &program, const QStringList &args, const QString &cwd,
                                    int timeoutSec, const QString &label, const QProcessEnvironment *baseEnv, const QByteArray *stdinData)
{
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    if (!cwd.isEmpty()) p.setWorkingDirectory(cwd);
    QProcessEnvironment env = baseEnv ? *baseEnv : QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("DEBIAN_FRONTEND"), QStringLiteral("noninteractive"));
    env.insert(QStringLiteral("TERM"), QStringLiteral("dumb"));
    env.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    env.insert(QStringLiteral("PAGER"), QStringLiteral("cat"));
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    p.setProcessEnvironment(env);
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.setChildProcessModifier([] { ::setsid(); });      // own process group: the whole tree can be killed

    QByteArray out, partial;
    int shown = 0;
    bool done = false, timedOut = false;
    QEventLoop loop;

    auto emitLines = [&](bool flush) {
        int nl;
        while ((nl = partial.indexOf('\n')) >= 0 || (flush && !partial.isEmpty())) {
            QByteArray line = nl >= 0 ? partial.left(nl) : partial;
            partial.remove(0, nl >= 0 ? nl + 1 : partial.size());
            if (!onOutput) continue;
            if (shown < 400) {
                QString s = QString::fromUtf8(line).trimmed();
                for (const QString &sec : std::as_const(m_redact)) s.replace(sec, QStringLiteral("***"));
                if (s.size() > 300) s = s.left(300) + QStringLiteral("…");
                onOutput(QStringLiteral("   ") + s);
            } else if (shown == 400) {
                onOutput(QStringLiteral("   … (more output not shown here; the agent still receives it)"));
            }
            ++shown;
        }
    };
    QObject::connect(&p, &QProcess::readyRead, &loop, [&] {
        const QByteArray d = p.readAll();
        if (out.size() < 8 * 1024 * 1024) out += d;
        partial += d;
        emitLines(false);
    });
    QObject::connect(&p, &QProcess::finished, &loop, [&](int, QProcess::ExitStatus) { done = true; loop.quit(); });
    QObject::connect(&p, &QProcess::errorOccurred, &loop, [&](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) { done = true; loop.quit(); }
    });
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] { timedOut = true; killGroup(p); });

    m_aborted = false;
    m_proc = &p;
    p.start();
    if (!p.waitForStarted(5000)) {
        m_proc = nullptr;
        return err(QStringLiteral("could not start %1: %2").arg(program, p.errorString()));
    }
    if (stdinData && !stdinData->isEmpty()) p.write(*stdinData);
    p.closeWriteChannel();
    timer.start(timeoutSec * 1000);
    if (!done && p.state() != QProcess::NotRunning) loop.exec();
    m_proc = nullptr;
    timer.stop();
    out += p.readAll();
    partial += QByteArray();
    emitLines(true);

    QString text = QString::fromUtf8(out);
    for (const QString &sec : std::as_const(m_redact)) text.replace(sec, QStringLiteral("***"));
    const int kHead = 6000, kTail = 12000;
    if (text.size() > kHead + kTail)
        text = text.left(kHead) + QStringLiteral("\n… [%1 characters omitted] …\n").arg(text.size() - kHead - kTail) + text.right(kTail);

    ToolResult r;
    if (m_aborted) { r.isError = true; r.text = QStringLiteral("Error: cancelled by the user\n") + text; r.note = label + QStringLiteral(" — cancelled"); return r; }
    if (timedOut)  { r.isError = true; r.text = QStringLiteral("Error: timed out after %1 s and was killed\n").arg(timeoutSec) + text; r.note = label + QStringLiteral(" — timed out"); return r; }
    const int code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    r.isError = (code != 0);
    r.text = QStringLiteral("exit code: %1\n").arg(code) + text;
    r.note = QStringLiteral("%1 — exit code %2").arg(label).arg(code);
    return r;
}

ToolResult ToolContext::runCommand(const QString &command, const QString &cwd, int timeoutSec)
{
    if (m_mode == Mode::Manual)
        return err(QStringLiteral("running commands is disabled (Manual mode). Tell the user which commands to run instead."));
    if (command.trimmed().isEmpty()) return err(QStringLiteral("empty command"));
    QString why;
    if (m_remote) {          // a disposable VM: no local blocklist, sudo allowed (if passwordless)
        QString dir = cwd.trimmed().isEmpty() ? m_rroot : QString();
        if (dir.isEmpty()) {
            QString c = cwd.trimmed();
            if (!c.startsWith(QLatin1Char('/'))) c = m_rroot + QLatin1Char('/') + c;
            if (!resolve(c, &dir, &why)) return err(why);
        }
        return remoteRunCommand(dir, command, timeoutSec);
    }
    if (commandBlocked(command, &why)) return err(QStringLiteral("command blocked by the safety policy: ") + why);

    QString dir;
    if (cwd.trimmed().isEmpty()) {
        if (m_dirs.isEmpty()) return err(QStringLiteral("no permitted directory"));
        dir = m_dirs.first();
    } else if (!resolve(cwd, &dir, &why)) {
        return err(why);
    }
    if (!QFileInfo(dir).isDir()) return err(QStringLiteral("cwd is not a directory: ") + dir);
    if (timeoutSec <= 0) timeoutSec = 120;
    timeoutSec = qMin(timeoutSec, 1800);

    ConfirmRequest req;
    req.kind = QStringLiteral("command");
    req.title = QStringLiteral("Run this command?");
    req.detail = QStringLiteral("cd %1\n$ %2\n\n(timeout %3 s)").arg(dir, command.trimmed()).arg(timeoutSec);
    if (!ask(req)) return err(QStringLiteral("the user denied running this command"));

    if (onOutput) onOutput(QStringLiteral("$ ") + command.trimmed());
    return execProcess(QStringLiteral("/bin/bash"), {QStringLiteral("-c"), command}, dir, timeoutSec,
                       QStringLiteral("run_command `%1`").arg(command.trimmed().left(80)));
}

/* ── environment / packages ──────────────────────────────────────────────── */

static QString quick(const QString &prog, const QStringList &args, int ms = 2500, int *code = nullptr)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(prog, args);
    if (!p.waitForStarted(2000)) { if (code) *code = -1; return {}; }
    p.closeWriteChannel();
    if (!p.waitForFinished(ms)) { p.kill(); p.waitForFinished(500); if (code) *code = -2; return {}; }
    if (code) *code = p.exitCode();
    return QString::fromUtf8(p.readAll()).trimmed();
}

static bool validName(const QString &n)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_+.:\\-]{0,80}$"));
    return re.match(n).hasMatch();
}

ToolResult ToolContext::checkEnvironment(const QStringList &commandsIn, const QStringList &packages, const QStringList &pkgconfig)
{
    if (m_remote) return remoteCheckEnvironment(commandsIn, packages, pkgconfig);
    QStringList commands = commandsIn;
    if (commands.isEmpty() && packages.isEmpty() && pkgconfig.isEmpty())
        commands = {"gcc", "g++", "make", "cmake", "pkg-config", "python3", "git"};

    QJsonObject o;
    o["os"] = osSummary();
    o["is_root"] = (::geteuid() == 0);
    o["user"] = qEnvironmentVariable("USER");
    const bool apt = !QStandardPaths::findExecutable(QStringLiteral("apt-get")).isEmpty();
    o["package_manager"] = apt ? QStringLiteral("apt-get (Debian/Ubuntu)") : QStringLiteral("unknown (not apt based)");
    QString priv = QStringLiteral("none (ask the user to install packages manually)");
    if (::geteuid() == 0) priv = QStringLiteral("root");
    else if (!QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty()) priv = QStringLiteral("pkexec (graphical password prompt)");
    else if (!QStandardPaths::findExecutable(QStringLiteral("sudo")).isEmpty()) {
        int c = 0; quick(QStringLiteral("sudo"), {"-n", "true"}, 2000, &c);
        if (c == 0) priv = QStringLiteral("sudo without password");
    }
    o["install_privilege"] = priv;

    QJsonObject cmds;
    for (const QString &c : commands.mid(0, 40)) {
        if (!validName(c)) { cmds[c] = QStringLiteral("invalid name"); continue; }
        const QString path = QStandardPaths::findExecutable(c);
        if (path.isEmpty()) { cmds[c] = QJsonValue::Null; continue; }
        QString v = quick(path, {QStringLiteral("--version")}).split(QLatin1Char('\n')).value(0);
        if (v.size() > 120) v = v.left(120);
        cmds[c] = path + (v.isEmpty() ? QString() : QStringLiteral(" — ") + v);
    }
    o["commands"] = cmds;

    if (!packages.isEmpty()) {
        QJsonObject pk;
        for (const QString &n : packages.mid(0, 40)) {
            if (!validName(n)) { pk[n] = QStringLiteral("invalid name"); continue; }
            int code = 0;
            const QString s = quick(QStringLiteral("dpkg-query"), {"-W", "-f=${db:Status-Abbrev} ${Version}", n}, 3000, &code);
            pk[n] = (code == 0 && s.startsWith(QLatin1String("ii"))) ? QStringLiteral("installed ") + s.mid(3) : QJsonValue(QJsonValue::Null);
        }
        o["packages"] = pk;
    }
    if (!pkgconfig.isEmpty()) {
        QJsonObject pc;
        for (const QString &n : pkgconfig.mid(0, 40)) {
            if (!validName(n)) { pc[n] = QStringLiteral("invalid name"); continue; }
            int code = 0;
            const QString s = quick(QStringLiteral("pkg-config"), {"--modversion", n}, 3000, &code);
            pc[n] = (code == 0) ? QJsonValue(s) : QJsonValue(QJsonValue::Null);
        }
        o["pkg_config"] = pc;
    }
    ToolResult r;
    r.text = QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
    r.note = QStringLiteral("check_environment (%1 commands, %2 packages, %3 pkg-config)").arg(commands.size()).arg(packages.size()).arg(pkgconfig.size());
    return r;
}

ToolResult ToolContext::installPackages(const QStringList &packages, bool update)
{
    if (m_mode == Mode::Manual)
        return err(QStringLiteral("installing packages is disabled (Manual mode). Tell the user which packages to install instead."));
    if (packages.isEmpty() || packages.size() > 30) return err(QStringLiteral("give between 1 and 30 package names"));
    for (const QString &n : packages)
        if (!validName(n) || (!m_remote && n != n.toLower())) return err(QStringLiteral("invalid package name: ") + n);
    if (m_remote) return remoteInstall(packages, update);
    if (QStandardPaths::findExecutable(QStringLiteral("apt-get")).isEmpty())
        return err(QStringLiteral("apt-get not found: this is not a Debian/Ubuntu system. Ask the user to install: ") + packages.join(' '));

    const QString pk = packages.join(QLatin1Char(' '));
    const QString script = (update ? QStringLiteral("apt-get update && ") : QString())
                         + QStringLiteral("apt-get install -y --no-install-recommends ") + pk;
    QString program;
    QStringList args;
    if (::geteuid() == 0) {
        program = QStringLiteral("/bin/sh"); args = {QStringLiteral("-c"), script};
    } else if (!QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty()) {
        program = QStringLiteral("pkexec");
        args = {QStringLiteral("env"), QStringLiteral("DEBIAN_FRONTEND=noninteractive"), QStringLiteral("/bin/sh"), QStringLiteral("-c"), script};
    } else {
        int c = 0;
        if (!QStandardPaths::findExecutable(QStringLiteral("sudo")).isEmpty()) quick(QStringLiteral("sudo"), {"-n", "true"}, 2000, &c); else c = 1;
        if (c != 0)
            return err(QStringLiteral("no way to get root rights non-interactively (no pkexec, no passwordless sudo). Ask the user to run:\n  sudo apt install ") + pk);
        program = QStringLiteral("sudo");
        args = {QStringLiteral("-n"), QStringLiteral("env"), QStringLiteral("DEBIAN_FRONTEND=noninteractive"), QStringLiteral("/bin/sh"), QStringLiteral("-c"), script};
    }

    ConfirmRequest req;
    req.kind = QStringLiteral("install");
    req.title = QStringLiteral("Install system packages?");
    req.detail = QStringLiteral("apt-get install -y --no-install-recommends %1\n\nThis needs administrator rights; a system password dialog may appear.").arg(pk);
    if (!ask(req)) return err(QStringLiteral("the user denied installing packages"));

    if (onOutput) onOutput(QStringLiteral("$ apt-get install ") + pk);
    return execProcess(program, args, QString(), 1800, QStringLiteral("install_packages %1").arg(pk));
}

/* ── MCP ─────────────────────────────────────────────────────────────────── */

void ToolContext::startMcp(const QList<McpServerCfg> &servers, const std::function<void(const QString &)> &log)
{
    if (m_mode == Mode::Manual) return;      // tools of external servers can have side effects
    bool any = false;
    for (const McpServerCfg &s : servers) any = any || s.enabled;
    if (!any) return;
    // Check the runner of each stdio server first: one clear message instead of a failure per server.
    QList<McpServerCfg> ready;
    QMap<QString, QStringList> missing;                          // runner -> skipped servers
    const QString home = QDir::homePath();
    const QStringList extra = {home + "/.local/bin", home + "/.cargo/bin", home + "/.volta/bin", "/usr/local/bin", "/usr/local/node/bin", "/snap/bin"};
    for (McpServerCfg s : servers) {
        if (s.enabled && !s.isHttp()) {
            QStringList parts = QProcess::splitCommand(s.target);
            if (!parts.isEmpty() && !parts[0].contains(QLatin1Char('/'))) {
                QString exe = QStandardPaths::findExecutable(parts[0]);
                if (exe.isEmpty()) {
                    QStringList dirs = extra;
                    const QDir nvm(home + "/.nvm/versions/node");
                    for (const QString &v : nvm.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) dirs << nvm.filePath(v) + "/bin";
                    exe = QStandardPaths::findExecutable(parts[0], dirs);
                }
                if (exe.isEmpty()) { missing[parts[0]] << s.name; continue; }
                parts[0] = exe;
                QString t; for (const QString &p : std::as_const(parts)) t += (t.isEmpty() ? QString() : QStringLiteral(" ")) + (p.contains(QLatin1Char(' ')) ? shellQuote(p) : p);
                s.target = t;
            }
        }
        ready << s;
    }
    if (log)
        for (auto it = missing.cbegin(); it != missing.cend(); ++it) {
            const QString hint = it.key() == QLatin1String("npx") || it.key() == QLatin1String("node") ? QStringLiteral("install Node.js: sudo apt install nodejs npm")
                               : it.key() == QLatin1String("uvx") || it.key() == QLatin1String("uv") ? QStringLiteral("install uv: sudo apt install pipx && pipx install uv  (or: curl -LsSf https://astral.sh/uv/install.sh | sh)")
                               : QStringLiteral("install it or fix the command");
            log(QStringLiteral("MCP: '%1' is not installed — skipped: %2. To use them, %3").arg(it.key(), it.value().join(QStringLiteral(", ")), hint));
        }
    bool anyReady = false;
    for (const McpServerCfg &s : ready) anyReady = anyReady || s.enabled;
    if (!anyReady) return;
    m_mcp = std::make_unique<McpManager>();
    m_mcp->startAll(ready, log);
}

ToolResult ToolContext::callMcp(const QString &name, const QJsonObject &args)
{
    if (m_mode == Mode::Manual || !m_mcp) return err(QStringLiteral("MCP tools are not available in this mode"));
    ConfirmRequest req;
    req.kind = QStringLiteral("mcp");
    req.title = QStringLiteral("Call this MCP tool?");
    req.detail = QStringLiteral("%1\n\n%2").arg(name, QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Indented)));
    if (!ask(req)) return err(QStringLiteral("the user denied calling ") + name);

    if (onOutput) onOutput(QStringLiteral("$ MCP ") + name);
    QString text, error, label;
    bool isErr = false;
    m_aborted = false;
    if (!m_mcp->call(name, args, 120, &text, &isErr, &error, &label)) {
        if (m_aborted) return err(QStringLiteral("cancelled by the user"));
        return err(QStringLiteral("MCP call failed: ") + error);
    }
    const int kHead = 6000, kTail = 12000;
    if (text.size() > kHead + kTail)
        text = text.left(kHead) + QStringLiteral("\n… [%1 characters omitted] …\n").arg(text.size() - kHead - kTail) + text.right(kTail);
    ToolResult r;
    r.isError = isErr;
    r.text = text.isEmpty() ? QStringLiteral("(empty result)") : text;
    r.note = QStringLiteral("mcp %1%2").arg(label, isErr ? QStringLiteral(" — error") : QString());
    return r;
}

/* ── project folders ─────────────────────────────────────────────────────── */

QString createProjectDir(const QString &base, const QString &nameIn, bool create, QString *error)
{
    QString slug = nameIn.trimmed().toLower();
    slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9_-]+")), QStringLiteral("-"));
    slug.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    if (slug.isEmpty()) slug = QStringLiteral("project");
    slug = slug.left(40);
    const QString root = QDir::cleanPath(base);
    if (!QFileInfo(root).isDir()) { if (error) *error = QStringLiteral("base folder does not exist: ") + root; return {}; }
    for (int i = 0; i < 50; ++i) {
        const QString suffix = QString::number(QRandomGenerator::global()->bounded(0x1000000u, 0xFFFFFFFFu), 16).left(6);
        const QString path = root + QLatin1Char('/') + slug + QLatin1Char('-') + suffix;
        if (QFileInfo::exists(path)) continue;
        if (create && !QDir().mkpath(path)) { if (error) *error = QStringLiteral("cannot create ") + path; return {}; }
        return path;
    }
    if (error) *error = QStringLiteral("could not find a free folder name");
    return {};
}

/* ── SSH machines ────────────────────────────────────────────────────────── */

const RemoteHost *ToolContext::findHost(const QString &name, QString *error) const
{
    for (const RemoteHost &h : m_hosts)
        if (h.name.compare(name.trimmed(), Qt::CaseInsensitive) == 0) return &h;
    QStringList names;
    for (const RemoteHost &h : m_hosts) names << h.name;
    *error = QStringLiteral("unknown host '%1'. Configured hosts: %2").arg(name, names.isEmpty() ? QStringLiteral("(none)") : names.join(QStringLiteral(", ")));
    return nullptr;
}

QString ToolContext::sshPipelineBase(const RemoteHost &h) const
{
    QStringList parts{QStringLiteral("ssh")};
    for (const QString &o : sshOptions(h)) parts << shellQuote(o);
    parts << shellQuote(sshDestination(h));
    return parts.join(QLatin1Char(' '));
}

QJsonArray ToolContext::remoteToolDefs() const
{
    if (m_mode == Mode::Manual || m_hosts.isEmpty()) return {};
    auto str = [](const char *d) { return QJsonObject{{"type", "string"}, {"description", d}}; };
    auto strArr = [](const char *d) { return QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}}, {"description", d}}; };
    auto tool = [](const char *n, const char *d, const QJsonObject &p, const QJsonArray &r) {
        return QJsonObject{{"name", n}, {"description", d}, {"input_schema", QJsonObject{{"type", "object"}, {"properties", p}, {"required", r}}}};
    };
    QJsonArray a;
    a.append(tool("remote_hosts", "List the configured SSH machines (virtual machines) with their OS/architecture notes.", QJsonObject(), QJsonArray()));
    a.append(tool("remote_run",
                  "Run a shell command (sh -c) on a remote SSH machine, in its work folder. Use it to install packages, compile, run and test on another "
                  "OS/architecture without touching this computer. Non-interactive; sudo works only if passwordless.",
                  QJsonObject{{"host", str("Host name from remote_hosts")}, {"command", str("Shell command")},
                              {"cwd", str("Directory on the remote machine (absolute, or relative to its work folder)")},
                              {"timeout", QJsonObject{{"type", "integer"}, {"description", "Seconds (default 300, max 3600)"}}}},
                  QJsonArray{"host", "command"}));
    a.append(tool("remote_upload",
                  "Copy a local file or folder (inside the permitted directories) to a remote machine (tar over ssh). Excludes .git, build and node_modules by default.",
                  QJsonObject{{"host", str("Host name")}, {"local_path", str("Absolute local file or folder; default: the first permitted folder")},
                              {"remote_path", str("Destination folder on the remote machine; default: <work folder>/<local folder name>")},
                              {"exclude", strArr("Extra patterns to leave out")}},
                  QJsonArray{"host"}));
    a.append(tool("remote_download",
                  "Copy a remote file or folder back into a local folder inside the permitted directories (tar over ssh), e.g. build results.",
                  QJsonObject{{"host", str("Host name")}, {"remote_path", str("Remote file or folder (absolute, or relative to its work folder)")},
                              {"local_dir", str("Absolute local destination folder")}, {"exclude", strArr("Patterns to leave out")}},
                  QJsonArray{"host", "remote_path", "local_dir"}));
    return a;
}

QJsonArray ToolContext::builtinDefs() const
{
    return (m_mode == Mode::Manual || !m_web) ? QJsonArray() : builtinToolDefs();
}

bool ToolContext::isBuiltinTool(const QString &name) const
{
    return m_mode != Mode::Manual && m_web && ai::isBuiltinTool(name);
}

ToolResult ToolContext::callBuiltin(const QString &name, const QJsonObject &args)
{
    if (!isBuiltinTool(name)) return err(QStringLiteral("tool not available in this mode: ") + name);
    if (name == QLatin1String("web_fetch") || name == QLatin1String("web_search")) {
        ConfirmRequest req;
        req.kind = QStringLiteral("web");
        req.title = QStringLiteral("Allow this web request?");
        req.detail = name == QLatin1String("web_fetch") ? QStringLiteral("GET %1").arg(args.value("url").toString())
                                                         : QStringLiteral("Web search (DuckDuckGo): %1").arg(args.value("query").toString());
        if (!ask(req)) return err(QStringLiteral("the user denied the web request"));
    }
    if (onOutput && (name == QLatin1String("web_fetch") || name == QLatin1String("web_search")))
        onOutput(QStringLiteral("   ↗ ") + (name == QLatin1String("web_fetch") ? args.value("url").toString() : args.value("query").toString()));
    m_aborted = false;
    return runBuiltinTool(name, args, [this] { return m_aborted; });
}

QJsonArray ToolContext::extraToolDefs() const
{
    QJsonArray a = mcpToolDefs();
    for (const QJsonValue &v : builtinDefs()) a.append(v);
    if (m_remote) return a;                  // everything already runs on the target machine
    for (const QJsonValue &v : remoteToolDefs()) a.append(v);
    return a;
}

QString ToolContext::remoteSummary() const
{
    if (m_remote)
        return QStringLiteral("EXECUTION TARGET: the remote machine '%1' (%2@%3:%4, %5%6). ALL your tools — list_dir, read_file, search_files, write_file, edit_file, "
                              "check_environment, install_packages and run_command — operate ON THAT MACHINE; the project lives there in %7. Nothing is created or installed on "
                              "the user's own computer (MCP tools, if any, still run on the user's computer and cannot see the project). "
                              "Package manager there: %8. sudo works in run_command and install_packages when check_environment reports an install_privilege (a saved password is supplied automatically; never print or look for it). "
                              "If check_environment reports install_privilege NONE, do not try to work around it (no su, no other tricks): tell the user what to configure and stop. "
                              "Do all building, installing and testing on this machine.%9")
            .arg(m_rhost.name, m_rhost.user, m_rhost.host).arg(m_rhost.port).arg(m_ros)
            .arg(m_rhost.notes.isEmpty() ? QString() : QStringLiteral("; ") + m_rhost.notes)
            .arg(m_rroot, m_rpm.isEmpty() ? QStringLiteral("unknown") : m_rpm)
            .arg(QString());
    if (m_mode == Mode::Manual || m_hosts.isEmpty()) return {};
    QStringList l;
    for (const RemoteHost &h : m_hosts)
        l << QStringLiteral("  - %1: %2@%3:%4%5").arg(h.name, h.user, h.host).arg(h.port).arg(h.notes.isEmpty() ? QString() : QStringLiteral("  (%1)").arg(h.notes));
    return QStringLiteral("Remote SSH machines (virtual machines) you can use with remote_run / remote_upload / remote_download:\n") + l.join(QLatin1Char('\n'))
         + QStringLiteral("\nPrefer them for installing packages, compiling and testing, so this computer stays clean: remote_upload the project, "
                          "remote_run the build and tests there, remote_download artifacts only if needed. Pick the machine whose OS/architecture matches the task.");
}

ToolResult ToolContext::remoteHostsInfo()
{
    QJsonArray arr;
    for (const RemoteHost &h : m_hosts)
        arr.append(QJsonObject{{"name", h.name}, {"address", QStringLiteral("%1@%2:%3").arg(h.user, h.host).arg(h.port)}, {"auth", h.auth},
                               {"work_dir", h.remoteDir}, {"notes", h.notes}});
    ToolResult r;
    r.text = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    r.note = QStringLiteral("remote_hosts (%1)").arg(m_hosts.size());
    return r;
}

ToolResult ToolContext::remoteRun(const QString &host, const QString &command, const QString &cwd, int timeoutSec)
{
    if (m_mode == Mode::Manual) return err(QStringLiteral("remote machines are not available in Manual mode"));
    QString e;
    const RemoteHost *h = findHost(host, &e);
    if (!h) return err(e);
    if (command.trimmed().isEmpty()) return err(QStringLiteral("empty command"));
    if (timeoutSec <= 0) timeoutSec = 300;
    timeoutSec = qMin(timeoutSec, 3600);
    QString dir = cwd.trimmed().isEmpty() ? h->remoteDir : cwd.trimmed();
    if (!dir.startsWith(QLatin1Char('/')) && !dir.startsWith(QLatin1Char('~')))
        dir = h->remoteDir.trimmed().isEmpty() ? dir : h->remoteDir.trimmed() + QLatin1Char('/') + dir;

    ConfirmRequest req;
    req.kind = QStringLiteral("remote");
    req.title = QStringLiteral("Run this command on the remote machine?");
    req.detail = QStringLiteral("%1  (%2@%3:%4)\ncd %5\n$ %6\n\n(timeout %7 s)").arg(h->name, h->user, h->host).arg(h->port).arg(dir, command.trimmed()).arg(timeoutSec);
    if (!ask(req)) return err(QStringLiteral("the user denied running this command on ") + h->name);

    if (!m_usedHosts.contains(h->name)) m_usedHosts << h->name;
    if (onOutput) onOutput(QStringLiteral("[%1] $ %2").arg(h->name, command.trimmed()));
    QStringList args = sshOptions(*h);
    args << sshDestination(*h) << remoteCommand(remoteScript(dir, command));
    const QProcessEnvironment env = sshEnvironment(*h);
    ToolResult r = execProcess(QStringLiteral("ssh"), args, QString(), timeoutSec, QStringLiteral("remote_run [%1] `%2`").arg(h->name, command.trimmed().left(70)), &env);
    if (r.isError && r.text.contains(QLatin1String("exit code: 255")))
        r.text += QStringLiteral("\n(exit code 255 usually means the SSH connection itself failed: check the host, port, user, key/password in the SSH settings and that the VM is running)");
    return r;
}

ToolResult ToolContext::remoteUpload(const QString &host, const QString &localPathIn, const QString &remotePathIn, const QStringList &exclude)
{
    if (m_mode == Mode::Manual) return err(QStringLiteral("remote machines are not available in Manual mode"));
    QString e;
    const RemoteHost *h = findHost(host, &e);
    if (!h) return err(e);
    QString lp, why;
    const QString want = localPathIn.trimmed().isEmpty() ? (m_dirs.isEmpty() ? QString() : m_dirs.first()) : localPathIn.trimmed();
    if (!resolve(want, &lp, &why)) return err(why);
    const QFileInfo fi(lp);
    if (!fi.exists()) return err(QStringLiteral("local path does not exist: ") + lp);
    QString rp = remotePathIn.trimmed();
    if (rp.isEmpty()) rp = (h->remoteDir.trimmed().isEmpty() ? QStringLiteral("~/geany-ai-work") : h->remoteDir.trimmed()) + QLatin1Char('/') + (fi.isDir() ? fi.fileName() : QString());
    else if (!rp.startsWith(QLatin1Char('/')) && !rp.startsWith(QLatin1Char('~'))) rp = h->remoteDir.trimmed() + QLatin1Char('/') + rp;
    while (rp.size() > 1 && rp.endsWith(QLatin1Char('/'))) rp.chop(1);

    ConfirmRequest req;
    req.kind = QStringLiteral("remote");
    req.title = QStringLiteral("Upload files to the remote machine?");
    req.detail = QStringLiteral("%1 (%2@%3:%4)\n%5  ->  %6").arg(h->name, h->user, h->host).arg(h->port).arg(lp, rp);
    if (!ask(req)) return err(QStringLiteral("the user denied the upload"));

    QStringList ex = QStringList{".git", "build", "node_modules"} + exclude;
    QStringList exArgs;
    for (const QString &x : std::as_const(ex)) if (!x.trimmed().isEmpty()) exArgs << QStringLiteral("--exclude=") + shellQuote(x.trimmed());
    const QString tarCmd = fi.isDir()
        ? QStringLiteral("tar -C %1 %2 -cf - .").arg(shellQuote(lp), exArgs.join(QLatin1Char(' ')))
        : QStringLiteral("tar -C %1 -cf - %2").arg(shellQuote(fi.absolutePath()), shellQuote(fi.fileName()));
    const QString remote = QStringLiteral("mkdir -p %1 && tar -C %1 -xf -").arg(remotePathExpr(rp));
    const QString pipeline = QStringLiteral("set -o pipefail; %1 | %2 %3").arg(tarCmd, sshPipelineBase(*h), shellQuote(remoteCommand(remote)));

    if (!m_usedHosts.contains(h->name)) m_usedHosts << h->name;
    if (onOutput) onOutput(QStringLiteral("[%1] upload %2 -> %3").arg(h->name, lp, rp));
    const QProcessEnvironment env = sshEnvironment(*h);
    ToolResult r = execProcess(QStringLiteral("/bin/bash"), {QStringLiteral("-c"), pipeline}, QString(), 900,
                               QStringLiteral("remote_upload [%1] %2 -> %3").arg(h->name, lp, rp), &env);
    if (!r.isError) r.text = QStringLiteral("uploaded %1 to %2:%3 (excluded: %4)\nUse remote_run with cwd=%3 to build and test there.").arg(lp, h->name, rp, ex.join(QStringLiteral(", ")));
    return r;
}

ToolResult ToolContext::remoteDownload(const QString &host, const QString &remotePathIn, const QString &localDirIn, const QStringList &exclude)
{
    if (m_mode == Mode::Manual) return err(QStringLiteral("remote machines are not available in Manual mode"));
    QString e;
    const RemoteHost *h = findHost(host, &e);
    if (!h) return err(e);
    QString ld, why;
    if (!resolve(localDirIn, &ld, &why)) return err(why);
    if (QFileInfo::exists(ld) && !QFileInfo(ld).isDir()) return err(QStringLiteral("local_dir is not a directory: ") + ld);
    QString rp = remotePathIn.trimmed();
    if (rp.isEmpty()) return err(QStringLiteral("remote_path is required"));
    if (!rp.startsWith(QLatin1Char('/')) && !rp.startsWith(QLatin1Char('~'))) rp = h->remoteDir.trimmed() + QLatin1Char('/') + rp;

    ConfirmRequest req;
    req.kind = QStringLiteral("remote");
    req.title = QStringLiteral("Download files from the remote machine?");
    req.detail = QStringLiteral("%1 (%2@%3:%4)\n%5  ->  %6\n\nExisting local files with the same names are overwritten (not covered by \"Undo all changes\").").arg(h->name, h->user, h->host).arg(h->port).arg(rp, ld);
    if (!ask(req)) return err(QStringLiteral("the user denied the download"));
    if (!QDir().mkpath(ld)) return err(QStringLiteral("cannot create ") + ld);

    QStringList exArgs;
    for (const QString &x : exclude) if (!x.trimmed().isEmpty()) exArgs << QStringLiteral("--exclude=") + shellQuote(x.trimmed());
    // a directory is copied as its content; a single file keeps its name
    const QString remote = QStringLiteral("p=%1; if [ -d \"$p\" ]; then tar -C \"$p\" %2 -cf - .; else tar -C \"$(dirname \"$p\")\" -cf - \"$(basename \"$p\")\"; fi")
                               .arg(remotePathExpr(rp), exArgs.join(QLatin1Char(' ')));
    const QString pipeline = QStringLiteral("set -o pipefail; %1 %2 | tar -C %3 --no-same-owner -xf -")
                                 .arg(sshPipelineBase(*h), shellQuote(remoteCommand(remote)), shellQuote(ld));
    if (!m_usedHosts.contains(h->name)) m_usedHosts << h->name;
    if (onOutput) onOutput(QStringLiteral("[%1] download %2 -> %3").arg(h->name, rp, ld));
    const QProcessEnvironment env = sshEnvironment(*h);
    ToolResult r = execProcess(QStringLiteral("/bin/bash"), {QStringLiteral("-c"), pipeline}, QString(), 900,
                               QStringLiteral("remote_download [%1] %2 -> %3").arg(h->name, rp, ld), &env);
    if (!r.isError) r.text = QStringLiteral("downloaded %1:%2 into %3").arg(h->name, rp, ld);
    return r;
}

ToolResult ToolContext::remoteTest(const RemoteHost &h)
{
    QStringList args = sshOptions(h);
    args << sshDestination(h)
         << remoteCommand(QStringLiteral("echo \"user: $(id -un)\"; uname -srm; . /etc/os-release 2>/dev/null && echo \"$PRETTY_NAME\"; "
                                         "for c in gcc g++ cmake make python3 git; do command -v $c >/dev/null 2>&1 && echo \"found: $c\"; done; echo \"work dir: $HOME\""));
    const QProcessEnvironment env = sshEnvironment(h);
    return execProcess(QStringLiteral("ssh"), args, QString(), 40, QStringLiteral("ssh test %1").arg(h.name), &env);
}


/* ── remote target: the whole workspace lives on an SSH machine ───────────── */

bool ToolContext::sshExec(const QString &script, const QByteArray &in, int timeoutSec, QByteArray *out, QByteArray *errOut, int *code, QString *error)
{
    QProcess p;
    QStringList args = sshOptions(m_rhost);
    args << sshDestination(m_rhost) << remoteCommand(script);
    p.setProgram(QStringLiteral("ssh"));
    p.setArguments(args);
    p.setProcessEnvironment(sshEnvironment(m_rhost));
    p.setChildProcessModifier([] { ::setsid(); });
    QByteArray so, se;
    QEventLoop loop;
    bool done = false, timedOut = false;
    QObject::connect(&p, &QProcess::readyReadStandardOutput, &loop, [&] { if (so.size() < 64 * 1024 * 1024) so += p.readAllStandardOutput(); });
    QObject::connect(&p, &QProcess::readyReadStandardError, &loop, [&] { if (se.size() < 1024 * 1024) se += p.readAllStandardError(); });
    QObject::connect(&p, &QProcess::finished, &loop, [&](int, QProcess::ExitStatus) { done = true; loop.quit(); });
    QObject::connect(&p, &QProcess::errorOccurred, &loop, [&](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) { done = true; loop.quit(); } });
    QTimer timer; timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] { timedOut = true; killGroup(p); });
    QTimer poll; poll.setInterval(150);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (m_aborted) killGroup(p); });
    m_proc = &p;
    p.start();
    if (!p.waitForStarted(5000)) { m_proc = nullptr; if (error) *error = QStringLiteral("could not start ssh: ") + p.errorString(); return false; }
    if (!in.isEmpty()) p.write(in);
    p.closeWriteChannel();
    timer.start(timeoutSec * 1000);
    poll.start();
    if (!done && p.state() != QProcess::NotRunning) loop.exec();
    m_proc = nullptr;
    timer.stop(); poll.stop();
    so += p.readAllStandardOutput(); se += p.readAllStandardError();
    if (!m_usedHosts.contains(m_rhost.name)) m_usedHosts << m_rhost.name;
    if (m_aborted) { if (error) *error = QStringLiteral("cancelled by the user"); return false; }
    if (timedOut)  { if (error) *error = QStringLiteral("timed out after %1 s").arg(timeoutSec); return false; }
    const int rc = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    if (code) *code = rc;
    if (out) *out = so;
    if (errOut) *errOut = se;
    if (rc == 255 && error) *error = QStringLiteral("SSH connection to %1 failed: %2").arg(m_rhost.name, QString::fromUtf8(se).trimmed());
    return rc != 255;
}

bool ToolContext::connectRemote(const RemoteHost &h, const QString &existingPath, bool newProject, const QString &projectName, QString *error)
{
    m_remote = true;
    m_rhost = h;
    bool known = false;
    for (const RemoteHost &x : std::as_const(m_hosts)) if (x.name == h.name) known = true;
    if (!known) m_hosts << h;

    QString slug = projectName.trimmed().toLower();
    slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9_-]+")), QStringLiteral("-"));
    slug.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    if (slug.isEmpty()) slug = QStringLiteral("project");
    slug = slug.left(40);

    QString script = QStringLiteral("base=%1; mkdir -p \"$base\" && base=$(cd \"$base\" && pwd -P) || { echo 'ERR cannot create the work folder'; exit 1; }\n")
                         .arg(remotePathExpr(h.remoteDir.trimmed().isEmpty() ? QStringLiteral("~/geany-ai-work") : h.remoteDir.trimmed()));
    if (!existingPath.isEmpty()) {
        script += QStringLiteral("d=%1; [ -d \"$d\" ] || { echo 'ERR the project folder does not exist on the machine'; exit 1; }; d=$(cd \"$d\" && pwd -P)\n")
                      .arg(remotePathExpr(existingPath));
    } else if (newProject) {
        script += QStringLiteral("d=; for i in 1 2 3 4 5 6 7 8; do s=$(od -An -N3 -tx1 /dev/urandom | tr -d ' \\n'); n=\"$base/%1-$s\"; mkdir \"$n\" 2>/dev/null && { d=$n; break; }; done\n"
                                 "[ -n \"$d\" ] || { echo 'ERR cannot create the project folder'; exit 1; }\n").arg(slug);
    } else {
        script += QStringLiteral("d=$base\n");
    }
    script += QStringLiteral("echo \"ROOT=$d\"\n"
                             ". /etc/os-release 2>/dev/null; echo \"OS=${PRETTY_NAME:-$(uname -s)}, $(uname -m)\"\n"
                             "pm=none; for c in apt-get dnf yum zypper pacman apk xbps-install; do command -v $c >/dev/null 2>&1 && { pm=$c; break; }; done; echo \"PM=$pm\"\n");
    QByteArray out, se; int code = -1; QString e;
    if (!sshExec(script, {}, 60, &out, &se, &code, &e)) { if (error) *error = e; return false; }
    QString root, os, pm;
    for (const QString &l : QString::fromUtf8(out).split(QLatin1Char('\n'))) {
        if (l.startsWith(QLatin1String("ROOT="))) root = l.mid(5).trimmed();
        else if (l.startsWith(QLatin1String("OS="))) os = l.mid(3).trimmed();
        else if (l.startsWith(QLatin1String("PM="))) pm = l.mid(3).trimmed();
        else if (l.startsWith(QLatin1String("ERR "))) { if (error) *error = l.mid(4); return false; }
    }
    if (root.isEmpty() || !root.startsWith(QLatin1Char('/'))) {
        if (error) *error = QStringLiteral("could not prepare the workspace on %1 (exit %2): %3 %4").arg(h.name).arg(code, 0, 10).arg(QString::fromUtf8(out).trimmed(), QString::fromUtf8(se).trimmed());
        return false;
    }
    m_rroot = QDir::cleanPath(root); m_ros = os; m_rpm = pm;
    m_redact.clear();
    for (const QString &sec : {h.password, h.sudoPassword}) if (sec.size() >= 3 && !m_redact.contains(sec)) m_redact << sec;
    m_dirs = QStringList{m_rroot};
    // exclusions: absolute ones as given, relative ones inside the project folder (lexical)
    const QStringList raw = m_exclRaw.split(QLatin1Char(','), Qt::SkipEmptyParts);
    m_excl.clear();
    for (QString x : raw) {
        x = x.trimmed();
        if (x.isEmpty()) continue;
        m_excl << QDir::cleanPath(x.startsWith(QLatin1Char('/')) ? x : m_rroot + QLatin1Char('/') + x);
    }
    return true;
}

bool ToolContext::remoteRead(const QString &p, qint64 limit, QByteArray *data, int *state, QString *error)
{
    const QString script = QStringLiteral("p=%1\nif [ -d \"$p\" ]; then echo DIR; elif [ -f \"$p\" ] || [ -L \"$p\" ]; then s=$(wc -c < \"$p\" | tr -d ' '); "
                                          "if [ \"$s\" -gt %2 ]; then echo \"BIG $s\"; else echo \"FILE $s\"; cat \"$p\"; fi; else echo MISSING; fi\n")
                               .arg(remotePathExpr(p)).arg(limit);
    QByteArray out, se; int code = 0; QString e;
    if (!sshExec(script, {}, 120, &out, &se, &code, &e)) { *error = e; return false; }
    const int nl = out.indexOf('\n');
    const QByteArray head = nl >= 0 ? out.left(nl) : out;
    if (code != 0 || nl < 0) { *error = QStringLiteral("cannot read %1: %2").arg(p, QString::fromUtf8(se).trimmed()); return false; }
    *state = head.startsWith("DIR") ? 2 : head.startsWith("BIG") ? 3 : head.startsWith("FILE") ? 1 : 0;
    *data = (*state == 1) ? out.mid(nl + 1) : QByteArray();
    return true;
}

bool ToolContext::pushToRemote(const QString &remotePath, const QByteArray &data, QString *error)
{
    if (!m_remote) { *error = QStringLiteral("no remote target"); return false; }
    QString why, resolved;
    if (!resolve(remotePath, &resolved, &why)) { *error = why; return false; }
    return remoteWrite(resolved, data, error);
}

bool ToolContext::remoteWrite(const QString &p, const QByteArray &data, QString *error)
{
    const QString script = QStringLiteral("p=%1; mkdir -p \"$(dirname \"$p\")\" && cat > \"$p\"\n").arg(remotePathExpr(p));
    QByteArray out, se; int code = 0; QString e;
    if (!sshExec(script, data, 300, &out, &se, &code, &e)) { *error = e; return false; }
    if (code != 0) { *error = QStringLiteral("cannot write %1: %2").arg(p, QString::fromUtf8(se).trimmed()); return false; }
    return true;
}

ToolResult ToolContext::remoteList(const QString &p)
{
    const QString script = QStringLiteral("p=%1; [ -d \"$p\" ] || { echo NODIR; exit 0; }\n"
                                          "for f in \"$p\"/* \"$p\"/.[!.]* \"$p\"/..?*; do [ -e \"$f\" ] || [ -L \"$f\" ] || continue; "
                                          "if [ -L \"$f\" ]; then t=symlink; s=0; elif [ -d \"$f\" ]; then t=dir; s=0; else t=file; s=$(wc -c < \"$f\" | tr -d ' '); fi; "
                                          "printf '%s\\t%s\\t%s\\n' \"$t\" \"$s\" \"${f##*/}\"; done | head -n 1500\n").arg(remotePathExpr(p));
    QByteArray out, se; int code = 0; QString e;
    if (!sshExec(script, {}, 60, &out, &se, &code, &e)) return err(e);
    if (out.startsWith("NODIR")) return err(QStringLiteral("directory does not exist: ") + p);
    struct Ent { QString type, name; qint64 size; };
    QList<Ent> ents;
    for (const QString &l : QString::fromUtf8(out).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList f = l.split(QLatin1Char('\t'));
        if (f.size() < 3) continue;
        if (!resolve(p + QLatin1Char('/') + f[2], nullptr, nullptr)) continue;      // hide excluded entries
        ents.append({f[0], f[2], f[1].toLongLong()});
    }
    std::sort(ents.begin(), ents.end(), [](const Ent &a, const Ent &b) {
        if ((a.type == QLatin1String("dir")) != (b.type == QLatin1String("dir"))) return a.type == QLatin1String("dir");
        return a.name < b.name;
    });
    QJsonArray arr;
    for (const Ent &en : ents) {
        if (arr.size() >= 1000) break;
        QJsonObject o{{"name", en.name}, {"type", en.type}};
        if (en.type == QLatin1String("file")) o["size"] = en.size;
        arr.append(o);
    }
    ToolResult r;
    r.text = QString::fromUtf8(QJsonDocument(QJsonObject{{"path", p}, {"entries", arr}}).toJson(QJsonDocument::Compact));
    r.note = QStringLiteral("list_dir [%1] %2 (%3 entries)").arg(m_rhost.name, p).arg(arr.size());
    return r;
}

ToolResult ToolContext::remoteSearch(const QString &p, const QString &pattern, const QString &glob)
{
    if (pattern.isEmpty()) return err(QStringLiteral("empty pattern"));
    QString inc;
    if (!glob.trimmed().isEmpty()) inc = QStringLiteral(" --include=") + shellQuote(glob.trimmed());
    const QString script = QStringLiteral("p=%1; [ -d \"$p\" ] || { echo NODIR; exit 0; }\n"
                                          "grep -rnIE%2 --exclude-dir=.git -e %3 -- \"$p\" 2>/dev/null | cut -c1-260 | head -n 200\n")
                               .arg(remotePathExpr(p), inc, shellQuote(pattern));
    QByteArray out, se; int code = 0; QString e;
    if (!sshExec(script, {}, 120, &out, &se, &code, &e)) return err(e);
    if (out.startsWith("NODIR")) return err(QStringLiteral("not a directory: ") + p);
    QStringList lines;
    for (const QString &l : QString::fromUtf8(out).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const int c = l.indexOf(QLatin1Char(':'));
        if (c > 0 && !resolve(l.left(c), nullptr, nullptr)) continue;           // excluded files
        lines << l;
    }
    ToolResult r;
    r.text = lines.isEmpty() ? QStringLiteral("no matches") : lines.join(QLatin1Char('\n'));
    if (lines.size() >= 200) r.text += QStringLiteral("\n… result truncated at 200 matches");
    r.note = QStringLiteral("search_files [%1] /%2/ in %3 (%4 matches)").arg(m_rhost.name, pattern, p).arg(lines.size());
    return r;
}

ToolResult ToolContext::remoteRunCommand(const QString &dir, const QString &command, int timeoutSec)
{
    if (timeoutSec <= 0) timeoutSec = 120;
    timeoutSec = qMin(timeoutSec, 3600);
    ConfirmRequest req;
    req.kind = QStringLiteral("command");
    req.title = QStringLiteral("Run this command?");
    req.target = m_rhost.name;
    req.detail = QStringLiteral("[%1] cd %2\n$ %3\n\n(timeout %4 s)").arg(m_rhost.name, dir, command.trimmed()).arg(timeoutSec);
    if (!ask(req)) return err(QStringLiteral("the user denied running this command"));
    if (onOutput) onOutput(QStringLiteral("[%1] $ %2").arg(m_rhost.name, command.trimmed()));
    QStringList args = sshOptions(m_rhost);
    QString script = remoteScript(dir, command);
    QByteArray in;
    const QString spw = effectiveSudoPassword(m_rhost);
    if (!spw.isEmpty()) { script = sudoPrelude() + script; in = (spw + QLatin1Char('\n')).toUtf8(); }     // sudo works, the password never touches a command line
    args << sshDestination(m_rhost) << remoteCommand(script);
    const QProcessEnvironment env = sshEnvironment(m_rhost);
    if (!m_usedHosts.contains(m_rhost.name)) m_usedHosts << m_rhost.name;
    ToolResult r = execProcess(QStringLiteral("ssh"), args, QString(), timeoutSec, QStringLiteral("run_command [%1] `%2`").arg(m_rhost.name, command.trimmed().left(70)), &env, in.isEmpty() ? nullptr : &in);
    if (r.isError && (r.text.contains(QLatin1String("a password is required")) || r.text.contains(QLatin1String("a terminal is required"))
                      || r.text.contains(QLatin1String("incorrect password")) || r.text.contains(QLatin1String("Sorry, try again"))
                      || (r.text.contains(QLatin1String("Permission denied")) && r.text.contains(QLatin1String("/var/lib/")))))
        r.text += QStringLiteral("\n(HINT: no root rights on this machine. %1 Do not try to work around it: tell the user to set a sudo password for this machine in Settings → SSH machines, "
                                 "or to log in as root / enable passwordless sudo, and stop.)")
                      .arg(spw.isEmpty() ? QStringLiteral("No sudo password is saved.") : QStringLiteral("The saved sudo password was rejected or sudo is not allowed for this user."));
    if (r.isError && r.text.contains(QLatin1String("exit code: 255")))
        r.text += QStringLiteral("\n(exit code 255 can mean that the SSH connection itself failed: is the VM still running?)");
    return r;
}

ToolResult ToolContext::remoteCheckEnvironment(const QStringList &commandsIn, const QStringList &packages, const QStringList &pkgconfig)
{
    QStringList commands = commandsIn;
    if (commands.isEmpty() && packages.isEmpty() && pkgconfig.isEmpty()) commands = {"gcc", "g++", "make", "cmake", "pkg-config", "python3", "git"};
    QString s = QStringLiteral(". /etc/os-release 2>/dev/null; echo \"os: ${PRETTY_NAME:-unknown}, $(uname -m)\"; echo \"kernel: $(uname -sr)\"; echo \"user: $(id -un) (uid $(id -u))\"\n"
                               "pm=none; for c in apt-get dnf yum zypper pacman apk xbps-install; do command -v $c >/dev/null 2>&1 && { pm=$c; break; }; done; echo \"package_manager: $pm\"\n"
                               "if [ \"$(id -u)\" = 0 ]; then echo 'install_privilege: root'; elif command sudo -n true 2>/dev/null; then echo 'install_privilege: sudo without password'; "
                               "elif [ -n \"$__gai_haspw\" ] && sudo true 2>/dev/null; then echo 'install_privilege: sudo with the saved password (used automatically by run_command and install_packages)'; "
                               "else echo 'install_privilege: NONE — installing is impossible; tell the user to set a sudo password in Settings → SSH machines, log in as root or enable passwordless sudo'; fi\n"
                               "echo \"disk_free_work_dir: $(df -h . 2>/dev/null | tail -1 | awk '{print $4}')\"\n");
    for (const QString &c : commands.mid(0, 40)) {
        if (!validName(c)) { s += QStringLiteral("echo %1\n").arg(shellQuote(QStringLiteral("command %1: invalid name").arg(c))); continue; }
        s += QStringLiteral("if p=$(command -v %1 2>/dev/null); then echo \"command %1: $p — $(%1 --version 2>&1 | head -1 | cut -c1-120)\"; else echo 'command %1: NOT FOUND'; fi\n").arg(c);
    }
    for (const QString &n : packages.mid(0, 40)) {
        if (!validName(n)) { s += QStringLiteral("echo %1\n").arg(shellQuote(QStringLiteral("package %1: invalid name").arg(n))); continue; }
        s += QStringLiteral("case $pm in apt-get) v=$(dpkg-query -W -f='${db:Status-Abbrev} ${Version}' %1 2>/dev/null); case $v in ii*) r=\"installed ${v#ii }\";; *) r='NOT installed';; esac;; "
                            "dnf|yum|zypper) r=$(rpm -q %1 2>/dev/null) || r='NOT installed';; pacman) r=$(pacman -Q %1 2>/dev/null) || r='NOT installed';; "
                            "apk) r=$(apk info -e %1 >/dev/null 2>&1 && echo installed || echo 'NOT installed');; *) r='unknown (no supported package manager)';; esac; echo \"package %1: $r\"\n").arg(n);
    }
    for (const QString &n : pkgconfig.mid(0, 40)) {
        if (!validName(n)) { s += QStringLiteral("echo %1\n").arg(shellQuote(QStringLiteral("pkg-config %1: invalid name").arg(n))); continue; }
        s += QStringLiteral("if v=$(pkg-config --modversion %1 2>/dev/null); then echo \"pkg-config %1: $v\"; else echo 'pkg-config %1: NOT FOUND'; fi\n").arg(n);
    }
    QByteArray out, se; int code = 0; QString e;
    QString full = remoteScript(m_rroot, s);
    QByteArray in;
    const QString spw = effectiveSudoPassword(m_rhost);
    if (!spw.isEmpty()) { full = sudoPrelude() + full; in = (spw + QLatin1Char('\n')).toUtf8(); }
    if (!sshExec(full, in, 90, &out, &se, &code, &e)) return err(e);
    ToolResult r;
    r.text = QStringLiteral("machine: %1\n").arg(m_rhost.name) + QString::fromUtf8(out).trimmed();
    r.note = QStringLiteral("check_environment [%1] (%2 commands, %3 packages, %4 pkg-config)").arg(m_rhost.name).arg(commands.size()).arg(packages.size()).arg(pkgconfig.size());
    return r;
}

ToolResult ToolContext::remoteInstall(const QStringList &packages, bool update)
{
    const QString pk = packages.join(QLatin1Char(' '));
    ConfirmRequest req;
    req.kind = QStringLiteral("install");
    req.title = QStringLiteral("Install system packages?");
    req.target = m_rhost.name;
    req.detail = QStringLiteral("On %1 (%2): install %3\n(uses the machine's package manager as root or with passwordless sudo)").arg(m_rhost.name, m_rpm.isEmpty() ? QStringLiteral("package manager unknown") : m_rpm, pk);
    if (!ask(req)) return err(QStringLiteral("the user denied installing packages"));
    const QString upd = update ? QStringLiteral("1") : QStringLiteral("0");
    const QString script = QStringLiteral(
        "if [ \"$(id -u)\" = 0 ]; then S=; elif command sudo -n true 2>/dev/null; then S='sudo -n'; "
        "elif [ -n \"$__gai_haspw\" ] && sudo true 2>/dev/null; then S=sudo; "
        "else echo 'no root rights on this machine: not root, and sudo needs a password that is not saved or was rejected. Set the sudo password for this machine in Settings → SSH machines, or log in as root, or configure passwordless sudo. Do not try to work around this; tell the user and stop.'; exit 77; fi\n"
        "U=%1; P=\"%2\"\n"
        "if command -v apt-get >/dev/null 2>&1; then [ $U = 1 ] && $S env DEBIAN_FRONTEND=noninteractive apt-get update; $S env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $P\n"
        "elif command -v dnf >/dev/null 2>&1; then $S dnf install -y $P\n"
        "elif command -v yum >/dev/null 2>&1; then $S yum install -y $P\n"
        "elif command -v zypper >/dev/null 2>&1; then $S zypper --non-interactive install $P\n"
        "elif command -v pacman >/dev/null 2>&1; then $S pacman -Sy --noconfirm --needed $P\n"
        "elif command -v apk >/dev/null 2>&1; then [ $U = 1 ] && $S apk update; $S apk add --no-cache $P\n"
        "elif command -v xbps-install >/dev/null 2>&1; then $S xbps-install -Sy $P\n"
        "else echo 'no supported package manager found (apt-get, dnf, yum, zypper, pacman, apk, xbps)'; exit 78; fi\n").arg(upd, pk);
    if (onOutput) onOutput(QStringLiteral("[%1] install %2").arg(m_rhost.name, pk));
    QStringList args = sshOptions(m_rhost);
    QString full = remoteScript(m_rroot, script);
    QByteArray in;
    const QString spw = effectiveSudoPassword(m_rhost);
    if (!spw.isEmpty()) { full = sudoPrelude() + full; in = (spw + QLatin1Char('\n')).toUtf8(); }
    args << sshDestination(m_rhost) << remoteCommand(full);
    const QProcessEnvironment env = sshEnvironment(m_rhost);
    return execProcess(QStringLiteral("ssh"), args, QString(), 3600, QStringLiteral("install_packages [%1] %2").arg(m_rhost.name, pk), &env, in.isEmpty() ? nullptr : &in);
}

bool ToolContext::revert(const QList<AppliedWrite> &applied, QStringList *errors)
{
    if (!m_remote) return revertApplied(applied, errors);
    bool ok = true;
    for (int i = applied.size() - 1; i >= 0; --i) {
        const AppliedWrite &a = applied[i];
        QString e;
        if (a.existed) {
            if (!remoteWrite(a.path, a.original, &e)) { ok = false; if (errors) *errors << a.path + QStringLiteral(": ") + e; }
        } else {
            QByteArray out, se; int code = 0;
            if (!sshExec(QStringLiteral("rm -f %1\n").arg(remotePathExpr(a.path)), {}, 60, &out, &se, &code, &e) || code != 0) {
                ok = false; if (errors) *errors << a.path + QStringLiteral(": ") + (e.isEmpty() ? QString::fromUtf8(se).trimmed() : e);
            }
        }
    }
    return ok;
}

ToolResult ToolContext::downloadProject(const QString &localDir)
{
    if (!m_remote) return err(QStringLiteral("no remote target"));
    QString name = QFileInfo(m_rroot).fileName();
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    if (name.isEmpty()) name = QStringLiteral("project");
    const QString folder = localDir + QLatin1Char('/') + name;
    if (!QDir().mkpath(folder)) return err(QStringLiteral("cannot create ") + folder);
    const QString remote = QStringLiteral("tar -C %1 -cf - .").arg(shellQuote(m_rroot));
    const QString pipeline = QStringLiteral("set -o pipefail; %1 %2 | tar -C %3 --no-same-owner -xf -")
                                 .arg(sshPipelineBase(m_rhost), shellQuote(remoteCommand(remote)), shellQuote(folder));
    const QProcessEnvironment env = sshEnvironment(m_rhost);
    ToolResult r = execProcess(QStringLiteral("/bin/bash"), {QStringLiteral("-c"), pipeline}, QString(), 1800,
                               QStringLiteral("download project [%1] %2 -> %3").arg(m_rhost.name, m_rroot, folder), &env);
    if (r.isError) return r;

    // pack the folder as .tar.gz and .zip next to it
    const QString pack = QStringLiteral(
        "cd %1 && tar -czf %2.tar.gz %2 && "
        "{ rm -f %2.zip; if command -v zip >/dev/null 2>&1; then zip -qr %2.zip %2; "
        "elif command -v python3 >/dev/null 2>&1; then python3 -m zipfile -c %2.zip %2 >/dev/null; else echo NOZIP; fi; }")
        .arg(shellQuote(localDir), shellQuote(name));
    const ToolResult p = execProcess(QStringLiteral("/bin/bash"), {QStringLiteral("-c"), pack}, QString(), 900,
                                     QStringLiteral("pack project %1").arg(name));
    QString text = QStringLiteral("downloaded %1:%2\n  folder:  %3\n").arg(m_rhost.name, m_rroot, folder);
    if (QFileInfo::exists(folder + QStringLiteral(".tar.gz"))) text += QStringLiteral("  tar.gz:  %1.tar.gz\n").arg(folder);
    if (QFileInfo::exists(folder + QStringLiteral(".zip")))    text += QStringLiteral("  zip:     %1.zip\n").arg(folder);
    else text += QStringLiteral("  (no .zip: neither 'zip' nor python3 is installed on this computer)\n");
    if (p.isError && !QFileInfo::exists(folder + QStringLiteral(".tar.gz"))) return err(QStringLiteral("packing failed: ") + p.text);
    r.text = text.trimmed();
    return r;
}


/* Shrinks compiler output: the first few errors with their code lines, no template "candidate" spew, plus Qt6 hints. */
QString condenseCompilerOutput(const QString &text)
{
    static const QRegularExpression errRe(QStringLiteral("^\\s*(\\S+?):(\\d+):(\\d+): (?:fatal )?error: "));
    static const QRegularExpression anyLoc(QStringLiteral("^\\s*\\S+?:\\d+:\\d+: "));
    const QStringList lines = text.split(QLatin1Char('\n'));
    QList<int> errs;
    for (int i = 0; i < lines.size(); ++i)
        if (errRe.match(lines[i]).hasMatch()) errs << i;
    if (errs.isEmpty() || text.size() < 2500) return text;
    QString out = QStringLiteral("(compiler output condensed: %1 error message(s); the first %2 are shown with their code lines. FIX THE FIRST ONE FIRST.)\n")
                      .arg(errs.size()).arg(qMin(5, int(errs.size())));
    for (int k = 0; k < errs.size() && k < 5; ++k) {
        int i = errs[k];
        out += lines[i].left(260) + QLatin1Char('\n');
        int shown = 0;
        for (int j = i + 1; j < lines.size() && shown < 4; ++j, ++shown) {
            if (anyLoc.match(lines[j]).hasMatch() || lines[j].startsWith(QLatin1String("make")) ) break;
            out += lines[j].left(200) + QLatin1Char('\n');
        }
    }
    auto has = [&](const char *s) { return text.contains(QLatin1String(s)); };
    QStringList hints;
    if (has("QTcpSocket::error") || has("QAbstractSocket::error") || has("QOverload<QAbstractSocket::SocketError>"))
        hints << QStringLiteral("Qt6: the signal is errorOccurred(QAbstractSocket::SocketError); connect(socket, &QAbstractSocket::errorOccurred, this, &Class::slot). error() is no longer a signal.");
    if (has("operator<") && has("QHostAddress"))
        hints << QStringLiteral("Qt6: QHostAddress has no operator<, so it cannot be a QMap/QSet key. Use QString (addr.toString()) or quint32 (addr.toIPv4Address()) as the key.");
    if (has("QRegExp")) hints << QStringLiteral("Qt6: QRegExp is gone; use QRegularExpression.");
    if (has("SkipEmptyParts")) hints << QStringLiteral("Qt6: use Qt::SkipEmptyParts instead of QString::SkipEmptyParts.");
    if (has("fromHostAddress")) hints << QStringLiteral("QHostInfo::fromHostAddress does not exist; use QHostInfo::lookupHost(name, receiver, slot) or QHostInfo::fromName.");
    if (has("no declaration matches") || has("was not declared in this scope"))
        hints << QStringLiteral("A member function or variable is used/defined in the .cpp but not declared in the class in the .h (or the declaration differs): add or fix it in the header with edit_file.");
    if (has("QStringRef") || has("QLinkedList") || has("qrand") || has("QTextCodec"))
        hints << QStringLiteral("Qt6 removed QStringRef, QLinkedList, qrand and (mostly) QTextCodec: use QStringView, std::list, QRandomGenerator, QStringConverter.");
    if (!hints.isEmpty()) out += QStringLiteral("HINTS:\n- ") + hints.join(QStringLiteral("\n- ")) + QLatin1Char('\n');
    out += QStringLiteral("--- end of the output:\n") + lines.mid(qMax(0, lines.size() - 4)).join(QLatin1Char('\n')).left(600);
    return out;
}

/* ── build ───────────────────────────────────────────────────────────────── */

static QString buildScript()
{
    return QStringLiteral(
        "LOG=$(mktemp 2>/dev/null || echo /tmp/geany-ai-build.$$); J=$(nproc 2>/dev/null || echo 2); STEP=; RC=0\n"
        "run() { STEP=\"$1\"; echo \"\\$ $1\"; sh -c \"$1\" >>\"$LOG\" 2>&1; RC=$?; return $RC; }\n"
        "if [ -n \"$GAI_USERCMD\" ]; then run \"$GAI_USERCMD\"\n"
        "elif [ -f CMakeLists.txt ]; then run \"cmake -S . -B build -DCMAKE_BUILD_TYPE=Release\" && run \"cmake --build build -j$J\"\n"
        "elif ls ./*.pro >/dev/null 2>&1; then P=$(ls ./*.pro | head -n 1); QM=$(command -v qmake6 || command -v qmake || echo qmake6); run \"$QM $P\" && run \"make -j$J\"\n"
        "elif [ -f meson.build ]; then run \"meson setup build --reconfigure || meson setup build\" && run \"ninja -C build\"\n"
        "elif [ -f Cargo.toml ]; then run \"cargo build\"\n"
        "elif [ -f go.mod ]; then run \"go build ./...\"\n"
        "elif [ -f Makefile ] || [ -f makefile ] || [ -f GNUmakefile ]; then run \"make -j$J\"\n"
        "elif [ -f package.json ]; then run \"npm install --no-audit --no-fund\" && run \"npm run build --if-present\"\n"
        "elif ls ./*.py >/dev/null 2>&1 || [ -f pyproject.toml ]; then run \"python3 -m compileall -q .\"\n"
        "else echo 'NO BUILD SYSTEM FOUND in this folder (looked for CMakeLists.txt, *.pro, meson.build, Cargo.toml, go.mod, Makefile, package.json, *.py). Create one (CMakeLists.txt is the usual choice) or call run_command with an explicit command.'; exit 3; fi\n"
        "if [ $RC -eq 0 ]; then\n"
        "  echo 'BUILD OK'; grep -E 'warning:' \"$LOG\" | head -n 5 | cut -c1-240\n"
        "  echo 'Executables:'; find . -maxdepth 3 -type f -perm -u+x ! -path './.git/*' ! -name '*.sh' ! -name '*.so*' ! -name '*.py' ! -path '*/CMakeFiles/*' 2>/dev/null | head -n 8\n"
        "  rm -f \"$LOG\"; exit 0\n"
        "fi\n"
        "echo \"BUILD FAILED in step: $STEP (exit $RC)\"\n"
        "echo '--- first errors:'\n"
        "grep -nE '(error|Error:|ERROR|fatal|undefined reference|CMake Error|No such file|not found|cannot find|Could not find|No package)' \"$LOG\" | head -n 14 | cut -c1-300\n"
        "echo '--- source lines around the first compiler errors:'\n"
        "grep -oE '^[^ :]+:[0-9]+:[0-9]+: (fatal )?error' \"$LOG\" | sed -n '1,4p' | while IFS=: read f l c rest; do\n"
        "  for cand in \"$f\" \"build/$f\"; do if [ -f \"$cand\" ]; then echo \"== $cand:$l\"; awk -v s=$((l>4?l-4:1)) -v e=$((l+3)) -v t=$l 'NR>=s&&NR<=e{printf \"%s%d: %s\\n\", (NR==t?\">\":\" \"), NR, $0}' \"$cand\"; break; fi; done\n"
        "done\n"
        "echo '--- end of the build log:'; tail -n 12 \"$LOG\" | cut -c1-240\n"
        "if grep -qE 'Could not find a package configuration file|No package .* found|CMake Error.*Could NOT find|CMake Error.*Could not find' \"$LOG\"; then echo 'HINT: a development package is missing. Find its exact name with check_environment (packages / pkgconfig), install it with install_packages, then build again. Do not edit CMakeLists to work around a missing package.'; fi\n"
        "if grep -qE 'fatal error: .*: No such file or directory' \"$LOG\"; then echo 'HINT: a header is missing: either the -dev package is not installed (install_packages) or the include path / file name is wrong (check the file really exists).'; fi\n"
        "if grep -qE 'undefined reference to .vtable for|undefined reference to .*staticMetaObject' \"$LOG\"; then echo 'HINT: a Qt class with Q_OBJECT is not processed by moc: add set(CMAKE_AUTOMOC ON) and list the header in add_executable (or include the .moc file).'; fi\n"
        "if grep -qiE 'uic|\\.ui' \"$LOG\"; then echo 'HINT: .ui (Qt Designer) files are easy to get wrong. Prefer building the interface in C++ code instead of a .ui file.'; fi\n"
        "if grep -qE \"(‘|')Q[A-Za-z]+(’|') was not declared in this scope|has no member named|incomplete type\" \"$LOG\"; then echo 'HINT: a Qt class is probably missing its #include (e.g. QFile -> #include <QFile>, QDir, QTextStream, QHostInfo, QMessageBox, QFileDialog). Add the include at the top of that file.'; fi\n"
        "if grep -qE 'is not a member of .Q[A-Za-z]+' \"$LOG\"; then echo 'HINT: you called a Qt function that does not exist in Qt 6. Check the real API (web_fetch https://doc.qt.io/qt-6/<classname-lowercase>.html or deepwiki) instead of guessing.'; fi\n"
        "if grep -qE 'for \\(int [a-z]+ : ' \"$LOG\" && grep -qE 'cannot convert .const QString. to .int.' \"$LOG\"; then echo 'HINT: the loop variable type does not match the container element type (QStringList holds QString, not int).'; fi\n"
        "if grep -qE 'has no member named .toUInt|toUInt\\(\\)' \"$LOG\"; then echo 'HINT: QHostAddress::toUInt() does not exist: use toIPv4Address() (returns quint32); QHostAddress(quint32) builds an address from it.'; fi\n"
        "if grep -q 'defaultInterface' \"$LOG\"; then echo 'HINT: QNetworkInterface::defaultInterface() does not exist: loop over QNetworkInterface::allInterfaces(), skip loopback / not IsUp, and use addressEntries() (ip(), netmask(), prefixLength()).'; fi\n"
        "if grep -qE 'QHostInfo.*fromAddress|fromAddress. is not a member' \"$LOG\"; then echo 'HINT: QHostInfo::fromAddress does not exist: use QHostInfo::fromName(ip.toString()) for a (blocking) reverse lookup inside a worker thread, or QHostInfo::lookupHost(name, receiver, slot) asynchronously.'; fi\n"
        "if grep -qE 'cannot convert .[A-Za-z]+\\*. to .QThread\\*.' \"$LOG\"; then echo 'HINT: moveToThread() needs a QThread*, not this; keep a QThread* member (or use QThread::currentThread()).'; fi\n"
        "if grep -qE 'qInetNetmask|qToBigEndian|htonl|ntohl' \"$LOG\"; then echo 'HINT: for netmask maths use quint32 mask = prefix==0 ? 0 : 0xFFFFFFFFu << (32-prefix); and QHostAddress::toIPv4Address(); include <arpa/inet.h> only if you really need htonl.'; fi\n"
        "if grep -q 'Cannot find source file' \"$LOG\"; then echo 'HINT: CMakeLists.txt lists a source file that does not exist (yet). Either create it, or replace the list by file(GLOB SOURCES CONFIGURE_DEPENDS *.cpp *.h) and add_executable(name ${SOURCES}).'; fi\n"
        "if grep -qE 'does not name a type|has not been declared|incomplete type|invalid use of incomplete' \"$LOG\"; then echo 'HINT: this is almost always a missing #include (the class header) or a class used before its declaration. Add the include of the header that declares it (for Qt: <QTcpSocket>, <QTimer>, ...).'; fi\n"
        "echo 'FIX THE FIRST ERROR FIRST (later errors are often a consequence), change as little as possible with edit_file, then build_project again.'\n"
        "rm -f \"$LOG\"; exit 1\n");
}

ToolResult ToolContext::buildProject(const QString &dirIn, const QString &command, int timeoutSec)
{
    if (m_mode == Mode::Manual)
        return err(QStringLiteral("building is disabled (Manual mode). Tell the user which commands to run instead."));
    QString dir, why;
    if (dirIn.trimmed().isEmpty()) {
        if (m_dirs.isEmpty()) return err(QStringLiteral("no permitted directory"));
        dir = m_dirs.first();
    } else if (!resolve(dirIn.trimmed(), &dir, &why)) {
        return err(why);
    }
    if (!m_remote && !QFileInfo(dir).isDir()) return err(QStringLiteral("not a directory: ") + dir);
    if (timeoutSec <= 0) timeoutSec = 600;
    timeoutSec = qMin(timeoutSec, 3600);
    if (!command.trimmed().isEmpty() && !m_remote) {
        QString w;
        if (commandBlocked(command, &w)) return err(QStringLiteral("command blocked by the safety policy: ") + w);
    }

    ConfirmRequest req;
    req.kind = QStringLiteral("command");
    req.title = QStringLiteral("Run this command?");
    req.target = targetName();
    req.detail = QStringLiteral("%1cd %2\n$ build%3\n\n(auto-detects CMake / meson / cargo / go / make / npm; timeout %4 s)")
                     .arg(m_remote ? QStringLiteral("[%1] ").arg(m_rhost.name) : QString(), dir,
                          command.trimmed().isEmpty() ? QString() : QStringLiteral(": ") + command.trimmed()).arg(timeoutSec);
    if (!ask(req)) return err(QStringLiteral("the user denied building"));

    const QString label = QStringLiteral("build_project%1").arg(m_remote ? QStringLiteral(" [%1]").arg(m_rhost.name) : QString());
    if (onOutput) onOutput(QStringLiteral("$ build ") + dir);
    const QString body = buildScript();
    if (m_remote) {
        QStringList args = sshOptions(m_rhost);
        QString script = remoteScript(dir, QStringLiteral("GAI_USERCMD=%1\n").arg(shellQuote(command.trimmed())) + body);
        QByteArray in;
        const QString spw = effectiveSudoPassword(m_rhost);
        if (!spw.isEmpty()) { script = sudoPrelude() + script; in = (spw + QLatin1Char('\n')).toUtf8(); }
        args << sshDestination(m_rhost) << remoteCommand(script);
        const QProcessEnvironment env = sshEnvironment(m_rhost);
        if (!m_usedHosts.contains(m_rhost.name)) m_usedHosts << m_rhost.name;
        return execProcess(QStringLiteral("ssh"), args, QString(), timeoutSec, label, &env, in.isEmpty() ? nullptr : &in);
    }
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GAI_USERCMD"), command.trimmed());
    return execProcess(QStringLiteral("/bin/bash"), {QStringLiteral("-c"), body}, dir, timeoutSec, label, &env);
}

} // namespace ai
