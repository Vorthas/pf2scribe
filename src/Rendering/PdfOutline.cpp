// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#include "Rendering/PdfOutline.h"

#include <QFile>
#include <QHash>
#include <QRegularExpression>

#include <algorithm>
#include <functional>
#include <optional>

namespace scribe
{
    namespace
    {
        // The bookmark tree: parent and children by index into the entries,
        // and where each one points once unknown places are filled in.
        struct Tree
        {
            std::vector<int> parent;
            std::vector<std::vector<int>> children;
            std::vector<int> roots;
            std::vector<OutlineEntry> resolved;
        };

        Tree buildTree(const std::vector<OutlineEntry> &entries)
        {
            Tree t;
            const int n = static_cast<int>(entries.size());
            t.parent.assign(static_cast<size_t>(n), -1);
            t.children.resize(static_cast<size_t>(n));
            std::vector<int> stack;
            for (int i = 0; i < n; ++i)
            {
                while (!stack.empty() &&
                       entries[static_cast<size_t>(stack.back())].level >=
                           entries[static_cast<size_t>(i)].level)
                {
                    stack.pop_back();
                }
                if (stack.empty())
                {
                    t.roots.push_back(i);
                }
                else
                {
                    t.parent[static_cast<size_t>(i)] = stack.back();
                    t.children[static_cast<size_t>(stack.back())].push_back(i);
                }
                stack.push_back(i);
            }
            // An entry without a place takes the next known one's (else the
            // last known before it, else the first page's top).
            t.resolved = entries;
            int next = -1;
            for (int i = n - 1; i >= 0; --i)
            {
                OutlineEntry &e = t.resolved[static_cast<size_t>(i)];
                if (e.page >= 0)
                    next = i;
                else if (next >= 0)
                {
                    e.page = entries[static_cast<size_t>(next)].page;
                    e.y = entries[static_cast<size_t>(next)].y;
                }
            }
            int previous = -1;
            for (int i = 0; i < n; ++i)
            {
                OutlineEntry &e = t.resolved[static_cast<size_t>(i)];
                if (e.page >= 0)
                    previous = i;
                else if (previous >= 0)
                {
                    e.page = t.resolved[static_cast<size_t>(previous)].page;
                    e.y = t.resolved[static_cast<size_t>(previous)].y;
                }
                else
                {
                    e.page = 0;
                    e.y = 0;
                }
            }
            return t;
        }

        // A PDF text string, in UTF-16BE so any title works.
        QByteArray pdfString(const QString &text)
        {
            QByteArray hex = "<FEFF";
            for (const QChar c : text)
            {
                hex += QByteArray::number(c.unicode() + 0x10000, 16).mid(1).toUpper();
            }
            return hex + '>';
        }

        QByteArray number(double value)
        {
            return QByteArray::number(value, 'f', 2);
        }

        // Where each object's dictionary starts (just after its "<<"); the
        // last definition, as later updates win.
        QHash<int, qsizetype> indexObjects(const QByteArray &pdf)
        {
            static const QRegularExpression start(
                QStringLiteral("(?:^|[\\r\\n])(\\d+) \\d+ obj\\s*<<"));
            QHash<int, qsizetype> index;
            auto it = start.globalMatch(QString::fromLatin1(pdf));
            while (it.hasNext())
            {
                const auto m = it.next();
                index.insert(m.captured(1).toInt(), m.capturedEnd());
            }
            return index;
        }

        // The text between object `object`'s outer "<<" and ">>".
        std::optional<QByteArray> dictionaryOf(const QByteArray &pdf,
                                               const QHash<int, qsizetype> &index,
                                               int object)
        {
            const auto found = index.constFind(object);
            if (found == index.cend())
            {
                return std::nullopt;
            }
            int depth = 1;
            const qsizetype begin = *found;
            for (qsizetype i = begin; i + 1 < pdf.size(); ++i)
            {
                if (pdf[i] == '<' && pdf[i + 1] == '<')
                {
                    ++depth;
                    ++i;
                }
                else if (pdf[i] == '>' && pdf[i + 1] == '>')
                {
                    if (--depth == 0)
                    {
                        return pdf.mid(begin, i - begin);
                    }
                    ++i;
                }
            }
            return std::nullopt;
        }

        std::optional<int> reference(const QByteArray &dictionary, const char *key)
        {
            const QRegularExpression re(
                QStringLiteral("/%1\\s+(\\d+)\\s+\\d+\\s+R").arg(QLatin1String(key)));
            const auto m = re.match(QString::fromLatin1(dictionary));
            if (!m.hasMatch())
            {
                return std::nullopt;
            }
            return m.captured(1).toInt();
        }

        // Height of a page's MediaBox, from the page or the page tree above.
        std::optional<double> mediaHeight(const QByteArray &dictionary)
        {
            static const QRegularExpression re(QStringLiteral(
                "/MediaBox\\s*\\[\\s*([-\\d.]+)\\s+([-\\d.]+)\\s+([-\\d.]+)\\s+([-\\d.]+)\\s*\\]"));
            const auto m = re.match(QString::fromLatin1(dictionary));
            if (!m.hasMatch())
            {
                return std::nullopt;
            }
            return m.captured(4).toDouble() - m.captured(2).toDouble();
        }
    } // namespace

    bool addPdfOutline(const QString &path,
                       const std::vector<OutlineEntry> &entries, QString *error)
    {
        auto fail = [error](const QString &why) {
            if (error)
                *error = why;
            return false;
        };
        if (entries.empty())
        {
            return true;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            return fail(file.errorString());
        }
        const QByteArray pdf = file.readAll();
        file.close();

        // The last trailer: /Size, /Root, /Info, /ID, and its xref offset.
        const qsizetype trailerAt = pdf.lastIndexOf("trailer");
        const qsizetype startxrefAt = pdf.lastIndexOf("startxref");
        if (trailerAt < 0 || startxrefAt < trailerAt)
        {
            return fail(QStringLiteral("no trailer"));
        }
        const QByteArray trailer = pdf.mid(trailerAt, startxrefAt - trailerAt);
        const QString trailerText = QString::fromLatin1(trailer);
        const auto size =
            QRegularExpression(QStringLiteral("/Size\\s+(\\d+)")).match(trailerText);
        const auto root = QRegularExpression(QStringLiteral("/Root\\s+(\\d+)\\s+(\\d+)\\s+R"))
                              .match(trailerText);
        const auto info = QRegularExpression(QStringLiteral("/Info\\s+(\\d+\\s+\\d+\\s+R)"))
                              .match(trailerText);
        const auto id = QRegularExpression(QStringLiteral("/ID\\s*\\[[^\\]]*\\]"))
                            .match(trailerText);
        const auto previous = QRegularExpression(QStringLiteral("startxref\\s+(\\d+)"))
                                  .match(QString::fromLatin1(pdf.mid(startxrefAt)));
        if (!size.hasMatch() || !root.hasMatch() || !previous.hasMatch())
        {
            return fail(QStringLiteral("unexpected trailer"));
        }
        const int rootObject = root.captured(1).toInt();
        const int rootGeneration = root.captured(2).toInt();
        const QHash<int, qsizetype> index = indexObjects(pdf);
        const auto catalog = dictionaryOf(pdf, index, rootObject);
        if (!catalog)
        {
            return fail(QStringLiteral("no catalog"));
        }

        // The pages in order, with their heights.
        struct PageRef
        {
            int object;
            double height;
        };
        std::vector<PageRef> pages;
        std::function<bool(int, double, int)> collect = [&](int object, double height,
                                                            int depth) {
            const auto dict = dictionaryOf(pdf, index, object);
            if (!dict || depth > 32)
            {
                return false;
            }
            height = mediaHeight(*dict).value_or(height);
            if (!dict->contains("/Kids"))
            {
                pages.push_back({object, height});
                return true;
            }
            const qsizetype open = dict->indexOf('[', dict->indexOf("/Kids"));
            const qsizetype close = dict->indexOf(']', open);
            static const QRegularExpression kid(QStringLiteral("(\\d+)\\s+\\d+\\s+R"));
            auto it = kid.globalMatch(QString::fromLatin1(dict->mid(open, close - open)));
            while (it.hasNext())
            {
                if (!collect(it.next().captured(1).toInt(), height, depth + 1))
                {
                    return false;
                }
            }
            return true;
        };
        const auto pagesRoot = reference(*catalog, "Pages");
        if (!pagesRoot || !collect(*pagesRoot, 842, 0) || pages.empty())
        {
            return fail(QStringLiteral("no pages"));
        }

        // The outline: the root object, then one per entry, numbered on from
        // /Size.
        const Tree tree = buildTree(entries);
        const int first = size.captured(1).toInt();
        const int n = static_cast<int>(entries.size());
        auto objectOf = [first](int entry) { return first + 1 + entry; };
        // Descendants shown when an entry is opened: its children (they all
        // start closed).
        std::vector<QByteArray> objects;
        {
            QByteArray outlines = "<< /Type /Outlines";
            outlines += " /First " + QByteArray::number(objectOf(tree.roots.front())) +
                        " 0 R /Last " + QByteArray::number(objectOf(tree.roots.back())) +
                        " 0 R /Count " + QByteArray::number(tree.roots.size()) + " >>";
            objects.push_back(outlines);
        }
        auto siblingsOf = [&tree](int entry) -> const std::vector<int> & {
            const int p = tree.parent[static_cast<size_t>(entry)];
            return p < 0 ? tree.roots : tree.children[static_cast<size_t>(p)];
        };
        for (int i = 0; i < n; ++i)
        {
            const OutlineEntry &e = tree.resolved[static_cast<size_t>(i)];
            const PageRef &page =
                pages[static_cast<size_t>(std::clamp(e.page, 0, static_cast<int>(pages.size()) - 1))];
            const int p = tree.parent[static_cast<size_t>(i)];
            QByteArray o = "<< /Title " + pdfString(e.title);
            o += " /Parent " + QByteArray::number(p < 0 ? first : objectOf(p)) + " 0 R";
            const std::vector<int> &siblings = siblingsOf(i);
            const auto at = std::find(siblings.begin(), siblings.end(), i) - siblings.begin();
            if (at > 0)
                o += " /Prev " + QByteArray::number(objectOf(siblings[static_cast<size_t>(at - 1)])) + " 0 R";
            if (at + 1 < static_cast<qsizetype>(siblings.size()))
                o += " /Next " + QByteArray::number(objectOf(siblings[static_cast<size_t>(at + 1)])) + " 0 R";
            const std::vector<int> &kids = tree.children[static_cast<size_t>(i)];
            if (!kids.empty())
            {
                // Negative: closed until clicked.
                o += " /First " + QByteArray::number(objectOf(kids.front())) + " 0 R";
                o += " /Last " + QByteArray::number(objectOf(kids.back())) + " 0 R";
                o += " /Count -" + QByteArray::number(kids.size());
            }
            o += " /Dest [" + QByteArray::number(page.object) + " 0 R /XYZ null " +
                 number(page.height - e.y) + " null] >>";
            objects.push_back(o);
        }

        // The catalog again, now pointing at the outline.
        QByteArray newCatalog = *catalog;
        {
            QString text = QString::fromLatin1(newCatalog);
            text.remove(QRegularExpression(QStringLiteral("/Outlines\\s+\\d+\\s+\\d+\\s+R")));
            text.remove(QRegularExpression(QStringLiteral("/PageMode\\s*/\\w+")));
            newCatalog = text.toLatin1();
        }
        newCatalog = "<<" + newCatalog + "/Outlines " + QByteArray::number(first) +
                     " 0 R\n/PageMode /UseOutlines\n>>";

        // Appended: the objects, an xref section for them, and a trailer
        // pointing back at the previous one.
        QByteArray update = pdf.endsWith('\n') ? QByteArray() : QByteArray("\n");
        const qsizetype base = pdf.size();
        std::vector<qsizetype> offsets;
        for (size_t i = 0; i < objects.size(); ++i)
        {
            offsets.push_back(base + update.size());
            update += QByteArray::number(first + static_cast<int>(i)) + " 0 obj\n" +
                      objects[i] + "\nendobj\n";
        }
        const qsizetype catalogOffset = base + update.size();
        update += QByteArray::number(rootObject) + ' ' + QByteArray::number(rootGeneration) +
                  " obj\n" + newCatalog + "\nendobj\n";
        const qsizetype xrefOffset = base + update.size();
        auto entry = [](qsizetype offset, int generation) {
            return QByteArray::number(offset).rightJustified(10, '0') + ' ' +
                   QByteArray::number(generation).rightJustified(5, '0') + " n\r\n";
        };
        update += "xref\n";
        update += QByteArray::number(rootObject) + " 1\n" + entry(catalogOffset, rootGeneration);
        update += QByteArray::number(first) + ' ' + QByteArray::number(objects.size()) + '\n';
        for (const qsizetype offset : offsets)
        {
            update += entry(offset, 0);
        }
        update += "trailer\n<<\n/Size " + QByteArray::number(first + static_cast<int>(objects.size())) +
                  "\n/Root " + QByteArray::number(rootObject) + ' ' +
                  QByteArray::number(rootGeneration) + " R\n";
        if (info.hasMatch())
            update += "/Info " + info.captured(1).toLatin1() + '\n';
        if (id.hasMatch())
            update += id.captured(0).toLatin1() + '\n';
        update += "/Prev " + previous.captured(1).toLatin1() + "\n>>\nstartxref\n" +
                  QByteArray::number(xrefOffset) + "\n%%EOF\n";

        if (!file.open(QIODevice::Append))
        {
            return fail(file.errorString());
        }
        if (file.write(update) != update.size())
        {
            return fail(file.errorString());
        }
        return true;
    }

    QString pdfmarkOutline(const std::vector<OutlineEntry> &entries,
                           double pageHeight)
    {
        const Tree tree = buildTree(entries);
        QString out;
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const OutlineEntry &e = tree.resolved[i];
            QString line = QStringLiteral("[");
            if (!tree.children[i].empty())
            {
                // Negative: closed until clicked.
                line += QStringLiteral("/Count -%1 ").arg(tree.children[i].size());
            }
            line += QStringLiteral("/Page %1 /View [/XYZ null %2 null] /Title %3 /OUT pdfmark\n")
                        .arg(e.page + 1)
                        .arg(QString::fromLatin1(number(pageHeight - e.y)))
                        .arg(QString::fromLatin1(pdfString(e.title)));
            out += line;
        }
        return out;
    }
} // namespace scribe
