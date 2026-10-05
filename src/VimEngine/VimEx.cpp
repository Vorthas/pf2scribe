// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

// VimEngine: the command line, ex commands, options, the vimrc, mappings,
// registers, search, and what the editor shows.

#include "VimEngine/VimEngine.h"

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPalette>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>

#include <algorithm>

namespace
{
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

    // "name" is an abbreviation of `full` at least `minimum` long.
    bool isCommand(const QString &name, const QString &full, int minimum)
    {
        return name.size() >= minimum && full.startsWith(name);
    }

    // Removes a trailing comment: '"' that isn't inside a string.
    QString stripComment(const QString &line)
    {
        bool single = false;
        for (int i = 0; i < line.size(); ++i)
        {
            const QChar c = line.at(i);
            if (c == QLatin1Char('\'') )
            {
                single = !single;
            }
            else if (c == QLatin1Char('"') && !single &&
                     (i == 0 || line.at(i - 1) != QLatin1Char('\\')))
            {
                return line.left(i);
            }
        }
        return line;
    }
} // namespace

// ---------------------------------------------------------------- command line

void VimEngine::commandLineKey(const QString &key)
{
    const bool searching = cmdType_ != QLatin1Char(':');

    auto updateIncremental = [&] {
        if (!searching || !options_.incSearch)
        {
            return;
        }
        incMatch_ = -1;
        if (cmdText_.isEmpty())
        {
            setPos(searchOrigin_);
            return;
        }
        const QRegularExpression re = compilePattern(cmdText_);
        if (!re.isValid())
        {
            return;
        }
        const QString text = doc()->toPlainText();
        const bool forward = cmdType_ == QLatin1Char('/');
        QRegularExpressionMatch found;
        auto it = re.globalMatch(text);
        QRegularExpressionMatch first;
        QRegularExpressionMatch lastBefore;
        QRegularExpressionMatch last;
        while (it.hasNext())
        {
            const QRegularExpressionMatch m = it.next();
            if (!first.hasMatch())
            {
                first = m;
            }
            last = m;
            if (forward && m.capturedStart() > searchOrigin_ && !found.hasMatch())
            {
                found = m;
                break;
            }
            if (!forward && m.capturedStart() < searchOrigin_)
            {
                lastBefore = m;
            }
        }
        if (!forward)
        {
            found = lastBefore;
        }
        if (!found.hasMatch() && options_.wrapScan)
        {
            found = forward ? first : last;
        }
        if (found.hasMatch())
        {
            incMatch_ = static_cast<int>(found.capturedStart());
            incMatchLen_ = static_cast<int>(found.capturedLength());
            setPos(incMatch_);
        }
        else
        {
            setPos(searchOrigin_);
        }
    };
    auto leave = [&] {
        incMatch_ = -1;
        mode_ = cmdReturnMode_;
        pending_.clear();
    };

    if (waitingRegister_)
    {
        waitingRegister_ = false;
        const QChar name = keyChar(key);
        if (!name.isNull())
        {
            cmdText_ += getRegister(name).text.split(QLatin1Char('\n')).value(0);
            updateIncremental();
        }
        return;
    }
    if (key == QLatin1String("<Esc>") || key == QLatin1String("<C-c>"))
    {
        if (searching)
        {
            setPos(searchOrigin_);
        }
        leave();
        if (mode_ == Mode::Normal)
        {
            setPos(clampNormal(pos()));
        }
        return;
    }
    if (key == QLatin1String("<CR>"))
    {
        const QString text = cmdText_;
        if (!text.isEmpty())
        {
            QStringList &list = history_[cmdType_];
            list.removeAll(text);
            list << text;
        }
        leave();
        if (!searching)
        {
            executeEx(text);
            return;
        }
        const QString pattern = text.isEmpty() ? lastPattern_ : text;
        if (pattern.isEmpty())
        {
            showMessage(QStringLiteral("E35: No previous regular expression"),
                        true);
            setPos(searchOrigin_);
            return;
        }
        lastPattern_ = pattern;
        lastForward_ = cmdType_ == QLatin1Char('/');
        highlightOn_ = options_.hlSearch;
        const int p = search(pattern, lastForward_, searchOrigin_, 1, true);
        setPos(p >= 0 ? p : searchOrigin_);
        if (mode_ == Mode::Normal)
        {
            setPos(clampNormal(pos()));
            wantedColumn_ = columnOf(pos());
        }
        return;
    }
    if (key == QLatin1String("<BS>") || key == QLatin1String("<C-h>"))
    {
        if (cmdText_.isEmpty())
        {
            if (searching)
            {
                setPos(searchOrigin_);
            }
            leave();
            return;
        }
        cmdText_.chop(1);
        updateIncremental();
        return;
    }
    if (key == QLatin1String("<C-u>"))
    {
        cmdText_.clear();
        updateIncremental();
        return;
    }
    if (key == QLatin1String("<C-w>"))
    {
        int end = static_cast<int>(cmdText_.size());
        while (end > 0 && cmdText_.at(end - 1).isSpace())
        {
            --end;
        }
        while (end > 0 && !cmdText_.at(end - 1).isSpace())
        {
            --end;
        }
        cmdText_.truncate(end);
        updateIncremental();
        return;
    }
    if (key == QLatin1String("<Up>") || key == QLatin1String("<Down>"))
    {
        const QStringList &list = history_[cmdType_];
        if (list.isEmpty())
        {
            return;
        }
        if (historyIndex_ < 0)
        {
            historyIndex_ = static_cast<int>(list.size());
        }
        historyIndex_ += key == QLatin1String("<Up>") ? -1 : 1;
        historyIndex_ = std::clamp(historyIndex_, 0, static_cast<int>(list.size()));
        cmdText_ = historyIndex_ < list.size() ? list.at(historyIndex_) : QString();
        updateIncremental();
        return;
    }
    if (key == QLatin1String("<C-r>"))
    {
        waitingRegister_ = true;
        return;
    }
    if (key == QLatin1String("<S-Insert>") || key == QLatin1String("<C-S-v>"))
    {
        cmdText_ += getRegister(QLatin1Char('+')).text.split(QLatin1Char('\n')).value(0);
        updateIncremental();
        return;
    }
    const QChar ch = keyChar(key);
    if (!ch.isNull())
    {
        cmdText_ += ch;
        updateIncremental();
    }
}

// ---------------------------------------------------------------- ex

bool VimEngine::parseRange(QString &cmd, int &first, int &last, bool &given)
{
    given = false;
    const int current = lineOf(pos());
    int i = 0;
    auto skipSpaces = [&] {
        while (i < cmd.size() && cmd.at(i).isSpace())
        {
            ++i;
        }
    };
    auto number = [&](int &out) {
        int start = i;
        while (i < cmd.size() && cmd.at(i).isDigit())
        {
            ++i;
        }
        if (i == start)
        {
            return false;
        }
        out = cmd.mid(start, i - start).toInt();
        return true;
    };
    // One address; false if there is none.
    auto address = [&](int &out, bool &ok) {
        ok = true;
        skipSpaces();
        bool have = false;
        int value = current;
        if (i < cmd.size())
        {
            const QChar c = cmd.at(i);
            if (c == QLatin1Char('.'))
            {
                ++i;
                have = true;
            }
            else if (c == QLatin1Char('$'))
            {
                value = lineCount() - 1;
                ++i;
                have = true;
            }
            else if (c.isDigit())
            {
                int n = 0;
                number(n);
                value = n - 1;
                have = true;
            }
            else if (c == QLatin1Char('\'') && i + 1 < cmd.size())
            {
                const QChar mark = cmd.at(i + 1);
                if (mark == QLatin1Char('<') || mark == QLatin1Char('>'))
                {
                    const int p = mark == QLatin1Char('<') ? markStart_ : markEnd_;
                    if (p < 0)
                    {
                        ok = false;
                        return false;
                    }
                    value = lineOf(p);
                    i += 2;
                    have = true;
                }
            }
        }
        while (i < cmd.size() &&
               (cmd.at(i) == QLatin1Char('+') || cmd.at(i) == QLatin1Char('-')))
        {
            const int sign = cmd.at(i) == QLatin1Char('+') ? 1 : -1;
            ++i;
            int n = 1;
            number(n);
            value += sign * n;
            have = true;
        }
        out = value;
        return have;
    };

    skipSpaces();
    if (i < cmd.size() && cmd.at(i) == QLatin1Char('%'))
    {
        first = 0;
        last = lineCount() - 1;
        given = true;
        cmd = cmd.mid(i + 1);
        return true;
    }
    bool ok = true;
    if (address(first, ok))
    {
        given = true;
        last = first;
        skipSpaces();
        if (i < cmd.size() &&
            (cmd.at(i) == QLatin1Char(',') || cmd.at(i) == QLatin1Char(';')))
        {
            ++i;
            if (!address(last, ok))
            {
                last = first;
            }
        }
    }
    if (!ok)
    {
        return false;
    }
    cmd = cmd.mid(i);
    if (given)
    {
        if (first > last)
        {
            std::swap(first, last);
        }
        if (first < 0 || last >= lineCount())
        {
            return false;
        }
    }
    return true;
}

void VimEngine::executeEx(const QString &command)
{
    QString cmd = command;
    while (cmd.startsWith(QLatin1Char(':')) || cmd.startsWith(QLatin1Char(' ')))
    {
        cmd.remove(0, 1);
    }
    if (cmd.trimmed().isEmpty())
    {
        return;
    }
    int first = lineOf(pos());
    int last = first;
    bool given = false;
    if (!parseRange(cmd, first, last, given))
    {
        showMessage(QStringLiteral("E16: Invalid range"), true);
        return;
    }
    cmd = cmd.trimmed();
    if (cmd.isEmpty())
    {
        // ":42": go to a line.
        setPos(firstNonBlank(last));
        wantedColumn_ = columnOf(pos());
        return;
    }

    // Name, "!", arguments.
    int i = 0;
    while (i < cmd.size() && cmd.at(i).isLetter())
    {
        ++i;
    }
    QString name = cmd.left(i);
    if (name.isEmpty())
    {
        name = cmd.left(1); // ">", "<", "&", ...
        i = 1;
    }
    const bool bang = i < cmd.size() && cmd.at(i) == QLatin1Char('!');
    if (bang)
    {
        ++i;
    }
    const QString args = cmd.mid(i).trimmed();

    auto writeFile = [&]() {
        if (!saveHandler_ || !saveHandler_())
        {
            showMessage(QStringLiteral("E212: Can't open file for writing"), true);
            return false;
        }
        showMessage(QStringLiteral("%1L, %2B written")
                        .arg(lineCount())
                        .arg(doc()->toPlainText().toUtf8().size() + 1));
        return true;
    };
    auto oneFile = [&] {
        showMessage(QStringLiteral("Scribe Native edits one file at a time; "
                                   "use File > Save As for a copy"),
                    true);
    };

    if (isCommand(name, QStringLiteral("write"), 1) ||
        isCommand(name, QStringLiteral("update"), 2))
    {
        if (!args.isEmpty())
        {
            oneFile();
            return;
        }
        if (name.startsWith(QLatin1Char('u')) && !doc()->isModified())
        {
            return;
        }
        writeFile();
        return;
    }
    if (name == QLatin1String("wq") || name == QLatin1String("wqa") ||
        name == QLatin1String("wqall") || isCommand(name, QStringLiteral("xit"), 1) ||
        isCommand(name, QStringLiteral("exit"), 3) || name == QLatin1String("xa") ||
        name == QLatin1String("xall"))
    {
        if (!args.isEmpty())
        {
            oneFile();
            return;
        }
        const bool always = name.startsWith(QLatin1String("wq"));
        if ((always || doc()->isModified()) && !writeFile())
        {
            return;
        }
        if (quitHandler_)
        {
            quitHandler_();
        }
        return;
    }
    if (isCommand(name, QStringLiteral("quit"), 1) || name == QLatin1String("qa") ||
        name == QLatin1String("qall") || name == QLatin1String("quitall"))
    {
        // With or without "!": the application asks about unsaved changes.
        if (quitHandler_)
        {
            quitHandler_();
        }
        return;
    }
    if (isCommand(name, QStringLiteral("set"), 2) ||
        isCommand(name, QStringLiteral("setlocal"), 4) ||
        isCommand(name, QStringLiteral("setglobal"), 4))
    {
        const QStringList parts =
            args.split(QRegularExpression(QStringLiteral("(?<!\\\\)\\s+")),
                       Qt::SkipEmptyParts);
        for (const QString &part : parts)
        {
            if (!setOption(part))
            {
                showMessage(QStringLiteral("E518: Unknown option: %1").arg(part),
                            true);
            }
        }
        return;
    }
    if (isCommand(name, QStringLiteral("nohlsearch"), 3))
    {
        highlightOn_ = false;
        return;
    }
    if (isCommand(name, QStringLiteral("undo"), 1) ||
        isCommand(name, QStringLiteral("redo"), 3))
    {
        undoRedo(name.startsWith(QLatin1String("red")), 1);
        return;
    }
    if (isCommand(name, QStringLiteral("delete"), 1) ||
        isCommand(name, QStringLiteral("yank"), 1) ||
        name == QLatin1String(">") || name == QLatin1String("<") ||
        isCommand(name, QStringLiteral("join"), 1))
    {
        Range r;
        r.type = MotionType::Linewise;
        r.firstLine = first;
        r.lastLine = last;
        const QChar reg = args.isEmpty() ? QLatin1Char('"') : args.at(0);
        beginChange();
        if (name.startsWith(QLatin1Char('d')))
        {
            deleteRange(r, reg, true);
        }
        else if (name.startsWith(QLatin1Char('y')))
        {
            yankRange(r, reg, false);
        }
        else if (name.startsWith(QLatin1Char('j')))
        {
            joinLines(first, std::max(2, last - first + 1), !bang);
        }
        else
        {
            // ":>>>" shifts three times.
            const int amount = 1 + static_cast<int>(args.count(name.at(0)));
            shiftLines(first, last,
                       name == QLatin1String(">") ? amount : -amount);
            setPos(firstNonBlank(last)); // Vim: on the range's last line
        }
        endChange();
        return;
    }
    if (isCommand(name, QStringLiteral("source"), 2))
    {
        QString path = args;
        if (path.startsWith(QLatin1Char('~')))
        {
            path = QDir::homePath() + path.mid(1);
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            showMessage(QStringLiteral("E484: Can't open file %1").arg(args), true);
            return;
        }
        for (const QString &line : QString::fromUtf8(file.readAll()).split(QLatin1Char('\n')))
        {
            sourceLine(line, false);
        }
        return;
    }
    if (name == QLatin1String("&"))
    {
        // ":&" repeats the last :s; ":&&" with its flags.
        QString rest = cmd.mid(1);
        const bool keepFlags = rest.startsWith(QLatin1Char('&'));
        if (keepFlags)
        {
            rest.remove(0, 1);
        }
        substitute(first, last, rest.trimmed(), keepFlags);
        return;
    }
    if (isCommand(name, QStringLiteral("substitute"), 1))
    {
        substitute(first, last, cmd.mid(name.size()), false);
        return;
    }
    if (isCommand(name, QStringLiteral("help"), 1))
    {
        showMessage(QStringLiteral("No help here; see https://vim.rtorr.com/"));
        return;
    }
    static const QStringList multiFile{
        QStringLiteral("e"),      QStringLiteral("edit"),    QStringLiteral("new"),
        QStringLiteral("vnew"),   QStringLiteral("sp"),      QStringLiteral("split"),
        QStringLiteral("vs"),     QStringLiteral("vsplit"),  QStringLiteral("tabnew"),
        QStringLiteral("tabe"),   QStringLiteral("tabedit"), QStringLiteral("bn"),
        QStringLiteral("bnext"),  QStringLiteral("bp"),      QStringLiteral("bprevious"),
        QStringLiteral("b"),      QStringLiteral("buffer"),  QStringLiteral("ls"),
        QStringLiteral("files"),  QStringLiteral("buffers"), QStringLiteral("n"),
        QStringLiteral("next"),   QStringLiteral("prev"),    QStringLiteral("saveas"),
        QStringLiteral("sav")};
    if (multiFile.contains(name))
    {
        oneFile();
        return;
    }
    // Mappings, "let" and the like typed at the command line.
    if (sourceLine(cmd, false))
    {
        return;
    }
    // Commands the application provides (":NERDTreeToggle", ...).
    if (commandHandler_ && commandHandler_(name, args))
    {
        return;
    }
    showMessage(QStringLiteral("E492: Not an editor command: %1").arg(command.trimmed()),
                true);
}

// ---------------------------------------------------------------- :s

// ":[range]s/pattern/replacement/[flags] [count]". Without a pattern (":s",
// ":s g", "&") the last substitution is repeated.
void VimEngine::substitute(int first, int last, const QString &rawArgs,
                           bool useLastFlags)
{
    QString pattern;
    QString replacement;
    QString flags;
    const QChar delimiter = rawArgs.isEmpty() ? QChar() : rawArgs.at(0);
    const bool repeat = rawArgs.isEmpty() || delimiter.isLetterOrNumber() ||
                        delimiter.isSpace() || delimiter == QLatin1Char('&');
    if (repeat)
    {
        if (!haveLastSub_)
        {
            showMessage(QStringLiteral("E35: No previous regular expression"), true);
            return;
        }
        pattern = lastSubPattern_;
        replacement = lastSubReplacement_;
        flags = rawArgs.trimmed();
        if (flags.startsWith(QLatin1Char('&')))
        {
            useLastFlags = true;
            flags.remove(0, 1);
        }
    }
    else
    {
        if (delimiter == QLatin1Char('\\') || delimiter == QLatin1Char('"') ||
            delimiter == QLatin1Char('|'))
        {
            showMessage(QStringLiteral("E146: Regular expressions can't be "
                                       "delimited by letters"),
                        true);
            return;
        }
        int i = 1;
        // Up to the next unescaped delimiter; "\/" stands for "/".
        auto part = [&](QString &out) {
            while (i < rawArgs.size())
            {
                const QChar c = rawArgs.at(i);
                if (c == QLatin1Char('\\') && i + 1 < rawArgs.size())
                {
                    if (rawArgs.at(i + 1) != delimiter)
                    {
                        out += c;
                    }
                    out += rawArgs.at(i + 1);
                    i += 2;
                    continue;
                }
                ++i;
                if (c == delimiter)
                {
                    return;
                }
                out += c;
            }
        };
        part(pattern);
        QString typed;
        part(typed);
        flags = rawArgs.mid(i).trimmed();
        if (flags.startsWith(QLatin1Char('&')))
        {
            useLastFlags = true;
            flags.remove(0, 1);
        }
        if (pattern.isEmpty())
        {
            pattern = lastPattern_; // ":s//x/": the last search
            if (pattern.isEmpty())
            {
                showMessage(QStringLiteral("E35: No previous regular expression"),
                            true);
                return;
            }
        }
        // "~" stands for the previous replacement.
        for (int k = 0; k < typed.size(); ++k)
        {
            if (typed.at(k) == QLatin1Char('\\') && k + 1 < typed.size())
            {
                replacement += typed.mid(k, 2);
                ++k;
            }
            else if (typed.at(k) == QLatin1Char('~'))
            {
                replacement += lastSubReplacement_;
            }
            else
            {
                replacement += typed.at(k);
            }
        }
        lastSubPattern_ = pattern;
        lastSubReplacement_ = replacement;
        haveLastSub_ = true;
    }

    // Flags, then an optional count: ":s/a/b/g 3" works on 3 lines from
    // the end of the range.
    QString letters;
    int k = 0;
    static const QString known = QStringLiteral("cegiInp#lr");
    while (k < flags.size() && known.contains(flags.at(k)))
    {
        letters += flags.at(k++);
    }
    const QString countText = flags.mid(k).trimmed();
    if (!countText.isEmpty())
    {
        bool ok = false;
        const int count = countText.toInt(&ok);
        if (!ok || count <= 0)
        {
            showMessage(QStringLiteral("E488: Trailing characters: %1").arg(countText),
                        true);
            return;
        }
        first = last;
        last = std::min(first + count - 1, lineCount() - 1);
    }
    if (useLastFlags)
    {
        letters = lastSubFlags_ + letters;
    }
    lastSubFlags_ = letters;

    Substitution s;
    s.re = compilePattern(pattern);
    QRegularExpression::PatternOptions options = s.re.patternOptions();
    if (letters.contains(QLatin1Char('i')))
    {
        options |= QRegularExpression::CaseInsensitiveOption;
    }
    if (letters.contains(QLatin1Char('I')))
    {
        options &= ~QRegularExpression::CaseInsensitiveOption;
    }
    s.re.setPatternOptions(options);
    if (!s.re.isValid())
    {
        showMessage(QStringLiteral("E383: Invalid search string: %1").arg(pattern),
                    true);
        return;
    }
    s.replacement = replacement;
    s.global = letters.count(QLatin1Char('g')) % 2 == 1;
    s.confirm = letters.contains(QLatin1Char('c'));
    s.countOnly = letters.contains(QLatin1Char('n'));
    s.quiet = letters.contains(QLatin1Char('e'));
    s.line = first;
    s.lastLine = last;
    sub_ = s;
    lastPattern_ = pattern;
    lastForward_ = true;
    highlightOn_ = options_.hlSearch;
    runSubstitution();
}

// Replaces matches until done, or stops at one for "c" to confirm.
void VimEngine::runSubstitution()
{
    while (true)
    {
        bool found = false;
        while (sub_.line <= sub_.lastLine && sub_.line < lineCount())
        {
            const QString text = lineText(sub_.line);
            if (sub_.column <= text.size())
            {
                const QRegularExpressionMatch m = sub_.re.match(text, sub_.column);
                if (m.hasMatch())
                {
                    sub_.match = m;
                    sub_.matchStart =
                        lineStart(sub_.line) + static_cast<int>(m.capturedStart());
                    sub_.matchLength = static_cast<int>(m.capturedLength());
                    found = true;
                    break;
                }
            }
            ++sub_.line;
            sub_.column = 0;
        }
        if (!found)
        {
            finishSubstitution();
            return;
        }
        if (sub_.confirm)
        {
            confirming_ = true;
            incMatch_ = sub_.matchStart;
            incMatchLen_ = std::max(1, sub_.matchLength);
            setPos(sub_.matchStart);
            showMessage(QStringLiteral("replace with %1 (y/n/a/q/l/^E/^Y)?")
                            .arg(sub_.replacement));
            return;
        }
        replaceCurrentMatch();
    }
}

void VimEngine::replaceCurrentMatch()
{
    const QString text = sub_.countOnly
                             ? sub_.match.captured(0)
                             : expandReplacement(sub_.replacement, sub_.match);
    if (!sub_.countOnly)
    {
        // All of one :s is one undo step.
        undoJoin_ = sub_.joinUndo;
        beginChange();
        QTextCursor c(doc());
        c.setPosition(sub_.matchStart);
        c.setPosition(sub_.matchStart + sub_.matchLength, QTextCursor::KeepAnchor);
        c.insertText(text);
        endChange();
        undoJoin_ = false;
        sub_.joinUndo = true;
    }
    ++sub_.count;
    if (!sub_.linesChanged.contains(sub_.line))
    {
        sub_.linesChanged << sub_.line;
    }
    // A replacement with line breaks ("\r") moves the lines below.
    sub_.lastLine += static_cast<int>(text.count(QLatin1Char('\n')));
    const int end = sub_.matchStart + static_cast<int>(text.size());
    sub_.lastChangedLine = lineOf(end);
    if (sub_.global)
    {
        sub_.line = lineOf(end);
        sub_.column = end - lineStart(sub_.line);
        if (sub_.matchLength == 0)
        {
            // An empty match: step over a character so it isn't found again.
            if (sub_.column < lineText(sub_.line).size())
            {
                ++sub_.column;
            }
            else
            {
                ++sub_.line;
                sub_.column = 0;
            }
        }
    }
    else
    {
        sub_.line = lineOf(end) + 1;
        sub_.column = 0;
    }
}

void VimEngine::finishSubstitution()
{
    confirming_ = false;
    incMatch_ = -1;
    if (sub_.count == 0)
    {
        if (!sub_.quiet)
        {
            showMessage(QStringLiteral("E486: Pattern not found: %1").arg(lastPattern_),
                        true);
        }
        else
        {
            message_.clear();
        }
        return;
    }
    if (!sub_.countOnly && sub_.lastChangedLine >= 0)
    {
        setPos(firstNonBlank(std::min(sub_.lastChangedLine, lineCount() - 1)));
        wantedColumn_ = columnOf(pos());
    }
    const int lines = static_cast<int>(sub_.linesChanged.size());
    auto plural = [](int n, const char *one, const char *many) {
        return QStringLiteral("%1 %2").arg(n).arg(QLatin1String(n == 1 ? one : many));
    };
    if (sub_.countOnly)
    {
        showMessage(plural(sub_.count, "match", "matches") + QStringLiteral(" on ") +
                    plural(lines, "line", "lines"));
    }
    else if (sub_.count > 2) // Vim's 'report'
    {
        showMessage(plural(sub_.count, "substitution", "substitutions") +
                    QStringLiteral(" on ") + plural(lines, "line", "lines"));
    }
    else
    {
        message_.clear();
    }
}

// The answer to "replace with ... (y/n/a/q/l/^E/^Y)?".
void VimEngine::confirmKey(const QString &key)
{
    if (key == QLatin1String("y"))
    {
        replaceCurrentMatch();
        runSubstitution();
    }
    else if (key == QLatin1String("l"))
    {
        replaceCurrentMatch();
        finishSubstitution();
    }
    else if (key == QLatin1String("a"))
    {
        sub_.confirm = false;
        replaceCurrentMatch();
        runSubstitution();
    }
    else if (key == QLatin1String("n"))
    {
        if (sub_.global)
        {
            sub_.column = sub_.matchStart - lineStart(sub_.line) +
                          std::max(1, sub_.matchLength);
        }
        else
        {
            ++sub_.line;
            sub_.column = 0;
        }
        runSubstitution();
    }
    else if (key == QLatin1String("q") || key == QLatin1String("<Esc>") ||
             key == QLatin1String("<C-c>"))
    {
        finishSubstitution();
    }
    else if (key == QLatin1String("<C-e>") || key == QLatin1String("<C-y>"))
    {
        QScrollBar *bar = editor_->verticalScrollBar();
        bar->setValue(bar->value() + (key == QLatin1String("<C-e>") ? 1 : -1));
    }
}

// Vim's replacement string: "&" and "\0" the match, "\1".."\9" groups,
// "\r" / "\n" a line break, "\t" a tab, "\u" "\l" the next character's
// case, "\U" "\L" up to "\E" / "\e".
QString VimEngine::expandReplacement(const QString &replacement,
                                     const QRegularExpressionMatch &match) const
{
    QString out;
    enum class Case
    {
        None,
        Upper,
        Lower
    };
    Case lasting = Case::None;
    Case once = Case::None;
    auto append = [&](const QString &text) {
        for (QChar c : text)
        {
            if (once != Case::None)
            {
                c = once == Case::Upper ? c.toUpper() : c.toLower();
                once = Case::None;
            }
            else if (lasting != Case::None)
            {
                c = lasting == Case::Upper ? c.toUpper() : c.toLower();
            }
            out += c;
        }
    };
    for (int i = 0; i < replacement.size(); ++i)
    {
        const QChar c = replacement.at(i);
        if (c == QLatin1Char('&'))
        {
            append(match.captured(0));
            continue;
        }
        if (c != QLatin1Char('\\') || i + 1 >= replacement.size())
        {
            append(QString(c));
            continue;
        }
        const QChar n = replacement.at(++i);
        if (n.isDigit())
        {
            append(match.captured(n.digitValue()));
        }
        else if (n == QLatin1Char('r') || n == QLatin1Char('n'))
        {
            out += QLatin1Char('\n');
        }
        else if (n == QLatin1Char('t'))
        {
            out += QLatin1Char('\t');
        }
        else if (n == QLatin1Char('u'))
        {
            once = Case::Upper;
        }
        else if (n == QLatin1Char('l'))
        {
            once = Case::Lower;
        }
        else if (n == QLatin1Char('U'))
        {
            lasting = Case::Upper;
        }
        else if (n == QLatin1Char('L'))
        {
            lasting = Case::Lower;
        }
        else if (n == QLatin1Char('E') || n == QLatin1Char('e'))
        {
            lasting = Case::None;
        }
        else
        {
            append(QString(n)); // "\&", "\\", "\~", ...
        }
    }
    return out;
}

// ---------------------------------------------------------------- options

bool VimEngine::setOption(const QString &rawArg)
{
    QString arg = rawArg.trimmed();
    if (arg.isEmpty() || arg == QLatin1String("all"))
    {
        return true;
    }
    static const QHash<QString, QString> aliases{
        {QStringLiteral("nu"), QStringLiteral("number")},
        {QStringLiteral("rnu"), QStringLiteral("relativenumber")},
        {QStringLiteral("ts"), QStringLiteral("tabstop")},
        {QStringLiteral("sts"), QStringLiteral("softtabstop")},
        {QStringLiteral("sw"), QStringLiteral("shiftwidth")},
        {QStringLiteral("et"), QStringLiteral("expandtab")},
        {QStringLiteral("ai"), QStringLiteral("autoindent")},
        {QStringLiteral("cin"), QStringLiteral("cindent")},
        {QStringLiteral("si"), QStringLiteral("smartindent")},
        {QStringLiteral("ic"), QStringLiteral("ignorecase")},
        {QStringLiteral("scs"), QStringLiteral("smartcase")},
        {QStringLiteral("is"), QStringLiteral("incsearch")},
        {QStringLiteral("hls"), QStringLiteral("hlsearch")},
        {QStringLiteral("ws"), QStringLiteral("wrapscan")},
        {QStringLiteral("cb"), QStringLiteral("clipboard")},
        {QStringLiteral("to"), QStringLiteral("timeout")},
        {QStringLiteral("tm"), QStringLiteral("timeoutlen")},
    };
    // Accepted and ignored: they make no difference here.
    static const QStringList ignored{
        QStringLiteral("compatible"),  QStringLiteral("cp"),
        QStringLiteral("background"),  QStringLiteral("bg"),
        QStringLiteral("backspace"),   QStringLiteral("bs"),
        QStringLiteral("backup"),      QStringLiteral("bk"),
        QStringLiteral("backupdir"),   QStringLiteral("bdir"),
        QStringLiteral("writebackup"), QStringLiteral("wb"),
        QStringLiteral("directory"),   QStringLiteral("dir"),
        QStringLiteral("undodir"),     QStringLiteral("udir"),
        QStringLiteral("undofile"),    QStringLiteral("udf"),
        QStringLiteral("swapfile"),    QStringLiteral("swf"),
        QStringLiteral("syntax"),      QStringLiteral("syn"),
        QStringLiteral("formatoptions"), QStringLiteral("fo"),
        QStringLiteral("encoding"),    QStringLiteral("enc"),
        QStringLiteral("fileencoding"), QStringLiteral("fenc"),
        QStringLiteral("fileencodings"), QStringLiteral("fencs"),
        QStringLiteral("fileformat"),  QStringLiteral("ff"),
        QStringLiteral("fileformats"), QStringLiteral("ffs"),
        QStringLiteral("termguicolors"), QStringLiteral("tgc"),
        QStringLiteral("mouse"),       QStringLiteral("ruler"),
        QStringLiteral("ru"),          QStringLiteral("showcmd"),
        QStringLiteral("sc"),          QStringLiteral("showmode"),
        QStringLiteral("smd"),         QStringLiteral("laststatus"),
        QStringLiteral("ls"),          QStringLiteral("wildmenu"),
        QStringLiteral("wmnu"),        QStringLiteral("wildmode"),
        QStringLiteral("wim"),         QStringLiteral("history"),
        QStringLiteral("hi"),          QStringLiteral("scrolloff"),
        QStringLiteral("so"),          QStringLiteral("sidescrolloff"),
        QStringLiteral("siso"),        QStringLiteral("hidden"),
        QStringLiteral("hid"),         QStringLiteral("cursorline"),
        QStringLiteral("cul"),         QStringLiteral("cursorcolumn"),
        QStringLiteral("cuc"),         QStringLiteral("list"),
        QStringLiteral("listchars"),   QStringLiteral("lcs"),
        QStringLiteral("wrap"),        QStringLiteral("linebreak"),
        QStringLiteral("lbr"),         QStringLiteral("spell"),
        QStringLiteral("spelllang"),   QStringLiteral("spl"),
        QStringLiteral("belloff"),     QStringLiteral("bo"),
        QStringLiteral("visualbell"),  QStringLiteral("vb"),
        QStringLiteral("errorbells"),  QStringLiteral("eb"),
        QStringLiteral("ttyfast"),     QStringLiteral("tf"),
        QStringLiteral("lazyredraw"),  QStringLiteral("lz"),
        QStringLiteral("modeline"),    QStringLiteral("ml"),
        QStringLiteral("modelines"),   QStringLiteral("mls"),
        QStringLiteral("foldmethod"),  QStringLiteral("fdm"),
        QStringLiteral("foldlevel"),   QStringLiteral("fdl"),
        QStringLiteral("foldenable"),  QStringLiteral("fen"),
        QStringLiteral("colorcolumn"), QStringLiteral("cc"),
        QStringLiteral("signcolumn"),  QStringLiteral("scl"),
        QStringLiteral("updatetime"),  QStringLiteral("ut"),
        QStringLiteral("splitbelow"),  QStringLiteral("sb"),
        QStringLiteral("splitright"),  QStringLiteral("spr"),
        QStringLiteral("autoread"),    QStringLiteral("ar"),
        QStringLiteral("showmatch"),   QStringLiteral("sm"),
        QStringLiteral("title"),       QStringLiteral("magic"),
        QStringLiteral("smarttab"),    QStringLiteral("sta"),
        QStringLiteral("shortmess"),   QStringLiteral("shm"),
        QStringLiteral("completeopt"), QStringLiteral("cot"),
        QStringLiteral("textwidth"),   QStringLiteral("tw"),
        QStringLiteral("numberwidth"), QStringLiteral("nuw"),
        QStringLiteral("cmdheight"),   QStringLiteral("ch"),
        QStringLiteral("ttimeout"),    QStringLiteral("ttimeoutlen"),
        QStringLiteral("ttm"),         QStringLiteral("guifont"),
        QStringLiteral("gfn"),         QStringLiteral("guioptions"),
        QStringLiteral("go"),          QStringLiteral("filetype"),
        QStringLiteral("ft"),          QStringLiteral("viminfo"),
        QStringLiteral("vi"),          QStringLiteral("shada"),
        QStringLiteral("sd"),          QStringLiteral("display"),
        QStringLiteral("dy"),          QStringLiteral("hlsearch_"),
        QStringLiteral("t_Co"),        QStringLiteral("nocompatible"),
        QStringLiteral("whichwrap"),   QStringLiteral("ww"),
        QStringLiteral("virtualedit"), QStringLiteral("ve"),
        QStringLiteral("iskeyword"),   QStringLiteral("isk"),
        QStringLiteral("path"),        QStringLiteral("pa"),
        QStringLiteral("tags"),        QStringLiteral("tag"),
        QStringLiteral("autochdir"),   QStringLiteral("acd")};

    auto canonical = [&](const QString &name) {
        return aliases.value(name, name);
    };
    QHash<QString, bool *> bools{
        {QStringLiteral("number"), &options_.number},
        {QStringLiteral("relativenumber"), &options_.relativeNumber},
        {QStringLiteral("expandtab"), &options_.expandTab},
        {QStringLiteral("autoindent"), &options_.autoIndent},
        {QStringLiteral("cindent"), &options_.autoIndent},
        {QStringLiteral("smartindent"), &options_.autoIndent},
        {QStringLiteral("ignorecase"), &options_.ignoreCase},
        {QStringLiteral("smartcase"), &options_.smartCase},
        {QStringLiteral("incsearch"), &options_.incSearch},
        {QStringLiteral("hlsearch"), &options_.hlSearch},
        {QStringLiteral("wrapscan"), &options_.wrapScan},
        {QStringLiteral("timeout"), &options_.timeout},
    };
    QHash<QString, int *> ints{
        {QStringLiteral("tabstop"), &options_.tabStop},
        {QStringLiteral("softtabstop"), &options_.softTabStop},
        {QStringLiteral("shiftwidth"), &options_.shiftWidth},
        {QStringLiteral("timeoutlen"), &options_.timeoutLen},
    };

    const bool query = arg.endsWith(QLatin1Char('?'));
    if (query)
    {
        arg.chop(1);
    }
    const int eq = static_cast<int>(arg.indexOf(QRegularExpression(QStringLiteral("[=:]"))));
    if (eq > 0)
    {
        QString name = arg.left(eq);
        QChar op;
        if (name.endsWith(QLatin1Char('+')) || name.endsWith(QLatin1Char('-')) ||
            name.endsWith(QLatin1Char('^')))
        {
            op = name.back();
            name.chop(1);
        }
        name = canonical(name);
        const QString value = arg.mid(eq + 1);
        if (ints.contains(name))
        {
            bool ok = false;
            const int n = value.toInt(&ok);
            if (!ok)
            {
                return false;
            }
            int &target = *ints.value(name);
            target = op == QLatin1Char('+')   ? target + n
                     : op == QLatin1Char('-') ? target - n
                     : op == QLatin1Char('^') ? target * n
                                              : n;
        }
        else if (name == QLatin1String("clipboard"))
        {
            if (op == QLatin1Char('+'))
            {
                options_.clipboard = value;
            }
            else if (op == QLatin1Char('-'))
            {
                options_.clipboard.clear();
            }
            else
            {
                options_.clipboard = value;
            }
        }
        else if (!ignored.contains(name))
        {
            return false;
        }
        emit optionsChanged();
        return true;
    }

    QString name = canonical(arg);
    bool value = true;
    if (name.endsWith(QLatin1Char('!')))
    {
        name = canonical(name.chopped(1));
        if (bools.contains(name))
        {
            *bools.value(name) = !*bools.value(name);
            emit optionsChanged();
            return true;
        }
        return ignored.contains(name);
    }
    if (!bools.contains(name) && !ints.contains(name) &&
        name != QLatin1String("clipboard") && !ignored.contains(name))
    {
        if (name.startsWith(QLatin1String("inv")) &&
            bools.contains(canonical(name.mid(3))))
        {
            bool *target = bools.value(canonical(name.mid(3)));
            *target = !*target;
            emit optionsChanged();
            return true;
        }
        if (name.startsWith(QLatin1String("no")))
        {
            name = canonical(name.mid(2));
            value = false;
        }
    }
    if (query)
    {
        if (bools.contains(name))
        {
            showMessage((*bools.value(name) ? QStringLiteral("  ")
                                            : QStringLiteral("no")) +
                        name);
        }
        else if (ints.contains(name))
        {
            showMessage(QStringLiteral("  %1=%2").arg(name).arg(*ints.value(name)));
        }
        else if (name == QLatin1String("clipboard"))
        {
            showMessage(QStringLiteral("  clipboard=") + options_.clipboard);
        }
        return true;
    }
    if (bools.contains(name))
    {
        *bools.value(name) = value;
        emit optionsChanged();
        return true;
    }
    if (ints.contains(name))
    {
        showMessage(QStringLiteral("  %1=%2").arg(name).arg(*ints.value(name)));
        return true;
    }
    return ignored.contains(name) || name == QLatin1String("clipboard");
}

// ---------------------------------------------------------------- vimrc

void VimEngine::loadVimrc()
{
    const QString home = QDir::homePath();
    QString config = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (config.isEmpty())
    {
        config = home + QStringLiteral("/.config");
    }
    const QStringList candidates{home + QStringLiteral("/.vimrc"),
                                 home + QStringLiteral("/.vim/vimrc"),
                                 config + QStringLiteral("/vim/vimrc")};
    QString path;
    for (const QString &candidate : candidates)
    {
        if (QFileInfo::exists(candidate))
        {
            path = candidate;
            break;
        }
    }
    if (path.isEmpty())
    {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return;
    }
    vimrcPath_ = path;

    // Join continuation lines ("\ ..." continues the line before).
    QStringList lines;
    for (QString line : QString::fromUtf8(file.readAll()).split(QLatin1Char('\n')))
    {
        if (line.endsWith(QLatin1Char('\r')))
        {
            line.chop(1);
        }
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QLatin1Char('\\')) && !lines.isEmpty())
        {
            lines.last() += trimmed.mid(1);
        }
        else
        {
            lines << line;
        }
    }

    // Conditionals, functions and embedded scripts can't be evaluated here
    // (they are mostly about plugins); they are skipped whole.
    int skipDepth = 0;
    QString heredocEnd;
    int unsupported = 0;
    static const QRegularExpression startsBlock(
        QStringLiteral("^(if|fu|fun|func|function|for|while|try|aug|augroup)\\b!?\\s*(.*)$"));
    static const QRegularExpression endsBlock(
        QStringLiteral("^(en|endif|endf|endfu|endfun|endfunc|endfunction|"
                       "endfor|endwhile|endw|endtry|endt)\\b"));
    static const QRegularExpression heredoc(
        QStringLiteral("^(lua|py|python|python3|py3|perl|ruby)\\s*<<\\s*(\\S*)"));
    for (const QString &line : lines)
    {
        const QString trimmed = line.trimmed();
        if (!heredocEnd.isEmpty())
        {
            if (trimmed == heredocEnd)
            {
                heredocEnd.clear();
            }
            continue;
        }
        const auto doc = heredoc.match(trimmed);
        if (doc.hasMatch())
        {
            heredocEnd = doc.captured(2).isEmpty() ? QStringLiteral(".")
                                                   : doc.captured(2);
            continue;
        }
        const auto start = startsBlock.match(trimmed);
        if (start.hasMatch())
        {
            // "augroup NAME" opens, "augroup END" closes.
            const bool augroup = start.captured(1).startsWith(QLatin1String("aug"));
            if (augroup && start.captured(2).compare(QLatin1String("END"),
                                                     Qt::CaseInsensitive) == 0)
            {
                skipDepth = std::max(0, skipDepth - 1);
            }
            else
            {
                ++skipDepth;
            }
            continue;
        }
        if (endsBlock.match(trimmed).hasMatch())
        {
            skipDepth = std::max(0, skipDepth - 1);
            continue;
        }
        if (skipDepth > 0)
        {
            continue;
        }
        if (!sourceLine(line, true))
        {
            ++unsupported;
        }
    }
    if (unsupported > 0)
    {
        showMessage(QStringLiteral("%1: %2 line(s) not understood")
                        .arg(QDir::toNativeSeparators(path))
                        .arg(unsupported));
    }
    emit optionsChanged();
}

// One line of a vimrc (or a ":" command): settings and mappings. False if
// it isn't understood.
bool VimEngine::sourceLine(const QString &rawLine, bool fromVimrc)
{
    // Only leading white space goes: a mapping keeps trailing spaces in its
    // right side, as in Vim.
    QString line = rawLine;
    while (!line.isEmpty() && line.at(0).isSpace())
    {
        line.remove(0, 1);
    }
    while (line.startsWith(QLatin1Char(':')))
    {
        line.remove(0, 1);
    }
    if (line.isEmpty() || line.startsWith(QLatin1Char('"')))
    {
        return true;
    }
    int i = 0;
    while (i < line.size() && line.at(i).isLetter())
    {
        ++i;
    }
    const QString name = line.left(i);
    const bool bang = i < line.size() && line.at(i) == QLatin1Char('!');
    QString rest = line.mid(bang ? i + 1 : i);
    while (!rest.isEmpty() && rest.at(0).isSpace())
    {
        rest.remove(0, 1);
    }

    if (name == QLatin1String("set") || name == QLatin1String("se") ||
        name == QLatin1String("setlocal") || name == QLatin1String("setl") ||
        name == QLatin1String("setglobal") || name == QLatin1String("setg"))
    {
        bool all = true;
        const QStringList parts =
            stripComment(rest).split(QRegularExpression(QStringLiteral("(?<!\\\\)\\s+")),
                                     Qt::SkipEmptyParts);
        for (const QString &part : parts)
        {
            if (!setOption(part))
            {
                all = false;
                if (!fromVimrc)
                {
                    showMessage(QStringLiteral("E518: Unknown option: %1").arg(part),
                                true);
                }
            }
        }
        return all;
    }
    if (name == QLatin1String("let"))
    {
        static const QRegularExpression leader(QStringLiteral(
            "^(g:)?(mapleader|maplocalleader)\\s*=\\s*([\"'])(.*?)\\3"));
        const auto m = leader.match(rest.trimmed());
        if (m.hasMatch() && m.captured(2) == QLatin1String("mapleader"))
        {
            QString value = m.captured(4);
            if (value == QLatin1String("\\<Space>") || value == QLatin1String("<Space>"))
            {
                value = QStringLiteral(" ");
            }
            mapLeader_ = value;
        }
        return true; // other variables configure plugins
    }

    // Mapping commands: name -> modes, and whether it is non-recursive.
    struct MapCommand
    {
        QString modes;
        bool noremap;
        bool unmap;
    };
    static const QHash<QString, MapCommand> mapCommands{
        {QStringLiteral("map"), {QStringLiteral("nxo"), false, false}},
        {QStringLiteral("nm"), {QStringLiteral("n"), false, false}},
        {QStringLiteral("nmap"), {QStringLiteral("n"), false, false}},
        {QStringLiteral("vm"), {QStringLiteral("x"), false, false}},
        {QStringLiteral("vmap"), {QStringLiteral("x"), false, false}},
        {QStringLiteral("xm"), {QStringLiteral("x"), false, false}},
        {QStringLiteral("xmap"), {QStringLiteral("x"), false, false}},
        {QStringLiteral("om"), {QStringLiteral("o"), false, false}},
        {QStringLiteral("omap"), {QStringLiteral("o"), false, false}},
        {QStringLiteral("im"), {QStringLiteral("i"), false, false}},
        {QStringLiteral("imap"), {QStringLiteral("i"), false, false}},
        {QStringLiteral("cm"), {QStringLiteral("c"), false, false}},
        {QStringLiteral("cmap"), {QStringLiteral("c"), false, false}},
        {QStringLiteral("no"), {QStringLiteral("nxo"), true, false}},
        {QStringLiteral("noremap"), {QStringLiteral("nxo"), true, false}},
        {QStringLiteral("nn"), {QStringLiteral("n"), true, false}},
        {QStringLiteral("nno"), {QStringLiteral("n"), true, false}},
        {QStringLiteral("nnoremap"), {QStringLiteral("n"), true, false}},
        {QStringLiteral("vn"), {QStringLiteral("x"), true, false}},
        {QStringLiteral("vno"), {QStringLiteral("x"), true, false}},
        {QStringLiteral("vnoremap"), {QStringLiteral("x"), true, false}},
        {QStringLiteral("xn"), {QStringLiteral("x"), true, false}},
        {QStringLiteral("xno"), {QStringLiteral("x"), true, false}},
        {QStringLiteral("xnoremap"), {QStringLiteral("x"), true, false}},
        {QStringLiteral("ono"), {QStringLiteral("o"), true, false}},
        {QStringLiteral("onoremap"), {QStringLiteral("o"), true, false}},
        {QStringLiteral("ino"), {QStringLiteral("i"), true, false}},
        {QStringLiteral("inoremap"), {QStringLiteral("i"), true, false}},
        {QStringLiteral("cno"), {QStringLiteral("c"), true, false}},
        {QStringLiteral("cnoremap"), {QStringLiteral("c"), true, false}},
        {QStringLiteral("unm"), {QStringLiteral("nxo"), false, true}},
        {QStringLiteral("unmap"), {QStringLiteral("nxo"), false, true}},
        {QStringLiteral("nun"), {QStringLiteral("n"), false, true}},
        {QStringLiteral("nunmap"), {QStringLiteral("n"), false, true}},
        {QStringLiteral("vu"), {QStringLiteral("x"), false, true}},
        {QStringLiteral("vunmap"), {QStringLiteral("x"), false, true}},
        {QStringLiteral("xu"), {QStringLiteral("x"), false, true}},
        {QStringLiteral("xunmap"), {QStringLiteral("x"), false, true}},
        {QStringLiteral("ou"), {QStringLiteral("o"), false, true}},
        {QStringLiteral("ounmap"), {QStringLiteral("o"), false, true}},
        {QStringLiteral("iu"), {QStringLiteral("i"), false, true}},
        {QStringLiteral("iunmap"), {QStringLiteral("i"), false, true}},
        {QStringLiteral("cu"), {QStringLiteral("c"), false, true}},
        {QStringLiteral("cunmap"), {QStringLiteral("c"), false, true}},
    };
    if (mapCommands.contains(name))
    {
        MapCommand mc = mapCommands.value(name);
        if (bang && (name.startsWith(QLatin1String("map")) ||
                     name.startsWith(QLatin1String("no")) ||
                     name.startsWith(QLatin1String("unm"))))
        {
            mc.modes = QStringLiteral("ic"); // "map!", "noremap!"
        }
        addMapping(QStringLiteral("%1|%2|%3")
                       .arg(mc.modes, mc.noremap ? QStringLiteral("1") : QStringLiteral("0"),
                            mc.unmap ? QStringLiteral("1") : QStringLiteral("0")),
                   rest);
        return true;
    }

    // Plugins, appearance, autocommands: nothing to do here.
    static const QStringList ignoredCommands{
        QStringLiteral("syntax"),      QStringLiteral("syn"),
        QStringLiteral("filetype"),    QStringLiteral("filet"),
        QStringLiteral("colorscheme"), QStringLiteral("colo"),
        QStringLiteral("autocmd"),     QStringLiteral("au"),
        QStringLiteral("call"),        QStringLiteral("cal"),
        QStringLiteral("Plug"),        QStringLiteral("Plugin"),
        QStringLiteral("silent"),      QStringLiteral("sil"),
        QStringLiteral("source"),      QStringLiteral("so"),
        QStringLiteral("runtime"),     QStringLiteral("ru"),
        QStringLiteral("command"),     QStringLiteral("com"),
        QStringLiteral("highlight"),   QStringLiteral("hi"),
        QStringLiteral("packadd"),     QStringLiteral("pa"),
        QStringLiteral("execute"),     QStringLiteral("exe"),
        QStringLiteral("echo"),        QStringLiteral("ec"),
        QStringLiteral("unlet"),       QStringLiteral("scriptencoding"),
        QStringLiteral("set_"),        QStringLiteral("abbreviate"),
        QStringLiteral("ab"),          QStringLiteral("iabbrev"),
        QStringLiteral("iab"),         QStringLiteral("cabbrev"),
        QStringLiteral("cab"),         QStringLiteral("smap"),
        QStringLiteral("snoremap"),    QStringLiteral("lmap"),
        QStringLiteral("lnoremap"),    QStringLiteral("tnoremap"),
        QStringLiteral("tmap")};
    if (fromVimrc && ignoredCommands.contains(name))
    {
        return true;
    }
    return false;
}

// `kind` is "modes|noremap|unmap", `args` the rest of the command line.
void VimEngine::addMapping(const QString &kind, const QString &args)
{
    const QStringList parts = kind.split(QLatin1Char('|'));
    const QString modes = parts.value(0);
    const bool noremap = parts.value(1) == QLatin1String("1");
    const bool unmap = parts.value(2) == QLatin1String("1");

    QString rest = args;
    // Attributes; "<expr>" mappings can't be evaluated here.
    static const QRegularExpression attribute(
        QStringLiteral("^<(silent|buffer|nowait|unique|special|script|expr)>\\s*"),
        QRegularExpression::CaseInsensitiveOption);
    bool expression = false;
    for (auto m = attribute.match(rest); m.hasMatch(); m = attribute.match(rest))
    {
        if (m.captured(1).compare(QLatin1String("expr"), Qt::CaseInsensitive) == 0)
        {
            expression = true;
        }
        rest = rest.mid(m.capturedLength());
    }
    if (expression)
    {
        return;
    }
    // The left side ends at the first unescaped white space; the right side
    // keeps everything after the white space that follows it, trailing
    // spaces included, as in Vim.
    int i = 0;
    while (i < rest.size() && !rest.at(i).isSpace())
    {
        i += rest.at(i) == QLatin1Char('\\') && i + 1 < rest.size() ? 2 : 1;
    }
    const QString lhsText = rest.left(i);
    while (i < rest.size() && rest.at(i).isSpace())
    {
        ++i;
    }
    const QString rhsText = rest.mid(i);
    if (lhsText.isEmpty())
    {
        return;
    }

    auto expandLeader = [&](const QStringList &keys) {
        QStringList out;
        for (const QString &key : keys)
        {
            if (key == QLatin1String("<Leader>"))
            {
                out << parseKeys(mapLeader_);
            }
            else
            {
                out << key;
            }
        }
        return out;
    };
    const QStringList lhs = expandLeader(parseKeys(lhsText));
    if (lhs.isEmpty())
    {
        return;
    }
    if (!unmap && rhsText.isEmpty())
    {
        return; // ":nmap x" only lists mappings
    }
    const QStringList rhs = expandLeader(parseKeys(rhsText));

    for (const QChar mode : modes)
    {
        QList<Mapping> &list = mappings_[mode];
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const Mapping &m) { return m.lhs == lhs; }),
                   list.end());
        if (!unmap)
        {
            list.append({lhs, rhs, noremap});
        }
    }
}

// ---------------------------------------------------------------- registers

void VimEngine::setRegister(QChar name, const Register &value, bool isDelete)
{
    if (name == QLatin1Char('_'))
    {
        return;
    }
    auto toClipboard = [](QChar which, const Register &r) {
        QClipboard *clipboard = QGuiApplication::clipboard();
        const QString text = r.kind == Register::Lines ? r.text + QLatin1Char('\n')
                                                       : r.text;
        const bool selection = which == QLatin1Char('*') && clipboard->supportsSelection();
        clipboard->setText(text, selection ? QClipboard::Selection
                                           : QClipboard::Clipboard);
    };

    Register stored = value;
    if (name.isUpper())
    {
        // "Ayy appends to register a.
        const QChar lower = name.toLower();
        Register old = registers_.value(lower);
        if (!old.text.isEmpty())
        {
            const bool lines = old.kind == Register::Lines || value.kind == Register::Lines;
            stored.text = old.text + (lines ? QStringLiteral("\n") : QString()) + value.text;
            stored.kind = lines ? Register::Lines : old.kind;
        }
        name = lower;
    }
    if (name == QLatin1Char('+') || name == QLatin1Char('*'))
    {
        toClipboard(name, stored);
    }
    else if (name != QLatin1Char('"'))
    {
        registers_[name] = stored;
    }
    registers_[QLatin1Char('"')] = stored;

    if (name == QLatin1Char('"'))
    {
        if (options_.clipboard.contains(QLatin1String("unnamedplus")))
        {
            toClipboard(QLatin1Char('+'), stored);
        }
        else if (options_.clipboard.contains(QLatin1String("unnamed")))
        {
            toClipboard(QLatin1Char('*'), stored);
        }
        if (!isDelete)
        {
            registers_[QLatin1Char('0')] = stored;
        }
    }
    if (isDelete && name == QLatin1Char('"'))
    {
        if (stored.kind == Register::Lines || stored.text.contains(QLatin1Char('\n')))
        {
            for (char r = '9'; r > '1'; --r)
            {
                registers_[QLatin1Char(r)] = registers_.value(QLatin1Char(r - 1));
            }
            registers_[QLatin1Char('1')] = stored;
        }
        else
        {
            registers_[QLatin1Char('-')] = stored;
        }
    }
}

VimEngine::Register VimEngine::getRegister(QChar name) const
{
    auto fromClipboard = [](QChar which) {
        QClipboard *clipboard = QGuiApplication::clipboard();
        const bool selection = which == QLatin1Char('*') && clipboard->supportsSelection();
        Register r;
        r.text = clipboard->text(selection ? QClipboard::Selection
                                           : QClipboard::Clipboard);
        r.text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
        if (r.text.endsWith(QLatin1Char('\n')))
        {
            r.text.chop(1);
            r.kind = Register::Lines;
        }
        return r;
    };
    if (name == QLatin1Char('+') || name == QLatin1Char('*'))
    {
        return fromClipboard(name);
    }
    if (name == QLatin1Char('"'))
    {
        if (options_.clipboard.contains(QLatin1String("unnamedplus")))
        {
            return fromClipboard(QLatin1Char('+'));
        }
        if (options_.clipboard.contains(QLatin1String("unnamed")))
        {
            return fromClipboard(QLatin1Char('*'));
        }
    }
    if (name == QLatin1Char('/'))
    {
        Register r;
        r.text = lastPattern_;
        return r;
    }
    if (name == QLatin1Char(':'))
    {
        Register r;
        r.text = history_.value(QLatin1Char(':')).value(
            history_.value(QLatin1Char(':')).size() - 1);
        return r;
    }
    return registers_.value(name.toLower());
}

// ---------------------------------------------------------------- search

// Vim's "magic" patterns, translated to Perl-style ones.
QRegularExpression VimEngine::compilePattern(const QString &pattern) const
{
    QString out;
    bool caseSensitive = !options_.ignoreCase;
    bool forcedCase = false;
    bool hasUpper = false;
    for (int i = 0; i < pattern.size(); ++i)
    {
        const QChar c = pattern.at(i);
        if (c == QLatin1Char('\\') && i + 1 < pattern.size())
        {
            const QChar n = pattern.at(++i);
            switch (n.unicode())
            {
            case '<':
            case '>':
                out += QStringLiteral("\\b");
                break;
            case '(':
            case ')':
            case '|':
            case '+':
            case '?':
            case '{':
            case '}':
                out += n;
                break;
            case '=':
                out += QLatin1Char('?');
                break;
            case 's':
                out += QStringLiteral("[ \\t]");
                break;
            case 'S':
                out += QStringLiteral("[^ \\t]");
                break;
            case 'a':
                out += QStringLiteral("[A-Za-z]");
                break;
            case 'l':
                out += QStringLiteral("[a-z]");
                break;
            case 'u':
                out += QStringLiteral("[A-Z]");
                break;
            case 'x':
                out += QStringLiteral("[0-9A-Fa-f]");
                break;
            case 'd':
            case 'D':
            case 'w':
            case 'W':
            case 'n':
            case 't':
                out += QLatin1Char('\\');
                out += n;
                break;
            case 'c':
                caseSensitive = false;
                forcedCase = true;
                break;
            case 'C':
                caseSensitive = true;
                forcedCase = true;
                break;
            default:
                if (n.isDigit())
                {
                    out += QLatin1Char('\\');
                    out += n;
                }
                else
                {
                    out += QRegularExpression::escape(QString(n));
                }
                break;
            }
            continue;
        }
        if (c.isUpper())
        {
            hasUpper = true;
        }
        static const QString literal = QStringLiteral("()|+?{}");
        if (literal.contains(c))
        {
            out += QLatin1Char('\\');
            out += c;
        }
        else
        {
            out += c;
        }
    }
    if (!forcedCase && options_.ignoreCase && options_.smartCase && hasUpper)
    {
        caseSensitive = true;
    }
    QRegularExpression::PatternOptions flags =
        QRegularExpression::MultilineOption | QRegularExpression::UseUnicodePropertiesOption;
    if (!caseSensitive)
    {
        flags |= QRegularExpression::CaseInsensitiveOption;
    }
    return QRegularExpression(out, flags);
}

int VimEngine::search(const QString &pattern, bool forward, int from, int count,
                      bool showMessages)
{
    const QRegularExpression re = compilePattern(pattern);
    if (!re.isValid())
    {
        if (showMessages)
        {
            showMessage(QStringLiteral("E383: Invalid search string: %1").arg(pattern),
                        true);
        }
        return -1;
    }
    const QString text = doc()->toPlainText();
    int p = from;
    bool wrapped = false;
    for (int k = 0; k < std::max(1, count); ++k)
    {
        int found = -1;
        if (forward)
        {
            QRegularExpressionMatch m;
            if (p + 1 <= text.size())
            {
                m = re.match(text, p + 1);
            }
            if (m.hasMatch())
            {
                found = static_cast<int>(m.capturedStart());
            }
            else if (options_.wrapScan)
            {
                m = re.match(text, 0);
                if (m.hasMatch() && m.capturedStart() <= p)
                {
                    found = static_cast<int>(m.capturedStart());
                    wrapped = true;
                }
            }
        }
        else
        {
            int before = -1;
            int last = -1;
            auto it = re.globalMatch(text);
            while (it.hasNext())
            {
                const QRegularExpressionMatch m = it.next();
                const int start = static_cast<int>(m.capturedStart());
                if (start < p)
                {
                    before = start;
                }
                last = start;
            }
            if (before >= 0)
            {
                found = before;
            }
            else if (options_.wrapScan && last >= p)
            {
                found = last;
                wrapped = true;
            }
        }
        if (found < 0)
        {
            if (showMessages)
            {
                showMessage(options_.wrapScan
                                ? QStringLiteral("E486: Pattern not found: %1").arg(pattern)
                                : (forward ? QStringLiteral("E385: Search hit BOTTOM "
                                                            "without match for: %1")
                                           : QStringLiteral("E384: Search hit TOP "
                                                            "without match for: %1"))
                                      .arg(pattern),
                            true);
            }
            return -1;
        }
        p = found;
    }
    if (showMessages)
    {
        if (wrapped)
        {
            showMessage(forward ? QStringLiteral("search hit BOTTOM, continuing at TOP")
                                : QStringLiteral("search hit TOP, continuing at BOTTOM"),
                        true);
        }
        else
        {
            showMessage(QString(forward ? QLatin1Char('/') : QLatin1Char('?')) + pattern);
        }
    }
    return p;
}

// ---------------------------------------------------------------- display

void VimEngine::showMessage(const QString &text, bool error)
{
    message_ = text;
    messageIsError_ = error;
}

QString VimEngine::modeText() const
{
    switch (mode_)
    {
    case Mode::Insert:
        return QStringLiteral("-- INSERT --");
    case Mode::Replace:
        return QStringLiteral("-- REPLACE --");
    case Mode::Visual:
        return QStringLiteral("-- VISUAL --");
    case Mode::VisualLine:
        return QStringLiteral("-- VISUAL LINE --");
    case Mode::VisualBlock:
        return QStringLiteral("-- VISUAL BLOCK --");
    default:
        return {};
    }
}

QString VimEngine::commandLineText() const
{
    if (mode_ != Mode::CommandLine)
    {
        return {};
    }
    return QString(cmdType_) + cmdText_;
}

QString VimEngine::pendingKeys() const
{
    QStringList keys = pending_;
    for (const Token &t : input_)
    {
        keys << t.key;
    }
    QString out;
    for (const QString &key : keys)
    {
        out += key == QLatin1String("<lt>") ? QStringLiteral("<") : key;
    }
    return out;
}

QList<QTextEdit::ExtraSelection> VimEngine::extraSelections() const
{
    QList<QTextEdit::ExtraSelection> list;
    if (!enabled_)
    {
        return list;
    }
    auto add = [&](int start, int end, const QTextCharFormat &format) {
        QTextEdit::ExtraSelection s;
        s.cursor = QTextCursor(doc());
        s.cursor.setPosition(start);
        s.cursor.setPosition(end, QTextCursor::KeepAnchor);
        s.format = format;
        list << s;
    };

    // Search matches ('hlsearch'), and the match being typed ('incsearch').
    if (highlightOn_ && options_.hlSearch && !lastPattern_.isEmpty())
    {
        const QRegularExpression re = compilePattern(lastPattern_);
        if (re.isValid())
        {
            QTextCharFormat format;
            format.setBackground(QColor(0xff, 0xdc, 0x5a));
            format.setForeground(Qt::black);
            auto it = re.globalMatch(doc()->toPlainText());
            for (int n = 0; it.hasNext() && n < 5000; ++n)
            {
                const QRegularExpressionMatch m = it.next();
                if (m.capturedLength() > 0)
                {
                    add(static_cast<int>(m.capturedStart()),
                        static_cast<int>(m.capturedEnd()), format);
                }
            }
        }
    }
    if ((mode_ == Mode::CommandLine || confirming_) && incMatch_ >= 0 &&
        incMatchLen_ > 0)
    {
        QTextCharFormat format;
        format.setBackground(QColor(0xff, 0x96, 0x32));
        format.setForeground(Qt::black);
        add(incMatch_, incMatch_ + incMatchLen_, format);
    }

    // The visual selection.
    const Mode visual = mode_ == Mode::CommandLine ? cmdReturnMode_ : mode_;
    if (visual == Mode::Visual || visual == Mode::VisualLine ||
        visual == Mode::VisualBlock)
    {
        const QPalette palette = editor_->palette();
        QTextCharFormat format;
        format.setBackground(palette.color(QPalette::Highlight));
        format.setForeground(palette.color(QPalette::HighlightedText));
        const int a = visualAnchor_;
        const int b = pos();
        if (visual == Mode::VisualLine)
        {
            QTextCharFormat full = format;
            full.setProperty(QTextFormat::FullWidthSelection, true);
            for (int line = std::min(lineOf(a), lineOf(b));
                 line <= std::max(lineOf(a), lineOf(b)); ++line)
            {
                add(lineStart(line), lineStart(line), full);
            }
        }
        else if (visual == Mode::VisualBlock)
        {
            const int first = std::min(lineOf(a), lineOf(b));
            const int last = std::max(lineOf(a), lineOf(b));
            const int c1 = std::min(columnOf(a), columnOf(b));
            const int c2 = std::max(columnOf(a), columnOf(b));
            for (int line = first; line <= last; ++line)
            {
                const int len = static_cast<int>(lineText(line).size());
                const int e = visualToEnd_ ? len : std::min(c2 + 1, len);
                if (c1 < e)
                {
                    add(lineStart(line) + c1, lineStart(line) + e, format);
                }
            }
        }
        else
        {
            const int s = std::min(a, b);
            const int e = std::min(std::max(a, b) + 1, doc()->characterCount() - 1);
            add(s, std::max(s, e), format);
        }
    }
    return list;
}
