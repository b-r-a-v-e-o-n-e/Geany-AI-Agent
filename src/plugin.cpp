// SPDX-License-Identifier: MIT
// geany-ai-qt — Claude assistant plugin for Geany, Qt6 user interface.
// Original GTK plugin: Copyright (c) 2026 Jack Sidman Smith (MIT).

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QRegularExpression>
#include <QDesktopServices>
#include <QUrl>
#include <QFileInfo>
#include <QMessageBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QStandardPaths>

#include "api.h"
#include "dialogs.h"
#include "diff.h"
#include "i18n.h"
#include "qtgtk.h"
#include "sessions.h"
#include "settings.h"
#include "tools.h"

#include <geanyplugin.h>

GeanyPlugin *geany_plugin;
GeanyData   *geany_data;

namespace {

GtkWidget       *g_itemAsk      = nullptr;
GtkWidget       *g_itemSettings = nullptr;
ai::Settings     g_settings;
ai::AiSession   *g_session      = nullptr;
ai::ChatDialog *g_chat          = nullptr;

void status(const QString &msg)
{
    const QByteArray b = msg.toUtf8();
    ui_set_statusbar(FALSE, "%s", b.constData());
}

void log(const QString &msg)
{
    const QByteArray b = msg.toUtf8();
    msgwin_status_add("%s", b.constData());
}

void refreshLanguage() { i18n::setLanguage(g_settings.language); }

void relabelMenus()
{
    if (g_itemAsk)
        gtk_menu_item_set_label(GTK_MENU_ITEM(g_itemAsk), TR("Ask AI Agent…").toUtf8().constData());
    if (g_itemSettings)
        gtk_menu_item_set_label(GTK_MENU_ITEM(g_itemSettings), TR("geany-ai Settings…").toUtf8().constData());
}

bool startQt()
{
    QString err;
    if (qtgtk::ensureApp(&err)) return true;
    dialogs_show_msgbox(GTK_MESSAGE_ERROR, "%s", err.toUtf8().constData());
    return false;
}

/* ── applying approved changes ───────────────────────────────────────────── */

QString backupRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("geany-ai/backups/") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
}

void applyWrites(const QList<ai::StagedWrite> &accepted)
{
    const QString backups = backupRoot();
    bool anyBackup = false;

    for (const ai::StagedWrite &w : accepted) {
        if (w.existed) {
            QString rel = w.path;
            rel.replace(QLatin1Char('/'), QLatin1Char('%'));
            QDir().mkpath(backups);
            QFile bf(QDir(backups).filePath(rel));
            if (bf.open(QIODevice::WriteOnly | QIODevice::Truncate)) { bf.write(w.original); anyBackup = true; }
        }
        QDir().mkpath(QFileInfo(w.path).absolutePath());
        // Write in place (not rename): keeps permissions, owner and inode of the file.
        QFile f(w.path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(w.content) != w.content.size()) {
            const QString m = TR("geany-ai: failed to write %1: %2").arg(w.path, f.errorString());
            log(m);
            status(m);
            continue;
        }
        f.close();

        GeanyDocument *doc = document_find_by_filename(w.path.toUtf8().constData());
        if (doc) document_reload_force(doc, nullptr);
        log(TR("geany-ai: applied %1").arg(w.path));
    }
    if (anyBackup) log(TR("geany-ai: backups saved in %1").arg(backups));
}

QString g_target;                 // SSH machine of the open conversation ("" = this computer): its files are not local

/* VM runs: the project lives on the machine, but every file the agent writes is mirrored here so it can be
   viewed and edited in Geany; saving a mirrored file in Geany uploads it back to the machine. */
QString mirrorBase()
{
    if (g_target.isEmpty() || !g_session || !g_session->context() || !g_session->context()->isRemote()) return {};
    gchar *d = g_build_filename(g_get_user_data_dir(), "geany-ai", "mirror", nullptr);
    QString base = QString::fromUtf8(d);
    g_free(d);
    QString m = g_target; m.replace(QLatin1Char('/'), QLatin1Char('-'));
    QString pr = QFileInfo(g_session->context()->remoteRoot()).fileName();
    pr.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    return base + QLatin1Char('/') + m + QLatin1Char('/') + (pr.isEmpty() ? QStringLiteral("project") : pr);
}

QString mirrorOf(const QString &remotePath)
{
    const QString base = mirrorBase();
    const QString root = g_session && g_session->context() ? g_session->context()->remoteRoot() : QString();
    if (base.isEmpty() || root.isEmpty() || !remotePath.startsWith(root + QLatin1Char('/'))) return {};
    return base + remotePath.mid(root.size());
}

/* The path as it exists on this computer: itself, or its mirror copy for a VM run. */
QString localPathFor(const QString &path)
{
    return g_target.isEmpty() ? path : mirrorOf(path);
}

void reloadIfClean(const QString &pathIn, bool warnDirty)
{
    const QString path = localPathFor(pathIn);
    if (path.isEmpty()) return;
    GeanyDocument *doc = document_find_by_filename(path.toUtf8().constData());
    if (!doc) return;
    if (doc->changed) {
        if (warnDirty) log(TR("geany-ai: %1 has unsaved changes in the editor and was not reloaded").arg(path));
        return;
    }
    document_reload_force(doc, nullptr);
}

void showTab(GeanyDocument *doc)
{
    const gint page = document_get_notebook_page(doc);
    if (page >= 0) gtk_notebook_set_current_page(GTK_NOTEBOOK(geany_data->main_widgets->notebook), page);
}

/* Show a file in a Geany tab (reloads it first if it is open and has no unsaved edits). */
void openInEditor(const QString &pathIn)
{
    const QString path = localPathFor(pathIn);
    if (path.isEmpty()) return;
    GeanyDocument *doc = document_find_by_filename(path.toUtf8().constData());
    if (doc) {
        if (!doc->changed) document_reload_force(doc, nullptr);
        showTab(doc);
        return;
    }
    if (QFileInfo(path).isFile()) document_open_file(path.toUtf8().constData(), FALSE, nullptr, nullptr);
}

/* Compact unified diff in the progress log, so the user can follow every change. */
void logDiff(const ai::AppliedWrite &w)
{
    if (!g_chat) return;
    const QVector<ai::DiffLine> lines = ai::unifiedDiff(QString::fromUtf8(w.original), QString::fromUtf8(w.content), 2);
    int shown = 0;
    for (const ai::DiffLine &l : lines) {
        if (shown++ >= 60) { g_chat->append(QStringLiteral("   … (diff truncated, open the file to see everything)")); break; }
        const QString p = l.kind == ai::DiffLine::Add ? QStringLiteral("+ ") : l.kind == ai::DiffLine::Del ? QStringLiteral("- ")
                        : l.kind == ai::DiffLine::Context ? QStringLiteral("  ") : QString();
        g_chat->append(QStringLiteral("   ") + p + l.text);
    }
}

int g_openedThisTurn = 0;

/* ── conversation (chat) lifecycle ───────────────────────────────────────── */

struct Conversation {
    QString     id, title;
    QStringList dirs;
    QJsonArray  digest;
    ai::Mode    mode = ai::Mode::Manual;
    QString     target;                  // SSH machine ("" = local)
    QString     lastInstruction, lastSummary;
    QStringList turnFiles;
} g_conv;

void saveConversation()
{
    if (!g_session || g_conv.id.isEmpty()) return;
    ai::SessionData sd;
    sd.meta.id = g_conv.id; sd.meta.title = g_conv.title; sd.meta.kind = g_session->kind();
    sd.meta.mode = ai::modeToString(g_conv.mode); sd.meta.dirs = g_conv.dirs; sd.meta.time = QDateTime::currentDateTime();
    sd.messages = g_session->history();
    sd.digest = g_conv.digest;
    ai::saveSession(sd);
}

void endConversation()
{
    if (g_session) {
        saveConversation();
        g_session->disconnect();
        if (!g_session->idle()) g_session->cancel();
        g_session->deleteLater();
        g_session = nullptr;
    }
    if (g_chat) g_chat->hide();
    g_conv = Conversation();
    g_target.clear();
    qtgtk::scheduleFlush();
    status(QStringLiteral("geany-ai: ") + TR("Session ended."));
}

void beginTurn(const QString &instruction)
{
    g_conv.lastInstruction = instruction;
    g_conv.turnFiles.clear();
    g_openedThisTurn = 0;
    g_chat->append(QString());
    g_chat->append(QStringLiteral("▶ ") + instruction);
    g_chat->setStatus(TR("The agent is working…"));
    g_chat->setBusy(true);
    status(TR("geany-ai: running…"));
}

void addDigest(const QString &summary)
{
    QJsonArray files;
    for (const QString &f : std::as_const(g_conv.turnFiles)) files.append(f);
    g_conv.digest.append(QJsonObject{{"instruction", g_conv.lastInstruction}, {"summary", summary}, {"files", files},
                                     {"time", QDateTime::currentDateTime().toString(Qt::ISODate)}});
    while (g_conv.digest.size() > 40) g_conv.digest.removeAt(0);
}

void onReview()
{
    if (!g_session || !g_session->context()) return;
    ai::ToolContext *ctx = g_session->context();
    const QList<ai::AppliedWrite> applied = ctx->applied();
    if (applied.isEmpty()) return;
    const bool undo = ai::runFinishDialog(g_conv.lastSummary, applied, ctx->backupDir(), QString(), [](const QString &p) { openInEditor(p); });
    if (!undo) return;
    QStringList errors;
    ctx->revert(applied, &errors);
    if (!g_target.isEmpty()) {                                       // keep the local mirror in step with the machine
        for (const ai::AppliedWrite &a : applied) {
            const QString lp = mirrorOf(a.path);
            if (lp.isEmpty()) continue;
            if (a.existed) { QFile f(lp); if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { f.write(a.original); f.close(); } }
            else QFile::remove(lp);
        }
    }
    for (const ai::AppliedWrite &a : applied) reloadIfClean(a.path, false);
    ctx->clearApplied();
    g_session->addNote(QStringLiteral("The user undid ALL file changes made so far; the files are back to their original state."));
    if (errors.isEmpty()) { g_chat->append(TR("geany-ai: all changes undone")); log(TR("geany-ai: all changes undone")); }
    else for (const QString &e : errors) { g_chat->append(TR("geany-ai: undo failed: %1").arg(e)); log(TR("geany-ai: undo failed: %1").arg(e)); }
    g_chat->setHasChanges(false);
}

void onFinished(const ai::ApiResult &res)
{
    if (!g_chat || !g_session) return;
    const QString tokens = TR("Tokens used: %1 in / %2 out").arg(res.inputTokens).arg(res.outputTokens);
    ai::ToolContext *ctx = g_session->context();

    auto idle = [&](const QString &st) {
        g_chat->setStatus(st);
        g_chat->setBusy(false);
        g_chat->setHasChanges(ctx && !ctx->applied().isEmpty());
        saveConversation();
        qtgtk::showWindow(g_chat);
        g_chat->focusInput();
    };

    if (res.cancelled) {
        g_chat->append(TR("Cancelled."));
        status(QStringLiteral("geany-ai: ") + TR("Cancelled."));
        idle(TR("Cancelled.") + QLatin1Char(' ') + TR("Type the next instruction to continue."));
        return;
    }
    if (!res.error.isEmpty()) {
        g_chat->append(QStringLiteral("✖ ") + res.error);
        status(TR("geany-ai error: %1").arg(res.error));
        log(TR("geany-ai error: %1").arg(res.error));
        idle(TR("Failed.") + QLatin1Char(' ') + TR("Type \"continue\" or a new instruction to try again."));
        return;
    }

    g_chat->append(QStringLiteral("✔ ") + res.summary);
    g_chat->append(tokens);
    log(QStringLiteral("geany-ai: ") + res.summary + QStringLiteral("  [") + tokens + QLatin1Char(']'));
    msgwin_switch_tab(MSG_STATUS, TRUE);
    g_conv.lastSummary = res.summary;

    if (res.mode != ai::Mode::Manual) {
        if (res.stoppedByUser) {
            for (const ai::AppliedWrite &a : res.applied) openInEditor(a.path);       // taking over: show everything
            g_chat->append(TR("Stopped at your request. The changed files are open in the editor for manual changes."));
        }
        if (!res.backupDir.isEmpty()) log(TR("geany-ai: backups saved in %1").arg(res.backupDir));
        addDigest(res.summary);
        status(QStringLiteral("geany-ai: ") + TR("Done."));
        idle(res.stoppedByUser ? TR("Stopped. Save your manual edits, then type the next instruction.")
                               : TR("Done. Type the next instruction, or end the session."));
        return;
    }

    // Manual mode: review the staged changes now, then continue the conversation.
    QList<ai::StagedWrite> changes;
    for (ai::StagedWrite w : res.writes) {
        if (w.existed && w.original == w.content) continue;
        GeanyDocument *doc = document_find_by_filename(w.path.toUtf8().constData());
        w.editorDirty = (doc && doc->changed);
        changes << w;
    }
    if (changes.isEmpty()) {
        log(TR("geany-ai: no file changes proposed"));
    } else {
        const QList<ai::StagedWrite> accepted = ai::runReviewDialog(changes, res.summary);
        QStringList acc, rej;
        for (const ai::StagedWrite &w : changes) {
            bool ok = false;
            for (const ai::StagedWrite &x : accepted) if (x.path == w.path) ok = true;
            (ok ? acc : rej) << w.path;
        }
        if (accepted.isEmpty()) {
            log(TR("geany-ai: all changes rejected"));
            g_chat->append(TR("geany-ai: all changes rejected"));
        } else {
            applyWrites(accepted);
            g_conv.turnFiles << acc;
            g_chat->append(TR("geany-ai: applied %1").arg(acc.join(QStringLiteral(", "))));
            if (g_settings.openInEditor) for (const ai::StagedWrite &w : accepted) openInEditor(w.path);
        }
        QString note = QStringLiteral("Review result: ");
        note += acc.isEmpty() ? QStringLiteral("no change was applied") : QStringLiteral("applied: ") + acc.join(QStringLiteral(", "));
        if (!rej.isEmpty()) note += QStringLiteral("; rejected (not written, files unchanged on disk): ") + rej.join(QStringLiteral(", "));
        g_session->addNote(note + QStringLiteral(". Files on disk are now exactly as described; read them again before editing."));
    }
    if (ctx) ctx->clearStaged();
    addDigest(res.summary);
    status(QStringLiteral("geany-ai: ") + TR("Done."));
    idle(TR("Done. Type the next instruction, or end the session."));
}

void onAsk(GtkMenuItem *, gpointer)
{
    if (!startQt()) return;
    refreshLanguage();

    if (g_session) {
        if (g_chat) qtgtk::showWindow(g_chat);
        status(TR("A conversation is open: type the next instruction there (End session starts a new one)."));
        return;
    }
    if (!g_settings.ready()) {
        status(TR("geany-ai: no API key — use Tools → geany-ai Settings"));
        if (ai::runSettingsDialog(&g_settings)) { ai::saveSettings(g_settings); refreshLanguage(); relabelMenus(); }
        if (!g_settings.ready()) return;
    }
    if (g_settings.permittedDirs.isEmpty() && g_settings.sshHosts.isEmpty()) {
        QMessageBox box(QMessageBox::Question, TR("Ask AI Agent"),
                        TR("No permitted directories are configured. Open the settings now?"),
                        QMessageBox::Yes | QMessageBox::No);
        if (qtgtk::execModal(&box) != QMessageBox::Yes) return;
        if (ai::runSettingsDialog(&g_settings)) { ai::saveSettings(g_settings); refreshLanguage(); relabelMenus(); }
        if (g_settings.permittedDirs.isEmpty() && g_settings.sshHosts.isEmpty()) return;
    }

    // Pre-tick the permitted dirs that contain the current document.
    QStringList pre;
    if (GeanyDocument *cur = document_get_current()) {
        if (cur->real_path) {
            const QString p = QFileInfo(QString::fromUtf8(cur->real_path)).canonicalFilePath();
            for (const QString &d : std::as_const(g_settings.permittedDirs)) {
                const QString cd = QFileInfo(d).canonicalFilePath();
                if (!cd.isEmpty() && (p == cd || p.startsWith(cd + QLatin1Char('/')))) pre << d;
            }
        }
    }

    ai::TaskSpec spec;
    if (!ai::runTaskDialog(g_settings, pre, &spec)) return;

    g_settings.mode = ai::modeToString(spec.mode);       // remember the last used mode
    g_settings.newProject = spec.newProject;
    g_settings.projectName = spec.projectName;
    g_settings.runTarget = spec.target;

    // ── where does the agent work, and with which history?
    ai::SessionData saved;
    bool resuming = false, sameKind = false;
    QString remoteExisting;                  // existing project folder on the SSH machine
    ai::ToolContext *ctx = nullptr;
    const ai::RemoteHost *rhost = nullptr;
    g_conv = Conversation();
    g_conv.mode = spec.mode;
    if (!spec.resumeId.isEmpty()) {
        if (!ai::loadSession(spec.resumeId, &saved)) {
            dialogs_show_msgbox(GTK_MESSAGE_ERROR, "%s", TR("The saved session could not be loaded.").toUtf8().constData());
            return;
        }
        QStringList dirs;
        QString rmach, rpath;
        if (!saved.meta.dirs.isEmpty() && ai::parseSshKey(saved.meta.dirs.first(), &rmach, &rpath)) {
            spec.target = rmach; remoteExisting = rpath;      // the session lives on an SSH machine
            dirs = saved.meta.dirs;
        } else {
            spec.target.clear();
            for (const QString &d : std::as_const(saved.meta.dirs)) if (QFileInfo(d).isDir()) dirs << d;
            if (dirs.isEmpty()) {
                dialogs_show_msgbox(GTK_MESSAGE_ERROR, "%s", TR("The folders of this session no longer exist.").toUtf8().constData());
                return;
            }
            spec.activeDirs = dirs;
        }
        resuming = true;
        sameKind = (saved.meta.kind == (g_settings.isOpenAi() ? QStringLiteral("openai") : QStringLiteral("anthropic")));
        g_conv.id = saved.meta.id; g_conv.title = saved.meta.title; g_conv.digest = saved.digest;
        if (!sameKind) {            // different API format: continue as a new conversation with a summary of the old one
            spec.hints += (spec.hints.isEmpty() ? QString() : QStringLiteral(" "))
                        + ai::digestForDirs(dirs);
            resuming = false;
        }
    } else if (!spec.target.isEmpty()) {
        if (!spec.projectDir.isEmpty()) remoteExisting = spec.projectDir;
    } else if (!spec.projectDir.isEmpty()) {
        const QString pd = QFileInfo(spec.projectDir).canonicalFilePath();
        if (pd.isEmpty()) {
            dialogs_show_msgbox(GTK_MESSAGE_ERROR, "%s", TR("The project folder does not exist.").toUtf8().constData());
            return;
        }
        spec.activeDirs = QStringList{pd};
        g_settings.recentProjects.removeAll(pd);
        g_settings.recentProjects.prepend(pd);
        while (g_settings.recentProjects.size() > 10) g_settings.recentProjects.removeLast();
        spec.hints += (spec.hints.isEmpty() ? QString() : QStringLiteral(" "))
                    + QStringLiteral("This is an EXISTING project in %1. Read the relevant files first and keep its style and structure.").arg(pd);
    } else if (spec.newProject) {
        // Fresh sub-folder with a unique suffix inside the first ticked folder; the agent may only touch that one.
        QString perr;
        const QString proj = ai::createProjectDir(spec.activeDirs.first(), spec.projectName, spec.mode != ai::Mode::Manual, &perr);
        if (proj.isEmpty()) {
            status(TR("geany-ai error: %1").arg(perr));
            dialogs_show_msgbox(GTK_MESSAGE_ERROR, "%s", perr.toUtf8().constData());
            return;
        }
        spec.activeDirs = QStringList{proj};
        spec.hints += (spec.hints.isEmpty() ? QString() : QStringLiteral(" ")) +
                      QStringLiteral("This is a NEW project: create all files inside %1 (it is the only folder you may use).").arg(proj);
        log(TR("geany-ai: new project folder %1").arg(proj));
    }
    if (!spec.target.isEmpty()) {
        for (const ai::RemoteHost &h : std::as_const(g_settings.sshHosts)) if (h.name == spec.target) rhost = &h;
        if (!rhost) {
            dialogs_show_msgbox(GTK_MESSAGE_ERROR, "%s", TR("The machine '%1' is not configured (Settings → SSH machines).").arg(spec.target).toUtf8().constData());
            return;
        }
        if (spec.mode == ai::Mode::Manual) { spec.mode = ai::Mode::Ask; g_conv.mode = spec.mode; }
        status(TR("geany-ai: connecting to %1…").arg(spec.target));
        qtgtk::scheduleFlush();
        ctx = new ai::ToolContext({}, spec.exclusions, spec.mode);
        ctx->setRemoteHosts(g_settings.sshHosts);
        QString cerr;
        const bool newRemote = spec.newProject && remoteExisting.isEmpty() && !resuming && spec.resumeId.isEmpty();
        if (!ctx->connectRemote(*rhost, remoteExisting, newRemote, spec.projectName, &cerr)) {
            delete ctx; ctx = nullptr;
            status(TR("geany-ai error: %1").arg(cerr));
            dialogs_show_msgbox(GTK_MESSAGE_ERROR, "%s", cerr.toUtf8().constData());
            return;
        }
        const QString root = ctx->remoteRoot();
        spec.activeDirs = QStringList{root};
        const QString key = ai::sshKey(spec.target, root);
        if (spec.resumeId.isEmpty() && (newRemote || !remoteExisting.isEmpty())) {
            g_settings.recentProjects.removeAll(key);
            g_settings.recentProjects.prepend(key);
            while (g_settings.recentProjects.size() > 10) g_settings.recentProjects.removeLast();
        }
        if (spec.resumeId.isEmpty())
            spec.hints += (spec.hints.isEmpty() ? QString() : QStringLiteral(" ")) +
                          (newRemote ? QStringLiteral("This is a NEW project on the machine: create all files inside %1 (it is the only folder you may use).").arg(root)
                                     : !remoteExisting.isEmpty() ? QStringLiteral("This is an EXISTING project on the machine in %1. Read the relevant files first and keep its style and structure.").arg(root)
                                     : QStringLiteral("Work in %1 on the machine (create a sub-folder per project).").arg(root));
        log(TR("geany-ai: working on %1 in %2").arg(spec.target, root));
        g_target = spec.target;
    }
    ai::saveSettings(g_settings);
    if (!resuming && !spec.newProject && spec.hints.indexOf(QLatin1String("Earlier work in this folder")) < 0) {
        const QString dg = ai::digestForDirs(spec.target.isEmpty() ? spec.activeDirs : QStringList{ai::sshKey(spec.target, spec.activeDirs.first())});
        if (!dg.isEmpty()) spec.hints += (spec.hints.isEmpty() ? QString() : QStringLiteral("\n")) + dg;
    }
    if (g_conv.id.isEmpty()) { g_conv.id = ai::newSessionId(); g_conv.title = spec.instruction.left(80); }
    g_conv.dirs = spec.target.isEmpty() ? spec.activeDirs : QStringList{ai::sshKey(spec.target, spec.activeDirs.first())};
    g_conv.target = spec.target;
    g_target = spec.target;

    if (!ctx) ctx = new ai::ToolContext(spec.activeDirs, spec.exclusions, spec.mode);
    ctx->confirm = [](const ai::ConfirmRequest &r) {
        if (g_chat) qtgtk::showWindow(g_chat);
        return ai::runConfirmDialog(r);
    };
    ctx->onOutput = [](const QString &l) { if (g_chat) g_chat->append(l); };
    ctx->onFileWritten = [](const ai::AppliedWrite &w) {
        logDiff(w);
        if (!g_conv.turnFiles.contains(w.path)) g_conv.turnFiles << w.path;
        QString lp = w.path;
        if (!g_target.isEmpty()) {                                  // lives on the machine: keep a local mirror copy for viewing / editing
            lp = mirrorOf(w.path);
            if (lp.isEmpty()) { if (g_chat) g_chat->setHasChanges(true); return; }
            QDir().mkpath(QFileInfo(lp).absolutePath());
            QFile f(lp);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { f.write(w.content); f.close(); }
        }
        GeanyDocument *doc = document_find_by_filename(lp.toUtf8().constData());
        if (doc) { reloadIfClean(w.path, true); if (g_settings.openInEditor) showTab(doc); }
        else if (g_settings.openInEditor && g_openedThisTurn < 10) { ++g_openedThisTurn; openInEditor(w.path); }
        if (g_chat) g_chat->setHasChanges(true);
    };
    ctx->setRemoteHosts(g_settings.sshHosts);
    ctx->setWebTools(g_settings.webTools);
    ctx->isEditorDirty = [](const QString &p) {
        const QString lp = localPathFor(p);
        if (lp.isEmpty()) return false;
        GeanyDocument *doc = document_find_by_filename(lp.toUtf8().constData());
        return doc && doc->changed;
    };
    g_session = new ai::AiSession(g_settings, ctx, spec.instruction, spec.hints, spec.exclusions);

    if (!g_chat) g_chat = new ai::ChatDialog;
    g_chat->append(QString());
    g_chat->append(QStringLiteral("— ") + (resuming ? TR("Resumed session: %1").arg(g_conv.title) : g_conv.title));
    g_chat->onCancel = [] { if (g_session) g_session->cancel(); };
    g_chat->onSend = [](const QString &t) {
        if (!g_session || !g_session->idle()) return;
        beginTurn(t);
        g_session->sendFollowUp(t);
    };
    g_chat->onReview = [] { onReview(); };
    g_chat->onEnd = [] { endConversation(); };
    g_chat->setRemote(spec.target);
    g_chat->onDownload = [] {
        if (!g_session || !g_session->idle() || !g_session->context() || !g_session->context()->isRemote()) return;
        const QString d = QFileDialog::getExistingDirectory(g_chat, TR("Select a local folder for the project copy"));
        if (d.isEmpty()) return;
        g_chat->append(TR("Downloading the project…"));
        const ai::ToolResult r = g_session->context()->downloadProject(d);
        g_chat->append(r.isError ? QStringLiteral("✖ ") + r.text : QStringLiteral("✔ ") + r.text);
        if (!r.isError) QDesktopServices::openUrl(QUrl::fromLocalFile(d));
    };
    g_chat->setHasChanges(false);

    QObject::connect(g_session, &ai::AiSession::progress, g_chat, [](const QString &l) { g_chat->append(l); });
    QObject::connect(g_session, &ai::AiSession::finished, g_session, [](const ai::ApiResult &r) { onFinished(r); });
    g_session->onStuck = [](const QString &reason, QString *guidance) {
        if (g_chat) qtgtk::showWindow(g_chat);
        return ai::runStuckDialog(reason, guidance);
    };

    beginTurn(spec.instruction);
    msgwin_switch_tab(MSG_STATUS, TRUE);
    qtgtk::showWindow(g_chat);
    ctx->startMcp(g_settings.mcpServers, [](const QString &l) { if (g_chat) g_chat->append(l); });
    if (resuming) g_session->resume(saved.messages, spec.instruction);
    else          g_session->start();
}

/* The user saved a mirrored file in Geany: upload it to the machine. */
void onDocumentSave(GObject *, GeanyDocument *doc, gpointer)
{
    if (!doc || !doc->file_name || g_target.isEmpty() || !g_session || !g_session->context()) return;
    const QString base = mirrorBase();
    const QString local = QString::fromUtf8(doc->file_name);
    if (base.isEmpty() || !local.startsWith(base + QLatin1Char('/'))) return;
    const QString remote = g_session->context()->remoteRoot() + local.mid(base.size());
    QFile f(local);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray data = f.readAll();
    QString err;
    const bool ok = g_session->context()->pushToRemote(remote, data, &err);
    const QString msg = ok ? TR("geany-ai: %1 uploaded to %2").arg(remote, g_target) : TR("geany-ai: upload of %1 failed: %2").arg(remote, err);
    if (g_chat) g_chat->append((ok ? QStringLiteral("↑ ") : QStringLiteral("✖ ")) + msg);
    log(msg);
}

void onSettings(GtkMenuItem *, gpointer)
{
    if (!startQt()) return;
    refreshLanguage();
    if (ai::runSettingsDialog(&g_settings)) {
        ai::saveSettings(g_settings);
        refreshLanguage();
        relabelMenus();
        status(TR("geany-ai: settings saved"));
    }
}

void onKeyAsk(guint) { onAsk(nullptr, nullptr); }

/* ── plugin lifecycle ────────────────────────────────────────────────────── */

gboolean pluginInit(GeanyPlugin *plugin, gpointer)
{
    ai::loadSettings(g_settings);
    refreshLanguage();

    GtkWidget *tools = plugin->geany_data->main_widgets->tools_menu;
    g_itemAsk = gtk_menu_item_new_with_label(TR("Ask AI Agent…").toUtf8().constData());
    gtk_widget_show(g_itemAsk);
    gtk_container_add(GTK_CONTAINER(tools), g_itemAsk);
    g_signal_connect(g_itemAsk, "activate", G_CALLBACK(onAsk), nullptr);

    g_itemSettings = gtk_menu_item_new_with_label(TR("geany-ai Settings…").toUtf8().constData());
    gtk_widget_show(g_itemSettings);
    gtk_container_add(GTK_CONTAINER(tools), g_itemSettings);
    g_signal_connect(g_itemSettings, "activate", G_CALLBACK(onSettings), nullptr);

    GeanyKeyGroup *kg = plugin_set_key_group(plugin, "geany_ai_qt", 1, nullptr);
    keybindings_set_item(kg, 0, [](guint id) { onKeyAsk(id); }, 0, GdkModifierType(0),
                         "ask_claude", "Ask AI Agent", g_itemAsk);

    plugin_signal_connect(plugin, nullptr, "document-save", TRUE, G_CALLBACK(onDocumentSave), nullptr);
    qtgtk::setMainWindow(plugin->geany_data->main_widgets->window);
    return TRUE;
}

void pluginCleanup(GeanyPlugin *, gpointer)
{
    if (g_session) { g_session->disconnect(); g_session->cancel(); delete g_session; g_session = nullptr; }
    if (g_chat) { g_chat->onCancel = nullptr; g_chat->onSend = nullptr; g_chat->onReview = nullptr; g_chat->onEnd = nullptr; g_chat->onDownload = nullptr; delete g_chat; g_chat = nullptr; }
    qtgtk::setMainWindow(nullptr);
    if (g_itemAsk)      { gtk_widget_destroy(g_itemAsk);      g_itemAsk = nullptr; }
    if (g_itemSettings) { gtk_widget_destroy(g_itemSettings); g_itemSettings = nullptr; }
}

} // namespace

extern "C" G_MODULE_EXPORT void geany_load_module(GeanyPlugin *plugin)
{
    geany_plugin = plugin;
    geany_data   = plugin->geany_data;

    plugin->info->name        = "geany-ai (Qt)";
    plugin->info->description = "AI agent (Claude, OpenAI, Groq, Google, DeepSeek, Qwen, Kimi, local models) — Qt6 interface";
    plugin->info->version     = GEANY_AI_VERSION;
    plugin->info->author      = "Based on geany-ai by Jack Sidman Smith";

    plugin->funcs->init    = pluginInit;
    plugin->funcs->cleanup = pluginCleanup;

    // 225 = oldest API this plugin needs (the GTK original demanded 248, which
    // Geany 2.0 as shipped in Debian 13 / Ubuntu 24.04 does not provide).
    GEANY_PLUGIN_REGISTER(plugin, 225);
}
