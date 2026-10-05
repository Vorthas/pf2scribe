// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#pragma once

#include "Rendering/ScribeParser.h"
#include "Rendering/Theme.h"

#include <QColor>
#include <QHash>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTextDocument>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

class QPainter;
class QThread;

namespace scribe
{
    // All units are points (1/72 inch). Values come from the site's
    // stylesheet: an A4 page with 5.25rem / 5.5rem padding, and columns that
    // are padded by 0.6rem on each side. See kRem in Theme.h.
    struct PageSpec
    {
        qreal width = 595.28;
        qreal height = 840.47; // 296.5 mm, the printed page height
        qreal marginX = 5.5 * kRem;
        qreal top = 5.25 * kRem;
        qreal contentBottomY = 783.36; // 10.88 in, where the screen page ends
        qreal columnPadding = 0.6 * kRem;

        qreal contentTop() const { return top; }
        qreal contentBottom() const { return contentBottomY; }
        qreal contentWidth() const { return width - 2 * marginX; }
    };

    // A rectangular slice of a QTextDocument drawn at a position on a page.
    struct Placement
    {
        QTextDocument *doc = nullptr;
        QRectF source; // region of the document, in document coordinates
        QPointF dest;  // top-left on the page
        QColor textColor;
        // How far glyphs may spill above / below the slice. Headings with a
        // tight line height draw above their line box, which is fine at the
        // start of a text block but not where a page cut the text.
        qreal bleedTop = 0;
        qreal bleedBottom = 0;
    };

    // A filled, rounded and/or edged rectangle drawn behind text (info boxes,
    // notes, sidebars, column dividers, ...).
    struct BoxDeco
    {
        QRectF rect;
        QColor fill; // invalid colour: no fill
        qreal radius = 0;
        Qt::Edges edges;
        QColor edgeColor;
        qreal edgeWidth = 0;
        bool squareTop = false;    // round the bottom corners only
        bool squareBottom = false; // round the top corners only
        // Not painted inside this rounded rectangle (radius `holeRadius`):
        // a shadow stays outside its box, like CSS box-shadow.
        QRectF hole{};
        qreal holeRadius = 0;
    };

    struct Page
    {
        QString title;
        QString watermark;
        std::vector<BoxDeco> decos;
        std::vector<Placement> items;
        // Sticky notes: drawn last, over everything else.
        std::vector<BoxDeco> overlayDecos;
        std::vector<Placement> overlayItems;
    };

    // A place in the laid-out text: position `pos` in the document of
    // placement `item` on page `page`. Items are numbered with a page's
    // items first, then its overlay items, which is also reading order, so
    // text points compare in reading order.
    struct TextPoint
    {
        int page = -1;
        int item = -1;
        int pos = 0;

        bool isValid() const { return page >= 0; }
        friend bool operator<(const TextPoint &a, const TextPoint &b)
        {
            if (a.page != b.page)
                return a.page < b.page;
            if (a.item != b.item)
                return a.item < b.item;
            return a.pos < b.pos;
        }
        friend bool operator==(const TextPoint &a, const TextPoint &b)
        {
            return a.page == b.page && a.item == b.item && a.pos == b.pos;
        }
    };

    // Text selected in the preview, between two points in either order.
    struct TextSelection
    {
        TextPoint anchor; // where the selection started
        TextPoint focus;  // where it ends now

        bool isEmpty() const { return !anchor.isValid() || anchor == focus; }
        TextPoint start() const { return focus < anchor ? focus : anchor; }
        TextPoint end() const { return focus < anchor ? anchor : focus; }
    };

    // Text marked in the preview besides the selection, such as find
    // matches: `background` behind it, and `text` (if valid) for its glyphs.
    struct Highlight
    {
        TextSelection range;
        QColor background;
        QColor text;
    };

    // The laid-out text as one string in reading order, the same text that
    // copying gives; `points[i]` is where `text[i]` comes from. Badges read
    // as their words and icons as their tokens (":a:").
    struct PlainText
    {
        QString text;
        std::vector<TextPoint> points;
    };

    // A lock taken in turn: the preview, painting again and again, can't
    // keep a build waiting for long, nor a build the preview. Not
    // recursive.
    class FairMutex
    {
        public:
        void lock()
        {
            std::unique_lock<std::mutex> guard(mutex_);
            const unsigned long long ticket = next_++;
            turn_.wait(guard, [&] { return serving_ == ticket; });
        }
        void unlock()
        {
            {
                const std::lock_guard<std::mutex> guard(mutex_);
                ++serving_;
            }
            turn_.notify_all();
        }

        private:
        std::mutex mutex_;
        std::condition_variable turn_;
        unsigned long long next_ = 0;
        unsigned long long serving_ = 0;
    };

    // Text laid out on one thread keeps using that thread's font engines
    // (and its FreeType library) when it is painted on another, and those
    // must not be used by two threads at once. Building a layout and every
    // use of a finished one take this lock, so a layout built in the
    // background can be painted while the next one is being built. The
    // building thread must also outlive every layout it built: its font
    // engines go away with it.
    FairMutex &textEngineMutex();

    // Result of laying out a scribe::Document onto pages.
    //
    // A document is a list of sections (bands). Each band has N equal columns
    // or a sidebar, and every column keeps its own cursor (page, y) so a tall
    // column can spill onto the next page independently. The band ends at the
    // lowest point reached by any column. Markdown becomes a QTextDocument
    // laid out as one tall column, which is sliced at line and block
    // boundaries and poured into the page column; boxes are sliced the same
    // way. In a sidebar band the sidebar is placed first and the rest of the
    // band flows around it, line by line, following the outline of its
    // images.
    class PageLayout
    {
        public:
        // Relative image paths ("![Map](Resources/map.png)") resolve against
        // `baseDir`, normally the markdown file's folder. May run on any
        // thread; returns nullptr if `cancel` is set before it's done.
        // `progress` hears how many of the document's sections are laid
        // out, between them (when the layout's text lock is free).
        ~PageLayout();

        using Progress = std::function<void(int done, int total)>;
        static std::shared_ptr<PageLayout>
        build(const Document &doc, const PageSpec &spec, const Theme &theme,
              const QString &baseDir = QString(),
              const std::atomic<bool> *cancel = nullptr,
              const Progress &progress = {});

        // Hands the layout's text documents to `thread` (call from the
        // thread that built it): a layout built in the background is
        // painted on the GUI thread.
        void moveToThread(QThread *thread);

        const PageSpec &spec() const { return spec_; }
        const std::vector<Page> &pages() const { return pages_; }

        // Paints page `index` with the origin at the page's top-left, in
        // points, marking `highlights` and then `selection` if given.
        void paintPage(QPainter &painter, int index,
                       const TextSelection *selection = nullptr,
                       const std::vector<Highlight> *highlights = nullptr) const;

        // The text position nearest to `point` (points, from the top-left
        // of page `page`); invalid if the page has no text.
        TextPoint hitTest(int page, const QPointF &point) const;
        // Start and end of the word at `point`.
        TextSelection wordAt(const TextPoint &point) const;
        // From the first character on the first page to the last one.
        TextSelection selectAll() const;
        // The selected text as plain text, in reading order.
        QString selectedText(const TextSelection &selection) const;
        // All of the text, for searching.
        PlainText plainText() const;
        // Where `point` is on its page, as tall as its line, in points
        // from the page's top-left; empty if it isn't laid out.
        QRectF caretRect(const TextPoint &point) const;
        // Where the heading of each table-of-contents entry (Document::toc,
        // by index) starts; invalid if it isn't on any page.
        const std::vector<TextPoint> &tocPoints() const { return tocPoints_; }

        private:
        struct ColCursor
        {
            int page = 0;
            qreal y = 0;
            // Negative margin the last block ended with, which collapses
            // with the gap before the next one (see trailingMargin()).
            qreal trailing = 0;
        };

        // What a slice of text covers on its page: the vertical spans of its
        // text lines (close ones merged), and its images.
        struct Coverage
        {
            std::vector<std::pair<qreal, qreal>> text;
            std::vector<std::pair<QRectF, QImage>> images;
        };

        PageLayout(const PageSpec &spec, const Theme &theme);
        void run(const Document &src);
        bool cancelled() const { return cancel_ && cancel_->load(); }
        void ensurePage(int index);
        void advance(ColCursor &cur);
        void placeColumn(const Column &column, qreal x, qreal w,
                         Role markdownRole, ColCursor &cur);
        void placeMarkdown(const QString &markdown, qreal x, qreal w, Role role,
                           ColCursor &cur);
        void placeBox(const Block &block, qreal x, qreal w, ColCursor &cur,
                      Role role);
        void addFeatBar(Page &page, const QRectF &rect) const;
        void placeSticky(const Block &block, int page);
        QString findImage(const QString &name);
        QColor imageTint(Role role) const;
        void prefetchImages(const Document &doc);
        void loadImages(QTextDocument &doc, qreal width, Role role);
        void addRule(Page &page, qreal x, qreal y, qreal w, Role role) const;
        Coverage coverage(const Placement &pl);
        void addObstacles(int page, const Placement &pl, qreal left,
                          qreal right);
        std::pair<qreal, qreal> freeSpan(int page, qreal left, qreal right,
                                         qreal top, qreal bottom) const;
        bool flowing(int page) const;
        std::pair<int, qreal> flowPosition(const ColCursor &cur,
                                           qreal offset) const;
        qreal wrap(QTextDocument *doc, const ColCursor &cur, qreal x,
                   qreal offset);
        std::pair<qreal, qreal>
        besideSidebar(ColCursor &cur, qreal x, qreal w,
                      const std::function<qreal(qreal)> &height);
        void discard(QTextDocument *doc);
        const Placement *placement(int page, int item) const;
        int itemCount(int page) const;
        std::pair<int, int> visibleRange(const Placement &pl) const;
        std::pair<int, int> rangeIn(const TextSelection &selection, int page,
                                    int item, const Placement &pl) const;
        void collectText(const TextSelection *range, QString &out,
                         std::vector<TextPoint> *points) const;
        void appendText(const Placement &pl, int page, int item, int from,
                        int to, QString &out,
                        std::vector<TextPoint> *points) const;
        QTextDocument *makeDoc(const QString &markdown, qreal width, Role role);
        void resolveToc(int entries);

        PageSpec spec_;
        Theme theme_;
        QString baseDir_;
        const std::atomic<bool> *cancel_ = nullptr;
        const Progress *progress_ = nullptr; // during build() only
        // Image files by the name used in the markdown (empty: not found).
        // The images themselves are in ImageCache, shared by all layouts.
        QHash<QString, QString> imagePaths_;
        // Room taken by the sidebar of the band being laid out, by page:
        // what the text beside it flows around, gaps included.
        std::map<int, std::vector<QRectF>> obstacles_;
        QString title_;
        QString watermark_;
        bool pageNumbers_ = false;

        // Text is laid out against this 720 dpi reference device (1 layout
        // unit == 0.1 pt, see kUnits in PageLayout.cpp), independent of the
        // screen. Declared before docs_ so it outlives them.
        QImage refDevice_;
        std::vector<std::unique_ptr<QTextDocument>> docs_;
        std::vector<Page> pages_;
        std::vector<TextPoint> tocPoints_;
    };
} // namespace scribe
