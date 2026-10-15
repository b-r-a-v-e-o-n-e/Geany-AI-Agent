// SPDX-License-Identifier: MIT
#pragma once
#include <QString>

namespace i18n {
/** lang: "auto" (system locale), "en" or "bg". */
void setLanguage(const QString &lang);
/** Translate an English UI string; unknown strings are returned unchanged. */
QString tr(const char *en);
}

#define TR(s) i18n::tr(s)
