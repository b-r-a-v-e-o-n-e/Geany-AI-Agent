// SPDX-License-Identifier: MIT
#include "settings.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <glib.h>
#include <glib/gstdio.h>

namespace ai {

const char *const kDefaultApiUrl = "https://api.anthropic.com/v1/messages";

static const char *kGroup = "geany-ai";

const QList<ProviderInfo> &providerList()
{
    // Model names are only suggestions: use "Load models" to list what the account/server really offers.
    static const QList<ProviderInfo> l = {
        {"anthropic", "Claude (Anthropic API)",                       "anthropic", "",                                                          "claude-sonnet-5-5",        true},
        {"openai",    "OpenAI",                                       "openai",    "https://api.openai.com/v1",                                 "gpt-5",                    true},
        {"groq",      "Groq",                                         "openai",    "https://api.groq.com/openai/v1",                            "llama-3.3-70b-versatile",  true},
        {"google",    "Google Gemini",                                "openai",    "https://generativelanguage.googleapis.com/v1beta/openai",   "gemini-2.5-pro",           true},
        {"deepseek",  "DeepSeek",                                     "openai",    "https://api.deepseek.com",                                  "deepseek-chat",            true},
        {"qwen",      "Qwen (Alibaba DashScope, international)",      "openai",    "https://dashscope-intl.aliyuncs.com/compatible-mode/v1",    "qwen3-coder-plus",         true},
        {"kimi",      "Kimi (Moonshot)",                              "openai",    "https://api.moonshot.ai/v1",                                "kimi-k2-0905-preview",     true},
        {"local",     "Local / custom OpenAI-compatible (llama.cpp, Ollama, …)", "openai", "http://127.0.0.1:8080/v1",                           "local",                    false},
    };
    return l;
}

const ProviderInfo *findProvider(const QString &id)
{
    for (const ProviderInfo &p : providerList()) if (p.id == id) return &p;
    return nullptr;
}

QString modeToString(Mode m)
{
    switch (m) { case Mode::Manual: return QStringLiteral("manual"); case Mode::Auto: return QStringLiteral("auto"); default: return QStringLiteral("ask"); }
}

Mode modeFromString(const QString &s)
{
    if (s == QLatin1String("manual")) return Mode::Manual;
    if (s == QLatin1String("auto"))   return Mode::Auto;
    return Mode::Ask;
}

const ProviderInfo &Settings::info() const
{
    const ProviderInfo *p = findProvider(provider);
    return p ? *p : providerList().first();
}
bool Settings::isOpenAi() const { return info().kind == QLatin1String("openai"); }
QString Settings::effectiveUrl() const
{
    const QString u = curCfg().url.trimmed();
    return u.isEmpty() ? info().defaultUrl : u;
}
QString Settings::effectiveModel() const
{
    const QString m = curCfg().model.trimmed();
    return m.isEmpty() ? info().defaultModel : m;
}
bool Settings::ready() const { return !info().keyRequired || !apiKey().isEmpty(); }

QString defaultSystemPrompt()
{
    return QStringLiteral(
        "You are an autonomous software engineering agent working inside the Geany editor. "
        "You can list, search, read, edit and write files, and depending on the mode also run shell commands "
        "and install system packages. When given a task:\n"
        "1. Explore the relevant files (list_dir, search_files, read_file).\n"
        "2. Make the changes (edit_file for small changes, write_file for new files or full rewrites; "
        "always complete content, no markdown fences, no placeholders).\n"
        "3. If you can run commands: check the environment first (check_environment), install what is missing "
        "(install_packages), then build and run/test the result with run_command, read any errors, fix them and repeat "
        "until it works.\n"
        "4. Call task_complete with a concise summary of what you did, how to build/run it, and anything left for the user.\n"
        "Never write a file you have not first read (unless you are creating a new one). "
        "Never touch files outside the permitted directories. Respect exclusions and hints. "
        "Use absolute paths. Preserve the existing coding style and line endings.");
}

QString configPath()
{
    gchar *p = g_build_filename(g_get_user_config_dir(), "geany", "geany-ai.conf", nullptr);
    QString s = QString::fromUtf8(p);
    g_free(p);
    return s;
}

static QString getStr(GKeyFile *kf, const QString &key)
{
    gchar *v = g_key_file_get_string(kf, kGroup, key.toUtf8().constData(), nullptr);
    QString s = v ? QString::fromUtf8(v) : QString();
    g_free(v);
    return s;
}

// Config key names. Anthropic and "local" keep their original (v0.1–0.3) names for compatibility.
static void keyNames(const QString &id, QString *key, QString *model, QString *url)
{
    if (id == QLatin1String("anthropic")) { *key = "api_key";       *model = "model";       *url = "api_url"; }
    else if (id == QLatin1String("local")) { *key = "local_api_key"; *model = "local_model"; *url = "local_url"; }
    else { *key = id + "_api_key"; *model = id + "_model"; *url = id + "_url"; }
}

void loadSettings(Settings &s)
{
    GKeyFile *kf = g_key_file_new();
    const QByteArray path = configPath().toUtf8();
    g_key_file_load_from_file(kf, path.constData(), G_KEY_FILE_NONE, nullptr);

    s = Settings();
    s.systemPrompt = getStr(kf, "system_prompt");
    s.language     = getStr(kf, "language");
    if (s.systemPrompt.isEmpty()) s.systemPrompt = defaultSystemPrompt();
    if (s.language.isEmpty())     s.language = QStringLiteral("en");

    for (const ProviderInfo &p : providerList()) {
        QString kk, km, ku;
        keyNames(p.id, &kk, &km, &ku);
        ProviderCfg c;
        c.apiKey = getStr(kf, kk);
        c.model  = getStr(kf, km);
        c.url    = getStr(kf, ku);
        s.cfg[p.id] = c;
    }
    if (s.cfg["local"].model.isEmpty()) s.cfg["local"].model = QStringLiteral("local");
    if (s.cfg["anthropic"].model.isEmpty()) s.cfg["anthropic"].model = findProvider("anthropic")->defaultModel;

    // v0.1-0.3 wrote provider=openai for the "local / custom" server; v0.4 has a real OpenAI entry.
    QString prov = getStr(kf, "provider");
    GError *err = nullptr;
    const int ver = g_key_file_get_integer(kf, kGroup, "config_version", &err);
    if (err) { g_error_free(err); }
    if (ver < 2 && prov == QLatin1String("openai")) prov = QStringLiteral("local");
    s.provider = findProvider(prov) ? prov : QStringLiteral("anthropic");

    const QString md = getStr(kf, "mode");
    s.mode = modeToString(modeFromString(md.isEmpty() ? QStringLiteral("ask") : md));

    err = nullptr;
    int mt = g_key_file_get_integer(kf, kGroup, "max_tokens", &err);
    if (err) { g_error_free(err); mt = 0; }
    if (mt >= 256 && mt <= 128000) s.maxTokens = mt;

    {
        const QJsonArray arr = QJsonDocument::fromJson(getStr(kf, "mcp_servers").toUtf8()).array();
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            McpServerCfg m;
            m.name = o.value("name").toString(); m.target = o.value("target").toString();
            m.env = o.value("env").toString(); m.header = o.value("header").toString();
            m.enabled = o.value("enabled").toBool(true);
            if (!m.name.isEmpty() && !m.target.isEmpty()) s.mcpServers << m;
        }
        auto getB = [&](const char *k, bool def) {
            GError *e = nullptr;
            const gboolean b = g_key_file_get_boolean(kf, kGroup, k, &e);
            if (e) { g_error_free(e); return def; }
            return b != FALSE;
        };
        s.openInEditor = getB("open_in_editor", true);
        s.newProject   = getB("new_project", false);
        s.projectName  = getStr(kf, "project_name");
    }

    gsize n = 0;
    gchar **dirs = g_key_file_get_string_list(kf, kGroup, "permitted_dirs", &n, nullptr);
    for (gsize i = 0; dirs && i < n; ++i) {
        const QString d = QString::fromUtf8(dirs[i]);
        if (!d.isEmpty() && !s.permittedDirs.contains(d))
            s.permittedDirs << d;
    }
    g_strfreev(dirs);
    g_key_file_free(kf);
}

bool saveSettings(const Settings &s)
{
    GKeyFile *kf = g_key_file_new();
    const QByteArray path = configPath().toUtf8();
    g_key_file_load_from_file(kf, path.constData(), G_KEY_FILE_NONE, nullptr);   // keep unknown keys

    auto setS = [&](const QString &k, const QString &v) {
        g_key_file_set_string(kf, kGroup, k.toUtf8().constData(), v.toUtf8().constData());
    };
    setS("system_prompt", s.systemPrompt);
    setS("language", s.language);
    setS("provider", s.provider);
    setS("mode", s.mode);
    g_key_file_set_integer(kf, kGroup, "max_tokens", s.maxTokens);
    g_key_file_set_integer(kf, kGroup, "config_version", 2);
    {
        QJsonArray arr;
        for (const McpServerCfg &m : s.mcpServers)
            arr.append(QJsonObject{{"name", m.name}, {"target", m.target}, {"env", m.env}, {"header", m.header}, {"enabled", m.enabled}});
        setS("mcp_servers", arr.isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
        g_key_file_set_boolean(kf, kGroup, "open_in_editor", s.openInEditor);
        g_key_file_set_boolean(kf, kGroup, "new_project", s.newProject);
        setS("project_name", s.projectName);
    }

    for (auto it = s.cfg.constBegin(); it != s.cfg.constEnd(); ++it) {
        QString kk, km, ku;
        keyNames(it.key(), &kk, &km, &ku);
        setS(kk, it.value().apiKey);
        setS(km, it.value().model);
        setS(ku, it.value().url);
    }

    if (s.permittedDirs.isEmpty()) {
        g_key_file_remove_key(kf, kGroup, "permitted_dirs", nullptr);
    } else {
        QList<QByteArray> store;
        QVector<const gchar *> ptrs;
        for (const QString &d : s.permittedDirs) { store << d.toUtf8(); }
        for (const QByteArray &b : store) ptrs << b.constData();
        g_key_file_set_string_list(kf, kGroup, "permitted_dirs", ptrs.constData(), (gsize)ptrs.size());
    }

    gsize len = 0;
    gchar *data = g_key_file_to_data(kf, &len, nullptr);

    gchar *dir = g_path_get_dirname(path.constData());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    gboolean ok = g_file_set_contents(path.constData(), data, (gssize)len, nullptr);
    if (ok) g_chmod(path.constData(), 0600);     // the file holds API keys

    g_free(data);
    g_key_file_free(kf);
    return ok;
}

} // namespace ai
