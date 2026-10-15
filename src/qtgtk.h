// SPDX-License-Identifier: MIT
// Glue that lets Qt widgets live inside Geany's GTK process.
#pragma once
#include <QString>

class QDialog;
class QWidget;
struct _GtkWidget;

namespace qtgtk {

/** Creates the (single, never destroyed) QApplication. Qt's GLib event dispatcher
 *  plugs into GTK's main loop, so no extra thread or timer is needed. */
bool ensureApp(QString *error);

/** Geany's main window; used to keep Qt dialogs on top of it and to block it while modal. */
void setMainWindow(_GtkWidget *w);

/** Show a Qt dialog modally with respect to Geany (main window is disabled meanwhile). */
int  execModal(QDialog *d);

/** Show a non-modal Qt window above Geany. */
void showWindow(QWidget *w);

/** QObject::deleteLater() never fires when no Qt event loop is running; call this to flush. */
void scheduleFlush();

} // namespace qtgtk
