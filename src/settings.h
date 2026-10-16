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

/** A machine reachable over SSH (typically a QEMU VM) used for builds and tests. */
struct RemoteHost {
    QString name;                 // short id used by the agent
    QString host;                 // address, e.g. 127.0.0.1 for QEMU user networking
    int     port = 22;
    QString user;
    QString auth = QStringLiteral("key");   // "key" | "password"
    QString keyPath;              // private key (auth = key); empty = ssh defaults / agent
    QString password;             // password, or the key's passphrase; stored in the config file (0600)
    QString sudoPassword;         // for sudo on the machine; empty = the login password when auth = "password"
    QString remoteDir = QStringLiteral("~/geany-ai-work");
    bool    trustChangedKey = false;        // VM re-created with the same address: ignore host key changes
    QString notes;                // e.g. "Debian 12 arm64 (QEMU)"
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
    QList<RemoteHost>          sshHosts;
    bool                       openInEditor = true;   // show changed files in Geany tabs
    bool                       newProject = false;    // create a unique sub-folder per task
    QString                    projectName;           // base name of that sub-folder
    bool                       webTools = true;       // built-in web_fetch / web_search / memory / clock tools
    QString                    runTarget;             // "" = this computer, otherwise the name of an SSH machine
    QStringList                recentProjects;        // existing project folders opened before (newest first)

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
