// SPDX-License-Identifier: MIT
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

namespace ai {

struct DiffLine {
    enum Kind { Context, Add, Del, Header };
    Kind    kind;
    QString text;
};

/** Full edit script (Myers O(ND)); falls back to "delete all, add all" for huge rewrites. */
QVector<DiffLine> diffScript(const QStringList &a, const QStringList &b);

/** Unified-diff style hunks with `context` lines around each change.
 *  Empty result means the texts are identical. */
QVector<DiffLine> unifiedDiff(const QString &oldText, const QString &newText, int context = 3);

} // namespace ai
