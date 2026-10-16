// SPDX-License-Identifier: MIT
#include "remote.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <unistd.h>

namespace ai {

QString shellQuote(const QString &s)
{
    QString o = s;
    o.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + o + QLatin1Char('\'');
}

QString remotePathExpr(const QString &pathIn)
{
    const QString p = pathIn.trimmed().isEmpty() ? QStringLiteral("~/geany-ai-work") : pathIn.trimmed();
    if (p == QLatin1String("~")) return QStringLiteral("\"$HOME\"");
    if (p.startsWith(QLatin1String("~/"))) return QStringLiteral("\"$HOME\"/") + shellQuote(p.mid(2));
    return shellQuote(p);
}

QString sshDestination(const RemoteHost &h)
{
    return h.user.trimmed().isEmpty() ? h.host.trimmed() : h.user.trimmed() + QLatin1Char('@') + h.host.trimmed();
}

static QString controlDir()
{
    const QString d = QDir::tempPath() + QStringLiteral("/geany-ai-ssh-%1").arg(::getuid());
    QDir().mkpath(d);
    QFile::setPermissions(d, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return d;
}

static QString knownHostsFile()
{
    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath(QStringLiteral("geany-ai"));
    QDir().mkpath(dir);
    return dir + QStringLiteral("/known_hosts");
}

QStringList sshOptions(const RemoteHost &h)
{
    // One shared connection per host *entry* (the same machine with other credentials must not reuse it).
    const QString tag = QString::fromLatin1(QCryptographicHash::hash((h.name + '|' + h.auth + '|' + h.keyPath + '|' + h.password).toUtf8(),
                                                                     QCryptographicHash::Sha1).toHex().left(8));
    QStringList o;
    o << "-p" << QString::number(h.port > 0 ? h.port : 22)
      << "-o" << "ConnectTimeout=15" << "-o" << "ServerAliveInterval=15" << "-o" << "ServerAliveCountMax=4"
      << "-o" << "LogLevel=ERROR" << "-o" << "ForwardAgent=no" << "-o" << "ForwardX11=no"
      << "-o" << "ControlMaster=auto" << "-o" << "ControlPath=" + controlDir() + "/" + tag + "-%C" << "-o" << "ControlPersist=600";
    if (h.trustChangedKey) {
        o << "-o" << "StrictHostKeyChecking=no" << "-o" << "UserKnownHostsFile=/dev/null";
    } else {
        o << "-o" << "StrictHostKeyChecking=accept-new" << "-o" << "UserKnownHostsFile=" + knownHostsFile();
    }
    if (h.auth == QLatin1String("password")) {
        o << "-o" << "PreferredAuthentications=password,keyboard-interactive" << "-o" << "PubkeyAuthentication=no"
          << "-o" << "NumberOfPasswordPrompts=1";
    } else {
        o << "-o" << "PreferredAuthentications=publickey";
        if (!h.keyPath.trimmed().isEmpty()) o << "-i" << h.keyPath.trimmed() << "-o" << "IdentitiesOnly=yes";
        if (h.password.isEmpty()) o << "-o" << "BatchMode=yes";      // no passphrase known: fail instead of waiting
    }
    return o;
}

static QString askpassScript()
{
    static QTemporaryDir dir;                 // private (0700), removed at exit
    static QString path;
    if (path.isEmpty() && dir.isValid()) {
        path = dir.filePath(QStringLiteral("askpass.sh"));
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write("#!/bin/sh\nprintf '%s\\n' \"$GEANY_AI_SSH_PW\"\n");
            f.close();
            f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        } else path.clear();
    }
    return path;
}

QProcessEnvironment sshEnvironment(const RemoteHost &h)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    if (!h.password.isEmpty()) {
        const QString script = askpassScript();
        if (!script.isEmpty()) {
            env.insert(QStringLiteral("SSH_ASKPASS"), script);
            env.insert(QStringLiteral("SSH_ASKPASS_REQUIRE"), QStringLiteral("force"));
            env.insert(QStringLiteral("GEANY_AI_SSH_PW"), h.password);
            env.insert(QStringLiteral("DISPLAY"), env.value(QStringLiteral("DISPLAY"), QStringLiteral(":0")));
        }
    }
    return env;
}

QString remoteScript(const QString &dir, const QString &cmd)
{
    return QStringLiteral("export DEBIAN_FRONTEND=noninteractive TERM=dumb PAGER=cat; mkdir -p %1 && cd %1 || exit 1\n%2\n")
        .arg(remotePathExpr(dir), cmd);
}

QString effectiveSudoPassword(const RemoteHost &h)
{
    if (!h.sudoPassword.isEmpty()) return h.sudoPassword;
    return h.auth == QLatin1String("password") ? h.password : QString();
}

QString sudoPrelude()
{
    return QStringLiteral("IFS= read -r __gai_pw; __gai_haspw=1\nsudo() { printf '%s\\n' \"$__gai_pw\" | command sudo -S -p '' \"$@\"; }\n");
}

QString remoteCommand(const QString &script) { return QStringLiteral("sh -c ") + shellQuote(script); }

QString sshKey(const QString &machine, const QString &path) { return QStringLiteral("ssh://") + machine + path; }

bool parseSshKey(const QString &key, QString *machine, QString *path)
{
    if (!key.startsWith(QLatin1String("ssh://"))) return false;
    const int slash = key.indexOf(QLatin1Char('/'), 6);
    if (slash < 7) return false;
    if (machine) *machine = key.mid(6, slash - 6);
    if (path) *path = key.mid(slash);
    return true;
}

void sshCloseConnection(const RemoteHost &h)
{
    QProcess p;
    p.setProcessEnvironment(sshEnvironment(h));
    QStringList a = sshOptions(h);
    a << "-O" << "exit" << sshDestination(h);
    p.start(QStringLiteral("ssh"), a);
    if (p.waitForStarted(1000)) { p.closeWriteChannel(); if (!p.waitForFinished(2000)) p.kill(); }
}

} // namespace ai
