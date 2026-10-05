// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#include "Rendering/PageLayout.h"

#include "Rendering/ImageCache.h"

#include "Rendering/TraitBadge.h"

#include <QAbstractTextDocumentLayout>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QPalette>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextLayout>
#include <QUrl>

#include <algorithm>
#include <climits>
#include <cmath>

namespace scribe
{
    namespace
    {
        // Between bands: the ".content.w-100" spacer's "* + .content" margin.
        constexpr qreal kRowGap = 0.5 * kRem;
        // Between blocks of a column ("* + .content", "* + .info", ...).
        constexpr qreal kBlockGap = 0.5 * kRem;
        constexpr qreal kInnerGap = 1.2 * kRem; // between columns in a box
        constexpr qreal kSidebarGap = 0.5 * kRem;
        constexpr qreal kSidebarFraction = 0.33;
        // Around a sidebar's images, which text follows the outline of: the
        // gap kept, and the height of the slices the outline is cut into.
        constexpr qreal kImageGap = 0.6 * kRem;
        constexpr qreal kImageGapY = 0.25 * kRem;
        constexpr qreal kSlice = 1;
        constexpr int kOpaque = 40; // alpha from which a pixel counts
        // Sidebar text lines closer than this count as one run (for its
        // rule, and for the room it takes).
        constexpr qreal kRunGap = 2 * kRem;
        // A box narrower than this beside a sidebar goes below it instead.
        constexpr qreal kMinBoxWidth = 12 * kRem;
        constexpr qreal kEpsilon = 0.5;
        // Block property: the table-of-contents entries (QVariantList of
        // indexes into Document::toc) whose heading the block is.
        constexpr int kTocEntries = QTextFormat::UserProperty + 40;

        // Takes the parser's table-of-contents anchors (kTocAnchorOpen) out
        // of `markdown`, noting for each entry which heading it is: the
        // index among the non-empty headings (Qt drops an empty heading at
        // the start of a text, so those can't be counted), or -1 for the
        // text's end. An entry on an empty heading ("# ((A))") goes to the
        // next heading.
        QString takeTocAnchors(const QString &markdown,
                               std::vector<std::pair<int, int>> &anchors)
        {
            static const QRegularExpression kAnchor(QStringLiteral("\uE012(\\d+)\uE013"));
            static const QRegularExpression kHeading(
                QStringLiteral(R"(^ {0,3}#{1,6}(?:\s+(.*))?$)"));
            if (!markdown.contains(QChar(kTocAnchorOpen)))
            {
                return withoutTocTags(markdown);
            }
            QStringList lines = markdown.split(QLatin1Char('\n'));
            bool fence = false;
            int heading = 0;
            std::vector<int> waiting; // on empty headings
            for (QString &line : lines)
            {
                std::vector<int> entries;
                auto it = kAnchor.globalMatch(line);
                while (it.hasNext())
                {
                    entries.push_back(it.next().captured(1).toInt());
                }
                line = withoutTocTags(line);
                const QString t = line.trimmed();
                if (t.startsWith(QLatin1String("```")) ||
                    t.startsWith(QLatin1String("~~~")))
                {
                    fence = !fence;
                }
                const auto m = fence ? QRegularExpressionMatch()
                                     : kHeading.match(line);
                if (!m.hasMatch())
                {
                    waiting.insert(waiting.end(), entries.begin(), entries.end());
                    continue;
                }
                QString text = m.captured(1).trimmed();
                while (text.endsWith(QLatin1Char('#'))) // a closing "###"
                {
                    text.chop(1);
                }
                waiting.insert(waiting.end(), entries.begin(), entries.end());
                if (!text.trimmed().isEmpty())
                {
                    for (const int entry : waiting)
                    {
                        anchors.emplace_back(entry, heading);
                    }
                    waiting.clear();
                    ++heading;
                }
            }
            for (const int entry : waiting)
            {
                anchors.emplace_back(entry, -1);
            }
            return lines.join(QLatin1Char('\n'));
        }
        constexpr qreal kBleed = 6; // see Placement::bleedTop

        // Text is laid out at 720 dpi, in tenths of a point: at 72 dpi Qt
        // rounds font sizes to whole pixels, i.e. whole points, which made
        // text up to 4% too wide or narrow. Page geometry stays in points.
        constexpr qreal kUnits = 10; // layout units per point

        // A stand-in for an image that couldn't be loaded, `size` in points,
        // drawn at 4 pixels per point.
        QImage missingImage(const QString &name, const QSizeF &size)
        {
            constexpr qreal kScale = 4;
            QImage image((size * kScale).toSize(),
                         QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter p(&image);
            p.scale(kScale, kScale);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(QColor(0, 0, 0, 110), 0.75, Qt::DashLine));
            p.drawRect(
                QRectF(QPointF(0, 0), size).adjusted(0.5, 0.5, -0.5, -0.5));
            QFont font;
            font.setPixelSize(7);
            p.setFont(font);
            p.setPen(QColor(0, 0, 0, 150));
            p.drawText(QRectF(QPointF(0, 0), size).adjusted(4, 0, -4, 0),
                       Qt::AlignCenter | Qt::TextWrapAnywhere,
                       QStringLiteral("Missing image: %1").arg(name));
            return image;
        }

        // Height of a document whose lines PageLayout::wrap() moved, in
        // layout units: Qt's own figure is out of date then.
        constexpr char kHeightProperty[] = "scribeHeight";

        qreal docHeight(QTextDocument &doc)
        {
            const QVariant height = doc.property(kHeightProperty);
            return (height.isValid()
                        ? height.toReal()
                        : doc.documentLayout()->documentSize().height()) /
                   kUnits;
        }

        void paintDeco(QPainter &p, const BoxDeco &d)
        {
            p.save();
            if (d.hole.isValid())
            {
                QPainterPath keep;
                keep.addRect(d.rect.adjusted(-1, -1, 1, 1));
                QPainterPath hole;
                hole.addRoundedRect(d.hole, d.holeRadius, d.holeRadius);
                p.setClipPath(keep.subtracted(hole), Qt::IntersectClip);
            }
            if (d.fill.isValid())
            {
                p.setPen(Qt::NoPen);
                p.setBrush(d.fill);
                p.drawRoundedRect(d.rect, d.radius, d.radius);
                if (d.squareBottom && d.radius > 0)
                {
                    p.drawRect(d.rect.adjusted(
                        0, std::min(d.radius, d.rect.height() / 2), 0, 0));
                }
                if (d.squareTop && d.radius > 0)
                {
                    p.drawRect(d.rect.adjusted(
                        0, 0, 0, -std::min(d.radius, d.rect.height() / 2)));
                }
            }
            if (d.edgeWidth > 0)
            {
                p.setPen(
                    QPen(d.edgeColor, d.edgeWidth, Qt::SolidLine, Qt::FlatCap));
                // Strokes are centred on their line; keep them inside the
                // rectangle, like CSS borders.
                const qreal h = d.edgeWidth / 2;
                const QRectF r = d.rect.adjusted(h, h, -h, -h);
                const qreal l = d.rect.left(), rt = d.rect.right();
                const qreal t = d.rect.top(), b = d.rect.bottom();
                if (d.edges.testFlag(Qt::TopEdge))
                {
                    p.drawLine(QPointF(l, r.top()), QPointF(rt, r.top()));
                }
                if (d.edges.testFlag(Qt::BottomEdge))
                {
                    p.drawLine(QPointF(l, r.bottom()), QPointF(rt, r.bottom()));
                }
                if (d.edges.testFlag(Qt::LeftEdge))
                {
                    p.drawLine(QPointF(r.left(), t), QPointF(r.left(), b));
                }
                if (d.edges.testFlag(Qt::RightEdge))
                {
                    p.drawLine(QPointF(r.right(), t), QPointF(r.right(), b));
                }
            }
            p.restore();
        }

        // Highlight colour of selected text in the preview.
        const QColor kSelection(51, 153, 255, 90);

        // Document positions [from, to) to mark, with their colours.
        struct Mark
        {
            int from = 0;
            int to = 0;
            QColor background;
            QColor text;
        };

        void paintPlacement(QPainter &p, const Placement &pl,
                            const std::vector<Mark> &marks = {})
        {
            p.save();
            // A little room on the sides too, for italic overhang.
            const QMarginsF bleed(kBleed, pl.bleedTop, kBleed, pl.bleedBottom);
            p.setClipRect(QRectF(pl.dest, pl.source.size()) + bleed);
            p.translate(pl.dest - pl.source.topLeft());
            p.scale(1 / kUnits, 1 / kUnits);

            QAbstractTextDocumentLayout::PaintContext ctx;
            // Colours for text and rules, so a dark desktop theme doesn't leak
            // into the page.
            ctx.palette.setColor(QPalette::Text, pl.textColor);
            // Qt's own rules are hidden (addRule() draws them). They use
            // Dark in older Qt, WindowText in newer.
            ctx.palette.setColor(QPalette::Dark, Qt::transparent);
            ctx.palette.setColor(QPalette::WindowText, Qt::transparent);
            const QRectF clip = pl.source + bleed;
            ctx.clip = QRectF(clip.topLeft() * kUnits, clip.size() * kUnits);
            for (const Mark &mark : marks)
            {
                if (mark.from >= mark.to)
                {
                    continue;
                }
                QAbstractTextDocumentLayout::Selection selection;
                selection.cursor = QTextCursor(pl.doc);
                selection.cursor.setPosition(mark.from);
                selection.cursor.setPosition(mark.to, QTextCursor::KeepAnchor);
                selection.format.setBackground(mark.background);
                if (mark.text.isValid())
                {
                    selection.format.setForeground(mark.text);
                }
                ctx.selections.append(selection);
            }
            pl.doc->documentLayout()->draw(&p, ctx);
            p.restore();
        }

        // Draws page furniture (watermark, title, page number) without the
        // whole-point font rounding described at kUnits.
        void drawFineText(QPainter &p, QFont font, const QRectF &rect,
                          int flags, const QString &text)
        {
            p.save();
            p.scale(1 / kUnits, 1 / kUnits);
            font.setPointSizeF(font.pointSizeF() * kUnits);
            p.setFont(font);
            p.drawText(QRectF(rect.topLeft() * kUnits, rect.size() * kUnits),
                       flags, text);
            p.restore();
        }

        Role roleFor(BlockKind kind)
        {
            switch (kind)
            {
            case BlockKind::Info:
                return Role::Info;
            case BlockKind::Rules:
                return Role::Rules;
            case BlockKind::Note:
                return Role::Note;
            case BlockKind::Math:
                return Role::Math;
            case BlockKind::Item:
                return Role::Item;
            case BlockKind::SidebarLeft:
                return Role::SidebarLeft;
            case BlockKind::SidebarRight:
                return Role::SidebarRight;
            case BlockKind::Sticky:
                return Role::Sticky;
            case BlockKind::Markdown:
                break;
            }
            return Role::Body;
        }

        // Candidate places to cut a tall column: the bottom of every laid-out
        // line and of every block (block bottoms include the paragraph's
        // bottom margin, so they are preferred when they fit).
        //
        // TODO: lines inside table cells are treated like any other line, so a
        // cut can land in the middle of a table row. Rows need to be atomic.
        // TODO: keep headings together with the following block.
        // Tops of the horizontal rules in `doc`, in points. PageLayout draws
        // them: Qt's own are a single device pixel thin.
        std::vector<qreal> ruleTops(QTextDocument &doc)
        {
            std::vector<qreal> tops;
            for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
            {
                if (b.blockFormat().hasProperty(
                        QTextFormat::BlockTrailingHorizontalRulerWidth))
                {
                    tops.push_back(
                        doc.documentLayout()->blockBoundingRect(b).top() /
                        kUnits);
                }
            }
            return tops;
        }

        std::vector<qreal> cutPoints(QTextDocument &doc)
        {
            std::vector<qreal> cuts;
            QAbstractTextDocumentLayout *layout = doc.documentLayout();
            for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
            {
                if (const QTextLayout *tl = b.layout())
                {
                    const QPointF pos = tl->position();
                    for (int i = 0; i < tl->lineCount(); ++i)
                    {
                        const QTextLine line = tl->lineAt(i);
                        cuts.push_back((pos.y() + line.y() + line.height()) /
                                       kUnits);
                    }
                }
                cuts.push_back(layout->blockBoundingRect(b).bottom() / kUnits);
            }
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end(),
                                   [](qreal a, qreal b) {
                                       return std::abs(a - b) < 0.01;
                                   }),
                       cuts.end());
            return cuts;
        }
    } // namespace

    FairMutex &textEngineMutex()
    {
        static FairMutex mutex;
        return mutex;
    }

    PageLayout::~PageLayout()
    {
        {
            const std::lock_guard<FairMutex> lock(textEngineMutex());
            docs_.clear();
        }
        // Its images may be unused now (see ImageCache::trim()).
        ImageCache::instance().trim();
    }

    PageLayout::PageLayout(const PageSpec &spec, const Theme &theme)
        : spec_(spec), theme_(theme), refDevice_(1, 1, QImage::Format_ARGB32)
    {
        const int dotsPerMeter = qRound(72 * kUnits / 0.0254);
        refDevice_.setDotsPerMeterX(dotsPerMeter);
        refDevice_.setDotsPerMeterY(dotsPerMeter);
    }

    std::shared_ptr<PageLayout> PageLayout::build(const Document &doc,
                                                  const PageSpec &spec,
                                                  const Theme &theme,
                                                  const QString &baseDir,
                                                  const std::atomic<bool> *cancel,
                                                  const Progress &progress)
    {
        std::shared_ptr<PageLayout> layout(new PageLayout(spec, theme));
        layout->baseDir_ = baseDir.isEmpty() ? QDir::currentPath() : baseDir;
        layout->cancel_ = cancel;
        layout->progress_ = progress ? &progress : nullptr;
        layout->prefetchImages(doc);
        layout->run(doc);
        layout->cancel_ = nullptr;
        layout->progress_ = nullptr;
        ImageCache::instance().trim();
        if (!(cancel && cancel->load()))
        {
            layout->resolveToc(static_cast<int>(doc.toc.size()));
        }
        if (cancel && cancel->load())
        {
            return nullptr;
        }
        return layout;
    }

    void PageLayout::moveToThread(QThread *thread)
    {
        for (const auto &doc : docs_)
        {
            doc->moveToThread(thread);
        }
    }

    QTextDocument *PageLayout::makeDoc(const QString &markdown, qreal width,
                                       Role role)
    {
        auto doc = std::make_unique<QTextDocument>();
        doc->documentLayout()->setPaintDevice(&refDevice_);
        // No page size yet: Qt doesn't lay the document out at all, where it
        // would otherwise do so again after every change made while building
        // and styling it. setTextWidth() below lays it out once.
        doc->setPageSize(QSizeF(0, 0));
        doc->setDocumentMargin(0);
        doc->setUseDesignMetrics(true);
        TraitBadge::install(*doc);
        std::vector<std::pair<int, int>> anchors;
        doc->setMarkdown(takeTocAnchors(markdown, anchors));
        if (!anchors.empty())
        {
            // Mark the headings (Theme::apply() keeps block properties).
            std::vector<QTextBlock> headings;
            for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
            {
                if (b.blockFormat().headingLevel() > 0 &&
                    !b.text().trimmed().isEmpty())
                {
                    headings.push_back(b);
                }
            }
            for (const auto &[entry, index] : anchors)
            {
                const QTextBlock b =
                    index >= 0 && index < static_cast<int>(headings.size())
                        ? headings[static_cast<size_t>(index)]
                        : doc->lastBlock();
                QVariantList entries = b.blockFormat().property(kTocEntries).toList();
                entries.append(entry);
                QTextBlockFormat mark;
                mark.setProperty(kTocEntries, entries);
                QTextCursor(b).mergeBlockFormat(mark);
            }
        }
        loadImages(*doc, width, role);
        theme_.apply(*doc, role);
        doc->setTextWidth(width * kUnits);
        Theme::finishLayout(*doc);
        docs_.push_back(std::move(doc));
        return docs_.back().get();
    }

    QString PageLayout::findImage(const QString &name)
    {
        if (const auto it = imagePaths_.constFind(name); it != imagePaths_.cend())
        {
            return *it;
        }

        // Local files only; web images would need a network stack.
        QString found;
        const QUrl url(name);
        if (url.scheme().isEmpty() || url.isLocalFile())
        {
            const QString path = url.isLocalFile() ? url.toLocalFile() : name;
            // "/favicon.png" is a path on the website, so a leading "/" is
            // also tried relative to the document, as is Resources/.
            QString relative = path;
            while (relative.startsWith(QLatin1Char('/')))
            {
                relative.remove(0, 1);
            }
            const QDir base(baseDir_);
            QStringList candidates;
            if (QFileInfo(path).isAbsolute())
            {
                candidates << path;
            }
            candidates << base.filePath(relative)
                       << base.filePath(QStringLiteral("Resources/") + relative)
                       << base.filePath(QStringLiteral("Resources/") +
                                        QFileInfo(relative).fileName());
            for (const QString &candidate : candidates)
            {
                const QFileInfo info(candidate);
                if (info.isFile())
                {
                    found = info.absoluteFilePath();
                    break;
                }
            }
        }
        imagePaths_.insert(name, found);
        return found;
    }

    // ".page img { mix-blend-mode: multiply }": over a flat background that
    // is the same as tinting the image by the background colour, which also
    // works in PDFs (Qt's PDF output ignores blend modes). Info boxes use
    // "mix-blend-mode: normal": invalid, no tint.
    QColor PageLayout::imageTint(Role role) const
    {
        if (role == Role::Info)
        {
            return {};
        }
        const QColor fill = theme_.boxStyle(role).fill;
        return fill.isValid() ? fill : theme_.pageBg;
    }

    // Starts decoding the document's images on other threads, to be ready
    // when the layout reaches them. A guess from the markdown: whatever it
    // misses is decoded when met.
    void PageLayout::prefetchImages(const Document &doc)
    {
        static const QRegularExpression pattern(
            QStringLiteral(R"(!\[[^\]]*\]\(\s*<?([^)\s>]+)|<img\s[^>]*src\s*=\s*["']([^"']+))"),
            QRegularExpression::CaseInsensitiveOption);
        std::vector<std::pair<QString, QColor>> wanted;
        for (const Section &section : doc.sections)
        {
            for (const Column &column : section.columns)
            {
                for (const Block &block : column.blocks)
                {
                    const Role role = block.kind != BlockKind::Markdown
                                          ? roleFor(block.kind)
                                      : section.kind == SectionKind::Head
                                          ? Role::Head
                                          : Role::Body;
                    for (const QString &markdown : block.columns)
                    {
                        auto it = pattern.globalMatch(markdown);
                        while (it.hasNext())
                        {
                            const QRegularExpressionMatch m = it.next();
                            const QString name = m.captured(1).isEmpty()
                                                     ? m.captured(2)
                                                     : m.captured(1);
                            const QString path = findImage(name);
                            if (!path.isEmpty())
                            {
                                wanted.emplace_back(path, imageTint(role));
                            }
                        }
                    }
                }
            }
        }
        ImageCache::instance().prefetch(wanted);
    }

    void PageLayout::loadImages(QTextDocument &doc, qreal width, Role role)
    {
        const QColor tint = imageTint(role); // see there

        // Collect first: changing formats invalidates fragment iterators.
        std::vector<std::pair<int, QTextImageFormat>> found;
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next())
        {
            for (auto it = b.begin(); !it.atEnd(); ++it)
            {
                const QTextFragment fragment = it.fragment();
                if (fragment.charFormat().isImageFormat())
                {
                    found.emplace_back(fragment.position(),
                                       fragment.charFormat().toImageFormat());
                }
            }
        }

        const qreal k = lengthScale(doc);
        QTextCursor cursor(&doc);
        for (auto &[position, format] : found)
        {
            const QString name = format.name();
            const QString path = findImage(name);
            QImage image =
                path.isEmpty() ? QImage() : ImageCache::instance().image(path, tint);
            QString resource = name;
            // One image pixel is one CSS pixel, 0.75pt, and
            // ".page img { max-width: 100% }".
            QSizeF size = image.size() * 0.75;
            if (image.isNull())
            {
                size = QSizeF(std::min(width, 12 * kRem), 2.5 * kRem);
                image = missingImage(name, size);
                resource = QStringLiteral("missing:") + name;
            }
            else
            {
                if (size.width() > width)
                {
                    size *= width / size.width();
                }
                if (tint.isValid())
                {
                    resource = QStringLiteral("%1#%2").arg(name, tint.name());
                }
            }

            doc.addResource(QTextDocument::ImageResource, QUrl(resource),
                            image);
            format.setName(resource);
            format.setWidth(size.width() * k);
            format.setHeight(size.height() * k);
            cursor.setPosition(position);
            cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
            cursor.setCharFormat(format);
        }
    }

    void PageLayout::ensurePage(int index)
    {
        while (static_cast<int>(pages_.size()) <= index)
        {
            Page page;
            page.title = title_;
            page.watermark = watermark_;
            pages_.push_back(std::move(page));
        }
    }

    void PageLayout::advance(ColCursor &cur)
    {
        ++cur.page;
        ensurePage(cur.page);
        cur.y = spec_.contentTop();
    }

    void PageLayout::run(const Document &src)
    {
        title_ = src.title;
        watermark_ = src.watermark;
        pageNumbers_ = src.pageNumbers;
        ensurePage(0);

        const qreal top = spec_.contentTop();
        const qreal contentW = spec_.contentWidth();
        const qreal pad = spec_.columnPadding;

        int curPage = 0;
        qreal rowTop = top;
        bool pageUsed = false;

        const int sections = static_cast<int>(src.sections.size());
        for (int done = 0; done < sections; ++done)
        {
            if (progress_)
            {
                (*progress_)(done, sections);
            }
            const Section &sec = src.sections.at(done);
            // Released between bands, so the preview can paint meanwhile.
            const std::lock_guard<FairMutex> lock(textEngineMutex());
            if (cancelled())
            {
                return;
            }
            if (sec.pageBreakBefore && pageUsed)
            {
                ++curPage;
                ensurePage(curPage);
                rowTop = top;
                pageUsed = false;
            }

            const int n = static_cast<int>(sec.columns.size());
            std::vector<qreal> xs(static_cast<size_t>(n));
            std::vector<qreal> ws(static_cast<size_t>(n));

            // Equal columns: each takes contentW / n and is padded inside.
            const qreal slot = contentW / n;
            // Sidebar bands: one padded container holding a sidebar (a third
            // of it) and the main text.
            const qreal x0 = spec_.marginX + pad;
            const qreal cw = contentW - 2 * pad;
            const qreal side = cw * kSidebarFraction;
            const qreal main = cw - side - kSidebarGap;

            if (sec.kind == SectionKind::Left)
            {
                xs = {x0, x0 + side + kSidebarGap};
                ws = {side, main};
            }
            else if (sec.kind == SectionKind::Right)
            {
                xs = {x0 + cw - side, x0};
                ws = {side, main};
            }
            else
            {
                for (int i = 0; i < n; ++i)
                {
                    const size_t k = static_cast<size_t>(i);
                    xs[k] = spec_.marginX + i * slot + pad;
                    ws[k] = slot - 2 * pad;
                }
            }

            const Role mdRole =
                (sec.kind == SectionKind::Head) ? Role::Head : Role::Body;
            std::vector<ColCursor> cursors(static_cast<size_t>(n),
                                           ColCursor{curPage, rowTop});
            // The main text follows the floated sidebar in the page's HTML,
            // so "* + .content" puts a gap above it.
            if (sec.kind == SectionKind::Left || sec.kind == SectionKind::Right)
            {
                cursors[1].y += kBlockGap;
            }
            const bool sidebar = (sec.kind == SectionKind::Left ||
                                  sec.kind == SectionKind::Right) &&
                                 n == 2;
            for (int i = 0; i < n; ++i)
            {
                const size_t k = static_cast<size_t>(i);
                if (sidebar && i == 1)
                {
                    // The rest of the band takes its whole width and flows
                    // around the sidebar.
                    placeColumn(sec.columns[i], x0, cw, mdRole, cursors[k]);
                    obstacles_.clear();
                    continue;
                }
                const int firstPage = cursors[k].page;
                const size_t firstItem =
                    pages_[static_cast<size_t>(firstPage)].items.size();
                placeColumn(sec.columns[i], xs[k], ws[k], mdRole, cursors[k]);
                if (sidebar)
                {
                    for (int page = firstPage; page <= cursors[k].page; ++page)
                    {
                        const auto &items =
                            pages_[static_cast<size_t>(page)].items;
                        for (size_t j = page == firstPage ? firstItem : 0;
                             j < items.size(); ++j)
                        {
                            addObstacles(page, items[j], xs[k], xs[k] + ws[k]);
                        }
                    }
                }
            }

            // The band ends at the lowest point reached, on the last page any
            // column reached.
            int endPage = curPage;
            for (const ColCursor &c : cursors)
            {
                endPage = std::max(endPage, c.page);
            }
            qreal endY = rowTop;
            for (const ColCursor &c : cursors)
            {
                if (c.page == endPage)
                {
                    endY = std::max(endY, c.y);
                }
            }

            if (endPage == curPage && endY <= rowTop + kEpsilon)
            {
                continue; // empty band
            }

            // Faint vertical dividers between equal columns.
            if (sec.kind == SectionKind::Normal && n > 1)
            {
                const qreal bottom =
                    (endPage == curPage) ? endY : spec_.contentBottom();
                for (int i = 1; i < n; ++i)
                {
                    BoxDeco line;
                    line.rect = QRectF(spec_.marginX + i * slot, rowTop, 0,
                                       bottom - rowTop);
                    line.edges = Qt::LeftEdge;
                    line.edgeColor = theme_.divider;
                    line.edgeWidth = 1;
                    pages_[static_cast<size_t>(curPage)].decos.push_back(line);
                }
            }

            curPage = endPage;
            rowTop = endY + kRowGap;
            pageUsed = true;
        }
    }

    void PageLayout::placeColumn(const Column &column, qreal x, qreal w,
                                 Role markdownRole, ColCursor &cur)
    {
        bool first = true;
        for (const Block &block : column.blocks)
        {
            if (cancelled())
            {
                return;
            }
            if (block.kind == BlockKind::Sticky)
            {
                // Out of the flow, on the page the column has reached.
                placeSticky(block, cur.page);
                continue;
            }
            const bool isText = block.kind == BlockKind::Markdown;
            if (isText && block.columns.value(0).trimmed().isEmpty())
            {
                continue;
            }
            if (!first && cur.y > spec_.contentTop() + kEpsilon)
            {
                cur.y += std::max(0.0, kBlockGap + cur.trailing);
            }
            cur.trailing = 0;
            first = false;

            if (isText && theme_.boxStyle(markdownRole).fill.isValid())
            {
                // Text in a box of its own: the Remaster head block.
                placeBox(block, x, w, cur, markdownRole);
            }
            else if (isText)
            {
                placeMarkdown(block.columns.value(0), x, w, markdownRole, cur);
            }
            else
            {
                placeBox(block, x, w, cur, roleFor(block.kind));
            }
        }
    }

    void PageLayout::placeMarkdown(const QString &markdown, qreal x, qreal w,
                                   Role role, ColCursor &cur)
    {
        if (markdown.trimmed().isEmpty())
        {
            return;
        }

        QTextDocument *doc = makeDoc(markdown, w, role);
        if (flowing(cur.page) && wrap(doc, cur, x, 0) < 0)
        {
            // Tables can't flow around a sidebar: the whole text goes beside
            // or below it.
            const auto [left, right] = besideSidebar(cur, x, w, [&](qreal width) {
                discard(doc);
                doc = makeDoc(markdown, width, role);
                return docHeight(*doc);
            });
            x = left;
            w = right - left;
        }
        const qreal total = docHeight(*doc);
        cur.trailing = trailingMargin(*doc);
        const std::vector<qreal> cuts = cutPoints(*doc);
        const qreal top = spec_.contentTop();
        const QColor textColor = theme_.textColor(role);
        const std::vector<qreal> rules = ruleTops(*doc);

        // Feat bars ("#### 1st Level"), in points within the document.
        std::vector<QRectF> bars;
        for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        {
            if (const qreal height = featBarHeight(b); height > 0)
            {
                const qreal top =
                    doc->documentLayout()->blockBoundingRect(b).top() / kUnits;
                bars.emplace_back(0, top, w, height);
            }
        }

        qreal srcY = 0;
        while (srcY < total - kEpsilon)
        {
            const qreal remaining = spec_.contentBottom() - cur.y;
            qreal end = total;

            if (total - srcY > remaining + kEpsilon)
            {
                // Largest cut point that still fits in the space left in this
                // column.
                const auto it = std::upper_bound(cuts.begin(), cuts.end(),
                                                 srcY + remaining + kEpsilon);
                end = (it == cuts.begin()) ? srcY : *(it - 1);

                if (end <= srcY + kEpsilon)
                {
                    if (cur.y > top + kEpsilon)
                    {
                        advance(cur); // nothing fits; retry on the next page
                        continue;
                    }
                    end = std::min(total, srcY + remaining); // hard cut
                }
            }

            for (const QRectF &bar : bars)
            {
                if (bar.top() < srcY - kEpsilon || bar.top() >= end - kEpsilon)
                {
                    continue;
                }
                const qreal top = cur.y + bar.top() - srcY;
                const auto [left, right] =
                    freeSpan(cur.page, x, x + w, top, top + bar.height());
                addFeatBar(pages_[static_cast<size_t>(cur.page)],
                           QRectF(left, top, right - left, bar.height()));
            }
            for (const qreal rule : rules)
            {
                if (rule >= srcY - kEpsilon && rule < end - kEpsilon)
                {
                    const qreal top = cur.y + rule - srcY;
                    const auto [left, right] =
                        freeSpan(cur.page, x, x + w, top, top + 1.5);
                    addRule(pages_[static_cast<size_t>(cur.page)], left, top,
                            right - left, role);
                }
            }
            pages_[static_cast<size_t>(cur.page)].items.push_back(
                {doc, QRectF(0, srcY, w, end - srcY), QPointF(x, cur.y),
                 textColor, srcY < kEpsilon ? kBleed : 0,
                 end > total - kEpsilon ? kBleed : 0});
            cur.y += end - srcY;
            srcY = end;

            if (srcY < total - kEpsilon)
            {
                advance(cur);
            }
        }
    }

    void PageLayout::placeSticky(const Block &block, int pageIndex)
    {
        // "position: absolute; left: Xmm; top: Ymm" from the page's corner,
        // as wide as its text ("shrink to fit") but not past the page edge.
        constexpr qreal kPointsPerMm = 72 / 25.4;
        const BoxStyle style = theme_.boxStyle(Role::Sticky);
        const QMarginsF &pad = style.padding;
        const QPointF topLeft = block.position * kPointsPerMm;
        const qreal maxWidth = std::max(2 * kRem, spec_.width - topLeft.x() -
                                                      pad.left() - pad.right());

        QTextDocument *doc =
            makeDoc(block.columns.value(0), maxWidth, Role::Sticky);
        const qreal ideal = doc->idealWidth() / kUnits;
        if (ideal < maxWidth - kEpsilon)
        {
            // A hair over the widest line, so nothing re-wraps.
            doc->setTextWidth((ideal + 0.1) * kUnits);
            Theme::finishLayout(*doc);
        }
        const qreal textW = doc->textWidth() / kUnits;
        const qreal textH = docHeight(*doc);
        const QRectF box(topLeft, QSizeF(textW + pad.left() + pad.right(),
                                         textH + pad.top() + pad.bottom()));

        Page &page = pages_[static_cast<size_t>(pageIndex)];
        // "box-shadow: 1px 2px 2px": offset by (0.75, 1.5)pt and blurred
        // over 1.5pt, approximated by a few widening, fainter layers.
        constexpr int kLayers = 3;
        for (int i = kLayers - 1; i >= 0; --i)
        {
            BoxDeco shadow;
            const qreal grow = 0.75 * i / (kLayers - 1) - 0.375;
            shadow.rect =
                box.translated(0.75, 1.5).adjusted(-grow, -grow, grow, grow);
            shadow.fill = theme_.stickyShadow;
            shadow.fill.setAlphaF(shadow.fill.alphaF() / kLayers);
            shadow.radius = style.radius + grow;
            shadow.hole = box;
            shadow.holeRadius = style.radius;
            page.overlayDecos.push_back(shadow);
        }
        BoxDeco note;
        note.rect = box;
        note.fill = style.fill;
        note.radius = style.radius;
        page.overlayDecos.push_back(note);

        page.overlayItems.push_back(
            {doc, QRectF(0, 0, textW, textH),
             QPointF(box.left() + pad.left(), box.top() + pad.top()),
             theme_.textColor(Role::Sticky), pad.top(), pad.bottom()});
    }

    void PageLayout::addRule(Page &page, qreal x, qreal y, qreal w,
                             Role role) const
    {
        constexpr qreal kPixel = 0.75; // the rule's two 1px borders
        BoxDeco upper;
        upper.rect = QRectF(x, y, w, kPixel);
        upper.fill = theme_.ruleColor(role);
        BoxDeco lower = upper;
        lower.rect.translate(0, kPixel);
        lower.fill = theme_.ruleColor(role, true);
        page.decos.push_back(upper);
        page.decos.push_back(lower);
    }

    void PageLayout::addFeatBar(Page &page, const QRectF &rect) const
    {
        // Rounded top corners ("border-radius: 0.75rem 0.75rem 0 0"), and a
        // 1px line 1.9rem below the bar's top (its "::after").
        BoxDeco bar;
        bar.rect = rect;
        bar.fill = theme_.navy;
        bar.radius = 0.75 * kRem;
        bar.squareBottom = true;
        page.decos.push_back(bar);

        BoxDeco line;
        line.rect = QRectF(rect.left(), rect.top(), rect.width(), 1.9 * kRem);
        line.edges = Qt::BottomEdge;
        line.edgeColor = theme_.navy;
        line.edgeWidth = 0.75;
        page.decos.push_back(line);
    }

    void PageLayout::placeBox(const Block &block, qreal x, qreal w,
                              ColCursor &cur, Role role)
    {
        const BoxStyle style = theme_.boxStyle(role);
        // Borders sit outside the padding, as in CSS.
        const auto border = [&style](Qt::Edge edge) {
            return style.edges.testFlag(edge) ? style.edgeWidth : 0.0;
        };
        const QMarginsF pad =
            style.padding + QMarginsF(border(Qt::LeftEdge), border(Qt::TopEdge),
                                      border(Qt::RightEdge),
                                      border(Qt::BottomEdge));
        const int n = std::max(1, static_cast<int>(block.columns.size()));
        const bool sidebar =
            role == Role::SidebarLeft || role == Role::SidebarRight;
        const auto innerWidth = [&](qreal width) {
            return (width - pad.left() - pad.right() - (n - 1) * kInnerGap) / n;
        };

        std::vector<QTextDocument *> docs;
        const auto makeDocs = [&](qreal width) {
            for (auto it = docs.rbegin(); it != docs.rend(); ++it)
            {
                discard(*it);
            }
            docs.clear();
            for (const QString &md : block.columns)
            {
                docs.push_back(makeDoc(md, innerWidth(width), role));
            }
        };
        makeDocs(w);

        // Beside a sidebar, text without a background flows around it line
        // by line, like the text outside boxes; any other box keeps its
        // shape and goes beside or below it.
        bool flows = false;
        if (flowing(cur.page))
        {
            flows = n == 1 && !style.fill.isValid() && !style.edges &&
                    wrap(docs[0], cur, x + pad.left(), pad.top()) >= 0;
            if (!flows)
            {
                const auto [left, right] =
                    besideSidebar(cur, x, w, [&](qreal width) {
                        makeDocs(width);
                        qreal height = 0;
                        for (QTextDocument *d : docs)
                        {
                            height = std::max(height, docHeight(*d));
                        }
                        return height + pad.top() + pad.bottom();
                    });
                x = left;
                w = right - left;
            }
        }
        const qreal innerW = innerWidth(w);

        std::vector<qreal> heights;
        std::vector<std::vector<qreal>> cuts;
        for (QTextDocument *d : docs)
        {
            heights.push_back(docHeight(*d));
            cuts.push_back(cutPoints(*d));
        }

        // Like a browser, a box that doesn't fit is split between pages:
        // the part on each page is drawn open where it was cut.
        const QColor textColor = theme_.textColor(role);
        std::vector<qreal> from(docs.size(), 0);
        bool first = true;
        while (true)
        {
            const qreal padTop = first ? pad.top() : 0;
            const qreal space = spec_.contentBottom() - cur.y;
            qreal rest = 0;
            for (size_t i = 0; i < docs.size(); ++i)
            {
                rest = std::max(rest, heights[i] - from[i]);
            }
            const bool last = padTop + rest <= space + kEpsilon;
            std::vector<qreal> to = heights;
            if (!last)
            {
                bool any = false;
                for (size_t i = 0; i < docs.size(); ++i)
                {
                    const qreal limit = from[i] + space - padTop + kEpsilon;
                    if (heights[i] > limit)
                    {
                        const auto it = std::upper_bound(cuts[i].begin(),
                                                         cuts[i].end(), limit);
                        to[i] = it == cuts[i].begin()
                                    ? from[i]
                                    : std::max(from[i], *(it - 1));
                    }
                    any = any || to[i] > from[i] + kEpsilon;
                }
                if (!any)
                {
                    if (cur.y > spec_.contentTop() + kEpsilon)
                    {
                        advance(cur); // nothing fits; start on the next page
                        continue;
                    }
                    for (size_t i = 0; i < docs.size(); ++i)
                    {
                        to[i] = std::min(heights[i], from[i] + space - padTop);
                    }
                }
            }
            const qreal height =
                last ? std::min(space, padTop + rest + pad.bottom()) : space;

            Page &page = pages_[static_cast<size_t>(cur.page)];
            // A sidebar's rule only runs beside its text (added below).
            if (!sidebar)
            {
                BoxDeco deco{QRectF(x, cur.y, w, height), style.fill,
                             style.radius, style.edges, style.edgeColor,
                             style.edgeWidth};
                if (!first)
                {
                    deco.edges.setFlag(Qt::TopEdge, false);
                    deco.squareTop = true;
                }
                if (!last)
                {
                    deco.edges.setFlag(Qt::BottomEdge, false);
                    deco.squareBottom = true;
                }
                page.decos.push_back(deco);
            }
            // ".info .column + .column { border-left: 2px solid }" (Remaster).
            if (role == Role::Info && theme_.infoDivider.isValid())
            {
                const qreal top = cur.y + padTop;
                const qreal bottom = cur.y + height - (last ? pad.bottom() : 0);
                for (int i = 1; i < n; ++i)
                {
                    BoxDeco divider;
                    const qreal lx = x + pad.left() + i * (innerW + kInnerGap) -
                                     kInnerGap / 2 - 0.75;
                    divider.rect = QRectF(lx, top, 1.5, bottom - top);
                    divider.fill = theme_.infoDivider;
                    page.decos.push_back(divider);
                }
            }
            for (size_t i = 0; i < docs.size(); ++i)
            {
                if (to[i] <= from[i] + kEpsilon && !(first && last))
                {
                    continue;
                }
                const qreal dx = static_cast<qreal>(i) * (innerW + kInnerGap);
                const QPointF dest(x + pad.left() + dx, cur.y + padTop);
                for (const qreal rule : ruleTops(*docs[i]))
                {
                    if (rule < from[i] - kEpsilon || rule >= to[i] - kEpsilon)
                    {
                        continue;
                    }
                    const qreal top = dest.y() + rule - from[i];
                    const auto [left, right] =
                        flows ? freeSpan(cur.page, dest.x(), dest.x() + innerW,
                                         top, top + 1.5)
                              : std::make_pair(dest.x(), dest.x() + innerW);
                    addRule(page, left, top, right - left, role);
                }
                page.items.push_back(
                    {docs[i], QRectF(0, from[i], innerW, to[i] - from[i]), dest,
                     textColor, first ? pad.top() : 0,
                     to[i] >= heights[i] - kEpsilon ? pad.bottom() : 0});
                if (sidebar)
                {
                    for (const auto &[top, bottom] :
                         coverage(page.items.back()).text)
                    {
                        BoxDeco rule;
                        rule.rect = QRectF(x, top, w, bottom - top);
                        rule.edges = style.edges;
                        rule.edgeColor = style.edgeColor;
                        rule.edgeWidth = style.edgeWidth;
                        page.decos.push_back(rule);
                    }
                }
            }
            if (last)
            {
                cur.y += height;
                break;
            }
            from = to;
            first = false;
            advance(cur);
        }
        // Margins only collapse through a box with nothing at its bottom
        // edge, such as an item.
        if (pad.bottom() < kEpsilon)
        {
            for (QTextDocument *d : docs)
            {
                cur.trailing = std::min(cur.trailing, trailingMargin(*d));
            }
        }
    }

    PageLayout::Coverage PageLayout::coverage(const Placement &pl)
    {
        const qreal k = lengthScale(*pl.doc);
        Coverage c;
        const QPointF origin = pl.dest - pl.source.topLeft();
        for (QTextBlock b = pl.doc->begin(); b.isValid(); b = b.next())
        {
            const QTextLayout *layout = b.layout();
            if (!layout)
            {
                continue;
            }
            const QPointF position = layout->position() / kUnits;
            for (int i = 0; i < layout->lineCount(); ++i)
            {
                const QTextLine line = layout->lineAt(i);
                const qreal top = position.y() + line.y() / kUnits;
                const qreal bottom = top + line.height() / kUnits;
                const qreal middle = (top + bottom) / 2;
                if (middle < pl.source.top() || middle >= pl.source.bottom())
                {
                    continue;
                }
                const int from = b.position() + line.textStart();
                const int to = from + line.textLength();
                bool text = false;
                for (auto it = b.begin(); !it.atEnd(); ++it)
                {
                    const QTextFragment fragment = it.fragment();
                    const int start = std::max(fragment.position(), from);
                    const int end =
                        std::min(fragment.position() + fragment.length(), to);
                    if (start >= end)
                    {
                        continue;
                    }
                    const QTextCharFormat format = fragment.charFormat();
                    if (!format.isImageFormat())
                    {
                        text = text || !fragment.text()
                                            .mid(start - fragment.position(),
                                                 end - start)
                                            .trimmed()
                                            .isEmpty();
                        continue;
                    }
                    // An inline image stands on the baseline.
                    const QTextImageFormat image = format.toImageFormat();
                    const QSizeF size =
                        QSizeF(image.width(), image.height()) / k;
                    const QImage pixels =
                        pl.doc->resource(QTextDocument::ImageResource,
                                         QUrl(image.name()))
                            .value<QImage>();
                    for (int pos = start; pos < end; ++pos)
                    {
                        const QPointF topLeft(
                            position.x() +
                                line.cursorToX(pos - b.position()) / kUnits,
                            top + line.ascent() / kUnits - size.height());
                        c.images.emplace_back(QRectF(origin + topLeft, size),
                                              pixels);
                    }
                }
                if (text)
                {
                    c.text.emplace_back(
                        origin.y() + std::max(top, pl.source.top()),
                        origin.y() + std::min(bottom, pl.source.bottom()));
                }
            }
        }
        std::vector<std::pair<qreal, qreal>> runs;
        for (const auto &line : c.text)
        {
            if (!runs.empty() && line.first - runs.back().second < kRunGap)
            {
                runs.back().second = std::max(runs.back().second, line.second);
            }
            else
            {
                runs.push_back(line);
            }
        }
        c.text = std::move(runs);
        return c;
    }

    void PageLayout::addObstacles(int page, const Placement &pl, qreal left,
                                  qreal right)
    {
        std::vector<QRectF> &list = obstacles_[page];
        const Coverage c = coverage(pl);
        // Text takes the sidebar's whole width.
        for (const auto &[top, bottom] : c.text)
        {
            list.emplace_back(QPointF(left - kSidebarGap, top),
                              QPointF(right + kSidebarGap, bottom));
        }
        // Images only what they cover: thin slices of their opaque part.
        for (const auto &[rect, image] : c.images)
        {
            if (image.isNull() || image.width() == 0 || image.height() == 0)
            {
                list.push_back(rect.adjusted(-kImageGap, -kImageGapY, kImageGap,
                                             kImageGapY));
                continue;
            }
            const auto outline = ImageCache::instance().outline(image, kOpaque);
            const std::vector<int> &lo = outline->first; // -1: see-through
            const std::vector<int> &hi = outline->last;
            const int rows = image.height();
            const qreal sx = rect.width() / image.width();
            const qreal sy = rect.height() / rows;
            for (qreal top = 0; top < rect.height(); top += kSlice)
            {
                const qreal bottom = std::min(top + kSlice, rect.height());
                const int r0 = std::clamp(static_cast<int>(top / sy), 0, rows - 1);
                const int r1 = std::clamp(static_cast<int>(std::ceil(bottom / sy)),
                                          r0 + 1, rows);
                int first = image.width();
                int last = -1;
                for (int r = r0; r < r1; ++r)
                {
                    if (hi[static_cast<size_t>(r)] >= 0)
                    {
                        first = std::min(first, lo[static_cast<size_t>(r)]);
                        last = std::max(last, hi[static_cast<size_t>(r)]);
                    }
                }
                if (last < 0)
                {
                    continue; // see-through all the way across
                }
                list.emplace_back(
                    QPointF(rect.left() + first * sx - kImageGap,
                            rect.top() + top - kImageGapY),
                    QPointF(rect.left() + (last + 1) * sx + kImageGap,
                            rect.top() + bottom + kImageGapY));
            }
        }
    }

    // The part of [left, right] free of obstacles between `top` and `bottom`
    // on `page`. An obstacle on the right half moves the right end in, one
    // on the left half the left end.
    std::pair<qreal, qreal> PageLayout::freeSpan(int page, qreal left,
                                                 qreal right, qreal top,
                                                 qreal bottom) const
    {
        const auto it = obstacles_.find(page);
        if (it == obstacles_.end())
        {
            return {left, right};
        }
        const qreal middle = (left + right) / 2;
        qreal l = left;
        qreal r = right;
        for (const QRectF &o : it->second)
        {
            if (o.bottom() <= top || o.top() >= bottom || o.right() <= left ||
                o.left() >= right)
            {
                continue;
            }
            if (o.center().x() >= middle)
            {
                r = std::min(r, o.left());
            }
            else
            {
                l = std::max(l, o.right());
            }
        }
        return {l, std::max(l, r)};
    }

    bool PageLayout::flowing(int page) const
    {
        return obstacles_.lower_bound(page) != obstacles_.end();
    }

    // Page and y of the point `offset` below `cur`, if the column went on
    // from the top of the next page where this one ends.
    std::pair<int, qreal> PageLayout::flowPosition(const ColCursor &cur,
                                                   qreal offset) const
    {
        int page = cur.page;
        qreal y = cur.y + offset;
        while (y > spec_.contentBottom())
        {
            y = spec_.contentTop() + (y - spec_.contentBottom());
            ++page;
        }
        return {page, y};
    }

    // Flows `doc`, whose top is `offset` below `cur` at `x`, around the
    // sidebar. Returns its new height in layout units, or -1 if it can't.
    qreal PageLayout::wrap(QTextDocument *doc, const ColCursor &cur, qreal x,
                           qreal offset)
    {
        const qreal width = doc->textWidth() / kUnits;
        const auto room = [&](qreal top, qreal bottom) {
            const auto [page, y] = flowPosition(cur, offset + top / kUnits);
            const auto [left, right] = freeSpan(page, x, x + width, y,
                                                y + (bottom - top) / kUnits);
            return std::make_pair((left - x) * kUnits, (right - x) * kUnits);
        };
        const qreal before = doc->documentLayout()->documentSize().height();
        qreal height = Theme::reflow(*doc, room);
        if (height > before + 0.01)
        {
            // Qt draws nothing below the height it laid the document out to:
            // make that room with a bottom margin, then flow again.
            const qreal extra = height - before;
            QTextFrameFormat format = doc->rootFrame()->frameFormat();
            format.setBottomMargin(extra * lengthScale(*doc) / kUnits);
            doc->rootFrame()->setFrameFormat(format);
            Theme::finishLayout(*doc);
            const qreal margin =
                doc->documentLayout()->documentSize().height() - before;
            height = Theme::reflow(*doc, room) - margin;
        }
        if (height >= 0)
        {
            doc->setProperty(kHeightProperty, height);
        }
        return height;
    }

    // For a block that can't flow around the sidebar: the span beside it
    // that the block fits in, `height(width)` tall at a width (which may
    // rebuild the block). If that's too narrow, the block keeps its width
    // and moves below the sidebar instead.
    std::pair<qreal, qreal>
    PageLayout::besideSidebar(ColCursor &cur, qreal x, qreal w,
                              const std::function<qreal(qreal)> &height)
    {
        qreal left = x;
        qreal right = x + w;
        qreal h = height(w);
        for (int tries = 0; tries < 4; ++tries)
        {
            const auto [l, r] = freeSpan(cur.page, x, x + w, cur.y, cur.y + h);
            if (r - l >= right - left - kEpsilon)
            {
                break;
            }
            left = l;
            right = r;
            h = height(right - left);
        }
        if (right - left >= std::min(w, kMinBoxWidth) - kEpsilon)
        {
            return {left, right};
        }
        const auto it = obstacles_.find(cur.page);
        if (it != obstacles_.end())
        {
            for (const QRectF &o : it->second)
            {
                if (o.right() > x && o.left() < x + w)
                {
                    cur.y = std::max(cur.y, o.bottom());
                }
            }
        }
        height(w);
        return {x, x + w};
    }

    void PageLayout::discard(QTextDocument *doc)
    {
        if (!docs_.empty() && docs_.back().get() == doc)
        {
            docs_.pop_back();
        }
    }

    void PageLayout::paintPage(QPainter &p, int index,
                               const TextSelection *selection,
                               const std::vector<Highlight> *highlights) const
    {
        const std::lock_guard<FairMutex> lock(textEngineMutex());
        const Page &page = pages_.at(static_cast<size_t>(index));

        p.save();
        p.fillRect(QRectF(0, 0, spec_.width, spec_.height), theme_.pageBg);

        // Watermark: small italic line centred at the very top.
        if (!page.watermark.isEmpty())
        {
            p.setPen(theme_.watermark);
            drawFineText(
                p, makeFont(theme_.timesFamilies, 0.9 * kRem, false, true),
                QRectF(0, 0.25 * kRem, spec_.width, 14),
                Qt::AlignHCenter | Qt::AlignTop, page.watermark.simplified());
        }

        // Title banner: half the page wide, text right-aligned, with a
        // border (gold, in the Pathfinder theme) on every side but the left.
        if (!page.title.isEmpty())
        {
            constexpr qreal kTop = 1.5 * kRem;
            constexpr qreal kBorder = 0.33 * kRem;
            constexpr qreal kPadX = 0.5 * kRem;
            // ".page h1" overrides ".title h1"'s 1.25rem; line-height is 1.
            constexpr qreal kFont = kRem;
            constexpr qreal kInner = kFont + 2 * 0.25 * kRem;
            const qreal height = kInner + 2 * kBorder;
            const QRectF outer(0, kTop, spec_.width / 2, height);
            const QRectF inner(outer.left(), outer.top() + kBorder,
                               outer.width() - kBorder,
                               outer.height() - 2 * kBorder);
            p.fillRect(outer, theme_.titleEdge);
            p.fillRect(inner, theme_.titleFill);

            p.setPen(theme_.titleText);
            // Scaled when a wider stand-in replaces Taroca (Starfinder).
            const qreal fontSize = theme_.fitFont(theme_.tarocaFamilies, kFont, false).first;
            drawFineText(p, makeFont(theme_.tarocaFamilies, fontSize),
                         inner.adjusted(0, 0, -kPadX, 0),
                         Qt::AlignRight | Qt::AlignVCenter,
                         page.title.simplified());
        }

        // Page number (only when the document contains a "pagenumbers" line).
        if (pageNumbers_)
        {
            p.setPen(theme_.pageNumber);
            // 2rem from the bottom and right edges of the page.
            constexpr qreal kInset = 2 * kRem;
            const QRectF box(spec_.width - kInset - 80,
                             spec_.height - kInset - 16, 80, 16);
            drawFineText(
                p, makeFont(theme_.timesFamilies, 0.9 * kRem, true, true), box,
                Qt::AlignRight | Qt::AlignBottom, QString::number(index + 1));
        }

        for (const BoxDeco &d : page.decos)
        {
            paintDeco(p, d);
        }
        // What part of item `item` `selection` covers.
        // Highlights touching this page, then the selection on top.
        auto marks = [&](int item, const Placement &pl) {
            std::vector<Mark> out;
            if (highlights)
            {
                for (const Highlight &h : *highlights)
                {
                    if (h.range.start().page > index || h.range.end().page < index)
                    {
                        continue;
                    }
                    const auto [from, to] = rangeIn(h.range, index, item, pl);
                    out.push_back({from, to, h.background, h.text});
                }
            }
            if (selection && !selection->isEmpty())
            {
                const auto [from, to] = rangeIn(*selection, index, item, pl);
                out.push_back({from, to, kSelection, QColor()});
            }
            return out;
        };

        const int itemCount = static_cast<int>(page.items.size());
        for (int i = 0; i < itemCount; ++i)
        {
            paintPlacement(p, page.items[static_cast<size_t>(i)],
                           marks(i, page.items[static_cast<size_t>(i)]));
        }
        for (const BoxDeco &d : page.overlayDecos)
        {
            paintDeco(p, d);
        }
        for (size_t i = 0; i < page.overlayItems.size(); ++i)
        {
            paintPlacement(p, page.overlayItems[i],
                           marks(itemCount + static_cast<int>(i),
                                 page.overlayItems[i]));
        }
        p.restore();
    }

    const Placement *PageLayout::placement(int page, int item) const
    {
        if (page < 0 || page >= static_cast<int>(pages_.size()) || item < 0)
            return nullptr;
        const Page &pg = pages_[static_cast<size_t>(page)];
        const size_t i = static_cast<size_t>(item);
        if (i < pg.items.size())
            return &pg.items[i];
        if (i - pg.items.size() < pg.overlayItems.size())
            return &pg.overlayItems[i - pg.items.size()];
        return nullptr;
    }

    int PageLayout::itemCount(int page) const
    {
        if (page < 0 || page >= static_cast<int>(pages_.size()))
            return 0;
        const Page &pg = pages_[static_cast<size_t>(page)];
        return static_cast<int>(pg.items.size() + pg.overlayItems.size());
    }

    std::pair<int, int> PageLayout::visibleRange(const Placement &pl) const
    {
        // Document positions of the lines whose middle lies in the slice.
        int from = -1;
        int to = -1;
        for (QTextBlock b = pl.doc->begin(); b.isValid(); b = b.next())
        {
            const QTextLayout *layout = b.layout();
            if (!layout)
                continue;
            for (int i = 0; i < layout->lineCount(); ++i)
            {
                const QTextLine line = layout->lineAt(i);
                const qreal middle =
                    (layout->position().y() + line.y() + line.height() / 2) /
                    kUnits;
                if (middle < pl.source.top() || middle >= pl.source.bottom())
                    continue;
                const int start = b.position() + line.textStart();
                const int end = start + line.textLength();
                if (from < 0 || start < from)
                    from = start;
                to = std::max(to, end);
            }
        }
        return {from, to};
    }

    TextPoint PageLayout::hitTest(int page, const QPointF &point) const
    {
        const std::lock_guard<FairMutex> lock(textEngineMutex());
        // The item under the point, sticky notes first as they're on top;
        // else the nearest one.
        int best = -1;
        qreal bestDistance = 0;
        for (int i = itemCount(page) - 1; i >= 0; --i)
        {
            const Placement *pl = placement(page, i);
            if (visibleRange(*pl).first < 0)
                continue;
            const QRectF r(pl->dest, pl->source.size());
            const qreal dx = std::max({r.left() - point.x(), 0.0,
                                       point.x() - r.right()});
            const qreal dy = std::max({r.top() - point.y(), 0.0,
                                       point.y() - r.bottom()});
            const qreal distance = std::hypot(dx, dy);
            if (best < 0 || distance < bestDistance)
            {
                best = i;
                bestDistance = distance;
            }
            if (distance == 0)
                break;
        }
        if (best < 0)
            return {};

        const Placement *pl = placement(page, best);
        QPointF local = point - pl->dest + pl->source.topLeft();
        local.setX(std::clamp(local.x(), 0.0, pl->source.width()));
        local.setY(std::clamp(local.y(), pl->source.top(),
                              pl->source.bottom() - 0.01));
        const auto [from, to] = visibleRange(*pl);
        int pos = pl->doc->documentLayout()->hitTest(local * kUnits,
                                                     Qt::FuzzyHit);
        if (pos < 0)
            pos = point.y() < pl->dest.y() ? from : to;
        return {page, best, std::clamp(pos, from, to)};
    }

    TextSelection PageLayout::wordAt(const TextPoint &point) const
    {
        const std::lock_guard<FairMutex> lock(textEngineMutex());
        const Placement *pl = placement(point.page, point.item);
        if (!pl)
            return {};
        QTextCursor cursor(pl->doc);
        cursor.setPosition(point.pos);
        cursor.select(QTextCursor::WordUnderCursor);
        return {{point.page, point.item, cursor.selectionStart()},
                {point.page, point.item, cursor.selectionEnd()}};
    }

    TextSelection PageLayout::selectAll() const
    {
        const std::lock_guard<FairMutex> lock(textEngineMutex());
        TextSelection all;
        for (int page = 0; page < static_cast<int>(pages_.size()); ++page)
        {
            for (int item = 0; item < itemCount(page); ++item)
            {
                const auto [from, to] = visibleRange(*placement(page, item));
                if (from < 0)
                    continue;
                if (!all.anchor.isValid())
                    all.anchor = {page, item, from};
                all.focus = {page, item, to};
            }
        }
        return all;
    }

    // Document positions of `pl` (item `item` of page `page`) inside
    // `selection`; from >= to for none.
    std::pair<int, int> PageLayout::rangeIn(const TextSelection &selection,
                                            int page, int item,
                                            const Placement &pl) const
    {
        std::pair<int, int> range{0, 0};
        if (selection.isEmpty())
            return range;
        const TextPoint start = selection.start();
        const TextPoint end = selection.end();
        const TextPoint here{page, item, 0};
        const TextPoint first{start.page, start.item, 0};
        const TextPoint last{end.page, end.item, 0};
        if (here < first || last < here)
            return range;
        range = visibleRange(pl);
        if (here == first)
            range.first = start.pos;
        if (here == last)
            range.second = end.pos;
        return range;
    }

    // Plain text of document positions [from, to) of `pl`: one line per
    // block, badges as their words, icons back as their ":a:" tokens. With
    // `points`, also where each character comes from.
    void PageLayout::appendText(const Placement &pl, int page, int item,
                                int from, int to, QString &out,
                                std::vector<TextPoint> *points) const
    {
        QHash<QChar, QString> tokens;
        for (const auto &[token, glyph] : theme_.actionGlyphs)
        {
            if (glyph.size() == 1)
                tokens.insert(glyph.at(0), token);
        }
        auto add = [&](const QString &text, int pos) {
            out += text;
            if (points)
                points->insert(points->end(), text.size(), TextPoint{page, item, pos});
        };

        QTextDocument *doc = pl.doc;
        for (QTextBlock b = doc->findBlock(from); b.isValid() && b.position() < to;
             b = b.next())
        {
            if (b.position() > from)
                add(QStringLiteral("\n"), b.position());
            for (auto it = b.begin(); !it.atEnd(); ++it)
            {
                const QTextFragment fragment = it.fragment();
                const int fs = fragment.position();
                const int s = std::max(fs, from);
                const int e = std::min(fs + fragment.length(), to);
                if (s >= e)
                    continue;
                const QTextCharFormat format = fragment.charFormat();
                if (format.objectType() == TraitBadge::kObjectType)
                {
                    const QString word = TraitBadge::text(format);
                    if (!word.isEmpty())
                        add(word + QLatin1Char(' '), s);
                }
                else if (!format.isImageFormat())
                {
                    const QString text = fragment.text();
                    for (int pos = s; pos < e; ++pos)
                    {
                        const QChar c = text.at(pos - fs);
                        if (c == QChar::LineSeparator)
                            add(QStringLiteral("\n"), pos);
                        else if (c == QChar::Nbsp)
                            add(QStringLiteral(" "), pos);
                        else if (const auto t = tokens.constFind(c); t != tokens.cend())
                            add(*t, pos);
                        else
                            add(QString(c), pos);
                    }
                }
            }
        }
    }

    // The text inside `range` (all of it when null) in reading order: a
    // line per text, with a text that continues on the next page carried on.
    void PageLayout::collectText(const TextSelection *range, QString &out,
                                 std::vector<TextPoint> *points) const
    {
        const TextPoint start = range ? range->start() : TextPoint{0, 0, 0};
        const TextPoint end =
            range ? range->end()
                  : TextPoint{static_cast<int>(pages_.size()) - 1, INT_MAX, 0};
        const QTextDocument *lastDoc = nullptr;
        int lastEnd = 0;
        bool any = false;
        for (int page = start.page; page <= end.page; ++page)
        {
            for (int item = 0; item < itemCount(page); ++item)
            {
                const TextPoint here{page, item, 0};
                if (here < TextPoint{start.page, start.item, 0} ||
                    TextPoint{end.page, end.item, 0} < here)
                {
                    continue;
                }
                const Placement *pl = placement(page, item);
                auto [from, to] = visibleRange(*pl);
                if (from < 0)
                    continue;
                if (range && page == start.page && item == start.item)
                    from = start.pos;
                if (range && page == end.page && item == end.item)
                    to = end.pos;
                if (pl->doc == lastDoc && any)
                {
                    // The same text continued on another page: carry on
                    // from where the last slice stopped.
                    appendText(*pl, page, item, lastEnd, to, out, points);
                }
                else if (from < to)
                {
                    if (any)
                    {
                        out += QLatin1Char('\n');
                        if (points)
                            points->push_back({page, item, from});
                    }
                    appendText(*pl, page, item, from, to, out, points);
                    any = true;
                }
                lastEnd = pl->doc == lastDoc ? std::max(lastEnd, to) : to;
                lastDoc = pl->doc;
            }
        }
    }

    QString PageLayout::selectedText(const TextSelection &selection) const
    {
        const std::lock_guard<FairMutex> lock(textEngineMutex());
        if (selection.isEmpty())
            return {};
        QString out;
        collectText(&selection, out, nullptr);
        return out.trimmed();
    }

    PlainText PageLayout::plainText() const
    {
        const std::lock_guard<FairMutex> lock(textEngineMutex());
        PlainText result;
        collectText(nullptr, result.text, &result.points);
        return result;
    }

    void PageLayout::resolveToc(int entries)
    {
        const std::lock_guard<FairMutex> lock(textEngineMutex());
        tocPoints_.assign(static_cast<size_t>(std::max(0, entries)), TextPoint{});
        for (int page = 0; page < static_cast<int>(pages_.size()); ++page)
        {
            for (int item = 0; item < itemCount(page); ++item)
            {
                const Placement *pl = placement(page, item);
                for (QTextBlock b = pl->doc->begin(); b.isValid(); b = b.next())
                {
                    const QVariant marks = b.blockFormat().property(kTocEntries);
                    const QTextLayout *layout = b.layout();
                    if (!marks.isValid() || !layout || layout->lineCount() == 0)
                    {
                        continue;
                    }
                    // On this page if its first line is (as visibleRange()).
                    const QTextLine line = layout->lineAt(0);
                    const qreal middle =
                        (layout->position().y() + line.y() + line.height() / 2) /
                        kUnits;
                    if (middle < pl->source.top() || middle >= pl->source.bottom())
                    {
                        continue;
                    }
                    for (const QVariant &entry : marks.toList())
                    {
                        const int i = entry.toInt();
                        if (i >= 0 && i < entries && !tocPoints_[static_cast<size_t>(i)].isValid())
                        {
                            tocPoints_[static_cast<size_t>(i)] = {page, item, b.position()};
                        }
                    }
                }
            }
        }
    }

    QRectF PageLayout::caretRect(const TextPoint &point) const
    {
        const std::lock_guard<FairMutex> lock(textEngineMutex());
        const Placement *pl = placement(point.page, point.item);
        if (!pl)
            return {};
        const QTextBlock block = pl->doc->findBlock(point.pos);
        const QTextLayout *layout = block.isValid() ? block.layout() : nullptr;
        if (!layout || layout->lineCount() == 0)
            return {};
        const int offset = point.pos - block.position();
        const QTextLine line = layout->lineForTextPosition(offset);
        if (!line.isValid())
            return {};
        const QPointF inDoc =
            (layout->position() + QPointF(line.cursorToX(offset), line.y())) /
            kUnits;
        return QRectF(pl->dest + inDoc - pl->source.topLeft(),
                      QSizeF(0, line.height() / kUnits));
    }
} // namespace scribe
