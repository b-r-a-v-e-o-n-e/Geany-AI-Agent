// SPDX-License-Identifier: MIT
#pragma once
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace ai {

/** Static description of a provider. kind: "anthropic" (Messages API) or "openai" (chat/completions). */
struct ProviderInfo {
    QString id;
    QString label;
    QString kind;
    QString defaultUrl;     // empty = built-in (Anthropic) / must be entered
    QString defaultModel;
    bool    keyRequired;
};

const QList<ProviderInfo> &providerList();
const ProviderInfo *findProvider(const QString &id);

struct ProviderCfg {
    QString apiKey;
    QString model;
    QString url;            // empty = provider default
};

enum class Mode { Manual, Ask, Auto };
QString modeToString(Mode m);
Mode    modeFromString(const QString &s);

/** An MCP (Model Context Protocol) server whose tools are offered to the agent. */
struct McpServerCfg {
    QString name;        // short id, becomes part of the tool names
    QString target;      // stdio: command line ("npx -y @modelcontextprotocol/server-filesystem /tmp"); HTTP: http(s):// URL
    QString env;         // stdio: extra environment, KEY=VALUE separated by ';'
    QString header;      // HTTP: optional header, e.g. "Authorization: Bearer xyz"
    bool    enabled = true;
    bool isHttp() const { return target.trimmed().startsWith(QLatin1String("http://")) || target.trimmed().startsWith(QLatin1String("https://")); }
};

struct Settings {
    QString                    provider = QStringLiteral("anthropic");
    QMap<QString, ProviderCfg> cfg;              // per provider id
    QString                    mode = QStringLiteral("ask");
    QString                    systemPrompt;
    QString                    language;         // "auto" | "en" | "bg"
    QStringList                permittedDirs;
    int                        maxTokens = 16000;
    QList<McpServerCfg>        mcpServers;
    bool                       openInEditor = true;   // show changed files in Geany tabs
    bool                       newProject = false;    // create a unique sub-folder per task
    QString                    projectName;           // base name of that sub-folder

    const ProviderInfo &info() const;
    ProviderCfg        &cur()       { return cfg[provider]; }
    ProviderCfg         curCfg() const { return cfg.value(provider); }
    bool                isOpenAi() const;
    QString             effectiveUrl() const;      // configured URL or provider default
    QString             effectiveModel() const;
    QString             apiKey() const { return curCfg().apiKey.trimmed(); }
    bool                ready() const;             // key present when required
    Mode                modeEnum() const { return modeFromString(mode); }
};

extern const char *const kDefaultApiUrl;           // Anthropic Messages endpoint
QString defaultSystemPrompt();

/** Same file as the original GTK plugin: ~/.config/geany/geany-ai.conf */
QString configPath();
void    loadSettings(Settings &s);
bool    saveSettings(const Settings &s);

} // namespace ai
