// SPDX-License-Identifier: MIT
// Saved agent sessions: full message history (to resume exactly) + a short digest (to give context later).
#pragma once
#include <QDateTime>
#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace ai {

struct SessionMeta {
    QString     id;
    QString     title;      // first instruction
    QString     kind;       // "anthropic" | "openai": the history format
    QString     mode;
    QStringList dirs;       // folders the agent may use
    QDateTime   time;       // last activity
};

struct SessionData {
    SessionMeta meta;
    QJsonArray  messages;   // exact API history
    QJsonArray  digest;     // [{instruction, summary, files[], time}]
};

QString sessionsDir();
QString newSessionId();
bool    saveSession(const SessionData &s);                 // 0600, keeps the 30 newest
bool    loadSession(const QString &id, SessionData *out);
bool    deleteSession(const QString &id);
QList<SessionMeta> listSessions();                         // newest first

/** Short text about earlier work in these folders (for a fresh conversation); empty if none. */
QString digestForDirs(const QStringList &dirs, int maxChars = 1800);

} // namespace ai
