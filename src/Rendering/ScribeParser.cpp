// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#include "Rendering/ScribeParser.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace scribe
{
    namespace
    {

        // Trailing "((Title))", "((+Info))", "((++Sub))" on a heading line.
        const QRegularExpression
            kTocMarker(R"(\s*\(\(\s*(\+*)\s*([^)]*?)\s*\)\)\s*$)");

        // "head (", "head(", "sticky(50 218" ...
        const QRegularExpression
            kBlockOpen(R"(^\s*([A-Za-z][A-Za-z0-9_-]*)\s*\((.*)$)");

        // "specialfeat {" opens a named content reference; a line with only "}"
        // closes it.
        const QRegularExpression
            kDefOpen(R"(^\s*([A-Za-z][A-Za-z0-9_-]*)\s*\{\s*$)");

        // "{{specialfeat}}" on a line by itself.
        const QRegularExpression kRefLine(R"(^\s*\{\{\s*([^{}]+?)\s*\}\}\s*$)");

        const QSet<QString> &blockNames()
        {
            static const QSet<QString> names{
                "watermark", "title", "head", "css",  "fonts", "sticky", "info",
                "rules",     "note",  "math", "left", "right", "item"};
            return names;
        }

        // A marked heading's source line, tagged on before anything else
        // reads the source (comments, "%", "{{name}}" all move lines about).
        constexpr char16_t kLineOpen = 0xE010;
        constexpr char16_t kLineClose = 0xE011;
        const QRegularExpression kLineTag(QStringLiteral("\uE010(\\d+)\uE011"));
        const QRegularExpression kAnyTag(
            QStringLiteral("[\uE010\uE012]\\d+[\uE011\uE013]"));

        QString stripTocMarker(const QString &line, QList<TocEntry> &toc)
        {
            if (!line.startsWith(QLatin1Char('#')))
            {
                return line;
            }
            const auto m = kTocMarker.match(line);
            if (!m.hasMatch())
            {
                return line;
            }
            QString heading = line.left(m.capturedStart());
            int source = -1;
            if (const auto tag = kLineTag.match(heading); tag.hasMatch())
            {
                source = tag.captured(1).toInt();
                heading.remove(tag.capturedStart(), tag.capturedLength());
            }
            toc.append({static_cast<int>(m.captured(1).size()), m.captured(2),
                        source});
            return heading + QChar(kTocAnchorOpen) +
                   QString::number(toc.size() - 1) + QChar(kTocAnchorClose);
        }

        // True if `line` is (part of) a plain paragraph in the markdown being
        // built.
        bool isParagraphText(const QString &line)
        {
            static const QRegularExpression kNotParagraph(
                R"(^\s*(#|[*+-]\s|\d+[.)]\s|>|\||\*\*\*$|```|~~~|([-=_*]\s*){3,}$))");
            // Table rows needn't start with "|", but they all hold one.
            return !line.trimmed().isEmpty() &&
                   !line.contains(QLatin1Char('|')) &&
                   !kNotParagraph.match(line).hasMatch();
        }

        bool isListItem(const QString &line)
        {
            static const QRegularExpression kItem(R"(^\s*([*+-]|\d+[.)])\s+\S)");
            return kItem.match(line).hasMatch();
        }

        // Joins `line` to the paragraph text before it, after a line break. A
        // line separator keeps both in one paragraph: Qt would turn a markdown
        // hard break into a new paragraph, with a paragraph's spacing.
        void appendLineBreak(QString &previous, const QString &line)
        {
            while (previous.endsWith(QLatin1Char(' ')))
            {
                previous.chop(1);
            }
            previous += QChar(QChar::LineSeparator) + line;
        }

        // Applies the per-line syntax shared by body text and blocks: rules,
        // item trait lines, indented lines and TOC markers. Action tokens (:a:
        // ...) are left for Theme to turn into icon glyphs. Lines in a fenced
        // code block (`verbatim`) pass through untouched.
        void emitLine(const QString &raw, QStringList &out, bool inItem,
                      QList<TocEntry> &toc, bool verbatim = false)
        {
            if (verbatim)
            {
                out << raw;
                return;
            }
            if (raw.trimmed() == QLatin1String("-"))
            {
                // A lone "-" is a horizontal rule. Blank lines keep markdown
                // from reading it as a setext heading underline for the
                // paragraph above.
                out << QString() << QStringLiteral("***") << QString();
                return;
            }
            if (inItem && raw.startsWith(QLatin1Char(';')))
            {
                // "; uncommon,class,feat" is a trait list. It becomes a
                // paragraph that Theme turns into trait badges.
                QStringList tags;
                for (const QString &tag :
                     raw.mid(1).split(QLatin1Char(','), Qt::SkipEmptyParts))
                {
                    tags << tag.trimmed();
                }
                out << QString()
                    << QChar(kTraitsMarker) + tags.join(QLatin1Char(','))
                    << QString();
                return;
            }
            if (raw.startsWith(QLatin1Char(' ')) && !raw.trimmed().isEmpty() &&
                !out.isEmpty() && isParagraphText(out.last()))
            {
                // A line starting with spaces under paragraph text stays in the
                // paragraph: Scribe breaks the line and keeps the spaces
                // ("<br>&nbsp;&nbsp;...").
                qsizetype spaces = 0;
                while (spaces < raw.size() &&
                       raw.at(spaces) == QLatin1Char(' '))
                {
                    ++spaces;
                }
                appendLineBreak(out.last(),
                                QString(spaces, QChar(0x00A0)) + raw.mid(spaces));
                return;
            }
            if (!out.isEmpty() && isParagraphText(raw) &&
                !raw.startsWith(QLatin1Char('<')) &&
                (isParagraphText(out.last()) || isListItem(out.last())))
            {
                // Scribe renders markdown with "breaks" on: a single newline
                // inside a paragraph (or list item) is a line break.
                appendLineBreak(out.last(), raw);
                return;
            }
            out << stripTocMarker(raw, toc);
        }

        // First pass over the raw text:
        //  * "%" on a line by itself hides everything after it
        //  * <!-- ... --> comments are dropped
        //  * "key { ... }" blocks are stored as content references (and shown
        //  in place unless they
        //    sit in a comment or the hidden area)
        QStringList preprocess(const QString &source,
                               QHash<QString, QStringList> &defs)
        {
            QStringList out;
            bool hidden = false, inComment = false, inFence = false,
                 inCss = false;
            bool inDef = false, defVisible = false;
            QString defName;
            QStringList defLines;

            for (QString line : source.split(QLatin1Char('\n')))
            {
                if (line.endsWith(QLatin1Char('\r')))
                {
                    line.chop(1);
                }
                const QString t = line.trimmed();

                if (inDef)
                {
                    if (t == QLatin1String("}"))
                    {
                        defs.insert(defName, defLines);
                        if (defVisible)
                        {
                            out += defLines;
                        }
                        inDef = false;
                    }
                    else
                    {
                        defLines << line;
                    }
                    continue;
                }

                if (t == QLatin1String("<!--"))
                {
                    inComment = true;
                    continue;
                }
                if (t == QLatin1String("-->"))
                {
                    inComment = false;
                    continue;
                }
                if (t.startsWith(QLatin1String("<!--")) &&
                    t.endsWith(QLatin1String("-->")))
                {
                    continue;
                }

                if (!hidden && !inComment && t == QLatin1String("%"))
                {
                    hidden = true;
                    continue;
                }
                const bool visible = !hidden && !inComment;

                // CSS blocks contain "selector {" lines that must not be read
                // as references.
                if (inCss)
                {
                    if (t == QLatin1String(")"))
                    {
                        inCss = false;
                    }
                    if (visible)
                    {
                        out << line;
                    }
                    continue;
                }
                if (visible &&
                    (t == QLatin1String("css (") || t == QLatin1String("css(")))
                {
                    inCss = true;
                    out << line;
                    continue;
                }

                if (visible && (t.startsWith(QLatin1String("```")) ||
                                t.startsWith(QLatin1String("~~~"))))
                {
                    inFence = !inFence;
                }

                if (!inFence)
                {
                    const auto m = kDefOpen.match(line);
                    if (m.hasMatch())
                    {
                        inDef = true;
                        defName = m.captured(1);
                        defLines.clear();
                        defVisible = visible;
                        continue;
                    }
                }
                if (visible)
                {
                    out << line;
                }
            }
            return out;
        }

        // Replaces "{{key}}" lines with the stored content (recursively, to a
        // small depth).
        QStringList expandRefs(const QStringList &lines,
                               const QHash<QString, QStringList> &defs,
                               int depth, Document &doc)
        {
            QStringList out;
            for (const QString &line : lines)
            {
                const auto m = kRefLine.match(line);
                if (!m.hasMatch())
                {
                    out << line;
                    continue;
                }
                const QString key = m.captured(1);
                if (!defs.contains(key))
                {
                    doc.warnings
                        << QStringLiteral("Unknown content reference '%1'")
                               .arg(key);
                    out << QStringLiteral("*[missing reference: %1]*").arg(key);
                }
                else if (depth >= 4)
                {
                    doc.warnings
                        << QStringLiteral(
                               "Content reference '%1' nests too deeply")
                               .arg(key);
                }
                else
                {
                    out += expandRefs(defs.value(key), defs, depth + 1, doc);
                }
            }
            return out;
        }

        Section newSection()
        {
            Section s;
            s.columns.append(Column{});
            return s;
        }

    } // namespace

    // Syntax, from the site's own example document:
    //  =            starts a new page
    //  /            ends the current band of columns; what follows starts a new
    //  band |            starts another column in the current band (columns =
    //  pipes + 1, equal width)
    //  -            horizontal rule
    //  head ( )     full-width header block
    //  left/right ( )  sidebar (1/3 width) next to the text that follows, until
    //  "/" info/rules/note/math/item ( )  boxes; "|" inside a box splits it
    //  into columns watermark/title ( )  text repeated on every page css/fonts
    //  ( ), sticky(x y ...)  parsed but not rendered yet key { } / {{key}}
    //  content references;  %  hides everything after it pagenumbers / reset
    //  directives on a line by themselves
    QString withoutTocTags(QString text)
    {
        return text.remove(kAnyTag);
    }

    Document parse(const QString &source)
    {
        Document doc;

        // Tag each marked heading with its line (see kLineOpen).
        QStringList tagged = source.split(QLatin1Char('\n'));
        for (qsizetype i = 0; i < tagged.size(); ++i)
        {
            QString &line = tagged[i];
            if (!line.startsWith(QLatin1Char('#')))
            {
                continue;
            }
            if (const auto m = kTocMarker.match(line); m.hasMatch())
            {
                line.insert(m.capturedStart(), QChar(kLineOpen) +
                                                   QString::number(i) +
                                                   QChar(kLineClose));
            }
        }

        QHash<QString, QStringList> defs;
        QStringList lines = preprocess(tagged.join(QLatin1Char('\n')), defs);
        lines = expandRefs(lines, defs, 0, doc);

        Section cur = newSection();
        QStringList bodyLines;
        bool pendingPage = false;
        bool inFence = false;

        auto warnOnce = [&](const QString &msg) {
            if (!doc.warnings.contains(msg))
            {
                doc.warnings << msg;
            }
        };

        auto hasContent = [](const Section &s) {
            for (const Column &c : s.columns)
            {
                if (!c.blocks.isEmpty())
                {
                    return true;
                }
            }
            return false;
        };

        auto pushSection = [&](Section s) {
            s.pageBreakBefore = pendingPage;
            pendingPage = false;
            doc.sections.append(std::move(s));
        };

        auto flushBody = [&] {
            const QString text = bodyLines.join(QLatin1Char('\n')).trimmed();
            bodyLines.clear();
            if (!text.isEmpty())
            {
                cur.columns.last().blocks.append(
                    Block{BlockKind::Markdown, QStringList{text}});
            }
        };

        auto closeSection = [&] {
            flushBody();
            if (hasContent(cur))
            {
                pushSection(std::move(cur));
            }
            cur = newSection();
        };

        auto handleBlock = [&](const QString &name, const QString &args,
                               const QStringList &content) {
            const bool isItem = (name == QLatin1String("item"));
            QStringList processed;
            bool fenced = false;
            for (const QString &l : content)
            {
                const bool fence = l.trimmed().startsWith(QLatin1String("```")) ||
                                   l.trimmed().startsWith(QLatin1String("~~~"));
                emitLine(l, processed, isItem, doc.toc, fenced && !fence);
                fenced ^= fence;
            }

            if (name == QLatin1String("watermark"))
            {
                doc.watermark = withoutTocTags(content.join(QLatin1Char(' ')))
                                    .simplified();
            }
            else if (name == QLatin1String("title"))
            {
                doc.title =
                    withoutTocTags(content.join(QLatin1Char(' '))).simplified();
            }
            else if (name == QLatin1String("css"))
            {
                doc.css += withoutTocTags(content.join(QLatin1Char('\n'))) +
                           QLatin1Char('\n');
            }
            else if (name == QLatin1String("fonts"))
            {
                for (const QString &l : content)
                {
                    if (!l.trimmed().isEmpty())
                    {
                        doc.fonts << l.trimmed();
                    }
                }
            }
            else if (name == QLatin1String("sticky"))
            {
                // "sticky(50 218": left and top in mm. Each line of the note is
                // a line of its own ("<br>" on the site), so they're joined
                // with line separators into one paragraph.
                const QStringList xy =
                    args.split(QRegularExpression(QStringLiteral("[\\s,]+")),
                               Qt::SkipEmptyParts);
                bool okX = false, okY = false;
                const double x = xy.value(0).toDouble(&okX);
                const double y = xy.value(1).toDouble(&okY);
                if (!okX || !okY)
                {
                    warnOnce(QStringLiteral(
                        "Sticky note without an X and Y position"));
                }
                QStringList text;
                for (const QString &l : content)
                {
                    if (!l.trimmed().isEmpty())
                    {
                        text << l.trimmed();
                    }
                }
                Block sticky{
                    BlockKind::Sticky,
                    QStringList{text.join(QChar(QChar::LineSeparator))}};
                sticky.position = QPointF(okX ? x : 0, okY ? y : 0);
                flushBody();
                cur.columns.last().blocks.append(sticky);
            }
            else if (name == QLatin1String("head"))
            {
                closeSection();
                Section h;
                h.kind = SectionKind::Head;
                Column c;
                c.blocks.append(Block{
                    BlockKind::Markdown,
                    QStringList{processed.join(QLatin1Char('\n')).trimmed()}});
                h.columns.append(c);
                pushSection(std::move(h));
            }
            else if (name == QLatin1String("left") ||
                     name == QLatin1String("right"))
            {
                closeSection();
                cur.kind = (name == QLatin1String("left")) ? SectionKind::Left
                                                           : SectionKind::Right;
                cur.columns.clear();
                const BlockKind sideKind = (name == QLatin1String("left"))
                                               ? BlockKind::SidebarLeft
                                               : BlockKind::SidebarRight;
                Column side;
                side.blocks.append(Block{
                    sideKind,
                    QStringList{processed.join(QLatin1Char('\n')).trimmed()}});
                cur.columns.append(side);
                cur.columns.append(
                    Column{}); // main text follows here until "/"
            }
            else
            {
                static const QHash<QString, BlockKind> kinds{
                    {QStringLiteral("info"), BlockKind::Info},
                    {QStringLiteral("rules"), BlockKind::Rules},
                    {QStringLiteral("note"), BlockKind::Note},
                    {QStringLiteral("math"), BlockKind::Math},
                    {QStringLiteral("item"), BlockKind::Item},
                };
                flushBody();
                // "|" inside a box splits it into columns.
                QStringList columns, current;
                for (const QString &l : processed)
                {
                    if (l.trimmed() == QLatin1String("|"))
                    {
                        columns << current.join(QLatin1Char('\n')).trimmed();
                        current.clear();
                    }
                    else
                    {
                        current << l;
                    }
                }
                columns << current.join(QLatin1Char('\n')).trimmed();
                cur.columns.last().blocks.append(
                    Block{kinds.value(name), columns});
            }
        };

        for (int i = 0; i < lines.size(); ++i)
        {
            const QString &line = lines[i];
            const QString t = line.trimmed();

            if (t.startsWith(QLatin1String("```")) ||
                t.startsWith(QLatin1String("~~~")))
            {
                inFence = !inFence;
            }

            if (!inFence)
            {
                const auto m = kBlockOpen.match(line);
                if (m.hasMatch())
                {
                    const QString name = m.captured(1).toLower();
                    const QString rest = m.captured(2).trimmed();
                    if (blockNames().contains(name) &&
                        (rest.isEmpty() || name == QLatin1String("sticky")))
                    {
                        QStringList content;
                        int j = i + 1;
                        for (; j < lines.size(); ++j)
                        {
                            if (lines[j].trimmed() == QLatin1String(")"))
                            {
                                break;
                            }
                            content << lines[j];
                        }
                        if (j >= lines.size())
                        {
                            doc.warnings
                                << QStringLiteral("Unterminated block '%1'")
                                       .arg(name);
                        }
                        i = j;
                        handleBlock(name, rest, content);
                        continue;
                    }
                }

                if (t == QLatin1String("="))
                {
                    closeSection();
                    pendingPage = true;
                    continue;
                }
                if (t == QLatin1String("/"))
                {
                    closeSection();
                    continue;
                }
                if (t == QLatin1String("|"))
                {
                    flushBody();
                    cur.columns.append(Column{});
                    continue;
                }
                if (t == QLatin1String("pagenumbers"))
                {
                    doc.pageNumbers = true;
                    continue;
                }
                if (t == QLatin1String("reset"))
                {
                    doc.reset = true;
                    continue;
                }
            }

            emitLine(line, bodyLines, false, doc.toc, inFence);
        }
        closeSection();
        return doc;
    }

} // namespace scribe
