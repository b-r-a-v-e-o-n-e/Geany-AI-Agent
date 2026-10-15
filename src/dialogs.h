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

/** Add / edit / test MCP servers. Returns true if the user pressed OK. */
bool runMcpDialog(QList<McpServerCfg> *servers);

/** Non-modal live log of a running task. */
class ProgressDialog : public QDialog {
public:
    ProgressDialog();
    void append(const QString &line);
    void setRunning(bool running);
    void setStatus(const QString &text);
    std::function<void()> onCancel;

protected:
    void reject() override;

private:
    QLabel         *m_status;
    QPlainTextEdit *m_log;
    QPushButton    *m_button;
    bool            m_running = true;
};

} // namespace ai
