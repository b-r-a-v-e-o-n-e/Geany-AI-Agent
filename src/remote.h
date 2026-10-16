// SPDX-License-Identifier: MIT
// SSH helpers: option building, askpass (saved passwords) and quoting. The actual execution lives in ToolContext.
#pragma once
#include "settings.h"
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace ai {

/** POSIX single-quote quoting. */
QString shellQuote(const QString &s);

/** Shell expression for a remote path: "~/x y" -> "$HOME"/'x y'; absolute paths are quoted. */
QString remotePathExpr(const QString &path);

/** user@host */
QString sshDestination(const RemoteHost &h);

/** Everything that goes before the destination: port, keys, timeouts, host-key policy, connection sharing. */
QStringList sshOptions(const RemoteHost &h);

/** Environment for ssh. With a saved password/passphrase, SSH_ASKPASS points to a private helper script that
 *  prints it from an environment variable (the password never appears on a command line). */
QProcessEnvironment sshEnvironment(const RemoteHost &h);

/** Script run on the remote side: sets a quiet non-interactive environment, enters `dir` (created if missing) and runs `cmd`. */
QString remoteScript(const QString &dir, const QString &cmd);

/** The full remote command string passed to ssh. */
QString remoteCommand(const QString &script);

/** Password for sudo on the machine: the explicit one, else the login password when logging in with a password. */
QString effectiveSudoPassword(const RemoteHost &h);

/** Script prefix that defines a `sudo` function feeding the password read from the first stdin line (never on a command line). */
QString sudoPrelude();

/** Keys for remote folders in sessions / recent projects: "ssh://<machine name>/abs/path". */
QString sshKey(const QString &machine, const QString &path);
bool    parseSshKey(const QString &key, QString *machine, QString *path);

/** Closes the shared connection of a host (ssh -O exit), ignoring errors. */
void sshCloseConnection(const RemoteHost &h);

} // namespace ai
