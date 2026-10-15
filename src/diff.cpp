// SPDX-License-Identifier: MIT
#include "diff.h"

namespace ai {

static QVector<DiffLine> coarse(const QStringList &A, const QStringList &B)
{
    QVector<DiffLine> out;
    for (const QString &l : A) out.append({DiffLine::Del, l});
    for (const QString &l : B) out.append({DiffLine::Add, l});
    return out;
}

static QVector<DiffLine> myers(const QStringList &A, const QStringList &B)
{
    const int N = A.size(), M = B.size();
    if (N == 0 || M == 0) return coarse(A, B);

    const int MAXD = qMin(N + M, 1500);
    const int off = MAXD + 1;
    QVector<int> v(2 * MAXD + 3, 0);
    QVector<QVector<int>> trace;
    int found = -1;

    for (int d = 0; d <= MAXD && found < 0; ++d) {
        trace.append(v);                       // state before round d
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && v[off + k - 1] < v[off + k + 1])) x = v[off + k + 1];
            else                                                        x = v[off + k - 1] + 1;
            int y = x - k;
            while (x < N && y < M && A[x] == B[y]) { ++x; ++y; }
            v[off + k] = x;
            if (x >= N && y >= M) { found = d; break; }
        }
    }
    if (found < 0) return coarse(A, B);

    QVector<DiffLine> rev;
    int x = N, y = M;
    for (int d = found; d >= 0; --d) {
        const QVector<int> &vv = trace[d];
        const int k = x - y;
        const int prevk = (k == -d || (k != d && vv[off + k - 1] < vv[off + k + 1])) ? k + 1 : k - 1;
        const int prevx = vv[off + prevk];
        const int prevy = prevx - prevk;
        while (x > prevx && y > prevy) { rev.append({DiffLine::Context, A[x - 1]}); --x; --y; }
        if (d > 0) {
            if (x == prevx) { rev.append({DiffLine::Add, B[y - 1]}); --y; }
            else            { rev.append({DiffLine::Del, A[x - 1]}); --x; }
        }
        x = prevx; y = prevy;
    }
    QVector<DiffLine> out;
    out.reserve(rev.size());
    for (int i = rev.size() - 1; i >= 0; --i) out.append(rev[i]);
    return out;
}

QVector<DiffLine> diffScript(const QStringList &a, const QStringList &b)
{
    const int n = a.size(), m = b.size();
    int pre = 0;
    while (pre < n && pre < m && a[pre] == b[pre]) ++pre;
    int suf = 0;
    while (suf < n - pre && suf < m - pre && a[n - 1 - suf] == b[m - 1 - suf]) ++suf;

    QVector<DiffLine> out;
    for (int i = 0; i < pre; ++i) out.append({DiffLine::Context, a[i]});
    out += myers(a.mid(pre, n - pre - suf), b.mid(pre, m - pre - suf));
    for (int i = n - suf; i < n; ++i) out.append({DiffLine::Context, a[i]});
    return out;
}

static QStringList splitLines(const QString &t)
{
    if (t.isEmpty()) return {};
    QStringList l = t.split(QLatin1Char('\n'));
    if (l.last().isEmpty()) l.removeLast();       // trailing newline is not an extra line
    return l;
}

QVector<DiffLine> unifiedDiff(const QString &oldText, const QString &newText, int context)
{
    const QVector<DiffLine> s = diffScript(splitLines(oldText), splitLines(newText));
    const int n = s.size();

    QVector<int> oldBefore(n + 1, 0), newBefore(n + 1, 0);
    QVector<bool> keep(n, false);
    bool anyChange = false;
    for (int i = 0; i < n; ++i) {
        oldBefore[i + 1] = oldBefore[i] + (s[i].kind != DiffLine::Add ? 1 : 0);
        newBefore[i + 1] = newBefore[i] + (s[i].kind != DiffLine::Del ? 1 : 0);
        if (s[i].kind != DiffLine::Context) {
            anyChange = true;
            for (int j = qMax(0, i - context); j <= qMin(n - 1, i + context); ++j) keep[j] = true;
        }
    }
    if (!anyChange) return {};

    QVector<DiffLine> out;
    int i = 0;
    while (i < n) {
        if (!keep[i]) { ++i; continue; }
        int j = i;
        while (j < n && keep[j]) ++j;
        const int oldLen = oldBefore[j] - oldBefore[i];
        const int newLen = newBefore[j] - newBefore[i];
        out.append({DiffLine::Header, QStringLiteral("@@ -%1,%2 +%3,%4 @@")
                        .arg(oldBefore[i] + (oldLen ? 1 : 0)).arg(oldLen)
                        .arg(newBefore[i] + (newLen ? 1 : 0)).arg(newLen)});
        for (int k = i; k < j; ++k) out.append(s[k]);
        i = j;
    }
    return out;
}

} // namespace ai
