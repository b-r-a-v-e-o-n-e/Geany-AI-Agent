// SPDX-License-Identifier: MIT
#include "qtgtk.h"
#include "i18n.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QLibraryInfo>
#include <QNetworkProxyFactory>
#include <QFileInfo>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPolygonF>
#include <QStandardPaths>
#include <QPluginLoader>
#include <QStyle>
#include <QStyleFactory>
#include <QWindow>

// Keep GTK/X11 headers after all Qt headers (X11 defines macros such as None/Bool).
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

namespace qtgtk {

static GtkWidget *g_mainWindow = nullptr;

void setMainWindow(_GtkWidget *w) { g_mainWindow = w; }

/* ── QApplication bootstrap ──────────────────────────────────────────────── */

static void syncTheme();

static bool platformLoadable(const QString &name, QString *errors)
{
    const QString dir = QLibraryInfo::path(QLibraryInfo::PluginsPath) + QStringLiteral("/platforms/");
    QStringList files;
    if (name == QLatin1String("xcb"))          files << "libqxcb.so";
    else if (name == QLatin1String("wayland")) files << "libqwayland-generic.so" << "libqwayland-egl.so";
    for (const QString &f : std::as_const(files)) {
        QPluginLoader pl(dir + f);
        if (pl.load()) return true;
        if (errors) *errors += name + QStringLiteral(": ") + pl.errorString() + QLatin1Char('\n');
    }
    return false;
}

bool ensureApp(QString *error)
{
    if (QCoreApplication::instance()) return true;

    // A failing Qt platform plugin would abort() the whole editor, so probe first.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        GdkDisplay *disp = gdk_display_get_default();
        const bool wayland = disp && QByteArray(G_OBJECT_TYPE_NAME(disp)).contains("Wayland");
        const QStringList order = wayland ? QStringList{"wayland", "xcb"} : QStringList{"xcb", "wayland"};
        QString errors, chosen;
        for (const QString &p : order) {
            if (platformLoadable(p, &errors)) { chosen = p; break; }
        }
        if (chosen.isEmpty()) {
            if (error) *error = TR("Qt could not start a graphical backend inside Geany.\n\n%1\n\nInstall the missing packages, e.g.:\n  sudo apt install qt6-wayland libxcb-cursor0").arg(errors.trimmed());
            return false;
        }
        qputenv("QT_QPA_PLATFORM", chosen.toUtf8());
    }

    static int   argc = 1;
    static char  arg0[] = "geany";
    static char *argv[] = {arg0, nullptr};
    auto *app = new QApplication(argc, argv);     // intentionally never deleted
    app->setQuitOnLastWindowClosed(false);
    QCoreApplication::setApplicationName(QStringLiteral("geany-ai"));
    QNetworkProxyFactory::setUseSystemConfiguration(true);
    syncTheme();
    return true;
}

/* ── theme: make Qt widgets look like Geany's GTK theme ─────────────────── */

static QColor mix(const QColor &a, const QColor &b, qreal t)   // t=0 -> a, t=1 -> b
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

static bool gtkColor(GtkStyleContext *sc, const char *name, QColor *out)
{
    GdkRGBA c;
    if (!gtk_style_context_lookup_color(sc, name, &c)) return false;
    *out = QColor::fromRgbF(c.red, c.green, c.blue);
    return true;
}

/** Builds a Qt palette from the GTK theme (Adwaita-style named colours, with fallbacks).
 *  GEANY_AI_QT_THEME=dark|light forces a built-in palette instead. */
static void syncTheme()
{
    QColor bg, fg, base, text, selBg, selFg;
    const QByteArray forced = qgetenv("GEANY_AI_QT_THEME").toLower();
    if (forced == "dark") {
        bg = QColor(0x35, 0x35, 0x35); fg = QColor(0xee, 0xee, 0xec);
        base = QColor(0x24, 0x24, 0x24); text = QColor(0xff, 0xff, 0xff);
        selBg = QColor(0x35, 0x84, 0xe4); selFg = Qt::white;
    } else if (forced == "light") {
        bg = QColor(0xf6, 0xf5, 0xf4); fg = QColor(0x2e, 0x34, 0x36);
        base = Qt::white; text = QColor(0x2e, 0x34, 0x36);
        selBg = QColor(0x35, 0x84, 0xe4); selFg = Qt::white;
    } else {
        if (!g_mainWindow) return;
        GtkStyleContext *sc = gtk_widget_get_style_context(g_mainWindow);
        GdkRGBA f, b;
        gtk_style_context_get_color(sc, GTK_STATE_FLAG_NORMAL, &f);
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gtk_style_context_get_background_color(sc, GTK_STATE_FLAG_NORMAL, &b);
        G_GNUC_END_IGNORE_DEPRECATIONS
        if (!gtkColor(sc, "theme_fg_color", &fg))  fg = QColor::fromRgbF(f.red, f.green, f.blue);
        if (!gtkColor(sc, "theme_bg_color", &bg))  bg = (b.alpha > 0.5) ? QColor::fromRgbF(b.red, b.green, b.blue)
                                                                        : (fg.lightness() > 128 ? QColor(0x30, 0x30, 0x30) : QColor(0xf0, 0xf0, 0xf0));
        const bool dark = bg.lightness() < 128;
        if (!gtkColor(sc, "theme_base_color", &base)) base = dark ? bg.darker(130) : QColor(Qt::white);
        if (!gtkColor(sc, "theme_text_color", &text)) text = fg;
        if (!gtkColor(sc, "theme_selected_bg_color", &selBg)) selBg = QColor(0x35, 0x84, 0xe4);
        if (!gtkColor(sc, "theme_selected_fg_color", &selFg)) selFg = Qt::white;
    }
    // Safety net: never end up with unreadable text whatever the theme says.
    if (qAbs(fg.lightness() - bg.lightness()) < 90)     fg   = bg.lightness() < 128 ? QColor(0xee, 0xee, 0xec) : QColor(0x2e, 0x34, 0x36);
    if (qAbs(text.lightness() - base.lightness()) < 90) text = base.lightness() < 128 ? QColor(0xff, 0xff, 0xff) : QColor(0x20, 0x20, 0x20);

    QPalette p;
    p.setColor(QPalette::Window, bg);
    p.setColor(QPalette::WindowText, fg);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, mix(base, bg, 0.5));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::PlaceholderText, mix(text, base, 0.45));
    p.setColor(QPalette::Button, mix(bg, fg, 0.06));
    p.setColor(QPalette::ButtonText, fg);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Highlight, selBg);
    p.setColor(QPalette::HighlightedText, selFg);
    p.setColor(QPalette::Link, selBg.lightness() < 128 ? selBg.lighter(160) : selBg);
    p.setColor(QPalette::Light, mix(bg, Qt::white, 0.25));
    p.setColor(QPalette::Midlight, mix(bg, Qt::white, 0.12));
    p.setColor(QPalette::Mid, mix(bg, fg, 0.25));
    p.setColor(QPalette::Dark, mix(bg, Qt::black, 0.35));
    p.setColor(QPalette::Shadow, Qt::black);
    const QColor dis = mix(fg, bg, 0.45), disText = mix(text, base, 0.45);
    p.setColor(QPalette::Disabled, QPalette::WindowText, dis);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, dis);
    p.setColor(QPalette::Disabled, QPalette::Text, disText);
    p.setColor(QPalette::Disabled, QPalette::Highlight, mix(selBg, bg, 0.5));

    // Fusion draws check boxes with an outline that vanishes on dark themes: style them explicitly.
    {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        const QString checkPng = dir + QStringLiteral("/geany-ai-qt-check-%1.png").arg(selBg.name().mid(1));
        if (!QFileInfo::exists(checkPng)) {
            QPixmap pm(28, 28);
            pm.fill(Qt::transparent);
            QPainter pt(&pm);
            pt.setRenderHint(QPainter::Antialiasing);
            pt.setPen(Qt::NoPen);
            pt.setBrush(selBg);
            pt.drawRoundedRect(QRectF(1, 1, 26, 26), 5, 5);
            QPen pen(selFg, 3.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            pt.setPen(pen);
            pt.drawPolyline(QPolygonF({QPointF(7, 14.5), QPointF(12, 19.5), QPointF(21, 8.5)}));
            pt.end();
            pm.save(checkPng, "PNG");
        }
        const QString border = mix(bg, fg, 0.55).name();
        const QString css = QStringLiteral(
            "QCheckBox::indicator, QListView::indicator { width: 14px; height: 14px; border: 1px solid %1; border-radius: 4px; background: %2; }"
            "QCheckBox::indicator:checked, QListView::indicator:checked { border: 1px solid %3; background: transparent; image: url(%4); }"
            "QCheckBox::indicator:disabled { border-color: %5; }")
            .arg(border, base.name(), selBg.name(), checkPng, mix(bg, fg, 0.25).name());
        if (qApp->styleSheet() != css) qApp->setStyleSheet(css);
    }

    if (QStyleFactory::keys().contains(QStringLiteral("Fusion"), Qt::CaseInsensitive)
        && QApplication::style()->objectName().compare(QLatin1String("fusion"), Qt::CaseInsensitive) != 0)
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    if (QApplication::palette() != p) QApplication::setPalette(p);
}

/* ── window integration ──────────────────────────────────────────────────── */

static void makeTransient(QWidget *w)
{
#ifdef GDK_WINDOWING_X11
    if (!g_mainWindow || QGuiApplication::platformName() != QLatin1String("xcb")) return;
    GdkWindow *gw = gtk_widget_get_window(g_mainWindow);
    if (!gw || !GDK_IS_X11_WINDOW(gw)) return;
    static QWindow *foreign = nullptr;
    static WId      foreignId = 0;
    const WId id = (WId)GDK_WINDOW_XID(gw);
    if (!foreign || foreignId != id) {
        foreign = QWindow::fromWinId(id);
        foreignId = id;
    }
    w->winId();                                   // force native window creation
    if (foreign && w->windowHandle())
        w->windowHandle()->setTransientParent(foreign);
#else
    Q_UNUSED(w);
#endif
}

namespace {
struct MainWindowGuard {
    MainWindowGuard()  { if (g_mainWindow) gtk_widget_set_sensitive(g_mainWindow, FALSE); }
    ~MainWindowGuard() { if (g_mainWindow) gtk_widget_set_sensitive(g_mainWindow, TRUE); }
};
}

int execModal(QDialog *d)
{
    syncTheme();
    makeTransient(d);
    MainWindowGuard guard;
    return d->exec();
}

void showWindow(QWidget *w)
{
    syncTheme();
    makeTransient(w);
    w->show();
    w->raise();
    w->activateWindow();
}

static gboolean flushCb(gpointer)
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return G_SOURCE_REMOVE;
}

void scheduleFlush() { g_idle_add(flushCb, nullptr); }

} // namespace qtgtk
