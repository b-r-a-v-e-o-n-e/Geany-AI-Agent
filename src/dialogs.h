// SPDX-License-Identifier: MIT
#pragma once
#include "settings.h"
#include "api.h"
#include "tools.h"
#include <QDialog>
#include <QList>
#include <functional>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace ai {

struct TaskSpec {
    QString     instruction;
    QString     hints;
    QString     exclusions;
    QStringList activeDirs;
    Mode        mode = Mode::Ask;
    bool        newProject = false;     // create <dir>/<name>-<suffix> for this task
    QString     projectName;
    QString     projectDir;             // existing project folder to continue in (empty = none)
    QString     resumeId;               // saved session to continue (empty = none)
    QString     target;                 // SSH machine to work on (empty = this computer)
};

/** "Ask AI Agent" dialog. Returns false if cancelled. */
bool runTaskDialog(const Settings &s, const QStringList &preChecked, TaskSpec *out);

/** Settings dialog. Returns true (and updates *s) if the user pressed Save. */
bool runSettingsDialog(Settings *s);

/** Shows all staged writes with diffs; returns the subset the user approved. */
QList<StagedWrite> runReviewDialog(const QList<StagedWrite> &writes, const QString &summary);

/** Asks whether the agent may run a command / write a file / install packages. Esc = Deny. */
Decision runConfirmDialog(const ConfirmRequest &req);

/** Shown after an Ask / Auto run: lists the changed files. Returns true if the user wants everything undone. */
bool runFinishDialog(const QString &summary, const QList<AppliedWrite> &applied, const QString &backupDir,
                     const QString &note, const std::function<void(const QString &)> &openInEditor);

/** The agent seems to loop: let the user choose how to go on. */
StuckAction runStuckDialog(const QString &reason, QString *guidance);

/** Add / edit / test SSH machines (QEMU VMs). Returns true if the user pressed OK. */
bool runSshDialog(QList<RemoteHost> *hosts);

/** Add / edit / test MCP servers. Returns true if the user pressed OK. */
bool runMcpDialog(QList<McpServerCfg> *servers, const QString &defaultPath = QString());

/** Non-modal conversation window: live log + input for the next instruction. */
class ChatDialog : public QDialog {
public:
    ChatDialog();
    void append(const QString &line);
    void setStatus(const QString &text);
    void setBusy(bool busy);                 // busy: generation running (input disabled, Cancel enabled)
    void setHasChanges(bool has);
    void setRemote(const QString &machine);   // shows the "Download project…" button and the machine in the title            // enables "Review / undo changes…"
    void focusInput();
    std::function<void()>                onCancel;
    std::function<void(const QString &)> onSend;
    std::function<void()>                onReview;
    std::function<void()>                onEnd;
    std::function<void()>                onDownload;

protected:
    void reject() override;

private:
    void send();
    QLabel         *m_status;
    QPlainTextEdit *m_log;
    QPlainTextEdit *m_input;
    QPushButton    *m_send, *m_cancel, *m_review, *m_end, *m_download;
    bool            m_busy = true;
};

} // namespace ai
