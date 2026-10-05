// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#pragma once

#include "Rendering/PageLayout.h"
#include "Widgets/ShortcutsDialog.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QString>
#include <QThreadPool>
#include <QTimer>

#include <array>
#include <atomic>
#include <memory>
#include <optional>

class CodeEditor;
class FileTree;
class FindBar;
class QLineEdit;
class QAction;
class QProgressBar;
class QActionGroup;
class QSplitter;
class PreviewFindBar;
class TocPanel;
class PreviewWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT
    public:
    MainWindow();
    ~MainWindow() override;

    void openFile(const QString &path);

    protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

    private slots:
    void open();
    bool save();
    bool saveAs();
    void exportPdf();
    void exportBookmarks();
    void relayout(); // lay out again now, and show it as soon as it's done
    void openFolder();

    private:
    // A finished layout of the text as it was when its build started.
    struct Rendered
    {
        std::shared_ptr<const scribe::PageLayout> layout;
        QString title;
        QString warning;
        QList<scribe::TocEntry> toc;
        int generation = 0;
    };

    void startBuild(bool showAtOnce);
    void buildFinished(const Rendered &result);
    void showPending();
    void present(const Rendered &result);
    void setupActions();
    bool writeFile(const QString &path);
    bool maybeSave();
    void setCurrentFile(const QString &path);
    QString documentDir() const;
    void updateWindowTitle();
    scribe::Theme theme() const;
    void setTreeFolder(const QString &folder);
    void openFromTree(const QString &path, bool focusEditor);
    void showTree(bool show);
    void goToLine(int line);
    void applyPanelShares();
    void storePanelShares();
    bool writePdf(const scribe::PageLayout &layout, const QString &title,
                  const QString &path,
                  const std::function<void(int done, int total)> &pageDone = {});
    // A long PDF export: a bar in the status bar, shown once it has taken
    // a moment; the window repaints meanwhile but takes no input. Value
    // out of kExportSteps, or negative for "busy".
    void exportProgress(int value);
    static constexpr int kExportSteps = 1000;
    QProgressBar *exportBar_ = nullptr;
    QElapsedTimer exportTimer_;
    bool exporting_ = false;
    QAction *ghostscriptAction_ = nullptr;
    QAction *undoAction_ = nullptr;
    QAction *redoAction_ = nullptr;
    QAction *cutAction_ = nullptr;
    QAction *copyAction_ = nullptr;
    QAction *pasteAction_ = nullptr;
    QAction *deleteAction_ = nullptr;
    QAction *selectAllAction_ = nullptr;
    QList<QMetaObject::Connection> fieldConnections_; // of the watched field
    bool vimCommand(const QString &name);
    bool previewHasFocus() const;

    // Edit > Undo ... Select All act on the side with the keyboard: the
    // editor, the preview (Copy, Select All) or a text field.
    enum class EditCommand
    {
        Undo,
        Redo,
        Cut,
        Copy,
        Paste,
        Delete,
        SelectAll
    };
    void editCommand(EditCommand command);
    void updateEditActions();
    void watchField(QLineEdit *field);

    // QSettings keys of the Options menu.
    static constexpr const char *kThemeKey = "themes/name";
    static constexpr const char *kRemasterKey = "options/remasterColors"; // old
    static constexpr const char *kCompactKey = "options/compactSpacing";
    static constexpr const char *kHighlightKey = "options/syntaxHighlighting";
    static constexpr const char *kGhostscriptKey = "options/ghostscriptExport";
    static constexpr const char *kVimKey = "options/vimMotions";
    static constexpr const char *kTreeFolderKey = "fileTree/folder";
    static constexpr const char *kTreeVisibleKey = "fileTree/visible";
    static constexpr const char *kTocVisibleKey = "contents/visible";
    static constexpr const char *kPanelSharesKey = "window/panelShares";

    CodeEditor *editor_ = nullptr;
    FindBar *findBar_ = nullptr;
    FileTree *fileTree_ = nullptr;
    QAction *treeAction_ = nullptr;
    QList<ShortcutEntry> shortcuts_; // for Settings > Keyboard Shortcuts
    QActionGroup *themeGroup_ = nullptr; // Themes menu, one checked
    QAction *compactAction_ = nullptr;
    QAction *vimAction_ = nullptr;
    PreviewWidget *preview_ = nullptr;
    PreviewFindBar *previewFind_ = nullptr;
    TocPanel *tocPanel_ = nullptr;
    QAction *tocAction_ = nullptr;
    QWidget *previewPane_ = nullptr; // the preview and its find bar
    QSplitter *splitter_ = nullptr;  // tree | editor | contents | preview
    // Each panel's share of the width, in the splitter's order. The defaults
    // were measured on a maximised 2560 px wide window.
    std::array<double, 4> panelShares_{0.0675, 0.505, 0.0925, 0.335};

    // Layouts are built on worker threads while the preview keeps showing
    // the last one, which is swapped for the new one when typing pauses.
    QThreadPool builders_;
    QTimer buildTimer_; // a short pause after a keystroke before building
    QTimer swapTimer_;  // shows a finished build once typing has paused
    QElapsedTimer sinceEdit_;
    int generation_ = 0;       // of the newest build started
    int showAtOnce_ = -1;      // a build to show without waiting for a pause
    std::shared_ptr<std::atomic<bool>> cancel_; // of the newest build
    std::optional<Rendered> pending_;           // done, waiting for a pause

    std::shared_ptr<const scribe::PageLayout> layout_;
    QString currentFile_;
    QString docTitle_;
};
