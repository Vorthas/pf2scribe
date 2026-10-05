// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>

#include <functional>

class QKeyEvent;
class QPlainTextEdit;

// Vim-style editing for a QPlainTextEdit: modes, counts, registers,
// operators with motions and text objects, visual modes, search, undo
// grouping, "." repeat, ex commands and key mappings read from the user's
// vimrc. Keys are handled as Vim key notation ("x", "<Esc>", "<C-r>").
class VimEngine : public QObject
{
    Q_OBJECT
    public:
    enum class Mode
    {
        Normal,
        Insert,
        Replace,
        Visual,
        VisualLine,
        VisualBlock,
        CommandLine
    };

    // ":set" options that are understood. Defaults are Vim's.
    struct Options
    {
        bool number = false;
        bool relativeNumber = false;
        int tabStop = 8;
        int softTabStop = 0;
        int shiftWidth = 8;
        bool expandTab = false;
        bool autoIndent = false;
        bool ignoreCase = false;
        bool smartCase = false;
        bool incSearch = false;
        bool hlSearch = false;
        bool wrapScan = true;
        QString clipboard; // "", "unnamed" or "unnamedplus"
        bool timeout = true;
        int timeoutLen = 1000;
    };

    explicit VimEngine(QPlainTextEdit *editor);

    bool isEnabled() const { return enabled_; }
    void setEnabled(bool on);
    Mode mode() const { return mode_; }
    const Options &options() const { return options_; }

    // Handles a key press; false if Vim has no use for it (modifier-only
    // keys and the like), so the editor may.
    bool handleKey(QKeyEvent *event);
    // Runs `keys` (Vim notation: "\"+y", "<Esc>u") as if typed, ignoring
    // mappings, like ":normal!". For the Edit menu.
    void runKeys(const QString &keys)
    {
        for (const QString &key : parseKeys(keys))
        {
            feed(key, false);
        }
    }
    bool inVisualMode() const
    {
        return mode_ == Mode::Visual || mode_ == Mode::VisualLine ||
               mode_ == Mode::VisualBlock;
    }
    // Whether a key that the application uses as a shortcut should go to
    // Vim instead (Ctrl+R, Ctrl+V, mapped keys, ...).
    bool wantsShortcut(QKeyEvent *event) const;

    // Reads the first vimrc found (~/.vimrc, ~/.vim/vimrc,
    // $XDG_CONFIG_HOME/vim/vimrc). Settings and mappings only; plugin
    // commands are skipped.
    void loadVimrc();
    // Path of the vimrc that was read; empty if none.
    QString vimrcPath() const { return vimrcPath_; }

    // Called for :w (return false if not saved) and :q.
    void setSaveHandler(std::function<bool()> handler);
    void setQuitHandler(std::function<void()> handler);
    // Called with an ex command Vim doesn't know (":NERDTreeToggle");
    // returns true if the application handled it.
    void setCommandHandler(
        std::function<bool(const QString &name, const QString &args)> handler);

    // For the status line.
    QString modeText() const;
    QString commandLineText() const;
    QString pendingKeys() const;
    QString message() const { return message_; }
    bool messageIsError() const { return messageIsError_; }

    // Visual selection and search matches, to be shown by the editor.
    QList<QTextEdit::ExtraSelection> extraSelections() const;

    signals:
    // Mode, status line or highlights changed.
    void stateChanged();
    // ":set" changed something the editor shows (line numbers, ...).
    void optionsChanged();

    private:
    enum class Status
    {
        Incomplete,
        Done,
        Invalid
    };

    enum class MotionType
    {
        Exclusive,
        Inclusive,
        Linewise
    };

    struct Motion
    {
        int pos = 0;
        MotionType type = MotionType::Exclusive;
        bool ok = true;
        bool keepColumn = false; // j/k: keep the wanted column
        bool wantsEnd = false;   // "$": stay at line ends after j/k
        QString key;             // the motion's key, for special cases
    };

    // A range of text an operator works on.
    struct Range
    {
        int start = 0; // charwise: [start, end); linewise: lines
        int end = 0;
        MotionType type = MotionType::Exclusive;
        bool block = false;
        int firstLine = 0, lastLine = 0; // block / linewise
        int firstCol = 0, lastCol = 0;   // block, inclusive
        bool toLineEnd = false;          // block with "$"
    };

    struct Register
    {
        enum Kind
        {
            Chars,
            Lines,
            Block
        };
        QString text; // lines: without the final newline
        Kind kind = Chars;
    };

    struct Mapping
    {
        QStringList lhs;
        QStringList rhs;
        bool noremap = true;
    };

    struct Token
    {
        QString key;
        bool remap = true;
    };

    // Keys and mappings.
    static QString keyToken(QKeyEvent *event);
    static QStringList parseKeys(const QString &text);
    QChar mapMode() const;
    void feed(const QString &key, bool remap = true);
    void processInput(bool timedOut = false);
    void dispatch(const QString &key);

    // Modes.
    void setMode(Mode mode);
    void normalKey(const QString &key);
    Status execNormal(const QStringList &keys);
    Status execVisual(const QStringList &keys);
    void insertKey(const QString &key);
    void commandLineKey(const QString &key);
    void enterInsert(Mode mode, int count);
    void leaveInsert();
    void startVisual(Mode mode);
    void exitVisual();

    // Text access.
    QTextDocument *doc() const;
    QTextCursor cursor() const;
    int pos() const;
    void setPos(int pos);
    int lineOf(int pos) const;
    int lineCount() const;
    int lineStart(int line) const;
    int lineEnd(int line) const; // position of the line's newline
    QString lineText(int line) const;
    int firstNonBlank(int line) const;
    int clampNormal(int pos) const;
    int columnOf(int pos) const { return pos - lineStart(lineOf(pos)); }
    QChar charAt(int pos) const;
    int charClass(int pos, bool bigWord) const;

    // Motions and text objects.
    Motion motion(const QStringList &keys, int &i, int count, bool forOperator,
                  Status &status);
    bool textObject(const QStringList &keys, int &i, int count, Range &range,
                    Status &status);
    int wordForward(int pos, bool bigWord, bool stopAtLineEnd) const;
    int wordEnd(int pos, bool bigWord) const;
    int wordBackward(int pos, bool bigWord) const;
    int wordEndBackward(int pos, bool bigWord) const;
    int matchPair(int pos) const;
    int findChar(int pos, const QString &target, QChar kind, int count) const;
    int moveLines(int pos, int lines);

    // Operators and edits.
    Range rangeFor(int from, const Motion &m) const;
    Range visualRange() const;
    bool applyOperator(const QString &op, Range range, QChar reg, int count);
    QString rangeText(const Range &range) const;
    void deleteRange(const Range &range, QChar reg, bool yankIt);
    void yankRange(const Range &range, QChar reg, bool isDelete);
    void shiftLines(int first, int last, int amount);
    void changeCase(const Range &range, const QString &how);
    void joinLines(int line, int count, bool spaces);
    void put(QChar reg, int count, bool before);
    void openLine(bool above);
    bool addToNumber(int line, int fromCol, int toCol, qint64 delta);
    void addToNumbers(qint64 step, bool progressive);
    QString indentOf(int line) const;
    QString makeIndent(int width) const;
    int indentWidth(const QString &indent) const;

    // Registers and the clipboard.
    void setRegister(QChar name, const Register &reg, bool isDelete);
    Register getRegister(QChar name) const;

    // Search.
    QRegularExpression compilePattern(const QString &pattern) const;
    int search(const QString &pattern, bool forward, int from, int count,
               bool showMessages);

    // :substitute.
    void substitute(int first, int last, const QString &args, bool useLastFlags);
    void runSubstitution();
    void replaceCurrentMatch();
    void finishSubstitution();
    void confirmKey(const QString &key);
    QString expandReplacement(const QString &replacement,
                              const QRegularExpressionMatch &match) const;

    // Ex commands and vimrc.
    void executeEx(const QString &command);
    bool sourceLine(const QString &line, bool fromVimrc);
    bool setOption(const QString &arg);
    // `kind`: "modes|noremap|unmap", e.g. "n|1|0" for nnoremap.
    void addMapping(const QString &kind, const QString &args);
    bool parseRange(QString &cmd, int &first, int &last, bool &given);

    // Edit grouping and repeat.
    void beginChange();
    void endChange();
    void undoRedo(bool redo, int count);

    // Messages and viewport.
    void showMessage(const QString &text, bool error = false);
    void scrollLines(int lines, bool moveCursor);
    void ensureCursorVisible();

    QPlainTextEdit *editor_;
    bool enabled_ = false;
    Mode mode_ = Mode::Normal;
    Options options_;
    QString vimrcPath_;
    std::function<bool()> saveHandler_;
    std::function<void()> quitHandler_;
    std::function<bool(const QString &, const QString &)> commandHandler_;

    // Mappings by mode: 'n', 'x', 'o', 'i', 'c'.
    QHash<QChar, QList<Mapping>> mappings_;
    QString mapLeader_ = QStringLiteral("\\");
    QList<Token> input_;
    QTimer mapTimer_;
    int mapDepth_ = 0;

    // Normal / visual command being typed.
    QStringList pending_;
    bool changeCommand_ = false;  // the command changes text ("." repeats)
    QStringList dotRequest_;      // "." keys to replay
    int wantedColumn_ = 0;
    bool wantEnd_ = false; // "$": stay at line ends

    // Visual mode.
    int visualAnchor_ = 0;
    bool visualToEnd_ = false; // block "$"
    Mode lastVisualMode_ = Mode::Visual;
    int lastVisualAnchor_ = -1, lastVisualPos_ = -1;
    int markStart_ = -1, markEnd_ = -1; // '< and '>

    // Insert mode.
    int insertCount_ = 1;
    QStringList insertKeys_;
    bool insertOpensLine_ = false;
    bool replayingInsert_ = false;
    bool waitingRegister_ = false; // <C-r> in insert / command line
    // Block insert (visual block I / A / c).
    bool blockInsert_ = false;
    int blockFirstLine_ = 0, blockLastLine_ = 0, blockColumn_ = 0;
    bool blockAppendToEnd_ = false;
    bool blockPadShort_ = false; // A pads short lines; I skips them
    int blockInsertStart_ = 0;

    // Undo grouping: an insert session joins the edit that started it.
    bool undoJoin_ = false;
    int changeDepth_ = 0;
    QTextCursor changeCursor_;
    int changeRevision_ = 0;
    // Content changes so far, and the first position changed since
    // changeMin_ was last reset (undo puts the cursor there).
    int contentChanges_ = 0;
    int changeMin_ = 0;

    // "." repeat.
    QStringList dotKeys_;
    QStringList recording_;
    bool isRecording_ = false;
    bool replaying_ = false;

    // Registers.
    QHash<QChar, Register> registers_;

    // Command line.
    QChar cmdType_; // ':', '/' or '?'
    QString cmdText_;
    Mode cmdReturnMode_ = Mode::Normal; // visual mode a search started in
    QHash<QChar, QStringList> history_;
    int historyIndex_ = -1;
    int searchOrigin_ = 0;

    // Search.
    QString lastPattern_;
    bool lastForward_ = true;
    bool highlightOn_ = false; // hlsearch, until :noh
    int incMatch_ = -1, incMatchLen_ = 0;
    QChar lastFindKind_;
    QString lastFindChar_;
    bool lastFindRepeat_ = false; // ";" / ",": skip an adjacent "t" match

    // :substitute in progress (the "c" flag waits for y/n/a/q/l).
    struct Substitution
    {
        QRegularExpression re;
        QString replacement;
        bool global = false;     // "g": every match on a line
        bool confirm = false;    // "c"
        bool countOnly = false;  // "n"
        bool quiet = false;      // "e": no error if nothing matched
        int line = 0, column = 0, lastLine = 0;
        int matchStart = 0, matchLength = 0;
        QRegularExpressionMatch match;
        int count = 0;
        QList<int> linesChanged;
        int lastChangedLine = -1;
        bool joinUndo = false;
    };
    Substitution sub_;
    bool confirming_ = false;
    QString lastSubPattern_, lastSubReplacement_, lastSubFlags_;
    bool haveLastSub_ = false;

    QString message_;
    bool messageIsError_ = false;
    bool busy_ = false; // moving the cursor ourselves
};
