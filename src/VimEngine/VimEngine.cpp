// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#include "VimEngine/VimEngine.h"

#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>

#include <algorithm>
#include <climits>

namespace
{
    bool isDigitKey(const QString &key)
    {
        return key.size() == 1 && key.at(0).isDigit();
    }

    // The character a single-character key stands for; null for special
    // keys such as "<Esc>".
    QChar keyChar(const QString &key)
    {
        if (key == QLatin1String("<lt>"))
        {
            return QLatin1Char('<');
        }
        if (key.size() == 1)
        {
            return key.at(0);
        }
        return {};
    }

    bool isBlank(QChar c)
    {
        return c == QLatin1Char(' ') || c == QLatin1Char('\t');
    }
} // namespace

VimEngine::VimEngine(QPlainTextEdit *editor) : QObject(editor), editor_(editor)
{
    connect(editor_->document(), &QTextDocument::contentsChange, this,
            [this](int position, int, int) {
                ++contentChanges_;
                changeMin_ = std::min(changeMin_, position);
            });
    mapTimer_.setSingleShot(true);
    connect(&mapTimer_, &QTimer::timeout, this, [this] { processInput(true); });

    // Mouse clicks and drags: keep the cursor on a character in Normal
    // mode, and turn a dragged selection into Visual mode.
    connect(editor_, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        if (!enabled_ || busy_)
        {
            return;
        }
        QTextCursor c = editor_->textCursor();
        if (mode_ == Mode::Normal && c.hasSelection())
        {
            visualAnchor_ = c.anchor();
            const int p = c.position() > c.anchor() ? c.position() - 1
                                                     : c.position();
            setMode(Mode::Visual);
            setPos(p);
            emit stateChanged();
        }
        else if (mode_ == Mode::Normal)
        {
            const int p = clampNormal(c.position());
            if (p != c.position())
            {
                setPos(p);
            }
            wantedColumn_ = columnOf(p);
        }
        else if (mode_ == Mode::Visual || mode_ == Mode::VisualLine ||
                 mode_ == Mode::VisualBlock)
        {
            if (c.hasSelection())
            {
                setPos(c.position());
            }
            emit stateChanged();
        }
    });
}

void VimEngine::setEnabled(bool on)
{
    if (enabled_ == on)
    {
        return;
    }
    enabled_ = on;
    pending_.clear();
    input_.clear();
    mapTimer_.stop();
    mode_ = Mode::Normal;
    if (on)
    {
        setPos(clampNormal(pos()));
    }
    message_.clear();
    emit optionsChanged();
    emit stateChanged();
}

void VimEngine::setSaveHandler(std::function<bool()> handler)
{
    saveHandler_ = std::move(handler);
}

void VimEngine::setQuitHandler(std::function<void()> handler)
{
    quitHandler_ = std::move(handler);
}

void VimEngine::setCommandHandler(
    std::function<bool(const QString &, const QString &)> handler)
{
    commandHandler_ = std::move(handler);
}

// ---------------------------------------------------------------- keys

QString VimEngine::keyToken(QKeyEvent *event)
{
    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool ctrl = mods & Qt::ControlModifier;
    const bool shift = mods & Qt::ShiftModifier;
    const bool alt = mods & Qt::AltModifier;

    switch (key)
    {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_Meta:
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
        return {};
    case Qt::Key_Escape:
        return QStringLiteral("<Esc>");
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return QStringLiteral("<CR>");
    case Qt::Key_Backspace:
        return QStringLiteral("<BS>");
    case Qt::Key_Tab:
        return QStringLiteral("<Tab>");
    case Qt::Key_Backtab:
        return QStringLiteral("<S-Tab>");
    case Qt::Key_Delete:
        return QStringLiteral("<Del>");
    case Qt::Key_Insert:
        return shift  ? QStringLiteral("<S-Insert>")
               : ctrl ? QStringLiteral("<C-Insert>")
                      : QStringLiteral("<Insert>");
    case Qt::Key_Left:
        return QStringLiteral("<Left>");
    case Qt::Key_Right:
        return QStringLiteral("<Right>");
    case Qt::Key_Up:
        return QStringLiteral("<Up>");
    case Qt::Key_Down:
        return QStringLiteral("<Down>");
    case Qt::Key_Home:
        return QStringLiteral("<Home>");
    case Qt::Key_End:
        return QStringLiteral("<End>");
    case Qt::Key_PageUp:
        return QStringLiteral("<PageUp>");
    case Qt::Key_PageDown:
        return QStringLiteral("<PageDown>");
    default:
        break;
    }

    if (alt)
    {
        return {}; // menu mnemonics
    }
    if (ctrl)
    {
        if (key >= Qt::Key_A && key <= Qt::Key_Z)
        {
            const QChar letter(QLatin1Char('a' + (key - Qt::Key_A)));
            if (shift && letter == QLatin1Char('v'))
            {
                return QStringLiteral("<C-S-v>");
            }
            return QStringLiteral("<C-%1>").arg(letter);
        }
        if (key == Qt::Key_BracketLeft)
        {
            return QStringLiteral("<Esc>");
        }
        return {};
    }

    const QString text = event->text();
    if (text.isEmpty() || text.at(0).category() == QChar::Other_Control)
    {
        return {};
    }
    if (text == QLatin1String("<"))
    {
        return QStringLiteral("<lt>");
    }
    return text;
}

// "y$ ", "<C-n>", ":NERDTree<CR>" -> keys in the notation keyToken() uses.
QStringList VimEngine::parseKeys(const QString &text)
{
    static const QHash<QString, QString> named{
        {QStringLiteral("esc"), QStringLiteral("<Esc>")},
        {QStringLiteral("cr"), QStringLiteral("<CR>")},
        {QStringLiteral("return"), QStringLiteral("<CR>")},
        {QStringLiteral("enter"), QStringLiteral("<CR>")},
        {QStringLiteral("nl"), QStringLiteral("<CR>")},
        {QStringLiteral("bs"), QStringLiteral("<BS>")},
        {QStringLiteral("tab"), QStringLiteral("<Tab>")},
        {QStringLiteral("s-tab"), QStringLiteral("<S-Tab>")},
        {QStringLiteral("del"), QStringLiteral("<Del>")},
        {QStringLiteral("space"), QStringLiteral(" ")},
        {QStringLiteral("lt"), QStringLiteral("<lt>")},
        {QStringLiteral("bar"), QStringLiteral("|")},
        {QStringLiteral("bslash"), QStringLiteral("\\")},
        {QStringLiteral("nop"), QString()},
        {QStringLiteral("leader"), QStringLiteral("<Leader>")},
        {QStringLiteral("localleader"), QStringLiteral("<Leader>")},
        {QStringLiteral("left"), QStringLiteral("<Left>")},
        {QStringLiteral("right"), QStringLiteral("<Right>")},
        {QStringLiteral("up"), QStringLiteral("<Up>")},
        {QStringLiteral("down"), QStringLiteral("<Down>")},
        {QStringLiteral("home"), QStringLiteral("<Home>")},
        {QStringLiteral("end"), QStringLiteral("<End>")},
        {QStringLiteral("pageup"), QStringLiteral("<PageUp>")},
        {QStringLiteral("pagedown"), QStringLiteral("<PageDown>")},
        {QStringLiteral("insert"), QStringLiteral("<Insert>")},
        {QStringLiteral("s-insert"), QStringLiteral("<S-Insert>")},
        {QStringLiteral("c-insert"), QStringLiteral("<C-Insert>")},
        {QStringLiteral("c-["), QStringLiteral("<Esc>")},
    };

    QStringList keys;
    for (int i = 0; i < text.size(); ++i)
    {
        const QChar c = text.at(i);
        if (c == QLatin1Char('<'))
        {
            const int close = text.indexOf(QLatin1Char('>'), i + 1);
            if (close > i + 1)
            {
                const QString name = text.mid(i + 1, close - i - 1);
                const QString lower = name.toLower();
                QString key;
                bool known = false;
                if (named.contains(lower))
                {
                    key = named.value(lower);
                    known = true;
                }
                else if (lower.size() == 3 && lower.startsWith(QLatin1String("c-")))
                {
                    key = QStringLiteral("<C-%1>").arg(lower.at(2));
                    known = true;
                }
                if (known)
                {
                    if (!key.isEmpty() || lower == QLatin1String("nop"))
                    {
                        if (!key.isEmpty())
                        {
                            keys << key;
                        }
                    }
                    i = close;
                    continue;
                }
            }
            keys << QStringLiteral("<lt>");
            continue;
        }
        keys << QString(c);
    }
    return keys;
}

// Which mapping table applies to the next key; null: none (the key is an
// argument, such as the character after "f").
QChar VimEngine::mapMode() const
{
    if (confirming_)
    {
        return {};
    }
    switch (mode_)
    {
    case Mode::Insert:
    case Mode::Replace:
        return waitingRegister_ ? QChar() : QLatin1Char('i');
    case Mode::CommandLine:
        return waitingRegister_ ? QChar() : QLatin1Char('c');
    case Mode::Visual:
    case Mode::VisualLine:
    case Mode::VisualBlock:
    case Mode::Normal: {
        const bool visual = mode_ != Mode::Normal;
        // Skip a register and count.
        int i = 0;
        if (i < pending_.size() && pending_.at(i) == QLatin1String("\""))
        {
            if (pending_.size() == 1)
            {
                return {};
            }
            i += 2;
        }
        while (i < pending_.size() && isDigitKey(pending_.at(i)))
        {
            ++i;
        }
        if (i == pending_.size())
        {
            return visual ? QLatin1Char('x') : QLatin1Char('n');
        }
        if (visual)
        {
            return {};
        }
        // An operator, possibly with a count: operator-pending.
        static const QStringList operators{
            QStringLiteral("d"), QStringLiteral("c"), QStringLiteral("y"),
            QStringLiteral(">"), QStringLiteral("<lt>")};
        QString op = pending_.at(i);
        if (op == QLatin1String("g") && i + 1 < pending_.size())
        {
            const QString next = pending_.at(i + 1);
            if (next == QLatin1String("~") || next == QLatin1String("u") ||
                next == QLatin1String("U"))
            {
                op = QStringLiteral("g~");
                ++i;
            }
        }
        if (!operators.contains(op) && op != QLatin1String("g~"))
        {
            return {};
        }
        ++i;
        while (i < pending_.size() && isDigitKey(pending_.at(i)))
        {
            ++i;
        }
        return i == pending_.size() ? QLatin1Char('o') : QChar();
    }
    }
    return {};
}

bool VimEngine::handleKey(QKeyEvent *event)
{
    if (!enabled_)
    {
        return false;
    }
    const QString key = keyToken(event);
    if (key.isEmpty())
    {
        return false; // modifier-only keys, Alt combinations
    }
    if (!message_.isEmpty() && mode_ != Mode::CommandLine)
    {
        message_.clear();
    }
    feed(key);
    return true;
}

bool VimEngine::wantsShortcut(QKeyEvent *event) const
{
    if (!enabled_)
    {
        return false;
    }
    const QString key = keyToken(event);
    if (key.isEmpty() || !key.startsWith(QLatin1String("<C-")))
    {
        return false;
    }
    static const QStringList normalKeys{
        QStringLiteral("<C-r>"), QStringLiteral("<C-v>"), QStringLiteral("<C-q>"),
        QStringLiteral("<C-d>"), QStringLiteral("<C-u>"), QStringLiteral("<C-f>"),
        QStringLiteral("<C-b>"), QStringLiteral("<C-e>"), QStringLiteral("<C-y>"),
        QStringLiteral("<C-c>"), QStringLiteral("<C-g>"), QStringLiteral("<C-h>"),
        QStringLiteral("<C-j>"), QStringLiteral("<C-n>"), QStringLiteral("<C-p>"),
        QStringLiteral("<C-a>"), QStringLiteral("<C-x>")};
    static const QStringList insertKeys{
        QStringLiteral("<C-w>"), QStringLiteral("<C-u>"), QStringLiteral("<C-h>"),
        QStringLiteral("<C-r>"), QStringLiteral("<C-c>"), QStringLiteral("<C-S-v>"),
        QStringLiteral("<C-Insert>")};
    const bool typing = mode_ == Mode::Insert || mode_ == Mode::Replace ||
                        mode_ == Mode::CommandLine;
    if ((typing ? insertKeys : normalKeys).contains(key))
    {
        return true;
    }
    const QChar m = mapMode();
    if (!m.isNull())
    {
        for (const Mapping &mapping : mappings_.value(m))
        {
            if (!mapping.lhs.isEmpty() && mapping.lhs.first() == key)
            {
                return true;
            }
        }
    }
    return false;
}

void VimEngine::feed(const QString &key, bool remap)
{
    input_.append({key, remap});
    processInput();
}

// Resolves mappings, then hands keys to the current mode. A key that could
// start a longer mapping waits for 'timeoutlen' ms.
void VimEngine::processInput(bool timedOut)
{
    mapTimer_.stop();
    while (!input_.isEmpty())
    {
        const QChar m = mapMode();
        if (input_.first().remap && !m.isNull() && mapDepth_ < 1000)
        {
            const Mapping *exact = nullptr;
            bool longer = false;
            for (const Mapping &mapping : mappings_[m])
            {
                const int n = std::min<int>(mapping.lhs.size(), input_.size());
                bool match = !mapping.lhs.isEmpty();
                for (int k = 0; k < n && match; ++k)
                {
                    match = mapping.lhs.at(k) == input_.at(k).key &&
                            input_.at(k).remap;
                }
                if (!match)
                {
                    continue;
                }
                if (mapping.lhs.size() <= input_.size())
                {
                    if (!exact || mapping.lhs.size() > exact->lhs.size())
                    {
                        exact = &mapping;
                    }
                }
                else
                {
                    longer = true;
                }
            }
            if (longer && !timedOut)
            {
                if (options_.timeout)
                {
                    mapTimer_.start(options_.timeoutLen);
                }
                emit stateChanged();
                return;
            }
            if (exact)
            {
                const qsizetype n = exact->lhs.size();
                const QStringList rhs = exact->rhs;
                const bool remap = !exact->noremap;
                input_.remove(0, n);
                for (qsizetype k = rhs.size() - 1; k >= 0; --k)
                {
                    input_.prepend({rhs.at(k), remap});
                }
                ++mapDepth_;
                timedOut = false;
                continue;
            }
        }
        const Token token = input_.takeFirst();
        timedOut = false;
        dispatch(token.key);
    }
    mapDepth_ = 0;
    ensureCursorVisible();
    emit stateChanged();
}

void VimEngine::dispatch(const QString &key)
{
    if (confirming_)
    {
        confirmKey(key); // ":s///c" waiting for y/n/a/q/l
        return;
    }
    if (isRecording_)
    {
        recording_ << key;
    }
    switch (mode_)
    {
    case Mode::Insert:
    case Mode::Replace:
        insertKey(key);
        break;
    case Mode::CommandLine:
        commandLineKey(key);
        break;
    default:
        normalKey(key);
        break;
    }
}

// ---------------------------------------------------------------- modes

void VimEngine::setMode(Mode mode)
{
    mode_ = mode;
    if (mode == Mode::Normal)
    {
        pending_.clear();
    }
}

void VimEngine::normalKey(const QString &key)
{
    pending_ << key;
    changeCommand_ = false;
    dotRequest_.clear();
    const int revision = contentChanges_;
    const bool visual = mode_ != Mode::Normal;
    const Status status = visual ? execVisual(pending_) : execNormal(pending_);
    if (status == Status::Incomplete)
    {
        return;
    }
    const QStringList done = pending_;
    pending_.clear();
    if (status == Status::Invalid)
    {
        return;
    }
    if (mode_ == Mode::Insert || mode_ == Mode::Replace)
    {
        // The text typed next joins this command's undo step if it
        // changed something ("cw", "o").
        undoJoin_ = contentChanges_ != revision;
    }
    if (changeCommand_ && !replaying_ && !visual)
    {
        if (mode_ == Mode::Insert || mode_ == Mode::Replace)
        {
            recording_ = done;
            isRecording_ = true;
        }
        else
        {
            dotKeys_ = done;
        }
    }
    if (mode_ == Mode::Normal)
    {
        setPos(clampNormal(pos()));
    }
    if (!dotRequest_.isEmpty())
    {
        const QStringList keys = dotRequest_;
        dotRequest_.clear();
        replaying_ = true;
        for (const QString &k : keys)
        {
            dispatch(k);
        }
        replaying_ = false;
    }
}

void VimEngine::enterInsert(Mode mode, int count)
{
    mode_ = mode;
    insertCount_ = std::max(1, count);
    insertKeys_.clear();
    waitingRegister_ = false;
}

void VimEngine::leaveInsert()
{
    // "3ihi<Esc>": type the text twice more.
    if (insertCount_ > 1 && !insertKeys_.isEmpty() && !replayingInsert_)
    {
        const QStringList keys = insertKeys_;
        const int repeats = insertCount_ - 1;
        insertCount_ = 1;
        replayingInsert_ = true;
        for (int r = 0; r < repeats; ++r)
        {
            if (insertOpensLine_)
            {
                insertKey(QStringLiteral("<CR>"));
            }
            for (const QString &k : keys)
            {
                insertKey(k);
            }
        }
        replayingInsert_ = false;
    }

    // Visual block I / A / c: repeat the text typed on the first line on
    // the other lines of the block.
    if (blockInsert_)
    {
        blockInsert_ = false;
        const int here = pos();
        if (lineOf(here) == blockFirstLine_ && here > blockInsertStart_)
        {
            const QString typed = doc()->toPlainText().mid(
                blockInsertStart_, here - blockInsertStart_);
            if (!typed.contains(QLatin1Char('\n')))
            {
                beginChange();
                QTextCursor c(doc());
                for (int line = blockFirstLine_ + 1; line <= blockLastLine_;
                     ++line)
                {
                    const QString text = lineText(line);
                    int column = blockColumn_;
                    if (blockAppendToEnd_)
                    {
                        column = static_cast<int>(text.size());
                    }
                    else if (text.size() < column)
                    {
                        if (!blockPadShort_)
                        {
                            continue;
                        }
                        c.setPosition(lineStart(line) +
                                      static_cast<int>(text.size()));
                        c.insertText(QString(column - text.size(),
                                             QLatin1Char(' ')));
                    }
                    c.setPosition(lineStart(line) + column);
                    c.insertText(typed);
                }
                endChange();
            }
        }
        setPos(here);
    }

    insertOpensLine_ = false;
    setMode(Mode::Normal);
    const int p = pos();
    if (columnOf(p) > 0)
    {
        setPos(p - 1);
    }
    setPos(clampNormal(pos()));
    wantedColumn_ = columnOf(pos());
    undoJoin_ = false;
    if (isRecording_)
    {
        dotKeys_ = recording_;
        isRecording_ = false;
    }
}

void VimEngine::startVisual(Mode mode)
{
    visualAnchor_ = pos();
    visualToEnd_ = false;
    mode_ = mode;
}

void VimEngine::exitVisual()
{
    lastVisualMode_ = mode_;
    lastVisualAnchor_ = visualAnchor_;
    lastVisualPos_ = pos();
    const Range r = visualRange();
    markStart_ = r.type == MotionType::Linewise || r.block
                     ? lineStart(r.firstLine)
                     : r.start;
    markEnd_ = r.type == MotionType::Linewise || r.block
                   ? lineStart(r.lastLine)
                   : std::max(r.start, r.end - 1);
    setMode(Mode::Normal);
    setPos(clampNormal(pos()));
}

// ---------------------------------------------------------------- text

QTextDocument *VimEngine::doc() const
{
    return editor_->document();
}

QTextCursor VimEngine::cursor() const
{
    return editor_->textCursor();
}

int VimEngine::pos() const
{
    return editor_->textCursor().position();
}

void VimEngine::setPos(int p)
{
    const int end = doc()->characterCount() - 1;
    p = std::clamp(p, 0, std::max(0, end));
    busy_ = true;
    QTextCursor c = editor_->textCursor();
    c.setPosition(p);
    editor_->setTextCursor(c);
    busy_ = false;
}

int VimEngine::lineOf(int p) const
{
    return doc()->findBlock(p).blockNumber();
}

int VimEngine::lineCount() const
{
    return doc()->blockCount();
}

int VimEngine::lineStart(int line) const
{
    return doc()->findBlockByNumber(line).position();
}

int VimEngine::lineEnd(int line) const
{
    const QTextBlock b = doc()->findBlockByNumber(line);
    return b.position() + b.length() - 1;
}

QString VimEngine::lineText(int line) const
{
    return doc()->findBlockByNumber(line).text();
}

int VimEngine::firstNonBlank(int line) const
{
    const QString text = lineText(line);
    int i = 0;
    while (i < text.size() && isBlank(text.at(i)))
    {
        ++i;
    }
    if (i == text.size() && i > 0)
    {
        --i;
    }
    return lineStart(line) + i;
}

// Normal mode keeps the cursor on a character: not past a line's end.
int VimEngine::clampNormal(int p) const
{
    const int line = lineOf(p);
    const int end = lineEnd(line);
    if (p >= end && end > lineStart(line))
    {
        return end - 1;
    }
    return std::min(p, end);
}

QChar VimEngine::charAt(int p) const
{
    if (p < 0 || p >= doc()->characterCount() - 1)
    {
        return QLatin1Char('\n');
    }
    const QChar c = doc()->characterAt(p);
    if (c == QChar::ParagraphSeparator || c == QChar::LineSeparator)
    {
        return QLatin1Char('\n');
    }
    return c;
}

// 0: blank or line break, 1: punctuation, 2: keyword characters.
int VimEngine::charClass(int p, bool bigWord) const
{
    const QChar c = charAt(p);
    if (c == QLatin1Char('\n') || isBlank(c))
    {
        return 0;
    }
    if (bigWord)
    {
        return 1;
    }
    return c.isLetterOrNumber() || c == QLatin1Char('_') ? 2 : 1;
}

QString VimEngine::indentOf(int line) const
{
    const QString text = lineText(line);
    int i = 0;
    while (i < text.size() && isBlank(text.at(i)))
    {
        ++i;
    }
    return text.left(i);
}

int VimEngine::indentWidth(const QString &indent) const
{
    int width = 0;
    const int ts = std::max(1, options_.tabStop);
    for (const QChar c : indent)
    {
        width = c == QLatin1Char('\t') ? (width / ts + 1) * ts : width + 1;
    }
    return width;
}

QString VimEngine::makeIndent(int width) const
{
    width = std::max(0, width);
    if (options_.expandTab)
    {
        return QString(width, QLatin1Char(' '));
    }
    const int ts = std::max(1, options_.tabStop);
    return QString(width / ts, QLatin1Char('\t')) +
           QString(width % ts, QLatin1Char(' '));
}

// ---------------------------------------------------------------- motions

int VimEngine::wordForward(int p, bool big, bool stopAtLineEnd) const
{
    const int n = doc()->characterCount() - 1;
    if (p >= n)
    {
        return n;
    }
    const int cls = charClass(p, big);
    if (charAt(p) != QLatin1Char('\n') && cls != 0)
    {
        while (p < n && charAt(p) != QLatin1Char('\n') &&
               charClass(p, big) == cls)
        {
            ++p;
        }
    }
    while (p < n)
    {
        if (charAt(p) == QLatin1Char('\n'))
        {
            if (stopAtLineEnd)
            {
                return p;
            }
            ++p;
            if (p < n && charAt(p) == QLatin1Char('\n'))
            {
                return p; // an empty line is a word
            }
            continue;
        }
        if (charClass(p, big) == 0)
        {
            ++p;
            continue;
        }
        break;
    }
    return p;
}

int VimEngine::wordEnd(int p, bool big) const
{
    const int n = doc()->characterCount() - 1;
    if (p >= n - 1)
    {
        return std::max(0, n - 1);
    }
    ++p;
    while (p < n && charClass(p, big) == 0)
    {
        ++p;
    }
    if (p >= n)
    {
        return std::max(0, n - 1);
    }
    const int cls = charClass(p, big);
    while (p + 1 < n && charAt(p + 1) != QLatin1Char('\n') &&
           charClass(p + 1, big) == cls)
    {
        ++p;
    }
    return p;
}

int VimEngine::wordBackward(int p, bool big) const
{
    if (p <= 0)
    {
        return 0;
    }
    --p;
    while (p > 0 && charClass(p, big) == 0)
    {
        if (charAt(p) == QLatin1Char('\n') && charAt(p - 1) == QLatin1Char('\n'))
        {
            return p; // empty line
        }
        --p;
    }
    if (charClass(p, big) == 0)
    {
        return p;
    }
    const int cls = charClass(p, big);
    while (p > 0 && charAt(p - 1) != QLatin1Char('\n') &&
           charClass(p - 1, big) == cls)
    {
        --p;
    }
    return p;
}

int VimEngine::wordEndBackward(int p, bool big) const
{
    if (p <= 0)
    {
        return 0;
    }
    const int cls = charClass(p, big);
    if (cls != 0)
    {
        while (p > 0 && charAt(p - 1) != QLatin1Char('\n') &&
               charClass(p - 1, big) == cls)
        {
            --p;
        }
    }
    --p;
    while (p > 0 && charClass(p, big) == 0)
    {
        if (charAt(p) == QLatin1Char('\n') && charAt(p - 1) == QLatin1Char('\n'))
        {
            return p;
        }
        --p;
    }
    return std::max(0, p);
}

// "%": the bracket matching the one under or after the cursor on its line.
int VimEngine::matchPair(int p) const
{
    static const QString opens = QStringLiteral("([{");
    static const QString closes = QStringLiteral(")]}");
    const int end = lineEnd(lineOf(p));
    int q = p;
    while (q < end && !opens.contains(charAt(q)) && !closes.contains(charAt(q)))
    {
        ++q;
    }
    if (q >= end)
    {
        return -1;
    }
    const QChar c = charAt(q);
    const int n = doc()->characterCount() - 1;
    int open = opens.indexOf(c);
    if (open >= 0)
    {
        int depth = 0;
        for (int i = q; i < n; ++i)
        {
            if (charAt(i) == c)
            {
                ++depth;
            }
            else if (charAt(i) == closes.at(open) && --depth == 0)
            {
                return i;
            }
        }
        return -1;
    }
    const int close = closes.indexOf(c);
    int depth = 0;
    for (int i = q; i >= 0; --i)
    {
        if (charAt(i) == c)
        {
            ++depth;
        }
        else if (charAt(i) == opens.at(close) && --depth == 0)
        {
            return i;
        }
    }
    return -1;
}

// f / F / t / T on the cursor's line; -1 if not found.
int VimEngine::findChar(int p, const QString &target, QChar kind,
                        int count) const
{
    const int line = lineOf(p);
    const int start = lineStart(line);
    const int end = lineEnd(line);
    const bool forward = kind == QLatin1Char('f') || kind == QLatin1Char('t');
    const bool till = kind == QLatin1Char('t') || kind == QLatin1Char('T');
    const QChar wanted = keyChar(target);
    if (wanted.isNull())
    {
        return -1;
    }
    int q = p;
    for (int k = 0; k < count; ++k)
    {
        // "t" repeated with ";" must not stick on the character before
        // the target.
        int from = q + (forward ? 1 : -1);
        if (till && k == 0 && count == 1 && lastFindRepeat_)
        {
            from += forward ? 1 : -1;
        }
        q = -1;
        for (int i = from; forward ? i < end : i >= start; i += forward ? 1 : -1)
        {
            if (charAt(i) == wanted)
            {
                q = i;
                break;
            }
        }
        if (q < 0)
        {
            return -1;
        }
    }
    if (till)
    {
        q += forward ? -1 : 1;
    }
    return q;
}

// Moves `lines` lines down (negative: up), keeping the wanted column.
int VimEngine::moveLines(int p, int lines)
{
    const int line = std::clamp(lineOf(p) + lines, 0, lineCount() - 1);
    const QString text = lineText(line);
    int column = wantEnd_ ? static_cast<int>(text.size()) : wantedColumn_;
    const bool normal = mode_ == Mode::Normal;
    const int maxColumn =
        std::max(0, static_cast<int>(text.size()) - (normal ? 1 : 0));
    column = std::min(column, wantEnd_ && !normal
                                  ? static_cast<int>(text.size())
                                  : maxColumn);
    return lineStart(line) + std::max(0, column);
}

VimEngine::Motion VimEngine::motion(const QStringList &k, int &i, int count,
                                    bool forOperator, Status &status)
{
    Motion m;
    m.pos = pos();
    status = Status::Done;
    if (i >= k.size())
    {
        status = Status::Incomplete;
        return m;
    }
    const QString c = k.at(i);
    const int n = std::max(1, count);
    const int cur = pos();
    const int line = lineOf(cur);
    const bool visual = mode_ == Mode::Visual || mode_ == Mode::VisualLine ||
                        mode_ == Mode::VisualBlock;
    m.key = c;

    auto finish = [&](int p, MotionType type, int consumed = 1) {
        m.pos = p;
        m.type = type;
        i += consumed;
        return m;
    };
    auto fail = [&] {
        m.ok = false;
        ++i;
        return m;
    };

    if (c == QLatin1String("h") || c == QLatin1String("<Left>") ||
        c == QLatin1String("<C-h>") || c == QLatin1String("<BS>"))
    {
        const bool wraps = c == QLatin1String("<BS>") ||
                           c == QLatin1String("<C-h>");
        int p = cur;
        for (int s = 0; s < n; ++s)
        {
            if (p > lineStart(lineOf(p)))
            {
                --p;
            }
            else if (wraps && p > 0)
            {
                p = clampNormal(p - 1);
            }
        }
        if (p == cur)
        {
            return fail();
        }
        return finish(p, MotionType::Exclusive);
    }
    if (c == QLatin1String("l") || c == QLatin1String("<Right>") ||
        c == QLatin1String(" "))
    {
        const bool wraps = c == QLatin1String(" ");
        int p = cur;
        for (int s = 0; s < n; ++s)
        {
            const int end = lineEnd(lineOf(p));
            const int last = forOperator || visual ? end : end - 1;
            if (p < last)
            {
                ++p;
            }
            else if (wraps && end < doc()->characterCount() - 1)
            {
                p = end + 1;
            }
        }
        if (p == cur)
        {
            return fail();
        }
        return finish(p, MotionType::Exclusive);
    }
    if (c == QLatin1String("j") || c == QLatin1String("<Down>") ||
        c == QLatin1String("<C-j>") || c == QLatin1String("<C-n>") ||
        c == QLatin1String("gj"))
    {
        if (line + 1 >= lineCount())
        {
            return fail();
        }
        m.keepColumn = true;
        return finish(moveLines(cur, n), MotionType::Linewise);
    }
    if (c == QLatin1String("k") || c == QLatin1String("<Up>") ||
        c == QLatin1String("<C-p>"))
    {
        if (line == 0)
        {
            return fail();
        }
        m.keepColumn = true;
        return finish(moveLines(cur, -n), MotionType::Linewise);
    }
    if (c == QLatin1String("+") || c == QLatin1String("<CR>") ||
        c == QLatin1String("-") || c == QLatin1String("_"))
    {
        int target = line;
        if (c == QLatin1String("-"))
        {
            target -= n;
        }
        else if (c == QLatin1String("_"))
        {
            target += n - 1;
        }
        else
        {
            target += n;
        }
        if (target < 0 || target >= lineCount())
        {
            return fail();
        }
        return finish(firstNonBlank(target), MotionType::Linewise);
    }
    if (c == QLatin1String("0") || c == QLatin1String("<Home>"))
    {
        return finish(lineStart(line), MotionType::Exclusive);
    }
    if (c == QLatin1String("^"))
    {
        return finish(firstNonBlank(line), MotionType::Exclusive);
    }
    if (c == QLatin1String("$") || c == QLatin1String("<End>"))
    {
        const int target = std::min(line + n - 1, lineCount() - 1);
        const int end = lineEnd(target);
        int p = end;
        if (!visual && !forOperator && end > lineStart(target))
        {
            p = end - 1;
        }
        else if (forOperator && end > lineStart(target))
        {
            p = end - 1;
        }
        m.wantsEnd = true;
        return finish(p, forOperator ? MotionType::Inclusive
                                     : MotionType::Exclusive);
    }
    if (c == QLatin1String("|"))
    {
        const int len = static_cast<int>(lineText(line).size());
        return finish(lineStart(line) + std::min(n - 1, std::max(0, len - 1)),
                      MotionType::Exclusive);
    }
    if (c == QLatin1String("w") || c == QLatin1String("W"))
    {
        const bool big = c == QLatin1String("W");
        int p = cur;
        for (int s = 0; s < n; ++s)
        {
            p = wordForward(p, big, false);
        }
        return finish(p, MotionType::Exclusive);
    }
    if (c == QLatin1String("b") || c == QLatin1String("B"))
    {
        const bool big = c == QLatin1String("B");
        int p = cur;
        for (int s = 0; s < n; ++s)
        {
            p = wordBackward(p, big);
        }
        if (p == cur)
        {
            return fail();
        }
        return finish(p, MotionType::Exclusive);
    }
    if (c == QLatin1String("e") || c == QLatin1String("E"))
    {
        const bool big = c == QLatin1String("E");
        int p = cur;
        for (int s = 0; s < n; ++s)
        {
            p = wordEnd(p, big);
        }
        return finish(p, MotionType::Inclusive);
    }
    if (c == QLatin1String("G"))
    {
        const int target = count > 0 ? std::min(count, lineCount()) - 1
                                     : lineCount() - 1;
        return finish(firstNonBlank(target), MotionType::Linewise);
    }
    if (c == QLatin1String("{") || c == QLatin1String("}"))
    {
        const bool down = c == QLatin1String("}");
        int target = line;
        for (int s = 0; s < n; ++s)
        {
            // Skip empty lines, then go to the next empty line.
            while (target + (down ? 1 : -1) >= 0 &&
                   target + (down ? 1 : -1) < lineCount() &&
                   lineText(target).isEmpty())
            {
                target += down ? 1 : -1;
            }
            while (target + (down ? 1 : -1) >= 0 &&
                   target + (down ? 1 : -1) < lineCount() &&
                   !lineText(target).isEmpty())
            {
                target += down ? 1 : -1;
            }
        }
        int p = lineStart(target);
        if (down && !lineText(target).isEmpty())
        {
            p = lineEnd(target); // the last line: its end
            if (!forOperator && p > lineStart(target))
            {
                p -= 1;
            }
        }
        return finish(p, MotionType::Exclusive);
    }
    if (c == QLatin1String("%"))
    {
        if (count > 0)
        {
            const int target = std::clamp((count * lineCount() + 99) / 100, 1,
                                          lineCount()) -
                               1;
            return finish(firstNonBlank(target), MotionType::Linewise);
        }
        const int p = matchPair(cur);
        if (p < 0)
        {
            return fail();
        }
        return finish(p, MotionType::Inclusive);
    }
    if (c == QLatin1String("H") || c == QLatin1String("L") ||
        c == QLatin1String("M"))
    {
        const int top = editor_->cursorForPosition(QPoint(0, 0)).blockNumber();
        const int bottom =
            editor_
                ->cursorForPosition(QPoint(0, editor_->viewport()->height() - 1))
                .blockNumber();
        int target = c == QLatin1String("H")   ? top + n - 1
                     : c == QLatin1String("L") ? bottom - (n - 1)
                                               : (top + bottom) / 2;
        target = std::clamp(target, 0, lineCount() - 1);
        return finish(firstNonBlank(target), MotionType::Linewise);
    }
    if (c == QLatin1String("n") || c == QLatin1String("N"))
    {
        if (lastPattern_.isEmpty())
        {
            showMessage(QStringLiteral("E35: No previous regular expression"),
                        true);
            return fail();
        }
        const bool forward = (c == QLatin1String("n")) == lastForward_;
        const int p = search(lastPattern_, forward, cur, n, true);
        if (p < 0)
        {
            return fail();
        }
        highlightOn_ = options_.hlSearch;
        return finish(p, MotionType::Exclusive);
    }
    if (c == QLatin1String("*") || c == QLatin1String("#"))
    {
        // The keyword under or after the cursor, as a whole word.
        int s = cur;
        const int end = lineEnd(line);
        while (s < end && charClass(s, false) != 2)
        {
            ++s;
        }
        if (s >= end)
        {
            showMessage(QStringLiteral("E348: No string under cursor"), true);
            return fail();
        }
        while (s > lineStart(line) && charClass(s - 1, false) == 2)
        {
            --s;
        }
        int e = s;
        while (e < end && charClass(e, false) == 2)
        {
            ++e;
        }
        const QString word = doc()->toPlainText().mid(s, e - s);
        lastPattern_ = QStringLiteral("\\<") + word + QStringLiteral("\\>");
        lastForward_ = c == QLatin1String("*");
        history_[QLatin1Char('/')] << lastPattern_;
        const int p = search(lastPattern_, lastForward_, s, n, true);
        if (p < 0)
        {
            return fail();
        }
        highlightOn_ = options_.hlSearch;
        return finish(p, MotionType::Exclusive);
    }
    if (c == QLatin1String("f") || c == QLatin1String("F") ||
        c == QLatin1String("t") || c == QLatin1String("T"))
    {
        if (i + 1 >= k.size())
        {
            status = Status::Incomplete;
            return m;
        }
        lastFindKind_ = c.at(0);
        lastFindChar_ = k.at(i + 1);
        lastFindRepeat_ = false;
        const int p = findChar(cur, lastFindChar_, lastFindKind_, n);
        if (p < 0)
        {
            m.ok = false;
            i += 2;
            return m;
        }
        const bool forward = c == QLatin1String("f") || c == QLatin1String("t");
        return finish(p, forward ? MotionType::Inclusive : MotionType::Exclusive,
                      2);
    }
    if (c == QLatin1String(";") || c == QLatin1String(","))
    {
        if (lastFindKind_.isNull())
        {
            return fail();
        }
        QChar kind = lastFindKind_;
        if (c == QLatin1String(","))
        {
            static const QString from = QStringLiteral("fFtT");
            static const QString to = QStringLiteral("FfTt");
            kind = to.at(from.indexOf(kind));
        }
        lastFindRepeat_ = true;
        const int p = findChar(cur, lastFindChar_, kind, n);
        lastFindRepeat_ = false;
        if (p < 0)
        {
            return fail();
        }
        const bool forward = kind == QLatin1Char('f') || kind == QLatin1Char('t');
        return finish(p, forward ? MotionType::Inclusive
                                 : MotionType::Exclusive);
    }
    if (c == QLatin1String("g"))
    {
        if (i + 1 >= k.size())
        {
            status = Status::Incomplete;
            return m;
        }
        const QString c2 = k.at(i + 1);
        if (c2 == QLatin1String("g"))
        {
            const int target = count > 0 ? std::min(count, lineCount()) - 1 : 0;
            return finish(firstNonBlank(target), MotionType::Linewise, 2);
        }
        if (c2 == QLatin1String("e") || c2 == QLatin1String("E"))
        {
            const bool big = c2 == QLatin1String("E");
            int p = cur;
            for (int s = 0; s < n; ++s)
            {
                p = wordEndBackward(p, big);
            }
            return finish(p, MotionType::Inclusive, 2);
        }
        if (c2 == QLatin1String("_"))
        {
            const int target = std::min(line + n - 1, lineCount() - 1);
            int p = lineEnd(target);
            while (p > lineStart(target) && isBlank(charAt(p - 1)))
            {
                --p;
            }
            return finish(std::max(lineStart(target), p - 1),
                          MotionType::Inclusive, 2);
        }
        if (c2 == QLatin1String("0") || c2 == QLatin1String("^"))
        {
            return finish(c2 == QLatin1String("0") ? lineStart(line)
                                                   : firstNonBlank(line),
                          MotionType::Exclusive, 2);
        }
        if (c2 == QLatin1String("$"))
        {
            const int end = lineEnd(line);
            return finish(end > lineStart(line) ? end - 1 : end,
                          MotionType::Inclusive, 2);
        }
        if (c2 == QLatin1String("j") || c2 == QLatin1String("k"))
        {
            const int dir = c2 == QLatin1String("j") ? 1 : -1;
            if (line + dir < 0 || line + dir >= lineCount())
            {
                m.ok = false;
                i += 2;
                return m;
            }
            m.keepColumn = true;
            return finish(moveLines(cur, dir * n), MotionType::Linewise, 2);
        }
        status = Status::Invalid;
        return m;
    }
    status = Status::Invalid;
    return m;
}

// "iw", "a(", "ip", ... with `k.at(i)` being "i" or "a".
bool VimEngine::textObject(const QStringList &k, int &i, int count,
                           Range &range, Status &status)
{
    status = Status::Done;
    if (i + 1 >= k.size())
    {
        status = Status::Incomplete;
        return false;
    }
    const bool inner = k.at(i) == QLatin1String("i");
    const QString what = k.at(i + 1);
    i += 2;
    const int cur = pos();
    const int line = lineOf(cur);
    const int n = std::max(1, count);

    if (what == QLatin1String("w") || what == QLatin1String("W"))
    {
        const bool big = what == QLatin1String("W");
        const int start = lineStart(line);
        const int end = lineEnd(line);
        auto spanAt = [&](int p, int &s, int &e) {
            const int cls = charClass(p, big);
            s = p;
            e = p;
            while (s > start && charClass(s - 1, big) == cls)
            {
                --s;
            }
            while (e < end && charClass(e, big) == cls)
            {
                ++e;
            }
        };
        if (cur >= end)
        {
            range.start = range.end = cur;
            range.type = MotionType::Exclusive;
            return true;
        }
        int s = 0, e = 0;
        spanAt(cur, s, e);
        const bool onBlank = charClass(cur, big) == 0;
        for (int r = 1; r < n && e < end; ++r)
        {
            int s2 = 0, e2 = 0;
            spanAt(e, s2, e2);
            e = e2;
        }
        if (!inner)
        {
            if (onBlank)
            {
                // "aw" on white space: the white space and the next word.
                if (e < end)
                {
                    int s2 = 0, e2 = 0;
                    spanAt(e, s2, e2);
                    e = e2;
                }
            }
            else if (e < end && charClass(e, big) == 0)
            {
                while (e < end && charClass(e, big) == 0)
                {
                    ++e;
                }
            }
            else
            {
                while (s > start && charClass(s - 1, big) == 0)
                {
                    --s;
                }
            }
        }
        range.start = s;
        range.end = e;
        range.type = MotionType::Exclusive;
        return true;
    }

    if (what == QLatin1String("p") || what == QLatin1String("P"))
    {
        const bool empty = lineText(line).isEmpty();
        int first = line;
        int last = line;
        while (first > 0 && lineText(first - 1).isEmpty() == empty)
        {
            --first;
        }
        while (last + 1 < lineCount() && lineText(last + 1).isEmpty() == empty)
        {
            ++last;
        }
        for (int r = 1; r < n && last + 1 < lineCount(); ++r)
        {
            const bool kind = lineText(last + 1).isEmpty();
            ++last;
            while (last + 1 < lineCount() &&
                   lineText(last + 1).isEmpty() == kind)
            {
                ++last;
            }
        }
        if (!inner)
        {
            // Include the blank lines after the paragraph.
            while (last + 1 < lineCount() && lineText(last + 1).isEmpty() &&
                   !empty)
            {
                ++last;
            }
        }
        range.type = MotionType::Linewise;
        range.firstLine = first;
        range.lastLine = last;
        return true;
    }

    static const QHash<QString, QPair<QChar, QChar>> brackets{
        {QStringLiteral("("), {QLatin1Char('('), QLatin1Char(')')}},
        {QStringLiteral(")"), {QLatin1Char('('), QLatin1Char(')')}},
        {QStringLiteral("b"), {QLatin1Char('('), QLatin1Char(')')}},
        {QStringLiteral("{"), {QLatin1Char('{'), QLatin1Char('}')}},
        {QStringLiteral("}"), {QLatin1Char('{'), QLatin1Char('}')}},
        {QStringLiteral("B"), {QLatin1Char('{'), QLatin1Char('}')}},
        {QStringLiteral("["), {QLatin1Char('['), QLatin1Char(']')}},
        {QStringLiteral("]"), {QLatin1Char('['), QLatin1Char(']')}},
        {QStringLiteral("<lt>"), {QLatin1Char('<'), QLatin1Char('>')}},
        {QStringLiteral(">"), {QLatin1Char('<'), QLatin1Char('>')}},
    };
    if (brackets.contains(what))
    {
        const QChar open = brackets.value(what).first;
        const QChar close = brackets.value(what).second;
        const int total = doc()->characterCount() - 1;
        // The n-th enclosing open bracket.
        int o = charAt(cur) == close ? cur - 1 : cur;
        if (charAt(cur) == open)
        {
            o = cur;
        }
        int depth = 0;
        int found = 0;
        int p = o;
        for (; p >= 0; --p)
        {
            if (charAt(p) == close && p != o)
            {
                ++depth;
            }
            else if (charAt(p) == open)
            {
                if (depth == 0)
                {
                    if (++found == n)
                    {
                        break;
                    }
                }
                else
                {
                    --depth;
                }
            }
        }
        if (p < 0)
        {
            return false;
        }
        const int openPos = p;
        depth = 0;
        int closePos = -1;
        for (int q = openPos; q < total; ++q)
        {
            if (charAt(q) == open)
            {
                ++depth;
            }
            else if (charAt(q) == close && --depth == 0)
            {
                closePos = q;
                break;
            }
        }
        if (closePos < 0)
        {
            return false;
        }
        range.type = MotionType::Exclusive;
        if (inner)
        {
            int s = openPos + 1;
            int e = closePos;
            if (charAt(s) == QLatin1Char('\n'))
            {
                ++s;
                // The closing bracket on a line of its own: stop before
                // its indentation.
                const int closeLine = lineOf(closePos);
                if (lineText(closeLine).left(columnOf(closePos)).trimmed().isEmpty())
                {
                    e = lineStart(closeLine);
                }
            }
            range.start = s;
            range.end = std::max(s, e);
        }
        else
        {
            range.start = openPos;
            range.end = closePos + 1;
        }
        return true;
    }

    if (what == QLatin1String("\"") || what == QLatin1String("'") ||
        what == QLatin1String("`"))
    {
        const QChar quote = what.at(0);
        const int start = lineStart(line);
        const QString text = lineText(line);
        QList<int> quotes;
        for (int q = 0; q < text.size(); ++q)
        {
            if (text.at(q) == quote && (q == 0 || text.at(q - 1) != QLatin1Char('\\')))
            {
                quotes << q;
            }
        }
        const int column = cur - start;
        for (int q = 0; q + 1 < quotes.size(); q += 2)
        {
            if (column <= quotes.at(q + 1))
            {
                range.type = MotionType::Exclusive;
                if (inner)
                {
                    range.start = start + quotes.at(q) + 1;
                    range.end = start + quotes.at(q + 1);
                }
                else
                {
                    range.start = start + quotes.at(q);
                    range.end = start + quotes.at(q + 1) + 1;
                    while (range.end < lineEnd(line) && isBlank(charAt(range.end)))
                    {
                        ++range.end;
                    }
                }
                return true;
            }
        }
        return false;
    }

    status = Status::Invalid;
    return false;
}

// ---------------------------------------------------------------- ranges

// The text an operator covers when the cursor moves from `from` by `m`.
VimEngine::Range VimEngine::rangeFor(int from, const Motion &m) const
{
    Range r;
    r.type = m.type;
    if (m.type == MotionType::Linewise)
    {
        r.firstLine = std::min(lineOf(from), lineOf(m.pos));
        r.lastLine = std::max(lineOf(from), lineOf(m.pos));
        return r;
    }
    int s = std::min(from, m.pos);
    int e = std::max(from, m.pos);
    if (m.type == MotionType::Inclusive)
    {
        e = std::min(e + 1, doc()->characterCount() - 1);
    }
    else if (e > s && e == lineStart(lineOf(e)) && lineOf(e) > lineOf(s))
    {
        // ":help exclusive": an exclusive motion ending at the start of a
        // line ends at the end of the previous one instead; linewise if it
        // also started at or before the first non-blank.
        const int prevLine = lineOf(e) - 1;
        e = lineEnd(prevLine);
        if (s <= firstNonBlank(lineOf(s)))
        {
            r.type = MotionType::Linewise;
            r.firstLine = lineOf(s);
            r.lastLine = prevLine;
            return r;
        }
    }
    r.start = s;
    r.end = e;
    return r;
}

VimEngine::Range VimEngine::visualRange() const
{
    Range r;
    const int a = visualAnchor_;
    const int b = pos();
    // A search typed in visual mode keeps the selection.
    const Mode visual = mode_ == Mode::CommandLine ? cmdReturnMode_ : mode_;
    if (visual == Mode::VisualLine)
    {
        r.type = MotionType::Linewise;
        r.firstLine = std::min(lineOf(a), lineOf(b));
        r.lastLine = std::max(lineOf(a), lineOf(b));
        return r;
    }
    if (visual == Mode::VisualBlock)
    {
        r.block = true;
        r.type = MotionType::Inclusive;
        r.firstLine = std::min(lineOf(a), lineOf(b));
        r.lastLine = std::max(lineOf(a), lineOf(b));
        r.firstCol = std::min(columnOf(a), columnOf(b));
        r.lastCol = std::max(columnOf(a), columnOf(b));
        r.toLineEnd = visualToEnd_;
        return r;
    }
    r.type = MotionType::Inclusive;
    r.start = std::min(a, b);
    r.end = std::min(std::max(a, b) + 1, doc()->characterCount() - 1);
    return r;
}

QString VimEngine::rangeText(const Range &r) const
{
    const QString all = doc()->toPlainText();
    if (r.block)
    {
        QStringList lines;
        for (int line = r.firstLine; line <= r.lastLine; ++line)
        {
            const QString text = lineText(line);
            const int last = r.toLineEnd ? static_cast<int>(text.size()) - 1
                                         : r.lastCol;
            lines << text.mid(r.firstCol, std::max(0, last - r.firstCol + 1));
        }
        return lines.join(QLatin1Char('\n'));
    }
    if (r.type == MotionType::Linewise)
    {
        const int s = lineStart(r.firstLine);
        return all.mid(s, lineEnd(r.lastLine) - s);
    }
    return all.mid(r.start, r.end - r.start);
}

// ---------------------------------------------------------------- edits

void VimEngine::beginChange()
{
    if (changeDepth_++ == 0)
    {
        changeCursor_ = QTextCursor(doc());
        if (undoJoin_)
        {
            changeCursor_.joinPreviousEditBlock();
        }
        else
        {
            changeCursor_.beginEditBlock();
        }
        changeRevision_ = contentChanges_;
    }
}

void VimEngine::endChange()
{
    if (--changeDepth_ == 0)
    {
        changeCursor_.endEditBlock();
        if ((mode_ == Mode::Insert || mode_ == Mode::Replace) &&
            contentChanges_ != changeRevision_)
        {
            undoJoin_ = true;
        }
    }
}

void VimEngine::yankRange(const Range &r, QChar reg, bool isDelete)
{
    Register value;
    value.text = rangeText(r);
    value.kind = r.block                            ? Register::Block
                 : r.type == MotionType::Linewise ? Register::Lines
                                                  : Register::Chars;
    setRegister(reg, value, isDelete);
}

void VimEngine::deleteRange(const Range &r, QChar reg, bool yankIt)
{
    if (yankIt)
    {
        yankRange(r, reg, true);
    }
    QTextCursor c(doc());
    if (r.block)
    {
        for (int line = r.lastLine; line >= r.firstLine; --line)
        {
            const QString text = lineText(line);
            if (text.size() <= r.firstCol)
            {
                continue;
            }
            const int last = r.toLineEnd ? static_cast<int>(text.size()) - 1
                                         : std::min(r.lastCol,
                                                    static_cast<int>(text.size()) - 1);
            c.setPosition(lineStart(line) + r.firstCol);
            c.setPosition(lineStart(line) + last + 1, QTextCursor::KeepAnchor);
            c.removeSelectedText();
        }
        setPos(clampNormal(lineStart(r.firstLine) +
                           std::min(r.firstCol,
                                    static_cast<int>(lineText(r.firstLine).size()))));
        return;
    }
    if (r.type == MotionType::Linewise)
    {
        int s = lineStart(r.firstLine);
        int e = lineEnd(r.lastLine) + 1;
        const int total = doc()->characterCount() - 1;
        if (e > total)
        {
            // The last line has no line break after it: take the one
            // before it instead.
            e = total;
            if (s > 0)
            {
                --s;
            }
        }
        c.setPosition(s);
        c.setPosition(e, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        const int line = std::min(r.firstLine, lineCount() - 1);
        setPos(firstNonBlank(line));
        return;
    }
    c.setPosition(r.start);
    c.setPosition(r.end, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    setPos(clampNormal(r.start));
}

void VimEngine::shiftLines(int first, int last, int amount)
{
    const int sw = options_.shiftWidth > 0 ? options_.shiftWidth
                                           : options_.tabStop;
    QTextCursor c(doc());
    for (int line = first; line <= last; ++line)
    {
        const QString text = lineText(line);
        if (text.isEmpty() || (amount > 0 && text.trimmed().isEmpty()))
        {
            continue;
        }
        const QString indent = indentOf(line);
        const int width = std::max(0, indentWidth(indent) + amount * sw);
        c.setPosition(lineStart(line));
        c.setPosition(lineStart(line) + static_cast<int>(indent.size()),
                      QTextCursor::KeepAnchor);
        c.insertText(makeIndent(width));
    }
    setPos(firstNonBlank(first));
}

void VimEngine::changeCase(const Range &r, const QString &how)
{
    auto convert = [&](const QString &text) {
        QString out = text;
        for (QChar &ch : out)
        {
            if (how == QLatin1String("u"))
            {
                ch = ch.toLower();
            }
            else if (how == QLatin1String("U"))
            {
                ch = ch.toUpper();
            }
            else
            {
                ch = ch.isUpper() ? ch.toLower() : ch.toUpper();
            }
        }
        return out;
    };
    QTextCursor c(doc());
    auto replace = [&](int s, int e) {
        if (e <= s)
        {
            return;
        }
        c.setPosition(s);
        c.setPosition(e, QTextCursor::KeepAnchor);
        c.insertText(convert(c.selectedText().replace(QChar::ParagraphSeparator,
                                                      QLatin1Char('\n'))));
    };
    if (r.block)
    {
        for (int line = r.firstLine; line <= r.lastLine; ++line)
        {
            const int len = static_cast<int>(lineText(line).size());
            const int last = r.toLineEnd ? len - 1 : std::min(r.lastCol, len - 1);
            if (r.firstCol < len)
            {
                replace(lineStart(line) + r.firstCol, lineStart(line) + last + 1);
            }
        }
        setPos(lineStart(r.firstLine) + r.firstCol);
    }
    else if (r.type == MotionType::Linewise)
    {
        replace(lineStart(r.firstLine), lineEnd(r.lastLine));
        setPos(lineStart(r.firstLine));
    }
    else
    {
        replace(r.start, r.end);
        setPos(r.start);
    }
}

void VimEngine::joinLines(int line, int count, bool spaces)
{
    const int joins = std::max(1, count - 1);
    QTextCursor c(doc());
    int joinPos = -1;
    for (int j = 0; j < joins && line + 1 < lineCount(); ++j)
    {
        const int end = lineEnd(line);
        const QString next = lineText(line + 1);
        int skip = 0;
        while (skip < next.size() && isBlank(next.at(skip)))
        {
            ++skip;
        }
        const QString current = lineText(line);
        c.setPosition(end);
        c.setPosition(end + 1 + skip, QTextCursor::KeepAnchor);
        const bool addSpace = spaces && !current.isEmpty() &&
                              !isBlank(current.back()) && skip < next.size() &&
                              next.at(skip) != QLatin1Char(')');
        c.insertText(addSpace ? QStringLiteral(" ") : QString());
        joinPos = end;
    }
    if (joinPos >= 0)
    {
        setPos(clampNormal(joinPos));
    }
}

void VimEngine::put(QChar reg, int count, bool before)
{
    const Register r = getRegister(reg);
    if (r.text.isEmpty() && r.kind != Register::Lines)
    {
        showMessage(QStringLiteral("E353: Nothing in register %1")
                        .arg(reg == QLatin1Char('"') ? QStringLiteral("\"")
                                                     : QString(reg)),
                    true);
        return;
    }
    const int n = std::max(1, count);
    const int cur = pos();
    const int line = lineOf(cur);
    QTextCursor c(doc());

    if (r.kind == Register::Lines)
    {
        QStringList copies;
        for (int k = 0; k < n; ++k)
        {
            copies << r.text;
        }
        const QString text = copies.join(QLatin1Char('\n'));
        if (before)
        {
            c.setPosition(lineStart(line));
            c.insertText(text + QLatin1Char('\n'));
            setPos(firstNonBlank(line));
        }
        else
        {
            c.setPosition(lineEnd(line));
            c.insertText(QLatin1Char('\n') + text);
            setPos(firstNonBlank(line + 1));
        }
        return;
    }

    if (r.kind == Register::Block)
    {
        const QStringList pieces = r.text.split(QLatin1Char('\n'));
        int width = 0;
        for (const QString &piece : pieces)
        {
            width = std::max(width, static_cast<int>(piece.size()));
        }
        const bool emptyLine = lineText(line).isEmpty();
        const int column = columnOf(cur) + (before || emptyLine ? 0 : 1);
        for (int j = 0; j < pieces.size(); ++j)
        {
            const int target = line + j;
            if (target >= lineCount())
            {
                c.setPosition(doc()->characterCount() - 1);
                c.insertText(QStringLiteral("\n"));
            }
            const QString text = lineText(target);
            if (text.size() < column)
            {
                c.setPosition(lineStart(target) + static_cast<int>(text.size()));
                c.insertText(QString(column - text.size(), QLatin1Char(' ')));
            }
            QString piece = pieces.at(j);
            if (column < lineText(target).size())
            {
                piece = piece.leftJustified(width, QLatin1Char(' '));
            }
            QString repeated;
            for (int k = 0; k < n; ++k)
            {
                repeated += piece;
            }
            c.setPosition(lineStart(target) + column);
            c.insertText(repeated);
        }
        setPos(lineStart(line) + column);
        return;
    }

    QString text;
    for (int k = 0; k < n; ++k)
    {
        text += r.text;
    }
    const bool emptyLine = lineText(line).isEmpty();
    const int at = before || emptyLine ? cur : cur + 1;
    c.setPosition(at);
    c.insertText(text);
    if (text.contains(QLatin1Char('\n')))
    {
        setPos(at);
    }
    else
    {
        setPos(at + static_cast<int>(text.size()) - 1);
    }
}

void VimEngine::openLine(bool above)
{
    const int line = lineOf(pos());
    const QString indent = options_.autoIndent ? indentOf(line) : QString();
    QTextCursor c(doc());
    if (above)
    {
        c.setPosition(lineStart(line));
        c.insertText(indent + QLatin1Char('\n'));
        setPos(lineStart(line) + static_cast<int>(indent.size()));
    }
    else
    {
        c.setPosition(lineEnd(line));
        c.insertText(QLatin1Char('\n') + indent);
        setPos(lineStart(line + 1) + static_cast<int>(indent.size()));
    }
}

// Ctrl+A / Ctrl+X: adds `delta` to the first number in columns
// [fromCol, toCol) of `line` that ends after `fromCol`, so the number under
// the cursor counts. Decimal with an optional "-" sign, hex ("0x1F") and
// binary ("0b101"), keeping leading zeros and the case of hex letters. The
// cursor goes to the number's last character.
bool VimEngine::addToNumber(int line, int fromCol, int toCol, qint64 delta)
{
    static const QRegularExpression number(
        QStringLiteral("(0[xX][0-9a-fA-F]+)|(0[bB][01]+)|(-?)([0-9]+)"));
    const QString text = lineText(line);
    auto it = number.globalMatch(text);
    while (it.hasNext())
    {
        const QRegularExpressionMatch m = it.next();
        const int start = static_cast<int>(m.capturedStart());
        const int end = static_cast<int>(m.capturedEnd());
        if (end <= fromCol)
        {
            continue;
        }
        if (start >= toCol)
        {
            return false;
        }
        QString replacement;
        if (m.hasCaptured(1) || m.hasCaptured(2))
        {
            const bool hex = m.hasCaptured(1);
            const QString whole = m.captured(hex ? 1 : 2);
            const QString digits = whole.mid(2);
            const quint64 value =
                digits.toULongLong(nullptr, hex ? 16 : 2) + quint64(delta);
            QString newDigits = QString::number(value, hex ? 16 : 2)
                                    .rightJustified(digits.size(), QLatin1Char('0'));
            // Hex letters keep the case of the number's last letter.
            for (qsizetype i = digits.size() - 1; i >= 0; --i)
            {
                if (digits.at(i).isLetter())
                {
                    if (digits.at(i).isUpper())
                    {
                        newDigits = newDigits.toUpper();
                    }
                    break;
                }
            }
            replacement = whole.left(2) + newDigits;
        }
        else
        {
            const QString digits = m.captured(4);
            const bool negative = !m.captured(3).isEmpty();
            const qint64 value = (negative ? -1 : 1) * digits.toLongLong() + delta;
            QString newDigits = QString::number(value < 0 ? -value : value);
            if (digits.size() > 1 && digits.startsWith(QLatin1Char('0')))
            {
                newDigits = newDigits.rightJustified(digits.size(), QLatin1Char('0'));
            }
            replacement = (value < 0 ? QStringLiteral("-") : QString()) + newDigits;
        }
        QTextCursor c(doc());
        c.setPosition(lineStart(line) + start);
        c.setPosition(lineStart(line) + end, QTextCursor::KeepAnchor);
        c.insertText(replacement);
        setPos(lineStart(line) + start + static_cast<int>(replacement.size()) - 1);
        return true;
    }
    return false;
}

// Visual Ctrl+A / Ctrl+X: the first number of each selected line; with
// `progressive` (g Ctrl+A) the k-th number found changes by k * step.
void VimEngine::addToNumbers(qint64 step, bool progressive)
{
    const Range r = visualRange();
    int first = 0, last = 0;
    int startPos = 0;
    if (r.block || r.type == MotionType::Linewise)
    {
        first = r.firstLine;
        last = r.lastLine;
        startPos = lineStart(first) + (r.block ? r.firstCol : 0);
    }
    else
    {
        first = lineOf(r.start);
        last = lineOf(std::max(r.start, r.end - 1));
        startPos = r.start;
    }
    beginChange();
    int changed = 0;
    for (int line = first; line <= last; ++line)
    {
        int from = 0;
        int to = INT_MAX;
        if (r.block)
        {
            from = r.firstCol;
            to = r.toLineEnd ? INT_MAX : r.lastCol + 1;
        }
        else if (r.type != MotionType::Linewise)
        {
            if (line == first)
            {
                from = columnOf(r.start);
            }
            if (line == last)
            {
                to = columnOf(std::max(r.start, r.end - 1)) + 1;
            }
        }
        if (addToNumber(line, from, to, step * (progressive ? changed + 1 : 1)))
        {
            ++changed;
        }
    }
    endChange();
    exitVisual();
    setPos(clampNormal(startPos));
}

bool VimEngine::applyOperator(const QString &op, Range r, QChar reg, int count)
{
    if (op == QLatin1String("y"))
    {
        yankRange(r, reg, false);
        if (r.block)
        {
            setPos(lineStart(r.firstLine) + r.firstCol);
        }
        else if (r.type == MotionType::Linewise)
        {
            if (lineOf(pos()) > r.firstLine || mode_ != Mode::Normal)
            {
                const int column = std::min(
                    columnOf(pos()), static_cast<int>(lineText(r.firstLine).size()));
                setPos(clampNormal(mode_ == Mode::Normal
                                       ? lineStart(r.firstLine) + column
                                       : lineStart(r.firstLine)));
            }
        }
        else
        {
            setPos(clampNormal(r.start));
        }
        return true;
    }
    if (op == QLatin1String("d"))
    {
        deleteRange(r, reg, true);
        return true;
    }
    if (op == QLatin1String("c"))
    {
        if (r.block)
        {
            deleteRange(r, reg, true);
            blockInsert_ = true;
            blockAppendToEnd_ = false;
            blockPadShort_ = false;
            blockFirstLine_ = r.firstLine;
            blockLastLine_ = r.lastLine;
            blockColumn_ = r.firstCol;
            setPos(lineStart(r.firstLine) +
                   std::min(r.firstCol,
                            static_cast<int>(lineText(r.firstLine).size())));
            blockInsertStart_ = pos();
        }
        else if (r.type == MotionType::Linewise)
        {
            // Keep the first line, emptied but for its indent.
            yankRange(r, reg, true);
            const QString indent =
                options_.autoIndent ? indentOf(r.firstLine) : QString();
            QTextCursor c(doc());
            c.setPosition(lineStart(r.firstLine));
            c.setPosition(lineEnd(r.lastLine), QTextCursor::KeepAnchor);
            c.insertText(indent);
            setPos(lineStart(r.firstLine) + static_cast<int>(indent.size()));
        }
        else
        {
            deleteRange(r, reg, true);
            setPos(r.start);
        }
        enterInsert(Mode::Insert, 1);
        return true;
    }
    if (op == QLatin1String(">") || op == QLatin1String("<lt>"))
    {
        const int first = r.block || r.type == MotionType::Linewise
                              ? r.firstLine
                              : lineOf(r.start);
        const int last = r.block || r.type == MotionType::Linewise
                             ? r.lastLine
                             : lineOf(std::max(r.start, r.end - 1));
        const int amount = std::max(1, count);
        shiftLines(first, last, op == QLatin1String(">") ? amount : -amount);
        return true;
    }
    if (op == QLatin1String("g~") || op == QLatin1String("~"))
    {
        changeCase(r, QStringLiteral("~"));
        return true;
    }
    if (op == QLatin1String("gu") || op == QLatin1String("u"))
    {
        changeCase(r, QStringLiteral("u"));
        return true;
    }
    if (op == QLatin1String("gU") || op == QLatin1String("U"))
    {
        changeCase(r, QStringLiteral("U"));
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- normal

VimEngine::Status VimEngine::execNormal(const QStringList &k)
{
    int i = 0;
    QChar reg = QLatin1Char('"');
    if (k.at(0) == QLatin1String("\""))
    {
        if (k.size() < 2)
        {
            return Status::Incomplete;
        }
        reg = keyChar(k.at(1));
        static const QString valid =
            QStringLiteral("\"0123456789abcdefghijklmnopqrstuvwxyz"
                           "ABCDEFGHIJKLMNOPQRSTUVWXYZ-+*_/:");
        if (reg.isNull() || !valid.contains(reg))
        {
            return Status::Invalid;
        }
        i = 2;
    }
    int count = 0;
    while (i < k.size() && isDigitKey(k.at(i)) &&
           !(k.at(i) == QLatin1String("0") && count == 0))
    {
        count = count * 10 + k.at(i).toInt();
        ++i;
    }
    if (i >= k.size())
    {
        return Status::Incomplete;
    }
    const QString c = k.at(i);
    const int n = std::max(1, count);
    const int cur = pos();
    const int line = lineOf(cur);

    if (c == QLatin1String("<Esc>") || c == QLatin1String("<C-c>"))
    {
        return Status::Done;
    }

    // Operators: d c y > < g~ gu gU, with a motion, a text object or
    // themselves (dd).
    QString op;
    int opLength = 1;
    if (c == QLatin1String("d") || c == QLatin1String("c") ||
        c == QLatin1String("y") || c == QLatin1String(">") ||
        c == QLatin1String("<lt>"))
    {
        op = c;
    }
    else if (c == QLatin1String("g"))
    {
        if (i + 1 >= k.size())
        {
            return Status::Incomplete;
        }
        const QString c2 = k.at(i + 1);
        if (c2 == QLatin1String("~") || c2 == QLatin1String("u") ||
            c2 == QLatin1String("U"))
        {
            op = QStringLiteral("g") + c2;
            opLength = 2;
        }
    }
    if (!op.isEmpty())
    {
        i += opLength;
        int count2 = 0;
        while (i < k.size() && isDigitKey(k.at(i)) &&
               !(k.at(i) == QLatin1String("0") && count2 == 0))
        {
            count2 = count2 * 10 + k.at(i).toInt();
            ++i;
        }
        if (i >= k.size())
        {
            return Status::Incomplete;
        }
        const bool counted = count > 0 || count2 > 0;
        const int total = std::max(1, count) * std::max(1, count2);
        Range range;

        // "dd", "cc", "yy", ">>", "g~~", "g~g~", "guu", "gUgU", ...
        bool self = k.at(i) == op;
        int selfLength = 1;
        if (op.startsWith(QLatin1Char('g')))
        {
            const QString last = op.mid(1);
            if (k.at(i) == last)
            {
                self = true;
            }
            else if (k.at(i) == QLatin1String("g"))
            {
                if (i + 1 >= k.size())
                {
                    return Status::Incomplete;
                }
                if (k.at(i + 1) == last)
                {
                    self = true;
                    selfLength = 2;
                }
            }
        }
        if (self)
        {
            i += selfLength;
            range.type = MotionType::Linewise;
            range.firstLine = line;
            range.lastLine = std::min(line + total - 1, lineCount() - 1);
        }
        else if (k.at(i) == QLatin1String("i") || k.at(i) == QLatin1String("a"))
        {
            Status st = Status::Done;
            const bool ok = textObject(k, i, total, range, st);
            if (st == Status::Incomplete)
            {
                return Status::Incomplete;
            }
            if (st == Status::Invalid || !ok)
            {
                return Status::Invalid;
            }
        }
        else
        {
            const QString mk = k.at(i);
            // "cw" on a word: like "ce", without the white space after it.
            if (op == QLatin1String("c") &&
                (mk == QLatin1String("w") || mk == QLatin1String("W")) &&
                charClass(cur, false) != 0)
            {
                const bool big = mk == QLatin1String("W");
                int p = cur;
                for (int s = 0; s < total; ++s)
                {
                    const bool atEnd =
                        charClass(p, big) != 0 &&
                        (charClass(p + 1, big) != charClass(p, big) ||
                         charAt(p + 1) == QLatin1Char('\n'));
                    if (!(s == 0 && atEnd))
                    {
                        p = wordEnd(p, big);
                    }
                }
                ++i;
                range.type = MotionType::Inclusive;
                range.start = cur;
                range.end = p + 1;
            }
            else
            {
                Status st = Status::Done;
                const Motion m = motion(k, i, counted ? total : 0, true, st);
                if (st == Status::Incomplete)
                {
                    return Status::Incomplete;
                }
                if (st == Status::Invalid || !m.ok)
                {
                    return Status::Invalid;
                }
                range = rangeFor(cur, m);
                // "dw" on the last word of a line stops at the line's end.
                if ((mk == QLatin1String("w") || mk == QLatin1String("W")) &&
                    lineOf(m.pos) > line)
                {
                    const int prevLine = lineOf(m.pos) - 1;
                    range.type = MotionType::Exclusive;
                    range.start = cur;
                    range.end = std::max(cur, lineEnd(prevLine));
                    if (m.pos >= doc()->characterCount() - 1)
                    {
                        range.end = m.pos;
                    }
                }
            }
        }
        changeCommand_ = op != QLatin1String("y");
        beginChange();
        applyOperator(op, range, reg, 1);
        endChange();
        return Status::Done;
    }

    // Motions.
    {
        int j = i;
        Status st = Status::Done;
        const Motion m = motion(k, j, count, false, st);
        if (st == Status::Incomplete)
        {
            return Status::Incomplete;
        }
        if (st == Status::Done)
        {
            if (m.ok)
            {
                setPos(clampNormal(m.pos));
                if (!m.keepColumn)
                {
                    wantedColumn_ = columnOf(pos());
                    wantEnd_ = m.wantsEnd;
                }
            }
            return Status::Done;
        }
    }

    auto operate = [&](const QString &op, Range range) {
        changeCommand_ = op != QLatin1String("y");
        beginChange();
        applyOperator(op, range, reg, 1);
        endChange();
    };
    auto charRange = [&](int s, int e) {
        Range r;
        r.type = MotionType::Exclusive;
        r.start = s;
        r.end = e;
        return r;
    };
    auto linesRange = [&](int first, int last) {
        Range r;
        r.type = MotionType::Linewise;
        r.firstLine = first;
        r.lastLine = std::min(last, lineCount() - 1);
        return r;
    };
    const int end = lineEnd(line);
    const int start = lineStart(line);

    if (c == QLatin1String("x") || c == QLatin1String("<Del>"))
    {
        if (end == start)
        {
            return Status::Done;
        }
        operate(QStringLiteral("d"), charRange(cur, std::min(cur + n, end)));
        return Status::Done;
    }
    if (c == QLatin1String("X"))
    {
        if (cur == start)
        {
            return Status::Invalid;
        }
        operate(QStringLiteral("d"), charRange(std::max(start, cur - n), cur));
        return Status::Done;
    }
    if (c == QLatin1String("D") || c == QLatin1String("C"))
    {
        const int last = std::min(line + n - 1, lineCount() - 1);
        operate(c == QLatin1String("D") ? QStringLiteral("d") : QStringLiteral("c"),
                charRange(cur, lineEnd(last)));
        if (c == QLatin1String("C"))
        {
            setPos(cur);
        }
        return Status::Done;
    }
    if (c == QLatin1String("s"))
    {
        operate(QStringLiteral("c"), charRange(cur, std::min(cur + n, end)));
        return Status::Done;
    }
    if (c == QLatin1String("S"))
    {
        operate(QStringLiteral("c"), linesRange(line, line + n - 1));
        return Status::Done;
    }
    if (c == QLatin1String("Y"))
    {
        operate(QStringLiteral("y"), linesRange(line, line + n - 1));
        return Status::Done;
    }
    if (c == QLatin1String("p") || c == QLatin1String("P"))
    {
        changeCommand_ = true;
        beginChange();
        put(reg, n, c == QLatin1String("P"));
        endChange();
        return Status::Done;
    }
    if (c == QLatin1String("<S-Insert>"))
    {
        changeCommand_ = true;
        beginChange();
        put(QLatin1Char('+'), n, true);
        endChange();
        return Status::Done;
    }
    if (c == QLatin1String("J"))
    {
        changeCommand_ = true;
        beginChange();
        joinLines(line, std::max(2, n), true);
        endChange();
        return Status::Done;
    }
    if (c == QLatin1String("~"))
    {
        if (end == start)
        {
            return Status::Done;
        }
        const int e = std::min(cur + n, end);
        changeCommand_ = true;
        beginChange();
        changeCase(charRange(cur, e), QStringLiteral("~"));
        endChange();
        setPos(clampNormal(e));
        return Status::Done;
    }
    if (c == QLatin1String("r"))
    {
        if (i + 1 >= k.size())
        {
            return Status::Incomplete;
        }
        const QString arg = k.at(i + 1);
        if (arg == QLatin1String("<Esc>") || cur + n > end)
        {
            return Status::Invalid;
        }
        changeCommand_ = true;
        beginChange();
        QTextCursor tc(doc());
        tc.setPosition(cur);
        tc.setPosition(cur + n, QTextCursor::KeepAnchor);
        if (arg == QLatin1String("<CR>"))
        {
            tc.insertText(QStringLiteral("\n"));
            endChange();
            setPos(lineStart(line + 1));
            return Status::Done;
        }
        const QChar ch = keyChar(arg);
        if (ch.isNull())
        {
            endChange();
            return Status::Invalid;
        }
        tc.insertText(QString(n, ch));
        endChange();
        setPos(cur + n - 1);
        return Status::Done;
    }
    if (c == QLatin1String("i") || c == QLatin1String("<Insert>"))
    {
        changeCommand_ = true;
        enterInsert(Mode::Insert, n);
        return Status::Done;
    }
    if (c == QLatin1String("a"))
    {
        changeCommand_ = true;
        if (end > start)
        {
            setPos(cur + 1);
        }
        enterInsert(Mode::Insert, n);
        return Status::Done;
    }
    if (c == QLatin1String("I"))
    {
        changeCommand_ = true;
        setPos(lineStart(line) + static_cast<int>(indentOf(line).size()));
        enterInsert(Mode::Insert, n);
        return Status::Done;
    }
    if (c == QLatin1String("A"))
    {
        changeCommand_ = true;
        setPos(end);
        enterInsert(Mode::Insert, n);
        return Status::Done;
    }
    if (c == QLatin1String("o") || c == QLatin1String("O"))
    {
        changeCommand_ = true;
        beginChange();
        openLine(c == QLatin1String("O"));
        endChange();
        enterInsert(Mode::Insert, n);
        insertOpensLine_ = true;
        return Status::Done;
    }
    if (c == QLatin1String("R"))
    {
        changeCommand_ = true;
        enterInsert(Mode::Replace, n);
        return Status::Done;
    }
    if (c == QLatin1String("u") || c == QLatin1String("<C-r>"))
    {
        undoRedo(c != QLatin1String("u"), n);
        return Status::Done;
    }
    if (c == QLatin1String("."))
    {
        if (dotKeys_.isEmpty())
        {
            return Status::Done;
        }
        QStringList keys = dotKeys_;
        if (count > 0)
        {
            // Replace the recorded count with the new one.
            int j = keys.value(0) == QLatin1String("\"") ? 2 : 0;
            while (j < keys.size() && isDigitKey(keys.at(j)) &&
                   !(keys.at(j) == QLatin1String("0") && j == 0))
            {
                keys.removeAt(j);
            }
            const QString digits = QString::number(count);
            for (int d = static_cast<int>(digits.size()) - 1; d >= 0; --d)
            {
                keys.insert(j, QString(digits.at(d)));
            }
        }
        dotRequest_ = keys;
        return Status::Done;
    }
    if (c == QLatin1String("v") || c == QLatin1String("V") ||
        c == QLatin1String("<C-v>") || c == QLatin1String("<C-q>"))
    {
        startVisual(c == QLatin1String("v")   ? Mode::Visual
                    : c == QLatin1String("V") ? Mode::VisualLine
                                              : Mode::VisualBlock);
        return Status::Done;
    }
    if (c == QLatin1String(":"))
    {
        cmdType_ = QLatin1Char(':');
        cmdText_ = count > 1 ? QStringLiteral(".,.+%1").arg(count - 1)
                             : QString();
        cmdReturnMode_ = Mode::Normal;
        historyIndex_ = -1;
        mode_ = Mode::CommandLine;
        return Status::Done;
    }
    if (c == QLatin1String("/") || c == QLatin1String("?"))
    {
        cmdType_ = c.at(0);
        cmdText_.clear();
        cmdReturnMode_ = Mode::Normal;
        searchOrigin_ = cur;
        historyIndex_ = -1;
        incMatch_ = -1;
        mode_ = Mode::CommandLine;
        return Status::Done;
    }
    if (c == QLatin1String("<C-d>") || c == QLatin1String("<C-u>") ||
        c == QLatin1String("<C-f>") || c == QLatin1String("<C-b>") ||
        c == QLatin1String("<PageDown>") || c == QLatin1String("<PageUp>"))
    {
        const int page = std::max(
            1, editor_->viewport()->height() / editor_->fontMetrics().lineSpacing());
        const bool half = c == QLatin1String("<C-d>") || c == QLatin1String("<C-u>");
        const bool down = c == QLatin1String("<C-d>") || c == QLatin1String("<C-f>") ||
                          c == QLatin1String("<PageDown>");
        const int lines = (half ? std::max(1, page / 2) : std::max(1, page - 2)) * n;
        scrollLines(down ? lines : -lines, true);
        return Status::Done;
    }
    if (c == QLatin1String("<C-e>") || c == QLatin1String("<C-y>"))
    {
        scrollLines(c == QLatin1String("<C-e>") ? n : -n, false);
        return Status::Done;
    }
    if (c == QLatin1String("<C-g>"))
    {
        showMessage(QStringLiteral("line %1 of %2 --%3%--")
                        .arg(line + 1)
                        .arg(lineCount())
                        .arg((line + 1) * 100 / std::max(1, lineCount())));
        return Status::Done;
    }
    if (c == QLatin1String("z"))
    {
        if (i + 1 >= k.size())
        {
            return Status::Incomplete;
        }
        const QString c2 = k.at(i + 1);
        QScrollBar *bar = editor_->verticalScrollBar();
        const int page = std::max(
            1, editor_->viewport()->height() / editor_->fontMetrics().lineSpacing());
        if (c2 == QLatin1String("z") || c2 == QLatin1String("."))
        {
            editor_->centerCursor();
        }
        else if (c2 == QLatin1String("t") || c2 == QLatin1String("<CR>"))
        {
            bar->setValue(line);
        }
        else if (c2 == QLatin1String("b") || c2 == QLatin1String("-"))
        {
            bar->setValue(std::max(0, line - page + 1));
        }
        else
        {
            return Status::Invalid;
        }
        return Status::Done;
    }
    if (c == QLatin1String("Z"))
    {
        if (i + 1 >= k.size())
        {
            return Status::Incomplete;
        }
        if (k.at(i + 1) == QLatin1String("Z"))
        {
            executeEx(QStringLiteral("x"));
        }
        else if (k.at(i + 1) == QLatin1String("Q"))
        {
            executeEx(QStringLiteral("q!"));
        }
        else
        {
            return Status::Invalid;
        }
        return Status::Done;
    }
    if (c == QLatin1String("g"))
    {
        if (i + 1 >= k.size())
        {
            return Status::Incomplete;
        }
        const QString c2 = k.at(i + 1);
        if (c2 == QLatin1String("v"))
        {
            if (lastVisualAnchor_ < 0)
            {
                return Status::Invalid;
            }
            mode_ = lastVisualMode_;
            visualAnchor_ =
                std::min(lastVisualAnchor_, doc()->characterCount() - 1);
            setPos(lastVisualPos_);
            return Status::Done;
        }
        if (c2 == QLatin1String("J"))
        {
            changeCommand_ = true;
            beginChange();
            joinLines(line, std::max(2, n), false);
            endChange();
            return Status::Done;
        }
        if (c2 == QLatin1String("&"))
        {
            // The last :s with its flags, on every line, for the last search.
            changeCommand_ = true;
            executeEx(QStringLiteral("%s//~/&"));
            return Status::Done;
        }
        if (c2 == QLatin1String("I"))
        {
            changeCommand_ = true;
            setPos(start);
            enterInsert(Mode::Insert, n);
            return Status::Done;
        }
        return Status::Invalid;
    }
    if (c == QLatin1String("&"))
    {
        // Repeat the last :s on this line, without its flags.
        changeCommand_ = true;
        executeEx(QStringLiteral("s"));
        return Status::Done;
    }
    if (c == QLatin1String("<C-a>") || c == QLatin1String("<C-x>"))
    {
        const qint64 delta = (c == QLatin1String("<C-a>") ? 1 : -1) * qint64(n);
        changeCommand_ = true;
        beginChange();
        const bool found = addToNumber(line, cur - start, INT_MAX, delta);
        endChange();
        return found ? Status::Done : Status::Invalid;
    }
    // q (macros) and @ are not supported; m, ` and ' (marks) neither.
    return Status::Invalid;
}

// ---------------------------------------------------------------- visual

VimEngine::Status VimEngine::execVisual(const QStringList &k)
{
    int i = 0;
    QChar reg = QLatin1Char('"');
    if (k.at(0) == QLatin1String("\""))
    {
        if (k.size() < 2)
        {
            return Status::Incomplete;
        }
        reg = keyChar(k.at(1));
        if (reg.isNull())
        {
            return Status::Invalid;
        }
        i = 2;
    }
    int count = 0;
    while (i < k.size() && isDigitKey(k.at(i)) &&
           !(k.at(i) == QLatin1String("0") && count == 0))
    {
        count = count * 10 + k.at(i).toInt();
        ++i;
    }
    if (i >= k.size())
    {
        return Status::Incomplete;
    }
    const QString c = k.at(i);
    const int n = std::max(1, count);

    if (c == QLatin1String("<Esc>") || c == QLatin1String("<C-c>"))
    {
        exitVisual();
        return Status::Done;
    }
    if (c == QLatin1String("v") || c == QLatin1String("V") ||
        c == QLatin1String("<C-v>") || c == QLatin1String("<C-q>"))
    {
        const Mode wanted = c == QLatin1String("v")   ? Mode::Visual
                            : c == QLatin1String("V") ? Mode::VisualLine
                                                      : Mode::VisualBlock;
        if (wanted == mode_)
        {
            exitVisual();
        }
        else
        {
            mode_ = wanted;
        }
        return Status::Done;
    }
    if (c == QLatin1String("o") || c == QLatin1String("O"))
    {
        const int p = pos();
        setPos(visualAnchor_);
        visualAnchor_ = p;
        return Status::Done;
    }
    if (c == QLatin1String("<C-Insert>"))
    {
        yankRange(visualRange(), QLatin1Char('+'), false);
        exitVisual();
        return Status::Done;
    }
    if (c == QLatin1String(":"))
    {
        exitVisual();
        cmdType_ = QLatin1Char(':');
        cmdText_ = QStringLiteral("'<,'>");
        cmdReturnMode_ = Mode::Normal;
        historyIndex_ = -1;
        mode_ = Mode::CommandLine;
        return Status::Done;
    }
    if (c == QLatin1String("/") || c == QLatin1String("?"))
    {
        cmdType_ = c.at(0);
        cmdText_.clear();
        cmdReturnMode_ = mode_;
        searchOrigin_ = pos();
        historyIndex_ = -1;
        incMatch_ = -1;
        mode_ = Mode::CommandLine;
        return Status::Done;
    }

    // Text objects extend the selection.
    if ((c == QLatin1String("i") || c == QLatin1String("a")) && mode_ == Mode::Visual)
    {
        Range r;
        Status st = Status::Done;
        const bool ok = textObject(k, i, n, r, st);
        if (st == Status::Incomplete)
        {
            return Status::Incomplete;
        }
        if (st == Status::Invalid || !ok)
        {
            return Status::Invalid;
        }
        if (r.type == MotionType::Linewise)
        {
            mode_ = Mode::VisualLine;
            visualAnchor_ = lineStart(r.firstLine);
            setPos(lineStart(r.lastLine));
        }
        else if (r.end > r.start)
        {
            visualAnchor_ = r.start;
            setPos(r.end - 1);
        }
        return Status::Done;
    }

    // Motions move the free end.
    if (c != QLatin1String("~") && c != QLatin1String("u") &&
        c != QLatin1String("U") && c != QLatin1String("r"))
    {
        int j = i;
        Status st = Status::Done;
        if (c == QLatin1String("$") && mode_ == Mode::VisualBlock)
        {
            visualToEnd_ = true;
        }
        const Motion m = motion(k, j, count, false, st);
        if (st == Status::Incomplete)
        {
            return Status::Incomplete;
        }
        if (st == Status::Done)
        {
            if (m.ok)
            {
                setPos(m.pos);
                if (!m.keepColumn)
                {
                    wantedColumn_ = columnOf(pos());
                    wantEnd_ = m.wantsEnd;
                    if (c != QLatin1String("$"))
                    {
                        visualToEnd_ = false;
                    }
                }
            }
            return Status::Done;
        }
    }

    Range range = visualRange();
    auto finishWith = [&](const QString &op, int amount = 1) {
        beginChange();
        applyOperator(op, range, reg, amount);
        endChange();
        if (mode_ != Mode::Insert)
        {
            exitVisual();
        }
        else
        {
            lastVisualMode_ = Mode::Visual;
        }
    };

    if (c == QLatin1String("d") || c == QLatin1String("x") ||
        c == QLatin1String("<Del>"))
    {
        finishWith(QStringLiteral("d"));
        return Status::Done;
    }
    if (c == QLatin1String("X") || c == QLatin1String("D"))
    {
        if (!range.block)
        {
            const Range lines = range;
            range = Range();
            range.type = MotionType::Linewise;
            range.firstLine = lines.type == MotionType::Linewise ? lines.firstLine
                                                                  : lineOf(lines.start);
            range.lastLine = lines.type == MotionType::Linewise
                                 ? lines.lastLine
                                 : lineOf(std::max(lines.start, lines.end - 1));
        }
        else
        {
            range.toLineEnd = true;
        }
        finishWith(QStringLiteral("d"));
        return Status::Done;
    }
    if (c == QLatin1String("y"))
    {
        finishWith(QStringLiteral("y"));
        return Status::Done;
    }
    if (c == QLatin1String("Y"))
    {
        if (!range.block && range.type != MotionType::Linewise)
        {
            const int first = lineOf(range.start);
            const int last = lineOf(std::max(range.start, range.end - 1));
            range = Range();
            range.type = MotionType::Linewise;
            range.firstLine = first;
            range.lastLine = last;
        }
        finishWith(QStringLiteral("y"));
        return Status::Done;
    }
    if (c == QLatin1String("c") || c == QLatin1String("s"))
    {
        finishWith(QStringLiteral("c"));
        return Status::Done;
    }
    if (c == QLatin1String("C") || c == QLatin1String("S") ||
        c == QLatin1String("R"))
    {
        if (range.block && c == QLatin1String("C"))
        {
            range.toLineEnd = true;
        }
        else if (!range.block)
        {
            const int first = range.type == MotionType::Linewise
                                  ? range.firstLine
                                  : lineOf(range.start);
            const int last = range.type == MotionType::Linewise
                                 ? range.lastLine
                                 : lineOf(std::max(range.start, range.end - 1));
            range = Range();
            range.type = MotionType::Linewise;
            range.firstLine = first;
            range.lastLine = last;
        }
        finishWith(QStringLiteral("c"));
        return Status::Done;
    }
    if (c == QLatin1String(">") || c == QLatin1String("<lt>"))
    {
        finishWith(c, n);
        return Status::Done;
    }
    if (c == QLatin1String("~") || c == QLatin1String("u") ||
        c == QLatin1String("U"))
    {
        finishWith(c);
        return Status::Done;
    }
    if (c == QLatin1String("g"))
    {
        if (i + 1 >= k.size())
        {
            return Status::Incomplete;
        }
        const QString c2 = k.at(i + 1);
        if (c2 == QLatin1String("~") || c2 == QLatin1String("u") ||
            c2 == QLatin1String("U"))
        {
            finishWith(QStringLiteral("g") + c2);
            return Status::Done;
        }
        if (c2 == QLatin1String("J"))
        {
            const int first = range.block || range.type == MotionType::Linewise
                                  ? range.firstLine
                                  : lineOf(range.start);
            const int last = range.block || range.type == MotionType::Linewise
                                 ? range.lastLine
                                 : lineOf(std::max(range.start, range.end - 1));
            beginChange();
            joinLines(first, std::max(2, last - first + 1), false);
            endChange();
            exitVisual();
            return Status::Done;
        }
        if (c2 == QLatin1String("v"))
        {
            return Status::Done;
        }
        if (c2 == QLatin1String("<C-a>") || c2 == QLatin1String("<C-x>"))
        {
            // A sequence: the first number +1, the next +2, ...
            addToNumbers((c2 == QLatin1String("<C-a>") ? 1 : -1) * qint64(n), true);
            return Status::Done;
        }
        return Status::Invalid;
    }
    if (c == QLatin1String("<C-a>") || c == QLatin1String("<C-x>"))
    {
        addToNumbers((c == QLatin1String("<C-a>") ? 1 : -1) * qint64(n), false);
        return Status::Done;
    }
    if (c == QLatin1String("J"))
    {
        const int first = range.block || range.type == MotionType::Linewise
                              ? range.firstLine
                              : lineOf(range.start);
        const int last = range.block || range.type == MotionType::Linewise
                             ? range.lastLine
                             : lineOf(std::max(range.start, range.end - 1));
        beginChange();
        joinLines(first, std::max(2, last - first + 1), true);
        endChange();
        exitVisual();
        return Status::Done;
    }
    if (c == QLatin1String("r"))
    {
        if (i + 1 >= k.size())
        {
            return Status::Incomplete;
        }
        const QChar ch = keyChar(k.at(i + 1));
        if (ch.isNull())
        {
            exitVisual();
            return Status::Done;
        }
        beginChange();
        QTextCursor tc(doc());
        auto fill = [&](int s, int e) {
            for (int p = s; p < e; ++p)
            {
                if (charAt(p) == QLatin1Char('\n'))
                {
                    continue;
                }
                tc.setPosition(p);
                tc.setPosition(p + 1, QTextCursor::KeepAnchor);
                tc.insertText(QString(ch));
            }
        };
        if (range.block)
        {
            for (int line = range.firstLine; line <= range.lastLine; ++line)
            {
                const int len = static_cast<int>(lineText(line).size());
                const int last = range.toLineEnd ? len - 1
                                                 : std::min(range.lastCol, len - 1);
                fill(lineStart(line) + range.firstCol, lineStart(line) + last + 1);
            }
        }
        else if (range.type == MotionType::Linewise)
        {
            fill(lineStart(range.firstLine), lineEnd(range.lastLine));
        }
        else
        {
            fill(range.start, range.end);
        }
        endChange();
        const int start = range.block || range.type == MotionType::Linewise
                              ? lineStart(range.firstLine) +
                                    (range.block ? range.firstCol : 0)
                              : range.start;
        exitVisual();
        setPos(clampNormal(start));
        return Status::Done;
    }
    if (c == QLatin1String("p") || c == QLatin1String("P"))
    {
        // Replace the selection with the register; the replaced text goes
        // to the unnamed register (for "p").
        const Register value = getRegister(reg);
        beginChange();
        const Range r = range;
        const QString old = rangeText(r);
        deleteRange(r, QLatin1Char('_'), false);
        if (r.type == MotionType::Linewise && value.kind != Register::Lines)
        {
            QTextCursor tc(doc());
            tc.setPosition(lineStart(lineOf(pos())));
            tc.insertText(QStringLiteral("\n"));
            setPos(lineStart(lineOf(pos())) - 1 >= 0 ? lineStart(r.firstLine)
                                                     : 0);
        }
        registers_[QLatin1Char('~')] = value;
        const bool before =
            r.type != MotionType::Linewise &&
            !(r.start >= lineEnd(lineOf(r.start)) && r.start > lineStart(lineOf(r.start)));
        put(QLatin1Char('~'), n,
            r.type == MotionType::Linewise ? true : before);
        if (c == QLatin1String("p"))
        {
            Register replaced;
            replaced.text = old;
            replaced.kind = r.block ? Register::Block
                            : r.type == MotionType::Linewise ? Register::Lines
                                                             : Register::Chars;
            setRegister(QLatin1Char('"'), replaced, true);
        }
        endChange();
        lastVisualMode_ = mode_;
        setMode(Mode::Normal);
        setPos(clampNormal(pos()));
        return Status::Done;
    }
    if ((c == QLatin1String("I") || c == QLatin1String("A")) &&
        mode_ == Mode::VisualBlock)
    {
        const bool append = c == QLatin1String("A");
        blockInsert_ = true;
        blockFirstLine_ = range.firstLine;
        blockLastLine_ = range.lastLine;
        blockAppendToEnd_ = append && range.toLineEnd;
        blockPadShort_ = append;
        blockColumn_ = append ? range.lastCol + 1 : range.firstCol;
        lastVisualMode_ = mode_;
        lastVisualAnchor_ = visualAnchor_;
        lastVisualPos_ = pos();
        const QString first = lineText(range.firstLine);
        int column = blockAppendToEnd_ ? static_cast<int>(first.size())
                                       : blockColumn_;
        beginChange();
        if (first.size() < column)
        {
            QTextCursor tc(doc());
            tc.setPosition(lineStart(range.firstLine) + static_cast<int>(first.size()));
            tc.insertText(QString(column - first.size(), QLatin1Char(' ')));
        }
        endChange();
        setPos(lineStart(range.firstLine) + column);
        blockInsertStart_ = pos();
        undoJoin_ = true;
        enterInsert(Mode::Insert, 1);
        return Status::Done;
    }
    if ((c == QLatin1String("I") || c == QLatin1String("A")))
    {
        const int start = range.type == MotionType::Linewise
                              ? lineStart(range.firstLine)
                              : range.start;
        exitVisual();
        if (c == QLatin1String("A"))
        {
            setPos(range.type == MotionType::Linewise ? lineEnd(range.lastLine)
                                                      : range.end);
        }
        else
        {
            setPos(range.type == MotionType::Linewise ? firstNonBlank(range.firstLine)
                                                      : start);
        }
        enterInsert(Mode::Insert, 1);
        return Status::Done;
    }
    return Status::Invalid;
}

// ---------------------------------------------------------------- insert

void VimEngine::insertKey(const QString &key)
{
    if (waitingRegister_)
    {
        waitingRegister_ = false;
        const QChar name = keyChar(key);
        if (name.isNull())
        {
            return;
        }
        const Register r = getRegister(name);
        beginChange();
        QTextCursor c = cursor();
        c.insertText(r.kind == Register::Lines ? r.text + QLatin1Char('\n')
                                               : r.text);
        editor_->setTextCursor(c);
        endChange();
        return;
    }
    if (key == QLatin1String("<Esc>") || key == QLatin1String("<C-c>"))
    {
        leaveInsert();
        return;
    }
    if (!replayingInsert_)
    {
        insertKeys_ << key;
    }

    const int cur = pos();
    const int line = lineOf(cur);
    const int start = lineStart(line);
    const int column = cur - start;

    auto insert = [&](const QString &text) {
        beginChange();
        QTextCursor c = cursor();
        c.insertText(text);
        busy_ = true;
        editor_->setTextCursor(c);
        busy_ = false;
        endChange();
    };
    auto removeBefore = [&](int count) {
        if (count <= 0 || cur - count < 0)
        {
            return;
        }
        beginChange();
        QTextCursor c(doc());
        c.setPosition(cur - count);
        c.setPosition(cur, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        endChange();
        setPos(cur - count);
    };

    if (key == QLatin1String("<CR>"))
    {
        insert(QLatin1Char('\n') +
               (options_.autoIndent ? indentOf(line).left(column) : QString()));
        return;
    }
    if (key == QLatin1String("<BS>") || key == QLatin1String("<C-h>"))
    {
        if (cur == 0)
        {
            return;
        }
        // With 'softtabstop', remove a whole stop's worth of spaces.
        const int sts = options_.softTabStop < 0
                            ? (options_.shiftWidth > 0 ? options_.shiftWidth
                                                       : options_.tabStop)
                            : options_.softTabStop;
        if (sts > 1 && column > 0)
        {
            const QString before = lineText(line).left(column);
            int stop = column % sts == 0 ? sts : column % sts;
            int spaces = 0;
            while (spaces < stop && spaces < before.size() &&
                   before.at(before.size() - 1 - spaces) == QLatin1Char(' '))
            {
                ++spaces;
            }
            if (spaces > 1)
            {
                removeBefore(spaces);
                return;
            }
        }
        removeBefore(1);
        return;
    }
    if (key == QLatin1String("<Del>"))
    {
        if (cur >= doc()->characterCount() - 1)
        {
            return;
        }
        beginChange();
        QTextCursor c(doc());
        c.setPosition(cur);
        c.setPosition(cur + 1, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        endChange();
        setPos(cur);
        return;
    }
    if (key == QLatin1String("<Tab>"))
    {
        if (!options_.expandTab)
        {
            insert(QStringLiteral("\t"));
            return;
        }
        const int stop = options_.softTabStop > 0 ? options_.softTabStop
                         : options_.softTabStop < 0
                             ? (options_.shiftWidth > 0 ? options_.shiftWidth
                                                        : options_.tabStop)
                             : options_.tabStop;
        const int width = indentWidth(lineText(line).left(column));
        insert(QString(stop - width % std::max(1, stop), QLatin1Char(' ')));
        return;
    }
    if (key == QLatin1String("<C-w>"))
    {
        if (column == 0)
        {
            removeBefore(1);
            return;
        }
        int p = cur;
        while (p > start && isBlank(charAt(p - 1)))
        {
            --p;
        }
        if (p > start)
        {
            const int cls = charClass(p - 1, false);
            while (p > start && charClass(p - 1, false) == cls)
            {
                --p;
            }
        }
        removeBefore(cur - p);
        return;
    }
    if (key == QLatin1String("<C-u>"))
    {
        removeBefore(column);
        return;
    }
    if (key == QLatin1String("<C-r>"))
    {
        waitingRegister_ = true;
        return;
    }
    if (key == QLatin1String("<S-Insert>") || key == QLatin1String("<C-S-v>"))
    {
        insert(getRegister(QLatin1Char('+')).text);
        return;
    }
    if (key == QLatin1String("<Insert>"))
    {
        mode_ = mode_ == Mode::Insert ? Mode::Replace : Mode::Insert;
        return;
    }
    if (key == QLatin1String("<Left>") || key == QLatin1String("<Right>") ||
        key == QLatin1String("<Up>") || key == QLatin1String("<Down>") ||
        key == QLatin1String("<Home>") || key == QLatin1String("<End>"))
    {
        int p = cur;
        if (key == QLatin1String("<Left>"))
        {
            p = std::max(start, cur - 1);
        }
        else if (key == QLatin1String("<Right>"))
        {
            p = std::min(lineEnd(line), cur + 1);
        }
        else if (key == QLatin1String("<Home>"))
        {
            p = start;
        }
        else if (key == QLatin1String("<End>"))
        {
            p = lineEnd(line);
        }
        else
        {
            const int target =
                std::clamp(line + (key == QLatin1String("<Down>") ? 1 : -1), 0,
                           lineCount() - 1);
            p = lineStart(target) +
                std::min(column, static_cast<int>(lineText(target).size()));
        }
        setPos(p);
        undoJoin_ = false; // moving around starts a new undo step
        return;
    }

    const QChar ch = keyChar(key);
    if (ch.isNull())
    {
        return; // other control keys
    }
    if (mode_ == Mode::Replace && cur < lineEnd(line))
    {
        beginChange();
        QTextCursor c(doc());
        c.setPosition(cur);
        c.setPosition(cur + 1, QTextCursor::KeepAnchor);
        c.insertText(QString(ch));
        endChange();
        setPos(cur + 1);
        return;
    }
    insert(QString(ch));
}

// Undo or redo `count` steps; like Vim, the cursor goes to the start of
// what changed.
void VimEngine::undoRedo(bool redo, int count)
{
    changeMin_ = doc()->characterCount();
    bool any = false;
    for (int r = 0; r < count; ++r)
    {
        if (redo ? !doc()->isRedoAvailable() : !doc()->isUndoAvailable())
        {
            showMessage(redo ? QStringLiteral("Already at newest change")
                             : QStringLiteral("Already at oldest change"));
            break;
        }
        redo ? editor_->redo() : editor_->undo();
        any = true;
    }
    if (any && changeMin_ < doc()->characterCount())
    {
        setPos(changeMin_);
    }
    setPos(clampNormal(pos()));
    wantedColumn_ = columnOf(pos());
}

// ---------------------------------------------------------------- view

void VimEngine::scrollLines(int lines, bool moveCursor)
{
    QScrollBar *bar = editor_->verticalScrollBar();
    bar->setValue(bar->value() + lines);
    if (moveCursor)
    {
        setPos(clampNormal(moveLines(pos(), lines)));
        return;
    }
    // Keep the cursor on screen.
    const QRect rect = editor_->cursorRect();
    const int height = editor_->viewport()->height();
    const int spacing = editor_->fontMetrics().lineSpacing();
    if (rect.top() < 0)
    {
        setPos(clampNormal(
            editor_->cursorForPosition(QPoint(1, spacing / 2)).position()));
    }
    else if (rect.bottom() > height)
    {
        setPos(clampNormal(
            editor_->cursorForPosition(QPoint(1, height - spacing / 2)).position()));
    }
}

void VimEngine::ensureCursorVisible()
{
    busy_ = true;
    editor_->ensureCursorVisible();
    busy_ = false;
}
