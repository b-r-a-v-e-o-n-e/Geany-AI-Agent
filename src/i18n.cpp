// SPDX-License-Identifier: MIT
#include "i18n.h"
#include <QHash>
#include <QLocale>

namespace {
bool g_bg = false;

const QHash<QString, QString> &bgTable()
{
    static const QHash<QString, QString> t = {
        // menu
        {"Ask AI Agent…", "Попитай AI агента…"},
        {"geany-ai Settings…", "Настройки на geany-ai…"},
        {"Ask AI Agent", "Попитай AI агента"},
        // task dialog
        {"Instruction:", "Инструкция:"},
        {"What should the agent do?", "Какво да направи агентът?"},
        {"Hints (optional — files or dirs to focus on):", "Подсказки (по избор — файлове или папки, върху които да се фокусира):"},
        {"e.g. src/api.c, include/", "напр. src/api.c, include/"},
        {"Exclusions (optional — files or dirs to leave alone, comma-separated):", "Изключения (по избор — файлове или папки, които да не се пипат, разделени със запетая):"},
        {"e.g. CMakeLists.txt, build/", "напр. CMakeLists.txt, build/"},
        {"Permitted directories for this task", "Разрешени папки за тази задача"},
        {"No permitted directories configured.\nAdd them in Tools → geany-ai Settings.",
         "Няма конфигурирани разрешени папки.\nДобавете ги от Инструменти → Настройки на geany-ai."},
        {"Send  (Ctrl+Enter)", "Изпрати  (Ctrl+Enter)"},
        {"Cancel", "Отказ"},
        {"Close", "Затвори"},
        {"Save", "Запази"},
        {"Add…", "Добави…"},
        {"Remove", "Премахни"},
        // settings dialog
        {"geany-ai Settings", "Настройки на geany-ai"},
        {"API key:", "API ключ:"},
        {"Show", "Покажи"},
        {"Model:", "Модел:"},
        {"System prompt:", "Системен промпт:"},
        {"Reset to default", "Върни по подразбиране"},
        {"Permitted directories:", "Разрешени папки:"},
        {"API URL (advanced):", "API адрес (разширено):"},
        {"Max output tokens:", "Макс. изходни токени:"},
        {"Interface language:", "Език на интерфейса:"},
        {"Automatic", "Автоматичен"},
        {"Select directory", "Изберете папка"},
        {"The key is stored in plain text in ~/.config/geany/geany-ai.conf (permissions 0600).",
         "Ключът се пази като обикновен текст в ~/.config/geany/geany-ai.conf (права 0600)."},
        {"Provider:", "Доставчик:"},
        {"Claude (Anthropic API)", "Claude (Anthropic API)"},
        {"Local / OpenAI-compatible server (llama.cpp, Ollama, …)", "Локален / OpenAI-съвместим сървър (llama.cpp, Ollama, …)"},
        {"API key (optional):", "API ключ (по избор):"},
        {"Server URL:", "Адрес на сървъра:"},
        {"usually empty for local servers", "обикновено празно за локални сървъри"},
        {"Load models", "Зареди модели"},
        {"Connecting…", "Свързване…"},
        {"Could not load models: %1", "Неуспешно зареждане на модели: %1"},
        {"the server returned no models", "сървърът не върна модели"},
        {"Found %1 model(s) — pick one from the list.", "Намерени модели: %1 — изберете от списъка."},
        {"Send is disabled: add a permitted directory in Tools → geany-ai Settings.", "„Изпрати“ е неактивно: добавете разрешена папка от Инструменти → Настройки на geany-ai."},
        {"Send is disabled: tick at least one directory above.", "„Изпрати“ е неактивно: отметнете поне една папка по-горе."},
        {"Send is disabled: write an instruction first.", "„Изпрати“ е неактивно: първо напишете инструкция."},
        // progress
        {"The agent is working…", "Агентът работи…"},
        {"Cancelled.", "Отменено."},
        {"Done.", "Готово."},
        {"Failed.", "Неуспех."},
        {"Turn %1: waiting for the model…", "Ход %1: изчакване на модела…"},
        {"Temporary API error (HTTP %1), retrying in %2 s…", "Временна грешка на API (HTTP %1), нов опит след %2 с…"},
        {"Tokens used: %1 in / %2 out", "Използвани токени: %1 вход / %2 изход"},
        // review
        {"Review proposed changes", "Преглед на предложените промени"},
        {"Apply selected", "Приложи избраните"},
        {"Reject all", "Отхвърли всички"},
        {"new file", "нов файл"},
        {"modified", "променен"},
        {"No changes", "Няма промени"},
        {"This file has unsaved changes in the editor; they will be lost when it is reloaded.",
         "Този файл има незаписани промени в редактора; те ще бъдат загубени при презареждане."},
        {"Tick the files to apply. Originals are backed up before overwriting.",
         "Отбележете файловете, които да се приложат. Оригиналите се архивират преди презаписване."},
        {"Summary:", "Резюме:"},
        // messages
        {"geany-ai: no API key — use Tools → geany-ai Settings", "geany-ai: няма API ключ — Инструменти → Настройки на geany-ai"},
        {"No permitted directories are configured. Open the settings now?", "Няма конфигурирани разрешени папки. Да отворя ли настройките?"},
        {"A request is already running.", "Заявка вече се изпълнява."},
        {"geany-ai: settings saved", "geany-ai: настройките са запазени"},
        {"geany-ai: running…", "geany-ai: изпълнява се…"},
        {"geany-ai: applied %1", "geany-ai: приложено %1"},
        {"geany-ai: failed to write %1: %2", "geany-ai: неуспешен запис на %1: %2"},
        {"geany-ai: all changes rejected", "geany-ai: всички промени са отхвърлени"},
        {"geany-ai: no file changes proposed", "geany-ai: не са предложени промени по файлове"},
        {"geany-ai: backups saved in %1", "geany-ai: архивите са запазени в %1"},
        {"geany-ai error: %1", "geany-ai грешка: %1"},
        {"Qt could not start a graphical backend inside Geany.\n\n%1\n\nInstall the missing packages, e.g.:\n  sudo apt install qt6-wayland libxcb-cursor0",
         "Qt не можа да стартира графичен бекенд в Geany.\n\n%1\n\nИнсталирайте липсващите пакети, напр.:\n  sudo apt install qt6-wayland libxcb-cursor0"},
        // v0.4.0: agent modes, providers, confirmations
        {"Local / custom OpenAI-compatible (llama.cpp, Ollama, …)", "Локален / собствен OpenAI-съвместим (llama.cpp, Ollama, …)"},
        {"Qwen (Alibaba DashScope, international)", "Qwen (Alibaba DashScope, международен)"},
        {"Server / base URL:", "Адрес на сървъра / базов URL:"},
        {"Default mode:", "Режим по подразбиране:"},
        {"Mode:", "Режим:"},
        {"Manual — review changes, no commands", "Ръчен — преглед на промените, без команди"},
        {"Ask before run — confirm every action", "Питай преди изпълнение — потвърждение на всяко действие"},
        {"Full auto — no questions asked", "Пълна автоматика — без питане"},
        {"Full auto: the agent writes files, installs packages and runs commands as your user without asking. Only use it on projects you can restore.",
         "Пълна автоматика: агентът записва файлове, инсталира пакети и изпълнява команди с вашия потребител без да пита. Ползвайте я само за проекти, които можете да възстановите."},
        {"The agent can only read and propose file changes; you review a diff and apply them.",
         "Агентът само чете и предлага промени; вие преглеждате разликите и ги прилагате."},
        {"The agent can write files, install packages and compile, but asks you before each step.",
         "Агентът може да пише файлове, да инсталира пакети и да компилира, но пита преди всяка стъпка."},
        {"The agent checks the system, installs what is missing, compiles and fixes errors on its own.",
         "Агентът проверява системата, инсталира липсващото, компилира и поправя грешките сам."},
        {"Run this command?", "Да изпълня ли тази команда?"},
        {"Modify this file?", "Да променя ли този файл?"},
        {"Create this file?", "Да създам ли този файл?"},
        {"Install system packages?", "Да инсталирам ли системни пакети?"},
        {"Allow", "Разреши"},
        {"Deny", "Откажи"},
        {"Allow all for this task", "Разреши всичко за тази задача"},
        {"Agent finished", "Агентът приключи"},
        {"No files were changed.", "Няма променени файлове."},
        {"Files changed on disk:", "Променени файлове на диска:"},
        {"Backups of the originals: %1", "Архиви на оригиналите: %1"},
        {"Keep changes", "Запази промените"},
        {"Undo all changes", "Отмени всички промени"},
        {"geany-ai: %1 file(s) changed", "geany-ai: променени файлове: %1"},
        {"geany-ai: all changes undone", "geany-ai: всички промени са отменени"},
        {"geany-ai: undo failed: %1", "geany-ai: неуспешна отмяна: %1"},
        {"geany-ai: %1 has unsaved changes in the editor and was not reloaded", "geany-ai: %1 има незаписани промени в редактора и не е презареден"},
        // v0.4.2: new project, MCP, editor, stuck checkpoints
        {"New project: create a sub-folder with a unique suffix", "Нов проект: създай подпапка с уникален суфикс"},
        {"Project name:", "Име на проекта:"},
        {"e.g. my-tool  → <folder>/my-tool-a1b2c3", "напр. my-tool  → <папка>/my-tool-a1b2c3"},
        {"geany-ai: new project folder %1", "geany-ai: нова папка за проект %1"},
        {"Tools:", "Инструменти:"},
        {"MCP servers… (%1 enabled)", "MCP сървъри… (включени: %1)"},
        {"Open changed files in the Geany editor (for control and manual fixes)", "Отваряй променените файлове в редактора на Geany (за контрол и ръчни корекции)"},
        {"MCP servers", "MCP сървъри"},
        {"MCP servers add tools to the agent (available in Ask / Auto modes). stdio: a command line such as  npx -y @modelcontextprotocol/server-filesystem /home/me/proj  — HTTP: the server URL (http:// or https://).",
         "MCP сървърите добавят инструменти на агента (в режими Ask / Auto). stdio: команден ред, напр.  npx -y @modelcontextprotocol/server-filesystem /home/me/proj  — HTTP: адрес на сървъра (http:// или https://)."},
        {"command line or http(s):// URL", "команден ред или http(s):// адрес"},
        {"stdio only: KEY=VALUE;KEY2=VALUE2", "само stdio: КЛЮЧ=СТОЙНОСТ;КЛЮЧ2=СТОЙНОСТ2"},
        {"HTTP only: Authorization: Bearer …", "само HTTP: Authorization: Bearer …"},
        {"Name:", "Име:"},
        {"Command or URL:", "Команда или адрес:"},
        {"Environment:", "Среда:"},
        {"HTTP header:", "HTTP заглавка:"},
        {"Enabled", "Включен"},
        {"Add", "Добави"},
        {"OK", "OK"},
        {"Test connection / list tools", "Тест на връзката / списък с инструменти"},
        {"Failed: %1", "Неуспех: %1"},
        {"Connected. %1 tool(s):", "Свързан. Инструменти: %1:"},
        {"Call this MCP tool?", "Да извикам ли този MCP инструмент?"},
        {"The agent may be stuck", "Агентът може да е затънал"},
        {"The agent may be going in circles", "Агентът може да се върти в кръг"},
        {"What should it do? (the log and the changed files are available in Geany while this window is open)", "Какво да направи? (логът и променените файлове са достъпни в Geany, докато този прозорец е отворен)"},
        {"Try a different approach — tell the model to step back and find another solution", "Друг подход — кажи на модела да отстъпи крачка назад и да търси друго решение"},
        {"Continue — let it keep trying", "Продължи — остави го да опитва"},
        {"Give it a hint — write guidance below", "Дай му подсказка — напиши насока по-долу"},
        {"Stop and take over — keep the files, open them in the editor and fix by hand", "Спри и поеми — запази файловете, отвори ги в редактора и поправи ръчно"},
        {"e.g. use std::filesystem instead of Qt here; the bug is in parse_args()", "напр. тук ползвай std::filesystem; грешката е в parse_args()"},
        {"The agent repeated the same tool call with the same result %1 times", "Агентът повтори същото извикване със същия резултат %1 пъти"},
        {"The same error appeared %1 times", "Същата грешка се появи %1 пъти"},
        {"%1 tool calls in a row failed", "%1 извиквания на инструменти поред са неуспешни"},
        {"Long run: %1 turns so far", "Дълго изпълнение: досега %1 хода"},
        {"The turn limit (%1) was reached", "Достигнат е лимитът от ходове (%1)"},
        {"Stopped by you at a checkpoint.", "Спряно от вас на контролна точка."},
        {"Stopped by you at the turn limit.", "Спряно от вас при лимита на ходовете."},
        {"Stopped at your request. The changed files are open in the editor for manual changes.", "Спряно по ваше искане. Променените файлове са отворени в редактора за ръчни корекции."},
        {"Files changed on disk (select one to see the diff):", "Променени файлове на диска (изберете за да видите разликите):"},
        {"Open in editor", "Отвори в редактора"},
        {"Open all in editor", "Отвори всички в редактора"},
        // api errors
        {"Network error: %1", "Мрежова грешка: %1"},
        {"API error (HTTP %1): %2", "Грешка на API (HTTP %1): %2"},
        {"Invalid response from the API: %1", "Невалиден отговор от API: %1"},
        {"The response was cut off (max_tokens reached). Increase \"Max output tokens\" in the settings or ask for smaller changes.",
         "Отговорът е прекъснат (достигнат max_tokens). Увеличете „Макс. изходни токени“ в настройките или поискайте по-малки промени."},
        {"Exceeded the maximum number of turns without completing.", "Надвишен е максималният брой хода без завършване."},
        {"The model declined to continue.", "Моделът отказа да продължи."},
    };
    return t;
}
} // namespace

namespace i18n {

void setLanguage(const QString &lang)
{
    if (lang == "bg")      g_bg = true;
    else if (lang == "en") g_bg = false;
    else                   g_bg = (QLocale::system().language() == QLocale::Bulgarian);
}

QString tr(const char *en)
{
    const QString key = QString::fromUtf8(en);
    if (g_bg) {
        auto it = bgTable().constFind(key);
        if (it != bgTable().constEnd())
            return it.value();
    }
    return key;
}

} // namespace i18n
