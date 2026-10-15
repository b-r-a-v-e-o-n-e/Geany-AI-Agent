// SPDX-License-Identifier: MIT
#include "tools.h"
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QRandomGenerator>
#include <QStringDecoder>
#include <QSysInfo>
#include <QTimer>
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

ToolContext::ToolContext(const QStringList &dirs, const QString &exclusions, Mode mode) : m_mode(mode)
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

ToolContext::~ToolContext() = default;

bool ToolContext::resolve(const QString &path, QString *resolved, QString *why) const
{
    if (path.isEmpty() || !QDir::isAbsolutePath(path)) {
        if (why) *why = QStringLiteral("path must be an absolute path: ") + path;
        return false;
    }
    const QString c = canonicalize(path);
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
    if (!fromStaged) {
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
    if (QFileInfo(p).isDir()) return err(QStringLiteral("path is a directory: ") + p);

    // Original: previously staged write wins in Manual mode, otherwise what is on disk now.
    QByteArray original;
    bool existed = false;
    StagedWrite *sw = nullptr;
    if (m_mode == Mode::Manual)
        for (StagedWrite &s : m_staged) if (s.path == p) { sw = &s; break; }

    if (sw) {
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
    req.editorDirty = isEditorDirty && isEditorDirty(p);
    if (!ask(req)) return err(QStringLiteral("the user denied writing ") + p);

    if (existed) {
        bool already = false;
        for (const AppliedWrite &a : std::as_const(m_applied)) if (a.path == p) { already = true; break; }
        if (!already) backup(p, original);
    }
    QDir().mkpath(QFileInfo(p).absolutePath());
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(bytes) != bytes.size())
        return err(QStringLiteral("cannot write %1: %2").arg(p, f.errorString()));
    f.close();

    bool found = false;
    for (AppliedWrite &a : m_applied) if (a.path == p) { a.content = bytes; found = true; break; }
    if (!found) m_applied.append({p, original, existed, bytes});
    if (onFileWritten) onFileWritten(AppliedWrite{p, original, existed, bytes});

    r.text = existed ? QStringLiteral("file updated on disk") : QStringLiteral("file created on disk");
    r.note = QStringLiteral("write_file %1 (%2 bytes, %3)").arg(p).arg(bytes.size()).arg(existed ? "updated" : "created");
    return r;
}

ToolResult ToolContext::writeFile(const QString &path, const QString &content)
{
    QString p, why;
    if (!resolve(path, &p, &why)) return err(why);
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
    if (count == 0) return err(QStringLiteral("old_str was not found in %1 (it must match exactly, including whitespace)").arg(p));
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
                                    int timeoutSec, const QString &label)
{
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    if (!cwd.isEmpty()) p.setWorkingDirectory(cwd);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
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
    p.closeWriteChannel();
    timer.start(timeoutSec * 1000);
    if (!done && p.state() != QProcess::NotRunning) loop.exec();
    m_proc = nullptr;
    timer.stop();
    out += p.readAll();
    partial += QByteArray();
    emitLines(true);

    QString text = QString::fromUtf8(out);
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
        if (!validName(n) || n != n.toLower()) return err(QStringLiteral("invalid package name: ") + n);
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
    m_mcp = std::make_unique<McpManager>();
    m_mcp->startAll(servers, log);
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

} // namespace ai
