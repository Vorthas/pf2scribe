// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#pragma once

#include <QColor>
#include <QHash>
#include <QImage>
#include <QSet>
#include <QString>
#include <QThreadPool>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace scribe
{
    // Image files decoded (and tinted) once, for every layout. A layout is
    // built again after each edit; without this it decoded every image of the
    // document again, and the two layouts alive during a swap each held their
    // own copies. A file is read again when its modification time or size
    // changes. Thread-safe: layouts are built on a worker thread, PDF exports
    // on the GUI thread.
    class ImageCache
    {
        public:
        static ImageCache &instance();

        // The image file at `path` (absolute), multiplied channel by channel
        // by `tint` when it's valid (".page img { mix-blend-mode: multiply }"
        // over a flat colour). Null if it can't be read.
        QImage image(const QString &path, const QColor &tint);

        // Starts decoding these (path, tint) on a few threads, in order, and
        // returns: a document seen for the first time is laid out while its
        // images decode. image() waits for one that is under way.
        void prefetch(const std::vector<std::pair<QString, QColor>> &wanted);

        // Per row of `image`, the first and last column whose alpha is at
        // least `opaque` (-1 when none is): the outline text wraps around.
        // Worked out once per image.
        struct Outline
        {
            std::vector<int> first;
            std::vector<int> last;
        };
        std::shared_ptr<const Outline> outline(const QImage &image, int opaque);

        // Forgets images no layout uses any more, least recently used first,
        // until those left take at most kIdleBytes: enough to come back to a
        // document without decoding its images again, but no more.
        void trim();
        static constexpr qint64 kIdleBytes = 128LL * 1024 * 1024;

        private:
        ImageCache();

        struct Entry
        {
            QImage image;
            qint64 modified = 0; // file's, ms since the epoch
            qint64 size = 0;     // file's, bytes
            quint64 used = 0;    // clock_ at the last lookup
        };
        static QString keyOf(const QString &path, const QColor &tint);

        std::mutex mutex_;
        QHash<QString, Entry> entries_;
        QSet<QString> decoding_;           // keys being decoded, by any thread
        std::condition_variable decoded_;  // one of them is done
        // By QImage::cacheKey() of the image (and alpha threshold).
        QHash<std::pair<qint64, int>, std::shared_ptr<const Outline>> outlines_;
        quint64 clock_ = 0;
        QThreadPool pool_; // for prefetch()
    };
} // namespace scribe
