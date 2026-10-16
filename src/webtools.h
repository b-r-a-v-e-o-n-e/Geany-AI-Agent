// SPDX-License-Identifier: MIT
// Built-in tools that need nothing installed (no Node.js, no uv): web fetch / search, clock, notes memory.
#pragma once
#include "tools.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace ai {

struct SearchHit { QString title, url, snippet; };

/** Tool definitions (Anthropic schema) of the built-in tools. */
QJsonArray builtinToolDefs();
bool       isBuiltinTool(const QString &name);

/** Pure helpers (unit tested). */
QString           htmlToText(const QString &html);
QList<SearchHit>  parseDuckDuckGo(const QString &html);
bool              webUrlAllowed(const QString &url, QString *why);     // http(s) only, no local / private addresses

/** Runs one built-in tool. `aborted` is polled during network waits. Does no confirmation (the caller asks). */
ToolResult runBuiltinTool(const QString &name, const QJsonObject &input, const std::function<bool()> &aborted);

/** Path of the notes file used by memory_* (overridable with GEANY_AI_MEMORY_FILE for tests). */
QString memoryFile();

} // namespace ai
