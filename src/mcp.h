// SPDX-License-Identifier: MIT
// Minimal MCP (Model Context Protocol) client: stdio and Streamable-HTTP transports, tools only.
#pragma once
#include "settings.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QString>
#include <functional>
#include <memory>

namespace ai {

struct McpTool {
    QString     server;
    QString     name;          // name on the MCP server
    QString     description;
    QJsonObject schema;        // JSON schema of the arguments
    QString     exposed;       // name offered to the model: mcp__<server>__<tool>
};

class McpClient {
public:
    explicit McpClient(const McpServerCfg &cfg);
    ~McpClient();
    McpClient(const McpClient &) = delete;
    McpClient &operator=(const McpClient &) = delete;

    /** Connects, performs the MCP handshake and lists the tools. Blocks (nested event loop). */
    bool start(QString *error);
    void stop();
    void abort() { m_abort = true; }
    void resetAbort() { m_abort = false; }

    const QList<McpTool> &tools() const { return m_tools; }
    const McpServerCfg &config() const { return m_cfg; }

    /** tools/call. Returns false on transport / protocol errors; *isError is the tool's own error flag. */
    bool call(const QString &tool, const QJsonObject &args, int timeoutSec, QString *text, bool *isError, QString *error);

private:
    bool request(const QString &method, const QJsonObject &params, QJsonObject *result, QString *error, int timeoutMs);
    void notify(const QString &method);
    bool sendHttp(const QJsonObject &msg, int id, QString *error, int timeoutMs);
    void handleMessage(const QJsonObject &msg);
    void pump(const QByteArray &data);

    McpServerCfg                m_cfg;
    std::unique_ptr<QProcess>   m_proc;
    std::unique_ptr<QNetworkAccessManager> m_nam;
    QByteArray                  m_buf;
    QMap<int, QJsonObject>      m_responses;
    QString                     m_stderr;
    QString                     m_session;
    int                         m_nextId = 1;
    bool                        m_abort = false;
    bool                        m_started = false;
    QList<McpTool>              m_tools;
};

/** Ready-made MCP servers that are free and need no API key. {PATH} is replaced by a project folder. */
struct McpPreset {
    QString name;          // suggested server name
    QString description;
    QString target;        // command line; may contain {PATH}
    QString requires_;     // what must be installed ("Node.js (npx)" or "uv (uvx)")
    bool    needsPath = false;
};
const QList<McpPreset> &mcpPresets();

/** All MCP servers of a task. */
class McpManager {
public:
    McpManager() = default;
    ~McpManager();

    /** Starts every enabled server; failures are reported through `log` and skipped. */
    void startAll(const QList<McpServerCfg> &servers, const std::function<void(const QString &)> &log);

    QJsonArray toolDefs() const;                       // Anthropic format (name, description, input_schema)
    bool       owns(const QString &exposed) const { return m_byName.contains(exposed); }
    int        toolCount() const { return m_byName.size(); }
    bool       call(const QString &exposed, const QJsonObject &args, int timeoutSec,
                    QString *text, bool *isError, QString *error, QString *label);
    void       abort();
    void       resetAbort();

    static QString exposedName(const QString &server, const QString &tool);

private:
    QList<std::shared_ptr<McpClient>>  m_clients;
    QMap<QString, QPair<McpClient *, McpTool>> m_byName;
};

} // namespace ai
