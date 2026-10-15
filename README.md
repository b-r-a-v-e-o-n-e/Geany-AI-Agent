# Geany-AI-Agent
Basic AI Agent Plugin for Geany 2.0 to automate programming tasks in C++ and Qt6.


An **AI agent plugin for Geany** (version 0.4.2). It works with **Claude, OpenAI, Groq, Google Gemini, DeepSeek, Qwen, Kimi** or a local model (llama.cpp, Ollama, …). The agent explores your project, writes code, **checks what it needs to build, installs missing packages, compiles on its own and fixes the errors**. Three modes are available: Manual, Ask before run, and Full auto.

This is a Qt6 port of [geany-ai](https://github.com/) by Jack Sidman Smith (MIT). The whole UI (dialogs, change review, networking) is **Qt6**.

> **About "Qt":** Geany itself is a GTK3 application and its plugins are loaded into its process. The plugin is therefore a thin GTK module (two menu items in *Tools*), and everything you see on screen is Qt6. Qt uses the GLib event loop, so it runs in the same process, with no threads and no second process.

## Differences from the GTK version

| | original | geany-ai-qt |
|---|---|---|
| UI | GTK3 | **Qt6** (English, optional Bulgarian translation) |
| Geany API version | 248 (**does not load** on Geany 2.0 from Debian 13 / Ubuntu 24.04, which are at 247) | 225 — loads on Geany 2.0+ |
| JSON | hand-written parser (breaks on `{`/`}` in code, `\uXXXX`) | `QJsonDocument` |
| Path checks | prefix match without a boundary (`/proj` allows `/proj-secret`), exclusions resolved against CWD | canonical paths, directory boundary, symlinks, exclusions resolved against the chosen folders |
| Diff | linear scan (confused by inserted lines) | Myers diff with context hunks |
| Review | one dialog per file | one window: checklist + coloured diff |
| `write_file` together with `task_complete` | changes were lost | applied |
| Backup before overwriting | none | `~/.local/share/geany-ai/backups/<time>/` |
| Agent abilities | read / write files | + edit, search, environment check, package install, build commands, MCP tools |
| Providers | Claude | Claude, OpenAI, Groq, Google, DeepSeek, Qwen, Kimi, local |
| Progress | status bar only | live request log + Cancel button |
| Retry | none | on HTTP 429/5xx/529 (up to 3 times) |
| Key file | default permissions | `0600` |

## Install (Debian 13)

```bash
sudo apt update
sudo apt install build-essential cmake pkg-config geany geany-common \
                 libgtk-3-dev qt6-base-dev libxcb-cursor0 qt6-wayland

cd geany-ai-qt
./install.sh            # current user only, no sudo
```

or manually:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
mkdir -p ~/.config/geany/plugins
cp build/geany-ai-qt.so ~/.config/geany/plugins/
```

For a system-wide install: `./install.sh --system` (copies to `/usr/lib/x86_64-linux-gnu/geany/`).

Enable it in **Tools → Plugin Manager → geany-ai (Qt)**. If you have the old GTK version (`geany-ai.so`) installed, disable it — both plugins share the same config file and have the same menu entries.

Notes:
* `libxcb-cursor0` is needed by Qt ≥ 6.5 for X11/XWayland; `qt6-wayland` if Geany runs in a pure Wayland session (the default in GNOME on Debian 13). If Qt cannot start, the plugin shows a message instead of crashing Geany.
* Geany headers come with `geany-common` on Debian (`libgeany-dev` is not needed). Check: `pkg-config --modversion geany` should print `2.0`.

## Agent modes

The mode is chosen per task in the *Ask AI Agent* dialog (the last one used is remembered; the default is set in Settings).

| Mode | What the agent can do |
|---|---|
| **Manual** | Only reads files and proposes changes. Nothing is written until you approve it in the review window (diff). It does not run commands or install packages; if needed, it tells you the exact commands in its summary. |
| **Ask before run** | Can write files, install packages and run commands, but **asks before every action** (showing a diff for files and the exact command). Buttons: Allow / Deny / "Allow all for this task". Esc = Deny. |
| **Full auto** | Asks nothing: checks the environment, installs, builds, fixes and repeats. A warning is shown. |

In Ask and Auto modes files are written immediately (otherwise the build could not see them) and the originals are backed up to `~/.local/share/geany-ai/backups/<time>/`. At the end a list of changed files is shown with an **Undo all changes** button (restores the originals, deletes created files).

### Agent tools

`list_dir`, `read_file`, `search_files` (regex, like grep), `write_file`, `edit_file` (exact replacement), `check_environment` (OS, package manager, install privileges, versions of compilers / packages / pkg-config modules — read only), `run_command`, `install_packages`, `task_complete`, plus any MCP tools. `run_command`, `install_packages` and MCP tools are only available in Ask and Auto modes.

**How it installs:** `install_packages` runs `apt-get install -y` through `pkexec` (the system password dialog on Debian/GNOME/KDE), directly as root, or through `sudo -n` if passwordless. If none of these is available, the agent returns the exact command for you to run manually. `sudo` inside `run_command` is blocked (there is nowhere to type a password).

**Commands:** `/bin/bash -c` in a permitted folder, stdin closed, non-interactive (`DEBIAN_FRONTEND=noninteractive`), default timeout 120 s (up to 1800); the whole process group is killed on timeout or Cancel. Output is streamed to the log; the model gets the beginning and the end (about 18 KB).

**Blocked even in Full auto:** `rm -rf /`, `~`, `$HOME`; `mkfs`; `dd of=/dev/…`; fork bombs; `shutdown/reboot`; `chmod -R … /`; `curl … | sh`; `sudo/su/pkexec`. This is a safety net against gross mistakes, **not a sandbox** — see "Security".

## New projects

Tick **New project** in the *Ask AI Agent* dialog and the plugin creates a fresh sub-folder with a unique random suffix inside the first ticked permitted folder, e.g. `~/Projects/my-tool-a1b2c3` (the name comes from the *Project name* field; `project` if empty). That sub-folder becomes the **only** folder the agent may use for the task, so every new project is isolated. In Manual mode the folder is only created when you apply the changes.

## Control and manual correction in the editor

* Every file the agent writes (Ask / Auto) is **opened in a Geany tab** (or reloaded if already open and without unsaved edits), and a compact diff of each change is printed in the progress log. Turn this off with *Settings → Open changed files in the Geany editor*.
* The final window lists the changed files; select one to see its coloured diff, **Open in editor** / **Open all in editor**, or **Undo all changes**.
* In Manual mode the review window shows the diff before anything is written, and the applied files are opened in tabs afterwards.
* If a file you are editing has unsaved changes, it is never reloaded over your edits: the confirmation dialog warns you, and a log line tells you. Save your edits before the agent touches the same file, because the agent reads from disk.

## When the agent loops

The plugin watches the run and pauses with a dialog when the agent
* repeats the same tool call with the same result 3 times,
* hits the same error 3 times, or has 6 failed tool calls in a row,
* runs long (every 30 turns), or reaches the turn limit.

You then choose:
* **Try a different approach** — the model is told to step back and find another solution,
* **Continue** — let it keep trying,
* **Give it a hint** — your text is sent to the model as guidance,
* **Stop and take over** — the files stay as they are, are opened in the editor, and you fix the rest by hand (closing the dialog also stops).

Running the same build command again after an edit is normal and does not count as a loop.

## MCP servers

**Settings → Tools → MCP servers…** adds Model Context Protocol servers whose tools the agent can call (Ask / Auto modes; disabled in Manual mode). Two transports are supported:

| Type | Command or URL field | Extra |
|---|---|---|
| stdio | a command line, e.g. `npx -y @modelcontextprotocol/server-filesystem /home/me/proj` | *Environment*: `KEY=VALUE;KEY2=VALUE2` |
| HTTP (Streamable HTTP, JSON or SSE replies) | `https://host/mcp` | *HTTP header*: e.g. `Authorization: Bearer …` |

Use **Test connection / list tools** to check a server. The tools appear to the model as `mcp__<server>__<tool>`; in Ask mode every MCP call asks for confirmation (kind "MCP tool", with its arguments), in Auto mode it does not. The first start of an `npx` server may download packages (up to 60 s is allowed). Only tools are supported (no resources/prompts/sampling), and MCP servers run with your user rights — only add servers you trust.

## First steps

1. **Tools → geany-ai Settings…** — pick a provider, enter the API key, choose a model and **add at least one permitted folder** (the projects the agent may access).
2. **Tools → Ask AI Agent…** (or assign a shortcut in *Edit → Preferences → Keybindings → geany-ai-qt*).
3. Describe the task, tick the folders for this task (the folder of the open file is ticked automatically), pick a mode, optionally add hints and exclusions (e.g. `build/, vendor/`). `Ctrl+Enter` sends.
4. Watch the log. In Manual mode a review window opens at the end: untick files you do not want and press **Apply selected**. In the other modes you get a summary and can undo everything.
5. Open files in Geany are reloaded (unless they have unsaved edits); `Ctrl+Z` restores the previous state. Originals are also in `~/.local/share/geany-ai/backups/`.

If the answer contains no changes (e.g. "explain this file"), the text is shown in a window.

## Providers

In **Settings → Provider** pick a provider; key, URL and model are stored separately for each. The **Load models** button queries `GET <url>/models` and fills the list.

| Provider | Default base URL | Suggested model |
|---|---|---|
| Claude (Anthropic) | `https://api.anthropic.com/v1/messages` | `claude-sonnet-5-5` |
| OpenAI | `https://api.openai.com/v1` | `gpt-5` |
| Groq | `https://api.groq.com/openai/v1` | `llama-3.3-70b-versatile` |
| Google Gemini | `https://generativelanguage.googleapis.com/v1beta/openai` | `gemini-2.5-pro` |
| DeepSeek | `https://api.deepseek.com` | `deepseek-chat` |
| Qwen (DashScope) | `https://dashscope-intl.aliyuncs.com/compatible-mode/v1` | `qwen3-coder-plus` |
| Kimi (Moonshot) | `https://api.moonshot.ai/v1` | `kimi-k2-0905-preview` |
| Local / custom | `http://127.0.0.1:8080/v1` | `local` |

* Model names are only suggestions — use **Load models** to see the real list for your key.
* **Qwen:** keys are bound to a region. For a key from the China region use `https://dashscope.aliyuncs.com/compatible-mode/v1`.
* The URL may end in `/v1`, `/v1beta/openai`, or have no path (then `/v1/chat/completions` is appended); `/chat/completions` is added automatically.
* Everything except Claude uses the OpenAI-compatible `chat/completions` API with function calling.

## Local models (llama.cpp, Ollama, LM Studio, vLLM)

In **Settings → Provider** choose *Local / custom OpenAI-compatible*. No API key is needed.

**llama.cpp** — start the server with `--jinja` (without it there is no function calling):
```bash
llama-server -m ~/models/qwen2.5-coder-14b-instruct-q4_k_m.gguf --jinja -c 32768 --port 8080
```
In the settings: Server URL = `http://127.0.0.1:8080/v1`, Model = `local` (llama-server ignores the name).

**Ollama** — Server URL = `http://127.0.0.1:11434/v1`, Model = the exact name, e.g. `qwen2.5-coder:14b`.

**Picking a model:** after entering the address (e.g. `http://192.168.1.50:9999/v1`) and, if any, a key, press **Load models** next to the *Model* field. The plugin asks the server (`GET /v1/models`) and fills the drop-down; pick one and press Save. Errors (wrong address, key) are shown under the field.

Notes:
* The model must support tool calling (e.g. Qwen2.5-Coder/Qwen3, Llama 3.1+, Mistral/Devstral). Small models often mix up tools or return whole files with mistakes — review the diff carefully.
* The context (`-c`) must fit the files the model reads; `write_file` returns the whole file, so "Max output tokens" should not exceed the context.
* Replies are not streamed: while a local model generates, the log only shows "waiting for the model…". The timeout is 1 hour.

## Settings

File: `~/.config/geany/geany-ai.conf` (same as the original):

| key | meaning |
|---|---|
| `provider` | `anthropic` (default), `openai`, `groq`, `google`, `deepseek`, `qwen`, `kimi`, `local` |
| `mode` | `manual`, `ask` (default) or `auto` |
| `api_key` / `model` / `api_url` | Anthropic (as in the original) |
| `local_api_key` / `local_model` / `local_url` | local server |
| `<id>_api_key` / `<id>_model` / `<id>_url` | other providers, e.g. `groq_api_key` |
| `max_tokens` | maximum output tokens per request (default 16000) |
| `system_prompt` | system instruction |
| `permitted_dirs` | permitted folders (separated by `;`) |
| `mcp_servers` | MCP servers as a JSON array (`name`, `target`, `env`, `header`, `enabled`) — edit in the dialog |
| `open_in_editor` | open changed files in Geany tabs (default `true`) |
| `new_project` / `project_name` | last used "New project" options |
| `language` | `en` (default), `bg` or `auto` (by locale) |
| `config_version` | `2`; an old v0.3 config with `provider=openai` is migrated to `local` |

## Appearance

Qt windows automatically take the colours of Geany's GTK theme (light or dark). If your theme is not detected well, force it with an environment variable before starting Geany: `GEANY_AI_QT_THEME=dark geany` (or `light`).

## Security

* The agent only sees files in the ticked folders; every path is canonicalised (symlinks resolved) and checked against the folders and exclusions. Excluded files cannot be read, written or listed.
* In **Manual** mode nothing is written until you approve it in the review. In **Ask** mode every command / write / install needs permission. In **Full auto** the agent runs commands as your user without asking — this is not a sandbox: a command can reach anything you have access to (only the working directory is restricted). For stricter isolation run Geany in a container/VM or use Ask mode.
* The contents of files read and command output are sent to the selected provider. Do not add folders with secrets (`~/.ssh`, `~/.gnupg`, your whole `$HOME`) as permitted.
* API keys are stored in plain text in the config file (permissions `0600`).
* Only UTF-8 text files up to 2 MB are read; binary and non-UTF-8 files are refused so they cannot be corrupted.

## Build and tests

```bash
cmake -S . -B build -DBUILD_TESTS=ON && cmake --build build
QT_QPA_PLATFORM=offscreen ./build/unit-tests
```

The tests cover the diff algorithm (against a reference LCS), the path sandbox, settings and migration, the agent tools (edit/search/run_command/timeout/blocking/modes/undo), project folders, the MCP client (`GEANY_AI_MCP_FAKE=tests/fake_mcp.py`: stdio, HTTP, SSE) and loop detection (`GEANY_AI_LOOP_URL`/`GEANY_AI_LOOP_DIR` with `tests/mock_loop.py`). With `GEANY_AI_MOCK_URL`/`GEANY_AI_MOCK_DIR` a full cycle runs against a fake API server, and with `GEANY_AI_AGENT_URL`/`GEANY_AI_AGENT_DIR`/`GEANY_AI_AGENT_FORMAT` an agent scenario runs (environment check → install → code → build → edit; fake servers are in `tests/`).

## Known limitations

* Not tested against real cloud APIs (Anthropic, OpenAI, Groq, Google, DeepSeek, Qwen, Kimi) nor against a real llama.cpp/Ollama — the protocols were verified against a fake server. URLs follow the providers' documentation; model names change, so use *Load models*.
* Package installation only works on Debian/Ubuntu (apt) and needs `pkexec` (polkit) or passwordless `sudo`; otherwise the agent gives you the command.
* No response streaming and no chat history between tasks — every task starts from scratch.
* MCP: tools only, no OAuth flow for HTTP servers (use a static header), no resources/prompts/sampling.
* On pure Wayland, dialogs cannot be attached as children of Geany's window (a Wayland limitation for windows from different toolkits): the main window is blocked, but the dialog is a standalone window. On X11 it is attached to Geany.
* Whole files are passed as text — very large files use many tokens.

## License

MIT. Copyright (c) 2026 Jack Sidman Smith (original); MIT. Copyright (c) 2026 Kristiyan Georgiev - Qt port and modifications.
