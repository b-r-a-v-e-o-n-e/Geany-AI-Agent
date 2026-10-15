// SPDX-License-Identifier: MIT
#include "dialogs.h"
#include "api.h"
#include "diff.h"
#include "i18n.h"
#include "mcp.h"
#include "qtgtk.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QCoreApplication>
#include <QScrollArea>
#include <QShortcut>
#include <QSpinBox>
#include <QSplitter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>
#include <QVBoxLayout>

namespace ai {

/* ── Ask AI Agent ──────────────────────────────────────────────────────────── */

bool runTaskDialog(const Settings &s, const QStringList &preChecked, TaskSpec *out)
{
    QDialog dlg;
    dlg.setWindowTitle(TR("Ask AI Agent"));
    dlg.setMinimumWidth(620);
    auto *lay = new QVBoxLayout(&dlg);

    lay->addWidget(new QLabel(TR("Instruction:")));
    auto *inst = new QPlainTextEdit;
    inst->setPlaceholderText(TR("What should the agent do?"));
    inst->setFixedHeight(inst->fontMetrics().lineSpacing() * 5 + 14);
    inst->setTabChangesFocus(true);
    lay->addWidget(inst);

    lay->addWidget(new QLabel(TR("Hints (optional — files or dirs to focus on):")));
    auto *hints = new QLineEdit;
    hints->setPlaceholderText(TR("e.g. src/api.c, include/"));
    lay->addWidget(hints);

    lay->addWidget(new QLabel(TR("Exclusions (optional — files or dirs to leave alone, comma-separated):")));
    auto *excl = new QLineEdit;
    excl->setPlaceholderText(TR("e.g. CMakeLists.txt, build/"));
    lay->addWidget(excl);

    auto *box = new QGroupBox(TR("Permitted directories for this task"));
    auto *boxLay = new QVBoxLayout(box);
    QList<QCheckBox *> checks;
    if (s.permittedDirs.isEmpty()) {
        boxLay->addWidget(new QLabel(TR("No permitted directories configured.\nAdd them in Tools → geany-ai Settings.")));
    } else {
        for (const QString &d : s.permittedDirs) {
            auto *cb = new QCheckBox(d);
            cb->setChecked(preChecked.contains(d) || s.permittedDirs.size() == 1);
            boxLay->addWidget(cb);
            checks << cb;
        }
    }
    boxLay->addStretch(1);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setMaximumHeight(150);
    scroll->setWidget(box);
    lay->addWidget(scroll);

    auto *newProj = new QCheckBox(TR("New project: create a sub-folder with a unique suffix"));
    newProj->setChecked(s.newProject);
    lay->addWidget(newProj);
    auto *projRow = new QHBoxLayout;
    auto *projLbl = new QLabel(TR("Project name:"));
    auto *projName = new QLineEdit(s.projectName);
    projName->setPlaceholderText(TR("e.g. my-tool  → <folder>/my-tool-a1b2c3"));
    projRow->addWidget(projLbl);
    projRow->addWidget(projName, 1);
    lay->addLayout(projRow);
    auto projVis = [=] { projLbl->setEnabled(newProj->isChecked()); projName->setEnabled(newProj->isChecked()); };
    QObject::connect(newProj, &QCheckBox::toggled, &dlg, projVis);
    projVis();

    auto *modeRow = new QHBoxLayout;
    modeRow->addWidget(new QLabel(TR("Mode:")));
    auto *modeBox = new QComboBox;
    modeBox->addItem(TR("Manual — review changes, no commands"), "manual");
    modeBox->addItem(TR("Ask before run — confirm every action"), "ask");
    modeBox->addItem(TR("Full auto — no questions asked"), "auto");
    modeBox->setCurrentIndex(qMax(0, modeBox->findData(modeToString(s.modeEnum()))));
    modeRow->addWidget(modeBox, 1);
    lay->addLayout(modeRow);
    auto *modeNote = new QLabel;
    modeNote->setWordWrap(true);
    modeNote->setEnabled(false);
    lay->addWidget(modeNote);
    auto *autoWarn = new QLabel(TR("Full auto: the agent writes files, installs packages and runs commands as your user without asking. Only use it on projects you can restore."));
    autoWarn->setWordWrap(true);
    autoWarn->setStyleSheet(QStringLiteral("color: #e5a50a; font-weight: bold;"));
    lay->addWidget(autoWarn);
    auto modeChanged = [=] {
        const QString m = modeBox->currentData().toString();
        autoWarn->setVisible(m == QLatin1String("auto"));
        modeNote->setText(m == QLatin1String("manual") ? TR("The agent can only read and propose file changes; you review a diff and apply them.")
                        : m == QLatin1String("ask")    ? TR("The agent can write files, install packages and compile, but asks you before each step.")
                                                       : TR("The agent checks the system, installs what is missing, compiles and fixes errors on its own."));
    };
    QObject::connect(modeBox, &QComboBox::currentIndexChanged, &dlg, modeChanged);
    modeChanged();

    auto *why = new QLabel;
    why->setWordWrap(true);
    why->setStyleSheet(QStringLiteral("color: #e5a50a;"));
    lay->addWidget(why);

    auto *bb = new QDialogButtonBox;
    auto *send = bb->addButton(TR("Send  (Ctrl+Enter)"), QDialogButtonBox::AcceptRole);
    bb->addButton(TR("Cancel"), QDialogButtonBox::RejectRole);
    lay->addWidget(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto update = [&] {
        bool any = false;
        for (QCheckBox *c : std::as_const(checks)) any = any || c->isChecked();
        const bool hasText = !inst->toPlainText().trimmed().isEmpty();
        send->setEnabled(any && hasText);
        if (checks.isEmpty())  why->setText(TR("Send is disabled: add a permitted directory in Tools → geany-ai Settings."));
        else if (!any)         why->setText(TR("Send is disabled: tick at least one directory above."));
        else if (!hasText)     why->setText(TR("Send is disabled: write an instruction first."));
        else                   why->clear();
        why->setVisible(!why->text().isEmpty());
    };
    QObject::connect(inst, &QPlainTextEdit::textChanged, &dlg, update);
    for (QCheckBox *c : std::as_const(checks)) QObject::connect(c, &QCheckBox::toggled, &dlg, update);
    auto *sc = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")), &dlg);
    QObject::connect(sc, &QShortcut::activated, &dlg, [&] { if (send->isEnabled()) dlg.accept(); });
    update();
    inst->setFocus();

    if (qtgtk::execModal(&dlg) != QDialog::Accepted) return false;

    out->instruction = inst->toPlainText().trimmed();
    out->hints       = hints->text().trimmed();
    out->exclusions  = excl->text().trimmed();
    out->mode = modeFromString(modeBox->currentData().toString());
    out->newProject = newProj->isChecked();
    out->projectName = projName->text().trimmed();
    out->activeDirs.clear();
    for (QCheckBox *c : std::as_const(checks))
        if (c->isChecked()) out->activeDirs << c->text();
    return true;
}

/* ── Settings ────────────────────────────────────────────────────────────── */

bool runSettingsDialog(Settings *s)
{
    QDialog dlg;
    dlg.setWindowTitle(TR("geany-ai Settings"));
    dlg.setMinimumWidth(640);
    auto *lay = new QVBoxLayout(&dlg);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    lay->addLayout(form);

    Settings work = *s;                       // edited copy; written back on Save

    auto *prov = new QComboBox;
    for (const ProviderInfo &p : providerList()) prov->addItem(TR(p.label.toUtf8().constData()), p.id);
    prov->setCurrentIndex(qMax(0, prov->findData(work.provider)));
    form->addRow(TR("Provider:"), prov);

    auto *keyLbl = new QLabel;
    auto *key = new QLineEdit;
    key->setEchoMode(QLineEdit::Password);
    auto *show = new QCheckBox(TR("Show"));
    QObject::connect(show, &QCheckBox::toggled, key, [key](bool on) {
        key->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    auto *keyRow = new QHBoxLayout;
    keyRow->addWidget(key, 1);
    keyRow->addWidget(show);
    form->addRow(keyLbl, keyRow);

    auto *model = new QComboBox;
    model->setEditable(true);
    model->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    model->setMinimumContentsLength(24);
    auto *loadBtn = new QPushButton(TR("Load models"));
    auto *modelRow = new QHBoxLayout;
    modelRow->addWidget(model, 1);
    modelRow->addWidget(loadBtn);
    form->addRow(TR("Model:"), modelRow);
    auto *modelStatus = new QLabel;
    modelStatus->setWordWrap(true);
    modelStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    modelStatus->hide();
    form->addRow(QString(), modelStatus);

    auto *urlLbl = new QLabel;
    auto *url = new QLineEdit;
    form->addRow(urlLbl, url);

    QString cur = work.provider;
    // Connect to the server and list the models it offers.
    auto *nam = new QNetworkAccessManager(&dlg);
    QObject::connect(loadBtn, &QPushButton::clicked, &dlg, [&] {
        const QString p = prov->currentData().toString();
        const ProviderInfo *pinfo = findProvider(p);
        const QString effUrl = url->text().trimmed().isEmpty() && pinfo ? pinfo->defaultUrl : url->text();
        QNetworkRequest req{QUrl(modelsEndpoint(pinfo ? pinfo->kind : QStringLiteral("openai"), effUrl))};
        req.setTransferTimeout(20000);
        const QString k = key->text().trimmed();
        const ProviderInfo *pi = findProvider(p);
        if (pi && pi->kind == QLatin1String("openai")) {
            if (!k.isEmpty()) req.setRawHeader("Authorization", "Bearer " + k.toUtf8());
        } else {
            req.setRawHeader("x-api-key", k.toUtf8());
            req.setRawHeader("anthropic-version", "2023-06-01");
        }
        loadBtn->setEnabled(false);
        loadBtn->setText(TR("Connecting…"));
        modelStatus->hide();
        QNetworkReply *r = nam->get(req);
        QObject::connect(r, &QNetworkReply::finished, &dlg, [&, r, p] {
            const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray data = r->readAll();
            const QString netErr = r->errorString();
            const bool netFail = (status == 0 && r->error() != QNetworkReply::NoError);
            r->deleteLater();
            loadBtn->setEnabled(true);
            loadBtn->setText(TR("Load models"));
            if (prov->currentData().toString() != p) return;          // provider switched meanwhile
            const QStringList ids = (status >= 200 && status < 300) ? parseModelList(data) : QStringList();
            QString msg;
            bool ok = false;
            if (netFail) {
                msg = TR("Could not load models: %1").arg(netErr);
            } else if (status < 200 || status >= 300) {
                const QJsonValue ev = QJsonDocument::fromJson(data).object().value("error");
                QString m = ev.isObject() ? ev.toObject().value("message").toString() : ev.toString();
                if (m.isEmpty()) m = QString::fromUtf8(data.left(200));
                msg = TR("Could not load models: %1").arg(QStringLiteral("HTTP %1 %2").arg(status).arg(m));
            } else if (ids.isEmpty()) {
                msg = TR("Could not load models: %1").arg(TR("the server returned no models"));
            } else {
                ok = true;
                const QString keep = model->currentText();
                model->clear();
                model->addItems(ids);
                const int i = ids.indexOf(keep);
                model->setCurrentIndex(i >= 0 ? i : 0);
                msg = TR("Found %1 model(s) — pick one from the list.").arg(ids.size());
            }
            modelStatus->setText(msg);
            modelStatus->setStyleSheet(ok ? QString() : QStringLiteral("color: #e5484d;"));
            modelStatus->show();
        });
    });

    auto commit = [&](const QString &p) {
        ProviderCfg &pc = work.cfg[p];
        pc.apiKey = key->text().trimmed();
        pc.model  = model->currentText().trimmed();
        pc.url    = url->text().trimmed();
    };
    auto load = [&](const QString &p) {
        const ProviderInfo *pi = findProvider(p);
        const ProviderCfg pc = work.cfg.value(p);
        model->clear();
        modelStatus->hide();
        keyLbl->setText(pi && pi->keyRequired ? TR("API key:") : TR("API key (optional):"));
        urlLbl->setText(p == QLatin1String("anthropic") ? TR("API URL (advanced):") : TR("Server / base URL:"));
        key->setText(pc.apiKey);
        key->setPlaceholderText(p == QLatin1String("anthropic") ? QStringLiteral("sk-ant-…")
                                : pi && pi->keyRequired ? QStringLiteral("sk-…") : TR("usually empty for local servers"));
        if (pi) {
            model->addItem(pi->defaultModel);
            if (p == QLatin1String("anthropic")) model->addItems({"claude-opus-5-5", "claude-haiku-5-5"});
        }
        model->setCurrentText(pc.model.isEmpty() && pi ? pi->defaultModel : pc.model);
        url->setText(pc.url);
        url->setPlaceholderText(pi ? (pi->defaultUrl.isEmpty() ? QString::fromLatin1(kDefaultApiUrl) : pi->defaultUrl) : QString());
    };
    load(cur);
    QObject::connect(prov, &QComboBox::currentIndexChanged, &dlg, [&](int) {
        commit(cur);
        cur = prov->currentData().toString();
        load(cur);
    });

    auto *defMode = new QComboBox;
    defMode->addItem(TR("Manual — review changes, no commands"), "manual");
    defMode->addItem(TR("Ask before run — confirm every action"), "ask");
    defMode->addItem(TR("Full auto — no questions asked"), "auto");
    defMode->setCurrentIndex(qMax(0, defMode->findData(modeToString(s->modeEnum()))));
    form->addRow(TR("Default mode:"), defMode);

    auto *mcpBtn = new QPushButton;
    auto mcpLabel = [&] {
        int n = 0;
        for (const McpServerCfg &m : std::as_const(work.mcpServers)) n += m.enabled ? 1 : 0;
        mcpBtn->setText(TR("MCP servers… (%1 enabled)").arg(n));
    };
    mcpLabel();
    QObject::connect(mcpBtn, &QPushButton::clicked, &dlg, [&] { if (runMcpDialog(&work.mcpServers)) mcpLabel(); });
    form->addRow(TR("Tools:"), mcpBtn);

    auto *openEd = new QCheckBox(TR("Open changed files in the Geany editor (for control and manual fixes)"));
    openEd->setChecked(s->openInEditor);
    form->addRow(QString(), openEd);

    auto *maxTok = new QSpinBox;
    maxTok->setRange(256, 128000);
    maxTok->setSingleStep(1000);
    maxTok->setValue(s->maxTokens);
    form->addRow(TR("Max output tokens:"), maxTok);

    auto *lang = new QComboBox;
    lang->addItem(TR("Automatic"), "auto");
    lang->addItem("English", "en");
    lang->addItem("Български", "bg");
    lang->setCurrentIndex(qMax(0, lang->findData(s->language)));
    form->addRow(TR("Interface language:"), lang);

    auto *sys = new QPlainTextEdit(s->systemPrompt);
    sys->setMinimumHeight(110);
    auto *reset = new QPushButton(TR("Reset to default"));
    QObject::connect(reset, &QPushButton::clicked, sys, [sys] { sys->setPlainText(defaultSystemPrompt()); });
    auto *sysBox = new QVBoxLayout;
    sysBox->addWidget(sys);
    sysBox->addWidget(reset, 0, Qt::AlignRight);
    form->addRow(TR("System prompt:"), sysBox);

    auto *dirs = new QListWidget;
    dirs->addItems(s->permittedDirs);
    dirs->setMinimumHeight(90);
    auto *add = new QPushButton(TR("Add…"));
    auto *rem = new QPushButton(TR("Remove"));
    QObject::connect(add, &QPushButton::clicked, &dlg, [&] {
        const QString d = QFileDialog::getExistingDirectory(&dlg, TR("Select directory"));
        if (!d.isEmpty() && dirs->findItems(d, Qt::MatchExactly).isEmpty()) dirs->addItem(d);
    });
    QObject::connect(rem, &QPushButton::clicked, dirs, [dirs] { qDeleteAll(dirs->selectedItems()); });
    auto *dirBtns = new QHBoxLayout;
    dirBtns->addWidget(add);
    dirBtns->addWidget(rem);
    dirBtns->addStretch(1);
    auto *dirBox = new QVBoxLayout;
    dirBox->addWidget(dirs);
    dirBox->addLayout(dirBtns);
    form->addRow(TR("Permitted directories:"), dirBox);

    auto *note = new QLabel(TR("The key is stored in plain text in ~/.config/geany/geany-ai.conf (permissions 0600)."));
    note->setWordWrap(true);
    note->setEnabled(false);
    lay->addWidget(note);

    auto *bb = new QDialogButtonBox;
    bb->addButton(TR("Save"), QDialogButtonBox::AcceptRole);
    bb->addButton(TR("Cancel"), QDialogButtonBox::RejectRole);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);

    if (qtgtk::execModal(&dlg) != QDialog::Accepted) return false;

    commit(cur);
    *s = work;
    s->provider     = cur;
    s->maxTokens    = maxTok->value();
    s->language     = lang->currentData().toString();
    s->systemPrompt = sys->toPlainText();
    s->permittedDirs.clear();
    for (int i = 0; i < dirs->count(); ++i) s->permittedDirs << dirs->item(i)->text();
    s->mode = defMode->currentData().toString();
    s->openInEditor = openEd->isChecked();
    if (s->systemPrompt.trimmed().isEmpty()) s->systemPrompt = defaultSystemPrompt();
    return true;
}

/* ── Review ──────────────────────────────────────────────────────────────── */

static void renderDiff(QTextEdit *view, const StagedWrite &w)
{
    view->clear();
    const bool dark = view->palette().color(QPalette::Base).lightness() < 128;
    const QColor addBg = dark ? QColor(30, 72, 40)  : QColor(214, 245, 220);
    const QColor delBg = dark ? QColor(88, 36, 36)  : QColor(255, 222, 222);
    const QColor hdrBg = dark ? QColor(44, 54, 88)  : QColor(222, 232, 250);

    const QString oldT = QString::fromUtf8(w.original);
    const QString newT = QString::fromUtf8(w.content);
    const QVector<DiffLine> lines = unifiedDiff(oldT, newT, 3);

    view->setUpdatesEnabled(false);
    QTextCursor cur(view->document());
    bool first = true;
    if (lines.isEmpty()) {
        cur.insertText(TR("No changes"));
    }
    for (const DiffLine &l : lines) {
        QTextBlockFormat bf;
        QString prefix;
        switch (l.kind) {
        case DiffLine::Add:    bf.setBackground(addBg); prefix = QStringLiteral("+ "); break;
        case DiffLine::Del:    bf.setBackground(delBg); prefix = QStringLiteral("- "); break;
        case DiffLine::Header: bf.setBackground(hdrBg); break;
        case DiffLine::Context: prefix = QStringLiteral("  "); break;
        }
        if (first) { cur.setBlockFormat(bf); first = false; }
        else       { cur.insertBlock(bf); }
        cur.insertText(prefix + l.text);
    }
    view->setUpdatesEnabled(true);
    view->moveCursor(QTextCursor::Start);
}

QList<StagedWrite> runReviewDialog(const QList<StagedWrite> &writes, const QString &summary)
{
    QDialog dlg;
    dlg.setWindowTitle(TR("Review proposed changes"));
    dlg.resize(1100, 700);
    auto *lay = new QVBoxLayout(&dlg);

    if (!summary.trimmed().isEmpty()) {
        auto *sum = new QLabel(QStringLiteral("<b>%1</b> %2").arg(TR("Summary:").toHtmlEscaped(), summary.toHtmlEscaped().replace('\n', "<br>")));
        sum->setWordWrap(true);
        sum->setTextFormat(Qt::RichText);
        lay->addWidget(sum);
    }

    auto *split = new QSplitter;
    auto *list = new QListWidget;
    for (const StagedWrite &w : writes) {
        auto *it = new QListWidgetItem(QStringLiteral("%1  [%2]").arg(w.path, w.existed ? TR("modified") : TR("new file")));
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(Qt::Checked);
        it->setToolTip(w.path);
        list->addItem(it);
    }
    auto *right = new QWidget;
    auto *rlay = new QVBoxLayout(right);
    rlay->setContentsMargins(0, 0, 0, 0);
    auto *pathLbl = new QLabel;
    pathLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathLbl->setWordWrap(true);
    QFont pf = pathLbl->font();
    pf.setBold(true);
    pathLbl->setFont(pf);
    auto *warn = new QLabel;
    warn->setWordWrap(true);
    warn->setStyleSheet(QStringLiteral("color: #c62828; font-weight: bold;"));
    auto *view = new QTextEdit;
    view->setReadOnly(true);
    view->setLineWrapMode(QTextEdit::NoWrap);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    rlay->addWidget(pathLbl);
    rlay->addWidget(warn);
    rlay->addWidget(view, 1);
    split->addWidget(list);
    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    split->setSizes({300, 800});
    lay->addWidget(split, 1);

    QObject::connect(list, &QListWidget::currentRowChanged, &dlg, [&](int row) {
        if (row < 0 || row >= writes.size()) return;
        pathLbl->setText(writes[row].path);
        warn->setText(writes[row].editorDirty ? TR("This file has unsaved changes in the editor; they will be lost when it is reloaded.") : QString());
        warn->setVisible(writes[row].editorDirty);
        renderDiff(view, writes[row]);
    });
    list->setCurrentRow(0);

    auto *hint = new QLabel(TR("Tick the files to apply. Originals are backed up before overwriting."));
    hint->setEnabled(false);
    lay->addWidget(hint);

    auto *bb = new QDialogButtonBox;
    bb->addButton(TR("Apply selected"), QDialogButtonBox::AcceptRole);
    bb->addButton(TR("Reject all"), QDialogButtonBox::RejectRole);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);

    QList<StagedWrite> accepted;
    if (qtgtk::execModal(&dlg) == QDialog::Accepted) {
        for (int i = 0; i < list->count() && i < writes.size(); ++i)
            if (list->item(i)->checkState() == Qt::Checked) accepted << writes[i];
    }
    return accepted;
}

/* ── Confirm / finish ────────────────────────────────────────────────────── */

Decision runConfirmDialog(const ConfirmRequest &req)
{
    QDialog dlg;
    dlg.setWindowTitle(TR("Ask AI Agent"));
    dlg.resize(780, req.kind == QLatin1String("write") ? 560 : 300);
    auto *lay = new QVBoxLayout(&dlg);
    auto *title = new QLabel(TR(req.title.toUtf8().constData()));
    QFont f = title->font(); f.setBold(true); f.setPointSizeF(f.pointSizeF() * 1.15); title->setFont(f);
    lay->addWidget(title);

    if (req.kind == QLatin1String("write")) {
        auto *p = new QLabel(req.path);
        p->setTextInteractionFlags(Qt::TextSelectableByMouse);
        p->setWordWrap(true);
        lay->addWidget(p);
        if (req.editorDirty) {
            auto *w = new QLabel(TR("This file has unsaved changes in the editor; they will be lost when it is reloaded."));
            w->setWordWrap(true);
            w->setStyleSheet(QStringLiteral("color: #c62828; font-weight: bold;"));
            lay->addWidget(w);
        }
        auto *view = new QTextEdit;
        view->setReadOnly(true);
        view->setLineWrapMode(QTextEdit::NoWrap);
        view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        StagedWrite sw; sw.path = req.path; sw.original = req.before; sw.content = req.after; sw.existed = req.existed;
        renderDiff(view, sw);
        lay->addWidget(view, 1);
    } else {
        auto *view = new QPlainTextEdit(req.detail);
        view->setReadOnly(true);
        view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        lay->addWidget(view, 1);
    }

    Decision result = Decision::Deny;
    auto *bb = new QDialogButtonBox;
    auto *allow = bb->addButton(TR("Allow"), QDialogButtonBox::AcceptRole);
    auto *all   = bb->addButton(TR("Allow all for this task"), QDialogButtonBox::ActionRole);
    auto *deny  = bb->addButton(TR("Deny"), QDialogButtonBox::RejectRole);
    lay->addWidget(bb);
    QObject::connect(allow, &QPushButton::clicked, &dlg, [&] { result = Decision::Allow; dlg.accept(); });
    QObject::connect(all,   &QPushButton::clicked, &dlg, [&] { result = Decision::AllowAll; dlg.accept(); });
    QObject::connect(deny,  &QPushButton::clicked, &dlg, [&] { result = Decision::Deny; dlg.reject(); });
    deny->setDefault(false);
    allow->setDefault(true);
    qtgtk::execModal(&dlg);          // Esc / close = Deny
    return result;
}

bool runFinishDialog(const QString &summary, const QList<AppliedWrite> &applied, const QString &backupDir,
                     const QString &note, const std::function<void(const QString &)> &openInEditor)
{
    QDialog dlg;
    dlg.setWindowTitle(TR("Agent finished"));
    dlg.resize(1000, 640);
    auto *lay = new QVBoxLayout(&dlg);
    if (!note.isEmpty()) {
        auto *n = new QLabel(note);
        n->setWordWrap(true);
        n->setStyleSheet(QStringLiteral("color: #e5484d; font-weight: bold;"));
        lay->addWidget(n);
    }
    auto *sum = new QLabel(QStringLiteral("<b>%1</b> %2").arg(TR("Summary:").toHtmlEscaped(), summary.toHtmlEscaped().replace('\n', "<br>")));
    sum->setWordWrap(true);
    sum->setTextFormat(Qt::RichText);
    lay->addWidget(sum);

    lay->addWidget(new QLabel(applied.isEmpty() ? TR("No files were changed.") : TR("Files changed on disk (select one to see the diff):")));
    auto *list = new QListWidget;
    for (const AppliedWrite &a : applied)
        list->addItem(QStringLiteral("%1  [%2]").arg(a.path, a.existed ? TR("modified") : TR("new file")));
    auto *view = new QTextEdit;
    view->setReadOnly(true);
    view->setLineWrapMode(QTextEdit::NoWrap);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    if (!applied.isEmpty()) {
        auto *split = new QSplitter(Qt::Vertical);
        split->addWidget(list);
        split->addWidget(view);
        split->setStretchFactor(0, 1);
        split->setStretchFactor(1, 4);
        lay->addWidget(split, 1);
        QObject::connect(list, &QListWidget::currentRowChanged, &dlg, [&](int row) {
            if (row < 0 || row >= applied.size()) return;
            const AppliedWrite &a = applied[row];
            StagedWrite sw; sw.path = a.path; sw.original = a.original; sw.content = a.content; sw.existed = a.existed;
            renderDiff(view, sw);
        });
        list->setCurrentRow(0);
        QObject::connect(list, &QListWidget::itemDoubleClicked, &dlg, [&](QListWidgetItem *) {
            const int r = list->currentRow();
            if (openInEditor && r >= 0 && r < applied.size()) openInEditor(applied[r].path);
        });
    } else {
        lay->addStretch(1);
    }
    if (!backupDir.isEmpty()) {
        auto *b = new QLabel(TR("Backups of the originals: %1").arg(backupDir));
        b->setTextInteractionFlags(Qt::TextSelectableByMouse);
        b->setWordWrap(true);
        b->setEnabled(false);
        lay->addWidget(b);
    }
    bool undo = false;
    auto *bb = new QDialogButtonBox;
    auto *ok = bb->addButton(TR("Keep changes"), QDialogButtonBox::AcceptRole);
    QPushButton *un = nullptr;
    if (!applied.isEmpty()) {
        auto *open = bb->addButton(TR("Open in editor"), QDialogButtonBox::ActionRole);
        QObject::connect(open, &QPushButton::clicked, &dlg, [&] {
            if (!openInEditor) return;
            const int r = list->currentRow();
            if (r >= 0 && r < applied.size()) openInEditor(applied[r].path);
        });
        auto *openAll = bb->addButton(TR("Open all in editor"), QDialogButtonBox::ActionRole);
        QObject::connect(openAll, &QPushButton::clicked, &dlg, [&] {
            if (openInEditor) for (const AppliedWrite &a : applied) openInEditor(a.path);
        });
        un = bb->addButton(TR("Undo all changes"), QDialogButtonBox::DestructiveRole);
    }
    lay->addWidget(bb);
    QObject::connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
    if (un) QObject::connect(un, &QPushButton::clicked, &dlg, [&] { undo = true; dlg.accept(); });
    qtgtk::execModal(&dlg);
    return undo;
}

StuckAction runStuckDialog(const QString &reason, QString *guidance)
{
    QDialog dlg;
    dlg.setWindowTitle(TR("The agent may be stuck"));
    dlg.setMinimumWidth(560);
    auto *lay = new QVBoxLayout(&dlg);
    auto *title = new QLabel(TR("The agent may be going in circles"));
    QFont f = title->font(); f.setBold(true); f.setPointSizeF(f.pointSizeF() * 1.15); title->setFont(f);
    lay->addWidget(title);
    auto *why = new QLabel(reason);
    why->setWordWrap(true);
    why->setStyleSheet(QStringLiteral("color: #e5a50a;"));
    lay->addWidget(why);
    lay->addWidget(new QLabel(TR("What should it do? (the log and the changed files are available in Geany while this window is open)")));

    auto *rDiff = new QRadioButton(TR("Try a different approach — tell the model to step back and find another solution"));
    auto *rCont = new QRadioButton(TR("Continue — let it keep trying"));
    auto *rHint = new QRadioButton(TR("Give it a hint — write guidance below"));
    auto *rStop = new QRadioButton(TR("Stop and take over — keep the files, open them in the editor and fix by hand"));
    rDiff->setChecked(true);
    auto *hint = new QPlainTextEdit;
    hint->setPlaceholderText(TR("e.g. use std::filesystem instead of Qt here; the bug is in parse_args()"));
    hint->setFixedHeight(hint->fontMetrics().lineSpacing() * 4 + 14);
    hint->setTabChangesFocus(true);
    for (QRadioButton *r : {rDiff, rCont, rHint, rStop}) lay->addWidget(r);
    lay->addWidget(hint);
    QObject::connect(hint, &QPlainTextEdit::textChanged, &dlg, [&] { if (!hint->toPlainText().isEmpty()) rHint->setChecked(true); });

    auto *bb = new QDialogButtonBox;
    auto *go = bb->addButton(TR("OK"), QDialogButtonBox::AcceptRole);
    lay->addWidget(bb);
    QObject::connect(go, &QPushButton::clicked, &dlg, &QDialog::accept);
    qtgtk::execModal(&dlg);                      // closing the window = Stop
    if (dlg.result() != QDialog::Accepted) return StuckAction::Stop;
    if (rCont->isChecked()) return StuckAction::Continue;
    if (rStop->isChecked()) return StuckAction::Stop;
    if (rHint->isChecked()) { *guidance = hint->toPlainText().trimmed(); return guidance->isEmpty() ? StuckAction::DifferentApproach : StuckAction::Guidance; }
    return StuckAction::DifferentApproach;
}

bool runMcpDialog(QList<McpServerCfg> *servers)
{
    QDialog dlg;
    dlg.setWindowTitle(TR("MCP servers"));
    dlg.resize(820, 520);
    auto *lay = new QVBoxLayout(&dlg);
    auto *intro = new QLabel(TR("MCP servers add tools to the agent (available in Ask / Auto modes). "
                                "stdio: a command line such as  npx -y @modelcontextprotocol/server-filesystem /home/me/proj  — "
                                "HTTP: the server URL (http:// or https://)."));
    intro->setWordWrap(true);
    lay->addWidget(intro);

    QList<McpServerCfg> work = *servers;
    auto *split = new QSplitter;
    auto *list = new QListWidget;
    auto *right = new QWidget;
    auto *form = new QFormLayout(right);
    auto *name = new QLineEdit;
    auto *target = new QLineEdit;
    target->setPlaceholderText(TR("command line or http(s):// URL"));
    auto *env = new QLineEdit;
    env->setPlaceholderText(TR("stdio only: KEY=VALUE;KEY2=VALUE2"));
    auto *header = new QLineEdit;
    header->setPlaceholderText(TR("HTTP only: Authorization: Bearer …"));
    auto *enabled = new QCheckBox(TR("Enabled"));
    form->addRow(TR("Name:"), name);
    form->addRow(TR("Command or URL:"), target);
    form->addRow(TR("Environment:"), env);
    form->addRow(TR("HTTP header:"), header);
    form->addRow(QString(), enabled);
    auto *test = new QPushButton(TR("Test connection / list tools"));
    form->addRow(QString(), test);
    auto *out = new QPlainTextEdit;
    out->setReadOnly(true);
    out->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    form->addRow(out);
    split->addWidget(list);
    split->addWidget(right);
    split->setStretchFactor(1, 3);
    lay->addWidget(split, 1);

    auto *btns = new QHBoxLayout;
    auto *add = new QPushButton(TR("Add"));
    auto *rem = new QPushButton(TR("Remove"));
    btns->addWidget(add);
    btns->addWidget(rem);
    btns->addStretch(1);
    lay->addLayout(btns);

    int cur = -1;
    auto label = [](const McpServerCfg &m) { return (m.enabled ? QString() : QStringLiteral("(off) ")) + m.name; };
    auto commit = [&] {
        if (cur < 0 || cur >= work.size()) return;
        McpServerCfg &m = work[cur];
        m.name = name->text().trimmed(); m.target = target->text().trimmed();
        m.env = env->text().trimmed(); m.header = header->text().trimmed(); m.enabled = enabled->isChecked();
        if (auto *it = list->item(cur)) it->setText(label(m));
    };
    auto show = [&](int row) {
        cur = -1;
        const bool ok = row >= 0 && row < work.size();
        for (QWidget *w : std::initializer_list<QWidget *>{name, target, env, header, enabled, test}) w->setEnabled(ok);
        if (ok) {
            const McpServerCfg &m = work[row];
            name->setText(m.name); target->setText(m.target); env->setText(m.env); header->setText(m.header); enabled->setChecked(m.enabled);
        } else {
            name->clear(); target->clear(); env->clear(); header->clear(); enabled->setChecked(false);
        }
        cur = ok ? row : -1;
        out->clear();
    };
    for (const McpServerCfg &m : std::as_const(work)) list->addItem(label(m));
    QObject::connect(list, &QListWidget::currentRowChanged, &dlg, [&](int row) { commit(); show(row); });
    QObject::connect(add, &QPushButton::clicked, &dlg, [&] {
        commit();
        McpServerCfg m;
        m.name = QStringLiteral("server%1").arg(work.size() + 1);
        work << m;
        list->addItem(label(m));
        list->setCurrentRow(work.size() - 1);
        target->setFocus();
    });
    QObject::connect(rem, &QPushButton::clicked, &dlg, [&] {
        const int r = list->currentRow();
        if (r < 0) return;
        cur = -1;
        work.removeAt(r);
        delete list->takeItem(r);
        show(list->currentRow());
    });
    QObject::connect(test, &QPushButton::clicked, &dlg, [&] {
        commit();
        if (cur < 0) return;
        McpServerCfg m = work[cur];
        m.enabled = true;
        test->setEnabled(false);
        out->setPlainText(TR("Connecting…"));
        QCoreApplication::processEvents();
        McpClient c(m);
        QString err;
        if (!c.start(&err)) {
            out->setPlainText(TR("Failed: %1").arg(err));
        } else {
            QStringList l;
            for (const McpTool &t : c.tools()) l << QStringLiteral("%1 — %2").arg(t.name, t.description.simplified().left(90));
            out->setPlainText(TR("Connected. %1 tool(s):").arg(c.tools().size()) + QLatin1Char('\n') + l.join(QLatin1Char('\n')));
            c.stop();
        }
        test->setEnabled(true);
    });
    show(-1);
    if (!work.isEmpty()) list->setCurrentRow(0);

    auto *bb = new QDialogButtonBox;
    bb->addButton(TR("OK"), QDialogButtonBox::AcceptRole);
    bb->addButton(TR("Cancel"), QDialogButtonBox::RejectRole);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    if (qtgtk::execModal(&dlg) != QDialog::Accepted) return false;
    commit();
    servers->clear();
    QStringList seen;
    for (McpServerCfg m : std::as_const(work)) {
        if (m.name.isEmpty() || m.target.isEmpty() || seen.contains(m.name)) continue;
        seen << m.name;
        *servers << m;
    }
    return true;
}

/* ── Progress ────────────────────────────────────────────────────────────── */

ProgressDialog::ProgressDialog()
{
    setWindowTitle(TR("Ask AI Agent"));
    resize(640, 360);
    auto *lay = new QVBoxLayout(this);
    m_status = new QLabel(TR("The agent is working…"));
    QFont f = m_status->font();
    f.setBold(true);
    m_status->setFont(f);
    m_status->setWordWrap(true);
    lay->addWidget(m_status);
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    lay->addWidget(m_log, 1);
    m_button = new QPushButton(TR("Cancel"));
    lay->addWidget(m_button, 0, Qt::AlignRight);
    connect(m_button, &QPushButton::clicked, this, [this] { reject(); });
}

void ProgressDialog::append(const QString &line) { m_log->appendPlainText(line); }

void ProgressDialog::setStatus(const QString &text) { m_status->setText(text); }

void ProgressDialog::setRunning(bool running)
{
    m_running = running;
    m_button->setText(running ? TR("Cancel") : TR("Close"));
}

void ProgressDialog::reject()
{
    if (m_running) {
        if (onCancel) onCancel();
        return;
    }
    QDialog::reject();
}

} // namespace ai
