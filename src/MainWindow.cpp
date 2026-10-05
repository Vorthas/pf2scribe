// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#include "MainWindow.h"

#include "Editor/CodeEditor.h"
#include "Editor/FindBar.h"
#include "Rendering/PdfOutline.h"
#include "Rendering/ScribeParser.h"
#include "VimEngine/VimEngine.h"
#include "Widgets/FileTree.h"
#include "Widgets/IconBrowser.h"
#include "Widgets/PreviewFindBar.h"
#include "Widgets/PreviewWidget.h"
#include "Widgets/TocPanel.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QScopeGuard>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QVBoxLayout>

namespace
{
    // After a keystroke, how long to wait before starting a layout, and how
    // long typing must have paused before a finished one is shown (ms).
    constexpr int kBuildDelay = 100;
    constexpr qint64 kQuietBeforeSwap = 300;

    const char *kStarterText = R"(watermark (
Scribe for Pathfinder 2e: text that appears at the top of every page.
)
title (
Untitled
)
pagenumbers
head (
# Hello ((Title))
This block spans the full page width.
-
)
Text before a lone `|` line goes in the first column.
|
Text after it goes in the second. Add more `|` lines for more columns.
/
A lone `/` ends a band of columns, so this paragraph spans the full width.
=
A lone `=` starts a new page.
)";

} // namespace

MainWindow::MainWindow()
{
    editor_ = new CodeEditor;
    editor_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    editor_->setPlainText(QString::fromUtf8(kStarterText));
    editor_->document()->setModified(false);

    // Vim motions: ":w" and ":q" go through the window, so quitting with
    // unsaved changes always asks first.
    VimEngine *vim = editor_->vim();
    vim->setSaveHandler([this] { return save(); });
    vim->setQuitHandler([this] { QTimer::singleShot(0, this, &QWidget::close); });
    // ":NERDTree..." (from the user's vimrc mappings) drives the file tree.
    vim->setCommandHandler(
        [this](const QString &name, const QString &) { return vimCommand(name); });
    vim->loadVimrc();

    // The editor with Vim's status line under it.
    auto *editorPane = new QWidget;
    auto *editorLayout = new QVBoxLayout(editorPane);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(0);
    findBar_ = new FindBar(editor_);
    editorLayout->addWidget(editor_);
    editorLayout->addWidget(findBar_);
    editorLayout->addWidget(new VimStatusLine(vim));

    // The preview with its own find bar under it.
    preview_ = new PreviewWidget;
    previewFind_ = new PreviewFindBar(preview_);
    connect(preview_, &PreviewWidget::findRequested, previewFind_,
            &PreviewFindBar::openFind);
    previewPane_ = new QWidget;
    auto *previewLayout = new QVBoxLayout(previewPane_);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(0);
    previewLayout->addWidget(preview_);
    previewLayout->addWidget(previewFind_);

    fileTree_ = new FileTree;
    connect(fileTree_, &FileTree::fileChosen, this, &MainWindow::openFromTree);
    connect(fileTree_, &FileTree::openFolderRequested, this, &MainWindow::openFolder);
    connect(fileTree_, &FileTree::leaveRequested, this,
            [this] { editor_->setFocus(); });

    // The table of contents, between the editor and the preview.
    tocPanel_ = new TocPanel;
    connect(tocPanel_, &TocPanel::entryChosen, preview_,
            &PreviewWidget::scrollToPoint);
    connect(tocPanel_, &TocPanel::lineChosen, this, &MainWindow::goToLine);
    connect(tocPanel_, &TocPanel::leaveRequested, this,
            [this] { editor_->setFocus(); });

    splitter_ = new QSplitter;
    splitter_->addWidget(fileTree_);
    splitter_->addWidget(editorPane);
    splitter_->addWidget(tocPanel_);
    splitter_->addWidget(previewPane_);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    splitter_->setStretchFactor(2, 0);
    splitter_->setStretchFactor(3, 1);
    setCentralWidget(splitter_);

    exportBar_ = new QProgressBar;
    exportBar_->setMaximumWidth(240);
    exportBar_->setTextVisible(false);
    exportBar_->hide();
    statusBar()->addPermanentWidget(exportBar_);
    // The panels keep their shares of the width through resizes, maximising
    // included. Dragging a handle changes the shares, which are saved.
    const QVariantList saved = QSettings().value(kPanelSharesKey).toList();
    if (saved.size() == static_cast<int>(panelShares_.size()))
    {
        std::array<double, 4> shares{};
        double sum = 0;
        bool ok = true;
        for (size_t i = 0; i < shares.size() && ok; ++i)
        {
            shares[i] = saved.at(static_cast<int>(i)).toDouble(&ok);
            ok = ok && shares[i] > 0 && shares[i] < 1;
            sum += shares[i];
        }
        if (ok && sum > 0.5 && sum < 1.5)
        {
            panelShares_ = shares;
        }
    }
    splitter_->installEventFilter(this);
    connect(splitter_, &QSplitter::splitterMoved, this, [this] {
        storePanelShares();
        // Dragging the tree or the contents shut is the same as hiding them.
        if (splitter_->sizes().value(0) == 0 && treeAction_->isChecked())
        {
            treeAction_->setChecked(false);
        }
        if (splitter_->sizes().value(2) == 0 && tocAction_->isChecked())
        {
            tocAction_->setChecked(false);
        }
    });

    const QString folder = QSettings().value(kTreeFolderKey).toString();
    if (!folder.isEmpty() && QFileInfo(folder).isDir())
    {
        fileTree_->setRoot(folder);
    }

    // One build thread for the whole session: layouts it built keep using
    // its font engines, which die with the thread (see textEngineMutex()).
    // A new build waits for a cancelled one, which stops within a band.
    builders_.setMaxThreadCount(1);
    builders_.setExpiryTimeout(-1);
    buildTimer_.setSingleShot(true);
    buildTimer_.setInterval(kBuildDelay);
    connect(&buildTimer_, &QTimer::timeout, this, [this] { startBuild(false); });
    swapTimer_.setSingleShot(true);
    connect(&swapTimer_, &QTimer::timeout, this, &MainWindow::showPending);
    sinceEdit_.start();
    connect(editor_, &QPlainTextEdit::textChanged, this, [this] {
        // Anything built or being built is out of date now.
        sinceEdit_.restart();
        if (cancel_)
        {
            cancel_->store(true);
        }
        pending_.reset();
        swapTimer_.stop();
        buildTimer_.start();
    });
    connect(editor_->document(), &QTextDocument::modificationChanged, this,
            [this] { updateWindowTitle(); });

    setupActions();
    resize(1300, 850);
    updateWindowTitle();
    relayout();
}

void MainWindow::setupActions()
{
    // Every action with a shortcut is listed in Settings > Keyboard
    // Shortcuts, with these defaults.
    auto shortcut = [this](QAction *action, const QString &id,
                           const QList<QKeySequence> &defaults) {
        shortcuts_.append({id, action, defaults});
    };
    auto standard = [](QKeySequence::StandardKey key) {
        return QKeySequence::keyBindings(key).mid(0, 2);
    };

    QMenu *file = menuBar()->addMenu(tr("&File"));
    shortcut(file->addAction(tr("&Open..."), this, &MainWindow::open),
             QStringLiteral("file.open"), standard(QKeySequence::Open));
    shortcut(file->addAction(tr("Open &Folder..."), this, &MainWindow::openFolder),
             QStringLiteral("file.openFolder"), {});
    file->addSeparator();
    shortcut(file->addAction(tr("&Save"), this, &MainWindow::save),
             QStringLiteral("file.save"), standard(QKeySequence::Save));
    shortcut(file->addAction(tr("Save &As..."), this, &MainWindow::saveAs),
             QStringLiteral("file.saveAs"), standard(QKeySequence::SaveAs));
    file->addSeparator();
    shortcut(file->addAction(tr("&Export PDF..."), this, &MainWindow::exportPdf),
             QStringLiteral("file.exportPdf"),
             {QKeySequence(Qt::CTRL | Qt::Key_E)});
    shortcut(file->addAction(tr("Export &Bookmarks (index.info)..."), this,
                             &MainWindow::exportBookmarks),
             QStringLiteral("file.exportBookmarks"), {});
    file->addSeparator();
    shortcut(file->addAction(tr("&Quit"), this, &QWidget::close),
             QStringLiteral("file.quit"), standard(QKeySequence::Quit));

    QMenu *edit = menuBar()->addMenu(tr("&Edit"));
    auto editAction = [&](const QString &text, EditCommand command,
                          const QString &id, const QList<QKeySequence> &keys) {
        QAction *action = edit->addAction(text, this,
                                          [this, command] { editCommand(command); });
        shortcut(action, id, keys);
        return action;
    };
    undoAction_ = editAction(tr("&Undo"), EditCommand::Undo,
                             QStringLiteral("edit.undo"),
                             {QKeySequence(Qt::CTRL | Qt::Key_Z)});
    redoAction_ = editAction(tr("&Redo"), EditCommand::Redo,
                             QStringLiteral("edit.redo"),
                             {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z),
                              QKeySequence(Qt::CTRL | Qt::Key_Y)});
    edit->addSeparator();
    cutAction_ = editAction(tr("Cu&t"), EditCommand::Cut, QStringLiteral("edit.cut"),
                            {QKeySequence(Qt::CTRL | Qt::Key_X)});
    copyAction_ = editAction(tr("&Copy"), EditCommand::Copy, QStringLiteral("edit.copy"),
                             {QKeySequence(Qt::CTRL | Qt::Key_C)});
    pasteAction_ = editAction(tr("&Paste"), EditCommand::Paste,
                              QStringLiteral("edit.paste"),
                              {QKeySequence(Qt::CTRL | Qt::Key_V)});
    deleteAction_ = editAction(tr("&Delete"), EditCommand::Delete,
                               QStringLiteral("edit.delete"),
                               {QKeySequence(Qt::Key_Delete)});
    edit->addSeparator();
    selectAllAction_ = editAction(tr("Select &All"), EditCommand::SelectAll,
                                  QStringLiteral("edit.selectAll"),
                                  {QKeySequence(Qt::CTRL | Qt::Key_A)});
    edit->addSeparator();
    editor_->setVimReservedActions({undoAction_, redoAction_, cutAction_, copyAction_,
                                    pasteAction_, deleteAction_, selectAllAction_});
    preview_->setContextActions({copyAction_, selectAllAction_});
    // Their state follows the side with the keyboard and what it can do.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        watchField(qobject_cast<QLineEdit *>(now));
        updateEditActions();
    });
    for (auto signal : {&QPlainTextEdit::undoAvailable, &QPlainTextEdit::redoAvailable,
                        &QPlainTextEdit::copyAvailable})
    {
        connect(editor_, signal, this, &MainWindow::updateEditActions);
    }
    connect(editor_, &QPlainTextEdit::textChanged, this, &MainWindow::updateEditActions);
    connect(editor_, &QPlainTextEdit::selectionChanged, this,
            &MainWindow::updateEditActions);
    connect(preview_, &PreviewWidget::selectionChanged, this,
            &MainWindow::updateEditActions);
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this,
            &MainWindow::updateEditActions);
    connect(edit, &QMenu::aboutToShow, this, &MainWindow::updateEditActions);
    connect(editor_->vim(), &VimEngine::stateChanged, this,
            &MainWindow::updateEditActions);
    // Find, Find Next and Find Previous search whichever side has focus:
    // the markdown, or the rendered pages.
    shortcut(edit->addAction(tr("&Find..."), this,
                             [this] {
                                 if (previewHasFocus())
                                     previewFind_->openFind();
                                 else
                                     findBar_->openFind();
                             }),
             QStringLiteral("edit.find"), {QKeySequence(Qt::CTRL | Qt::Key_F)});
    shortcut(edit->addAction(tr("Find and &Replace..."), findBar_,
                             &FindBar::openReplace),
             QStringLiteral("edit.replace"),
             {QKeySequence(Qt::CTRL | Qt::Key_R),
              QKeySequence(Qt::CTRL | Qt::Key_H)});
    shortcut(edit->addAction(tr("Find &Next"), this,
                             [this] {
                                 if (previewHasFocus())
                                     previewFind_->findNext();
                                 else
                                     findBar_->findNext();
                             }),
             QStringLiteral("edit.findNext"), {QKeySequence(Qt::Key_F3)});
    shortcut(edit->addAction(tr("Find &Previous"), this,
                             [this] {
                                 if (previewHasFocus())
                                     previewFind_->findPrevious();
                                 else
                                     findBar_->findPrevious();
                             }),
             QStringLiteral("edit.findPrevious"),
             {QKeySequence(Qt::SHIFT | Qt::Key_F3)});

    QMenu *view = menuBar()->addMenu(tr("&View"));
    treeAction_ = view->addAction(tr("File &Tree"));
    treeAction_->setCheckable(true);
    treeAction_->setChecked(QSettings().value(kTreeVisibleKey, true).toBool());
    fileTree_->setVisible(treeAction_->isChecked());
    connect(treeAction_, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(kTreeVisibleKey, on);
        fileTree_->setVisible(on);
        applyPanelShares(); // also back from being dragged shut
    });
    shortcut(treeAction_, QStringLiteral("view.fileTree"),
             {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E)});
    tocAction_ = view->addAction(tr("&Contents"));
    tocAction_->setCheckable(true);
    tocAction_->setChecked(QSettings().value(kTocVisibleKey, true).toBool());
    tocPanel_->setVisible(tocAction_->isChecked());
    connect(tocAction_, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(kTocVisibleKey, on);
        tocPanel_->setVisible(on);
        applyPanelShares(); // also back from being dragged shut
        if (on)
        {
            tocPanel_->focusTree();
        }
        else if (tocPanel_->isAncestorOf(QApplication::focusWidget()))
        {
            editor_->setFocus();
        }
    });
    shortcut(tocAction_, QStringLiteral("view.contents"),
             {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O)});
    view->addSeparator();
    shortcut(view->addAction(tr("Zoom &In"), this,
                             [this] { preview_->setZoom(preview_->zoom() * 1.1); }),
             QStringLiteral("view.zoomIn"), standard(QKeySequence::ZoomIn));
    shortcut(view->addAction(tr("Zoom &Out"), this,
                             [this] { preview_->setZoom(preview_->zoom() / 1.1); }),
             QStringLiteral("view.zoomOut"), standard(QKeySequence::ZoomOut));
    shortcut(view->addAction(tr("&Actual Size"), this,
                             [this] { preview_->setZoom(1.0); }),
             QStringLiteral("view.actualSize"), {QKeySequence(Qt::CTRL | Qt::Key_0)});
#ifndef NDEBUG
    // A look at the icon font's glyphs, for debugging; Debug builds only.
    view->addSeparator();
    view->addAction(tr("Icon Font &Glyphs..."), this, [this] {
        IconBrowser dialog(theme(), this);
        dialog.exec();
    });
#endif

    // One colour theme at a time. The old Options > Remaster Colors
    // setting becomes the Remaster theme the first time.
    QSettings saved;
    if (!saved.contains(kThemeKey) && saved.contains(kRemasterKey))
    {
        saved.setValue(kThemeKey, saved.value(kRemasterKey).toBool()
                                      ? QStringLiteral("remaster")
                                      : QStringLiteral("pathfinder"));
        saved.remove(kRemasterKey);
    }
    const QString chosen = saved.value(kThemeKey).toString();
    QMenu *themes = menuBar()->addMenu(tr("&Themes"));
    themeGroup_ = new QActionGroup(this);
    for (const scribe::Theme::Preset &preset : scribe::Theme::presets())
    {
        QAction *action = themes->addAction(
            QCoreApplication::translate("Theme", preset.name));
        action->setCheckable(true);
        action->setData(QString::fromLatin1(preset.key));
        themeGroup_->addAction(action);
        shortcut(action, QStringLiteral("themes.") + QLatin1String(preset.key),
                 {});
    }
    QAction *current = themeGroup_->actions().constFirst();
    for (QAction *action : themeGroup_->actions())
    {
        if (action->data().toString() == chosen)
        {
            current = action;
        }
    }
    current->setChecked(true);
    connect(themeGroup_, &QActionGroup::triggered, this, [this](QAction *action) {
        QSettings().setValue(kThemeKey, action->data());
        relayout();
    });

    QMenu *options = menuBar()->addMenu(tr("&Options"));
    compactAction_ = options->addAction(tr("&Compact Line Spacing"));
    compactAction_->setCheckable(true);
    compactAction_->setToolTip(
        tr("Body text and stat blocks with the line spacing of the printed "
           "rulebooks, tighter than the Scribe website's"));
    compactAction_->setChecked(QSettings().value(kCompactKey, false).toBool());
    connect(compactAction_, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(kCompactKey, on);
        relayout();
    });

    auto *highlightAction = options->addAction(tr("&Syntax Highlighting"));
    highlightAction->setCheckable(true);
    highlightAction->setToolTip(
        tr("Colour Markdown and Scribe's own syntax in the editor"));
    highlightAction->setChecked(QSettings().value(kHighlightKey, true).toBool());
    editor_->setSyntaxHighlighting(highlightAction->isChecked());
    connect(highlightAction, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(kHighlightKey, on);
        editor_->setSyntaxHighlighting(on);
    });

    // Optional: Ghostscript rewrites the PDF smaller (images compressed
    // again) and adds the bookmarks itself.
    ghostscriptAction_ = options->addAction(tr("Shrink PDFs with &Ghostscript"));
    ghostscriptAction_->setCheckable(true);
    const bool haveGs =
        !QStandardPaths::findExecutable(QStringLiteral("gs")).isEmpty();
    ghostscriptAction_->setEnabled(haveGs);
    ghostscriptAction_->setToolTip(
        haveGs ? tr("Exported PDFs go through Ghostscript: smaller files, "
                    "images compressed once more")
               : tr("Needs Ghostscript (gs), which isn't installed"));
    ghostscriptAction_->setChecked(haveGs &&
                                   QSettings().value(kGhostscriptKey, false).toBool());
    connect(ghostscriptAction_, &QAction::toggled, this,
            [](bool on) { QSettings().setValue(kGhostscriptKey, on); });

    vimAction_ = options->addAction(tr("&Vim Mode"));
    vimAction_->setCheckable(true);
    connect(vimAction_, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(kVimKey, on);
        editor_->setVimEnabled(on);
        editor_->setFocus();
    });
    vimAction_->setChecked(QSettings().value(kVimKey, false).toBool());

    QMenu *settings = menuBar()->addMenu(tr("&Settings"));
    settings->addAction(tr("&Keyboard Shortcuts..."), this, [this] {
        ShortcutsDialog dialog(shortcuts_, this);
        dialog.exec();
    });

    applySavedShortcuts(shortcuts_);
    updateEditActions();
}

MainWindow::~MainWindow()
{
    if (cancel_)
    {
        cancel_->store(true);
    }
    builders_.waitForDone();
    // Layouts go before the build thread does (with builders_), as they
    // use its font engines; the preview is only deleted after that.
    preview_->setPageLayout(nullptr);
    pending_.reset();
    layout_.reset();
}

void MainWindow::relayout()
{
    startBuild(true);
}

// Lays out a snapshot of the text on a worker thread, cancelling the build
// before it. `showAtOnce`: show the result as soon as it's done, instead of
// waiting for typing to pause (opening a file, changing colours, ...).
void MainWindow::startBuild(bool showAtOnce)
{
    buildTimer_.stop();
    swapTimer_.stop();
    pending_.reset();
    if (cancel_)
    {
        cancel_->store(true);
    }
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    const int generation = ++generation_;
    if (showAtOnce)
    {
        showAtOnce_ = generation;
    }

    builders_.start([this, generation, cancel = cancel_,
                     text = editor_->toPlainText(), theme = theme(),
                     dir = documentDir()] {
        const scribe::Document doc = scribe::parse(text);
        std::shared_ptr<scribe::PageLayout> layout = scribe::PageLayout::build(
            doc, scribe::PageSpec{}, theme, dir, cancel.get());
        if (!layout)
        {
            return; // cancelled
        }
        layout->moveToThread(QCoreApplication::instance()->thread());
        const Rendered result{std::move(layout), doc.title.simplified(),
                              doc.warnings.value(0), doc.toc, generation};
        QMetaObject::invokeMethod(
            this, [this, result] { buildFinished(result); },
            Qt::QueuedConnection);
    });
}

void MainWindow::buildFinished(const Rendered &result)
{
    if (result.generation != generation_)
    {
        return; // a newer build is on its way
    }
    pending_ = result;
    showPending();
}

// Shows the finished build, once there has been no typing for a moment, so
// the preview doesn't change under a sentence being written.
void MainWindow::showPending()
{
    if (!pending_)
    {
        return;
    }
    const qint64 quiet = sinceEdit_.elapsed();
    if (pending_->generation != showAtOnce_ && quiet < kQuietBeforeSwap)
    {
        swapTimer_.start(static_cast<int>(kQuietBeforeSwap - quiet));
        return;
    }
    const Rendered result = *pending_;
    pending_.reset();
    present(result);
}

void MainWindow::present(const Rendered &result)
{
    layout_ = result.layout; // the previous layout is freed here
    preview_->setPageLayout(layout_);
    tocPanel_->setContents(result.toc, layout_->tocPoints());

    docTitle_ = result.title;
    updateWindowTitle();

    QString status =
        tr("%n page(s)", nullptr, static_cast<int>(layout_->pages().size()));
    if (!result.warning.isEmpty())
    {
        status += QStringLiteral("  |  ") + result.warning;
    }
    if (!exporting_) // keep "Exporting ..." up
    {
        statusBar()->showMessage(status);
    }
}

void MainWindow::updateWindowTitle()
{
    const QString name = currentFile_.isEmpty()
                             ? tr("untitled")
                             : QFileInfo(currentFile_).fileName();
    QString title = name;
    if (!docTitle_.isEmpty())
    {
        title += QStringLiteral(" (%1)").arg(docTitle_);
    }
    // Ends with the display name, so Qt doesn't append it a second time.
    setWindowTitle(QStringLiteral("%1%2 - %3")
                       .arg(title,
                            editor_->document()->isModified()
                                ? QStringLiteral("*")
                                : QString(),
                            QGuiApplication::applicationDisplayName()));
}

// The theme chosen under Themes, with the spacing chosen under Options.
scribe::Theme MainWindow::theme() const
{
    const QAction *checked = themeGroup_ ? themeGroup_->checkedAction() : nullptr;
    scribe::Theme theme =
        scribe::Theme::preset(checked ? checked->data().toString() : QString());
    theme.compactSpacing = compactAction_ && compactAction_->isChecked();
    return theme;
}

// Folder that relative image paths resolve against: the markdown file's, or the
// working directory for an unsaved document.
QString MainWindow::documentDir() const
{
    return currentFile_.isEmpty() ? QDir::currentPath()
                                  : QFileInfo(currentFile_).absolutePath();
}

void MainWindow::openFolder()
{
    const QString start =
        fileTree_->root().isEmpty() ? documentDir() : fileTree_->root();
    const QString folder =
        QFileDialog::getExistingDirectory(this, tr("Open Folder"), start);
    if (folder.isEmpty())
    {
        return;
    }
    setTreeFolder(folder);
    showTree(true);
}

void MainWindow::setTreeFolder(const QString &folder)
{
    fileTree_->setRoot(folder);
    QSettings().setValue(kTreeFolderKey, fileTree_->root());
    if (!currentFile_.isEmpty())
    {
        fileTree_->setCurrentFile(currentFile_);
    }
}

void MainWindow::showTree(bool show)
{
    treeAction_->setChecked(show);
}

// A file picked in the tree. The open one is saved first; an untitled
// document has nowhere to go, so that one asks.
void MainWindow::openFromTree(const QString &path, bool focusEditor)
{
    if (QFileInfo(path) != QFileInfo(currentFile_))
    {
        if (editor_->document()->isModified())
        {
            const bool kept = currentFile_.isEmpty() ? maybeSave() : save();
            if (!kept)
            {
                fileTree_->setCurrentFile(currentFile_);
                return;
            }
        }
        openFile(path);
    }
    if (focusEditor)
    {
        editor_->setFocus();
    }
}

// The NERDTree commands of the user's vimrc, for the file tree.
bool MainWindow::vimCommand(const QString &name)
{
    if (name == QLatin1String("NERDTree") || name == QLatin1String("NERDTreeFocus"))
    {
        showTree(true);
        fileTree_->focusTree();
        return true;
    }
    if (name == QLatin1String("NERDTreeToggle"))
    {
        const bool show = !treeAction_->isChecked();
        showTree(show);
        if (show)
        {
            fileTree_->focusTree();
        }
        else
        {
            editor_->setFocus();
        }
        return true;
    }
    if (name == QLatin1String("NERDTreeFind"))
    {
        showTree(true);
        if (!currentFile_.isEmpty())
        {
            fileTree_->setCurrentFile(currentFile_);
        }
        fileTree_->focusTree();
        return true;
    }
    if (name == QLatin1String("NERDTreeClose"))
    {
        showTree(false);
        editor_->setFocus();
        return true;
    }
    return false;
}

void MainWindow::setCurrentFile(const QString &path)
{
    const QString oldDir = documentDir();
    currentFile_ = path;
    editor_->document()->setModified(false);
    updateWindowTitle();
    if (fileTree_ && !path.isEmpty())
    {
        fileTree_->setCurrentFile(path); // also after Save As
    }
    if (documentDir() != oldDir)
    {
        relayout(); // images may resolve differently from the new folder
    }
}

void MainWindow::openFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
        QMessageBox::warning(
            this, tr("Open"),
            tr("Cannot read %1:\n%2").arg(path, f.errorString()));
        return;
    }
    editor_->setPlainText(QString::fromUtf8(f.readAll()));
    setCurrentFile(path);
    // The tree follows the file's folder until a folder is chosen.
    if (fileTree_->root().isEmpty())
    {
        setTreeFolder(QFileInfo(path).absolutePath());
    }
    fileTree_->setCurrentFile(path);
    tocPanel_->forgetExpanded();
    relayout();
}

void MainWindow::open()
{
    if (!maybeSave())
    {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open"), QString(),
        tr("Markdown (*.md *.markdown *.txt);;All files (*)"));
    if (!path.isEmpty())
    {
        openFile(path);
    }
}

bool MainWindow::writeFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        QMessageBox::warning(
            this, tr("Save"),
            tr("Cannot write %1:\n%2").arg(path, f.errorString()));
        return false;
    }
    f.write(editor_->toPlainText().toUtf8());
    setCurrentFile(path);
    return true;
}

bool MainWindow::save()
{
    return currentFile_.isEmpty() ? saveAs() : writeFile(currentFile_);
}

bool MainWindow::saveAs()
{
    const QString path =
        QFileDialog::getSaveFileName(this, tr("Save As"), currentFile_,
                                     tr("Markdown (*.md);;All files (*)"));
    return !path.isEmpty() && writeFile(path);
}

bool MainWindow::maybeSave()
{
    if (!editor_->document()->isModified())
    {
        return true;
    }
    const auto answer = QMessageBox::warning(
        this, tr("Unsaved changes"), tr("Save changes before continuing?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Save)
    {
        return save();
    }
    return answer == QMessageBox::Discard;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (exporting_)
    {
        event->ignore(); // wait for the export
        return;
    }
    if (maybeSave())
    {
        event->accept();
    }
    else
    {
        event->ignore();
    }
}

namespace
{
    // The bookmarks of a document: its table of contents, at its headings.
    std::vector<scribe::OutlineEntry> outlineOf(const scribe::Document &doc,
                                                const scribe::PageLayout &layout)
    {
        std::vector<scribe::OutlineEntry> outline;
        const auto &points = layout.tocPoints();
        for (int i = 0; i < doc.toc.size(); ++i)
        {
            scribe::OutlineEntry entry{doc.toc.at(i).text, doc.toc.at(i).level};
            if (i < static_cast<int>(points.size()) && points[static_cast<size_t>(i)].isValid())
            {
                entry.page = points[static_cast<size_t>(i)].page;
                entry.y = layout.caretRect(points[static_cast<size_t>(i)]).top();
            }
            outline.push_back(entry);
        }
        return outline;
    }

    // The page size as the PDF writer stores it (whole points).
    QPageSize pdfPageSize(const scribe::PageSpec &spec)
    {
        return QPageSize(QSizeF(spec.width, spec.height), QPageSize::Point);
    }
} // namespace

// Writes the pages of `layout` to a PDF at `path`.
bool MainWindow::writePdf(const scribe::PageLayout &layout, const QString &title,
                          const QString &path,
                          const std::function<void(int done, int total)> &pageDone)
{
    QPdfWriter writer(path);
    writer.setResolution(
        72); // 1 painter unit == 1 pt, matching the layout coordinates
    writer.setPageSize(pdfPageSize(layout.spec()));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));
    if (!title.isEmpty())
    {
        writer.setTitle(title);
    }
    QPainter painter(&writer);
    if (!painter.isActive())
    {
        return false;
    }
    const int count = static_cast<int>(layout.pages().size());
    for (int i = 0; i < count; ++i)
    {
        if (i > 0)
        {
            writer.newPage();
        }
        layout.paintPage(painter, i);
        if (pageDone)
        {
            pageDone(i + 1, count);
        }
    }
    return painter.end();
}

void MainWindow::exportProgress(int value)
{
    // Small documents are done before the bar would mean anything.
    if (exportTimer_.elapsed() < 300 && exportBar_->isHidden())
    {
        return;
    }
    if (value < 0)
    {
        exportBar_->setRange(0, 0); // busy
    }
    else
    {
        exportBar_->setRange(0, kExportSteps);
        exportBar_->setValue(value);
    }
    exportBar_->show();
    // Paint the bar (and the rest of the window) without taking input, so
    // nothing changes the document mid-export.
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void MainWindow::exportPdf()
{
    QString suggested = currentFile_.isEmpty()
                            ? QStringLiteral("scribe.pdf")
                            : QFileInfo(currentFile_).completeBaseName() +
                                  QStringLiteral(".pdf");
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export PDF"), suggested, tr("PDF (*.pdf)"));
    if (path.isEmpty())
    {
        return;
    }

    // Laid out again here, on this thread: the PDF writer reads font data
    // when it finishes, outside any paintPage(), which the background
    // layouts' fonts (see textEngineMutex()) don't allow. It's also always
    // the latest text.
    // Laying out takes about 30% of the time (the preview has already
    // decoded the images), painting the pages the rest.
    exporting_ = true;
    exportTimer_.start();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    statusBar()->showMessage(tr("Exporting %1...").arg(path));
    const auto finished = qScopeGuard([this] {
        exporting_ = false;
        exportBar_->hide();
        QApplication::restoreOverrideCursor();
    });
    constexpr int kLaidOut = kExportSteps * 3 / 10;
    const scribe::Document doc = scribe::parse(editor_->toPlainText());
    const auto layout = scribe::PageLayout::build(
        doc, scribe::PageSpec{}, theme(), documentDir(), nullptr,
        [this](int done, int total) { exportProgress(kLaidOut * done / total); });
    if (layout->pages().empty())
    {
        statusBar()->clearMessage();
        return;
    }
    const auto outline = outlineOf(doc, *layout);
    const QString title = doc.title.simplified();
    const auto pageDone = [this](int done, int total) {
        exportProgress(kLaidOut + (kExportSteps - kLaidOut) * done / total);
    };
    QString message;

    if (ghostscriptAction_->isChecked())
    {
        // Qt's PDF and the bookmarks (as pdfmark lines) go through
        // Ghostscript, which writes the final file.
        QTemporaryDir temp;
        const QString raw = temp.filePath(QStringLiteral("pages.pdf"));
        const QString marks = temp.filePath(QStringLiteral("index.info"));
        QFile marksFile(marks);
        bool ok = temp.isValid() && writePdf(*layout, title, raw, pageDone) &&
                  marksFile.open(QIODevice::WriteOnly);
        if (ok)
        {
            marksFile.write(scribe::pdfmarkOutline(
                                outline, pdfPageSize(layout->spec()).sizePoints().height())
                                .toUtf8());
            marksFile.close();
            // Waited for in an event loop, so the window keeps painting.
            exportProgress(-1);
            QProcess gs;
            QEventLoop wait;
            connect(&gs, &QProcess::finished, &wait, &QEventLoop::quit);
            connect(&gs, &QProcess::errorOccurred, &wait, &QEventLoop::quit);
            QTimer::singleShot(300000, &wait, &QEventLoop::quit);
            gs.start(QStringLiteral("gs"),
                     {QStringLiteral("-sDEVICE=pdfwrite"), QStringLiteral("-q"),
                      QStringLiteral("-dBATCH"), QStringLiteral("-dNOPAUSE"),
                      QStringLiteral("-dPDFSETTINGS=/prepress"),
                      QStringLiteral("-sOutputFile=") + path, marks,
                      QStringLiteral("-f"), raw});
            if (gs.state() != QProcess::NotRunning)
            {
                wait.exec(QEventLoop::ExcludeUserInputEvents);
            }
            if (gs.state() != QProcess::NotRunning)
            {
                gs.kill(); // timed out
                gs.waitForFinished();
            }
            ok = gs.exitStatus() == QProcess::NormalExit && gs.exitCode() == 0 &&
                 gs.error() == QProcess::UnknownError;
            if (!ok)
            {
                message = tr(" (Ghostscript failed: %1; saved without it)")
                              .arg(QString::fromLocal8Bit(gs.readAllStandardError())
                                       .simplified()
                                       .left(120));
            }
        }
        if (ok)
        {
            statusBar()->showMessage(tr("Exported %1 (Ghostscript)").arg(path), 5000);
            return;
        }
    }

    if (!writePdf(*layout, title, path, pageDone))
    {
        statusBar()->clearMessage();
        QMessageBox::warning(this, tr("Export PDF"), tr("Cannot write %1").arg(path));
        return;
    }
    QString error;
    if (!scribe::addPdfOutline(path, outline, &error))
    {
        message += tr(" (no bookmarks: %1)").arg(error);
    }
    statusBar()->showMessage(tr("Exported %1").arg(path) + message, 8000);
}

// The bookmarks as Ghostscript pdfmark lines, for applying to a PDF by hand
// (gs -sDEVICE=pdfwrite -sOutputFile=out.pdf index.info -f in.pdf).
void MainWindow::exportBookmarks()
{
    const QString base = currentFile_.isEmpty()
                             ? QDir::currentPath()
                             : QFileInfo(currentFile_).absolutePath();
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export Bookmarks"), QDir(base).filePath(QStringLiteral("index.info")),
        tr("pdfmark files (*.info);;All files (*)"));
    if (path.isEmpty())
    {
        return;
    }
    const scribe::Document doc = scribe::parse(editor_->toPlainText());
    const auto layout = scribe::PageLayout::build(doc, scribe::PageSpec{},
                                                  theme(), documentDir());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        QMessageBox::warning(this, tr("Export Bookmarks"),
                             tr("Cannot write %1:\n%2").arg(path, file.errorString()));
        return;
    }
    file.write(scribe::pdfmarkOutline(outlineOf(doc, *layout),
                                      pdfPageSize(layout->spec()).sizePoints().height())
                   .toUtf8());
    statusBar()->showMessage(
        tr("Saved %n bookmark(s) to %1", nullptr, static_cast<int>(doc.toc.size())).arg(path),
        5000);
}

// True while the preview or its find bar has the keyboard.
bool MainWindow::previewHasFocus() const
{
    const QWidget *focus = QApplication::focusWidget();
    return focus && (focus == previewPane_ || previewPane_->isAncestorOf(focus));
}

// Sizes the panels from their shares of the width. A hidden panel's share
// goes to the others in proportion, and comes back when it is shown again.
void MainWindow::applyPanelShares()
{
    double visible = 0;
    int handles = -1;
    for (int i = 0; i < splitter_->count(); ++i)
    {
        if (splitter_->widget(i)->isVisibleTo(splitter_))
        {
            visible += panelShares_[static_cast<size_t>(i)];
            ++handles;
        }
    }
    if (visible <= 0)
    {
        return;
    }
    const int width =
        splitter_->width() - std::max(handles, 0) * splitter_->handleWidth();
    QList<int> sizes;
    for (int i = 0; i < splitter_->count(); ++i)
    {
        sizes << (splitter_->widget(i)->isVisibleTo(splitter_)
                      ? qRound(panelShares_[static_cast<size_t>(i)] / visible * width)
                      : 0);
    }
    splitter_->setSizes(sizes);
}

// After a drag: the visible panels' new shares, in the same total they had,
// so hidden panels keep theirs. A panel dragged shut keeps its old share for
// when it is shown again.
void MainWindow::storePanelShares()
{
    const QList<int> sizes = splitter_->sizes();
    double visible = 0;
    int total = 0;
    for (int i = 0; i < sizes.size(); ++i)
    {
        if (sizes[i] > 0)
        {
            visible += panelShares_[static_cast<size_t>(i)];
            total += sizes[i];
        }
    }
    if (total <= 0)
    {
        return;
    }
    QVariantList saved;
    for (int i = 0; i < sizes.size(); ++i)
    {
        if (sizes[i] > 0)
        {
            panelShares_[static_cast<size_t>(i)] = visible * sizes[i] / total;
        }
        saved << panelShares_[static_cast<size_t>(i)];
    }
    QSettings().setValue(kPanelSharesKey, saved);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == splitter_ && event->type() == QEvent::Resize)
    {
        applyPanelShares();
    }
    return QMainWindow::eventFilter(watched, event);
}

// Puts the editor's cursor at the start of `line` (from 0), in the middle of
// the view.
void MainWindow::goToLine(int line)
{
    const QTextBlock block = editor_->document()->findBlockByNumber(line);
    if (!block.isValid())
    {
        return;
    }
    editor_->setTextCursor(QTextCursor(block));
    editor_->centerCursor();
    editor_->setFocus();
}

// Follows the focused text field's state for the Edit actions.
void MainWindow::watchField(QLineEdit *field)
{
    for (const QMetaObject::Connection &connection : std::as_const(fieldConnections_))
    {
        disconnect(connection);
    }
    fieldConnections_.clear();
    if (field)
    {
        fieldConnections_
            << connect(field, &QLineEdit::textChanged, this, &MainWindow::updateEditActions)
            << connect(field, &QLineEdit::selectionChanged, this,
                       &MainWindow::updateEditActions);
    }
}

void MainWindow::updateEditActions()
{
    if (!undoAction_)
    {
        return;
    }
    QWidget *focus = QApplication::focusWidget();
    auto *field = qobject_cast<QLineEdit *>(focus);
    bool undo = false, redo = false, cut = false, copy = false, paste = false,
         remove = false, all = false;
    const bool clipboardText = !QGuiApplication::clipboard()->text().isEmpty();
    if (field)
    {
        const bool selected = field->hasSelectedText();
        const bool editable = !field->isReadOnly();
        undo = editable && field->isUndoAvailable();
        redo = editable && field->isRedoAvailable();
        cut = remove = editable && selected && field->echoMode() == QLineEdit::Normal;
        copy = selected && field->echoMode() == QLineEdit::Normal;
        paste = editable && clipboardText;
        all = !field->text().isEmpty();
    }
    else if (focus == editor_)
    {
        // In Vim mode a selection is Visual mode.
        const bool selected = editor_->textCursor().hasSelection() ||
                              (editor_->vim()->isEnabled() &&
                               editor_->vim()->inVisualMode());
        undo = editor_->document()->isUndoAvailable();
        redo = editor_->document()->isRedoAvailable();
        cut = copy = remove = selected;
        paste = editor_->canPaste();
        all = !editor_->document()->isEmpty();
    }
    else if (previewHasFocus())
    {
        copy = preview_->hasSelection();
        all = preview_->pageLayout() != nullptr;
    }
    undoAction_->setEnabled(undo);
    redoAction_->setEnabled(redo);
    cutAction_->setEnabled(cut);
    copyAction_->setEnabled(copy);
    pasteAction_->setEnabled(paste);
    deleteAction_->setEnabled(remove);
    selectAllAction_->setEnabled(all);
}

void MainWindow::editCommand(EditCommand command)
{
    QWidget *focus = QApplication::focusWidget();
    if (auto *field = qobject_cast<QLineEdit *>(focus))
    {
        switch (command)
        {
        case EditCommand::Undo: field->undo(); break;
        case EditCommand::Redo: field->redo(); break;
        case EditCommand::Cut: field->cut(); break;
        case EditCommand::Copy: field->copy(); break;
        case EditCommand::Paste: field->paste(); break;
        case EditCommand::Delete: field->del(); break;
        case EditCommand::SelectAll: field->selectAll(); break;
        }
    }
    else if (focus == editor_ && editor_->vim()->isEnabled())
    {
        // Vim's own commands, so its registers and undo steps stay right:
        // "+ is the system clipboard, "_ discards.
        VimEngine *vim = editor_->vim();
        const bool insert = vim->mode() == VimEngine::Mode::Insert ||
                            vim->mode() == VimEngine::Mode::Replace;
        const QString leave = vim->mode() == VimEngine::Mode::Normal ? QString()
                                                                    : QStringLiteral("<Esc>");
        switch (command)
        {
        case EditCommand::Undo: vim->runKeys(leave + QStringLiteral("u")); break;
        case EditCommand::Redo: vim->runKeys(leave + QStringLiteral("<C-r>")); break;
        case EditCommand::Cut: vim->runKeys(QStringLiteral("\"+d")); break;
        case EditCommand::Copy: vim->runKeys(QStringLiteral("\"+y")); break;
        case EditCommand::Delete: vim->runKeys(QStringLiteral("\"_d")); break;
        case EditCommand::Paste:
            if (insert)
                editor_->insertPlainText(QGuiApplication::clipboard()->text());
            else
                vim->runKeys(QStringLiteral("\"+p"));
            break;
        case EditCommand::SelectAll: vim->runKeys(leave + QStringLiteral("ggVG")); break;
        }
    }
    else if (focus == editor_)
    {
        switch (command)
        {
        case EditCommand::Undo: editor_->undo(); break;
        case EditCommand::Redo: editor_->redo(); break;
        case EditCommand::Cut: editor_->cut(); break;
        case EditCommand::Copy: editor_->copy(); break;
        case EditCommand::Paste: editor_->paste(); break;
        case EditCommand::Delete:
        {
            QTextCursor cursor = editor_->textCursor();
            cursor.removeSelectedText();
            editor_->setTextCursor(cursor);
            break;
        }
        case EditCommand::SelectAll: editor_->selectAll(); break;
        }
    }
    else if (previewHasFocus())
    {
        if (command == EditCommand::Copy)
            preview_->copy();
        else if (command == EditCommand::SelectAll)
            preview_->selectAll();
    }
    updateEditActions();
}
