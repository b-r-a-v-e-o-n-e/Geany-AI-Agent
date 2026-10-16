// SPDX-License-Identifier: MIT
#pragma once
#include "mcp.h"
#include "remote.h"
#include "settings.h"
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

class QProcess;

namespace ai {

QString condenseCompilerOutput(const QString &text);

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
    QString    kind;          // "command" | "write" | "install" | "mcp" | "remote"
    QString    target;        // name of the SSH machine the action runs on (empty = this computer)
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
    /** Detects the build system (CMake, meson, cargo, go, make, npm, python), builds, and returns only what matters:
     *  the first errors with the offending source lines, plus hints. `command` overrides the detection. */
    ToolResult buildProject(const QString &dir, const QString &command, int timeoutSec);

    /** MCP: start the configured servers (blocking), expose their tools, call them. */
    void       startMcp(const QList<McpServerCfg> &servers, const std::function<void(const QString &)> &log);
    QJsonArray mcpToolDefs() const { return m_mcp ? m_mcp->toolDefs() : QJsonArray(); }
    bool       isMcpTool(const QString &name) const { return m_mcp && m_mcp->owns(name); }
    ToolResult callMcp(const QString &name, const QJsonObject &args);

    /** Built-in tools needing nothing installed: web_fetch, web_search, current_time, memory_*. */
    void       setWebTools(bool on) { m_web = on; }
    QJsonArray builtinDefs() const;
    bool       isBuiltinTool(const QString &name) const;
    ToolResult callBuiltin(const QString &name, const QJsonObject &args);

    /** SSH machines (QEMU VMs): builds and tests run there, not on this computer. */
    void       setRemoteHosts(const QList<RemoteHost> &hosts) { m_hosts = hosts; }
    const QList<RemoteHost> &remoteHosts() const { return m_hosts; }
    QJsonArray remoteToolDefs() const;
    QJsonArray extraToolDefs() const;          // MCP + remote tools (agent modes only)
    QString    remoteSummary() const;          // text for the model's first message
    bool       isRemoteTool(const QString &name) const { return name.startsWith(QLatin1String("remote_")); }
    ToolResult remoteHostsInfo();
    ToolResult remoteRun(const QString &host, const QString &command, const QString &cwd, int timeoutSec);
    ToolResult remoteUpload(const QString &host, const QString &localPath, const QString &remotePath, const QStringList &exclude);
    ToolResult remoteDownload(const QString &host, const QString &remotePath, const QString &localDir, const QStringList &exclude);
    ToolResult remoteTest(const RemoteHost &h);  // used by the settings dialog

    /** Remote target: every file / command / package tool then runs ON an SSH machine and the project lives there.
     *  existingPath: an existing project folder on the machine; newProject: create <work folder>/<name>-<suffix>;
     *  otherwise the work folder itself is used. Blocking (connects). */
    bool       connectRemote(const RemoteHost &h, const QString &existingPath, bool newProject, const QString &projectName, QString *error);
    bool       isRemote() const { return m_remote; }
    QString    targetName() const { return m_remote ? m_rhost.name : QString(); }
    QString    remoteRoot() const { return m_rroot; }
    /** Upload a file edited locally (the mirror copy) to the remote project; the path must be inside the project. */
    bool       pushToRemote(const QString &remotePath, const QByteArray &data, QString *error);
    /** Restores originals of applied writes (locally or on the target machine). */
    bool       revert(const QList<AppliedWrite> &applied, QStringList *errors);
    /** Copies the whole remote project into a local folder (user action; not a tool). */
    ToolResult downloadProject(const QString &localDir);

    /** Kill a running command (called from the Cancel button). */
    void abort();
    bool aborted() const { return m_aborted; }
    void resetAbort() { m_aborted = false; if (m_mcp) m_mcp->resetAbort(); }
    void clearStaged() { m_staged.clear(); }
    void clearApplied() { m_applied.clear(); }

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
                           int timeoutSec, const QString &label, const QProcessEnvironment *baseEnv = nullptr,
                           const QByteArray *stdinData = nullptr);
    const RemoteHost *findHost(const QString &name, QString *error) const;
    QString sshPipelineBase(const RemoteHost &h) const;
    void backup(const QString &path, const QByteArray &original);
    // remote target
    bool sshExec(const QString &script, const QByteArray &in, int timeoutSec, QByteArray *out, QByteArray *errOut, int *code, QString *error);
    bool remoteRead(const QString &p, qint64 limit, QByteArray *data, int *state, QString *error);   // state: 0 missing, 1 file, 2 dir, 3 too big
    bool remoteWrite(const QString &p, const QByteArray &data, QString *error);
    ToolResult remoteList(const QString &p);
    ToolResult remoteSearch(const QString &p, const QString &pattern, const QString &glob);
    ToolResult remoteRunCommand(const QString &dir, const QString &command, int timeoutSec);
    ToolResult remoteCheckEnvironment(const QStringList &commands, const QStringList &packages, const QStringList &pkgconfig);
    ToolResult remoteInstall(const QStringList &packages, bool update);

    QStringList         m_dirs;
    QStringList         m_excl;
    Mode                m_mode;
    QString             m_exclRaw;
    bool                m_allowAll = false;
    bool                m_aborted = false;
    QProcess           *m_proc = nullptr;
    std::unique_ptr<McpManager> m_mcp;
    QList<RemoteHost>   m_hosts;
    bool                m_web = true;
    bool                m_remote = false;
    RemoteHost          m_rhost;
    QString             m_rroot, m_ros, m_rpm;
    QStringList         m_redact;          // secrets removed from remote output (log and model)
    QStringList         m_usedHosts;
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
