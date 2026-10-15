// SPDX-License-Identifier: MIT
#pragma once
#include "mcp.h"
#include "settings.h"
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

class QProcess;

namespace ai {

/** A file write held back for review (Manual mode). */
struct StagedWrite {
    QString    path;          // canonical absolute path
    QByteArray original;      // previous contents (empty if !existed)
    bool       existed = false;
    QByteArray content;       // proposed contents
    bool       editorDirty = false;   // filled in by the plugin: unsaved edits in Geany
};

/** A write that has already been applied to disk (Ask / Auto modes). */
struct AppliedWrite {
    QString    path;
    QByteArray original;
    bool       existed = false;
    QByteArray content;
};

struct ToolResult {
    QString text;
    bool    isError = false;
    QString note;             // short human-readable line for the progress log
};

enum class Decision { Deny, Allow, AllowAll };

struct ConfirmRequest {
    QString    kind;          // "command" | "write" | "install" | "mcp"
    QString    title;
    QString    detail;
    QString    path;
    QByteArray before, after; // for "write"
    bool       existed = false;
    bool       editorDirty = false;
};

/**
 * Sandbox + executor for the agent's tools.
 *  - Files: every path is canonicalised (symlinks resolved), must be inside a permitted directory
 *    and outside every exclusion.
 *  - Manual mode: writes are staged for review, commands are disabled.
 *  - Ask mode: every write / command / install asks for confirmation first.
 *  - Auto mode: everything runs without asking (backups are still made).
 */
class ToolContext {
public:
    ToolContext(const QStringList &dirs, const QString &exclusions, Mode mode = Mode::Manual);
    ~ToolContext();

    Mode mode() const { return m_mode; }
    bool canRun() const { return m_mode != Mode::Manual; }

    // Hooks, set by the plugin (all optional)
    std::function<Decision(const ConfirmRequest &)> confirm;       // Ask mode
    std::function<void(const QString &)>            onOutput;      // live process output, one line at a time
    std::function<void(const AppliedWrite &)>       onFileWritten; // after a direct write
    std::function<bool(const QString &)>            isEditorDirty; // unsaved buffer open in the editor?

    ToolResult readFile(const QString &path);
    ToolResult writeFile(const QString &path, const QString &content);
    ToolResult editFile(const QString &path, const QString &oldStr, const QString &newStr, bool replaceAll);
    ToolResult listDir(const QString &path);
    ToolResult searchFiles(const QString &path, const QString &pattern, const QString &glob);
    ToolResult runCommand(const QString &command, const QString &cwd, int timeoutSec);
    ToolResult checkEnvironment(const QStringList &commands, const QStringList &packages, const QStringList &pkgconfig);
    ToolResult installPackages(const QStringList &packages, bool update);

    /** MCP: start the configured servers (blocking), expose their tools, call them. */
    void       startMcp(const QList<McpServerCfg> &servers, const std::function<void(const QString &)> &log);
    QJsonArray mcpToolDefs() const { return m_mcp ? m_mcp->toolDefs() : QJsonArray(); }
    bool       isMcpTool(const QString &name) const { return m_mcp && m_mcp->owns(name); }
    ToolResult callMcp(const QString &name, const QJsonObject &args);

    /** Kill a running command (called from the Cancel button). */
    void abort();
    bool aborted() const { return m_aborted; }

    /** Resolves symlinks; the file need not exist. Returns false with a reason if not allowed. */
    bool resolve(const QString &path, QString *resolved, QString *why) const;

    const QStringList &permittedDirs() const { return m_dirs; }
    const QStringList &exclusions() const { return m_excl; }
    QList<StagedWrite> &staged() { return m_staged; }
    QList<AppliedWrite> &applied() { return m_applied; }
    QString backupDir() const { return m_backupDir; }
    QString osSummary() const;

    static bool commandBlocked(const QString &cmd, QString *why);

    static constexpr qint64 kMaxReadBytes = 2 * 1024 * 1024;

private:
    static bool isUnder(const QString &path, const QString &dir);
    static QString canonicalize(const QString &path);
    bool readText(const QString &resolved, QString *text, QString *error, bool preferStaged = true);
    ToolResult commit(const QString &resolved, QByteArray bytes);
    bool needConfirm() const { return m_mode == Mode::Ask && !m_allowAll; }
    bool ask(const ConfirmRequest &req);   // true = go ahead
    ToolResult execProcess(const QString &program, const QStringList &args, const QString &cwd,
                           int timeoutSec, const QString &label);
    void backup(const QString &path, const QByteArray &original);

    QStringList         m_dirs;
    QStringList         m_excl;
    Mode                m_mode;
    bool                m_allowAll = false;
    bool                m_aborted = false;
    QProcess           *m_proc = nullptr;
    std::unique_ptr<McpManager> m_mcp;
    QString             m_backupDir;
    QList<StagedWrite>  m_staged;
    QList<AppliedWrite> m_applied;
};

/** Picks a fresh, unique sub-folder name "<slug>-<random suffix>" inside `base` and (optionally) creates it.
 *  Returns the absolute path, or an empty string with *error set. */
QString createProjectDir(const QString &base, const QString &name, bool create, QString *error);

/** Restores the originals of applied writes (deletes files that did not exist before). */
bool revertApplied(const QList<AppliedWrite> &applied, QStringList *errors);

} // namespace ai
