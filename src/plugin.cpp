// SPDX-License-Identifier: MIT
// geany-ai-qt — Claude assistant plugin for Geany, Qt6 user interface.
// Original GTK plugin: Copyright (c) 2026 Jack Sidman Smith (MIT).

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QStandardPaths>

#include "api.h"
#include "dialogs.h"
#include "diff.h"
#include "i18n.h"
#include "qtgtk.h"
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
ai::ProgressDialog *g_progress  = nullptr;

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

void reloadIfClean(const QString &path, bool warnDirty)
{
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
void openInEditor(const QString &path)
{
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
    if (!g_progress) return;
    const QVector<ai::DiffLine> lines = ai::unifiedDiff(QString::fromUtf8(w.original), QString::fromUtf8(w.content), 2);
    int shown = 0;
    for (const ai::DiffLine &l : lines) {
        if (shown++ >= 60) { g_progress->append(QStringLiteral("   … (diff truncated, open the file to see everything)")); break; }
        const QString p = l.kind == ai::DiffLine::Add ? QStringLiteral("+ ") : l.kind == ai::DiffLine::Del ? QStringLiteral("- ")
                        : l.kind == ai::DiffLine::Context ? QStringLiteral("  ") : QString();
        g_progress->append(QStringLiteral("   ") + p + l.text);
    }
}

int g_openedThisTask = 0;

/* Ask / Auto: files are already on disk; show the result and offer "Undo all changes". */
void finishAgent(const ai::ApiResult &res, const QString &note)
{
    const QString summary = res.summary.isEmpty() ? (res.error.isEmpty() ? QString() : res.error) : res.summary;
    if (!res.applied.isEmpty()) log(TR("geany-ai: %1 file(s) changed").arg(res.applied.size()));
    if (!res.backupDir.isEmpty()) log(TR("geany-ai: backups saved in %1").arg(res.backupDir));
    if (res.stoppedByUser && !res.applied.isEmpty()) {
        for (const ai::AppliedWrite &a : res.applied) openInEditor(a.path);       // taking over: show everything
    }
    const bool undo = ai::runFinishDialog(summary, res.applied, res.backupDir, note, [](const QString &p) { openInEditor(p); });
    if (undo) {
        QStringList errors;
        ai::revertApplied(res.applied, &errors);
        for (const ai::AppliedWrite &a : res.applied) reloadIfClean(a.path, false);
        if (errors.isEmpty()) log(TR("geany-ai: all changes undone"));
        else for (const QString &e : errors) log(TR("geany-ai: undo failed: %1").arg(e));
    }
}

/* ── task lifecycle ──────────────────────────────────────────────────────── */

void endSession()
{
    if (g_session) { g_session->deleteLater(); g_session = nullptr; }
    qtgtk::scheduleFlush();
}

void onFinished(const ai::ApiResult &res)
{
    const QString tokens = TR("Tokens used: %1 in / %2 out").arg(res.inputTokens).arg(res.outputTokens);

    if (res.cancelled) {
        g_progress->append(TR("Cancelled."));
        g_progress->setStatus(TR("Cancelled."));
        g_progress->setRunning(false);
        status(QStringLiteral("geany-ai: ") + TR("Cancelled."));
        if (!res.applied.isEmpty()) finishAgent(res, TR("Cancelled."));
        endSession();
        return;
    }
    if (!res.error.isEmpty()) {
        g_progress->append(res.error);
        g_progress->setStatus(TR("Failed.") + QLatin1Char(' ') + res.error);
        g_progress->setRunning(false);
        status(TR("geany-ai error: %1").arg(res.error));
        log(TR("geany-ai error: %1").arg(res.error));
        if (!res.applied.isEmpty()) finishAgent(res, TR("Failed.") + QLatin1Char(' ') + res.error);
        endSession();
        return;
    }

    g_progress->append(tokens);
    g_progress->setStatus(TR("Done."));
    g_progress->setRunning(false);
    g_progress->hide();
    log(QStringLiteral("geany-ai: ") + res.summary + QStringLiteral("  [") + tokens + QLatin1Char(']'));
    msgwin_switch_tab(MSG_STATUS, TRUE);

    if (res.mode != ai::Mode::Manual) {
        finishAgent(res, res.stoppedByUser ? TR("Stopped at your request. The changed files are open in the editor for manual changes.") : QString());
        status(QStringLiteral("geany-ai: ") + TR("Done."));
        endSession();
        return;
    }

    // Drop no-op writes, flag files with unsaved editor changes.
    QList<ai::StagedWrite> changes;
    for (ai::StagedWrite w : res.writes) {
        if (w.existed && w.original == w.content) continue;
        GeanyDocument *doc = document_find_by_filename(w.path.toUtf8().constData());
        w.editorDirty = (doc && doc->changed);
        changes << w;
    }

    if (changes.isEmpty()) {
        log(TR("geany-ai: no file changes proposed"));
        // Pure answer (e.g. "explain this file"): show it.
        QMessageBox box(QMessageBox::NoIcon, TR("Ask AI Agent"), res.summary, QMessageBox::Close);
        box.setTextInteractionFlags(Qt::TextSelectableByMouse);
        qtgtk::execModal(&box);
    } else {
        const QList<ai::StagedWrite> accepted = ai::runReviewDialog(changes, res.summary);
        if (accepted.isEmpty()) log(TR("geany-ai: all changes rejected"));
        else {
            applyWrites(accepted);
            if (g_settings.openInEditor) for (const ai::StagedWrite &w : accepted) openInEditor(w.path);
        }
    }
    status(QStringLiteral("geany-ai: ") + TR("Done."));
    endSession();
}

void onAsk(GtkMenuItem *, gpointer)
{
    if (!startQt()) return;
    refreshLanguage();

    if (g_session) {
        if (g_progress) qtgtk::showWindow(g_progress);
        status(TR("A request is already running."));
        return;
    }
    if (!g_settings.ready()) {
        status(TR("geany-ai: no API key — use Tools → geany-ai Settings"));
        if (ai::runSettingsDialog(&g_settings)) { ai::saveSettings(g_settings); refreshLanguage(); relabelMenus(); }
        if (!g_settings.ready()) return;
    }
    if (g_settings.permittedDirs.isEmpty()) {
        QMessageBox box(QMessageBox::Question, TR("Ask AI Agent"),
                        TR("No permitted directories are configured. Open the settings now?"),
                        QMessageBox::Yes | QMessageBox::No);
        if (qtgtk::execModal(&box) != QMessageBox::Yes) return;
        if (ai::runSettingsDialog(&g_settings)) { ai::saveSettings(g_settings); refreshLanguage(); relabelMenus(); }
        if (g_settings.permittedDirs.isEmpty()) return;
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
    ai::saveSettings(g_settings);

    if (spec.newProject) {
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
    g_openedThisTask = 0;

    auto *ctx = new ai::ToolContext(spec.activeDirs, spec.exclusions, spec.mode);
    ctx->confirm = [](const ai::ConfirmRequest &r) {
        if (g_progress) qtgtk::showWindow(g_progress);
        return ai::runConfirmDialog(r);
    };
    ctx->onOutput = [](const QString &l) { if (g_progress) g_progress->append(l); };
    ctx->onFileWritten = [](const ai::AppliedWrite &w) {
        logDiff(w);
        GeanyDocument *doc = document_find_by_filename(w.path.toUtf8().constData());
        if (doc) { reloadIfClean(w.path, true); if (g_settings.openInEditor) showTab(doc); }
        else if (g_settings.openInEditor && g_openedThisTask < 10) { ++g_openedThisTask; openInEditor(w.path); }
    };
    ctx->isEditorDirty = [](const QString &p) {
        GeanyDocument *doc = document_find_by_filename(p.toUtf8().constData());
        return doc && doc->changed;
    };
    g_session = new ai::AiSession(g_settings, ctx, spec.instruction, spec.hints, spec.exclusions);

    if (!g_progress) g_progress = new ai::ProgressDialog;
    g_progress->setStatus(TR("The agent is working…"));
    g_progress->setRunning(true);
    g_progress->append(QStringLiteral("— ") + spec.instruction.split(QLatin1Char('\n')).first());
    g_progress->onCancel = [] { if (g_session) g_session->cancel(); };

    QObject::connect(g_session, &ai::AiSession::progress, g_progress, [](const QString &l) { g_progress->append(l); });
    QObject::connect(g_session, &ai::AiSession::finished, g_session, [](const ai::ApiResult &r) { onFinished(r); });

    g_session->onStuck = [](const QString &reason, QString *guidance) {
        if (g_progress) qtgtk::showWindow(g_progress);
        return ai::runStuckDialog(reason, guidance);
    };

    status(TR("geany-ai: running…"));
    msgwin_switch_tab(MSG_STATUS, TRUE);
    qtgtk::showWindow(g_progress);
    ctx->startMcp(g_settings.mcpServers, [](const QString &l) { if (g_progress) g_progress->append(l); });
    g_session->start();
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

    qtgtk::setMainWindow(plugin->geany_data->main_widgets->window);
    return TRUE;
}

void pluginCleanup(GeanyPlugin *, gpointer)
{
    if (g_session) { g_session->disconnect(); g_session->cancel(); delete g_session; g_session = nullptr; }
    if (g_progress) { g_progress->onCancel = nullptr; delete g_progress; g_progress = nullptr; }
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
