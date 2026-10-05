// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#include "Rendering/ImageCache.h"

#include <QDateTime>
#include <QFileInfo>
#include <QImageReader>
#include <QSet>
#include <QThread>
#include <QTimeZone>

#include <QCoreApplication>

#include <algorithm>

namespace scribe
{
    namespace
    {
        // `image` multiplied by `tint`, channel by channel, in place; alpha
        // is kept.
        void tint(QImage &image, const QColor &tint)
        {
            image.convertTo(QImage::Format_ARGB32);
            const int r = tint.red(), g = tint.green(), b = tint.blue();
            for (int y = 0; y < image.height(); ++y)
            {
                auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
                for (int x = 0; x < image.width(); ++x)
                {
                    const QRgb c = line[x];
                    line[x] = qRgba(qRed(c) * r / 255, qGreen(c) * g / 255,
                                    qBlue(c) * b / 255, qAlpha(c));
                }
            }
        }
    } // namespace

    ImageCache &ImageCache::instance()
    {
        static ImageCache cache;
        return cache;
    }

    ImageCache::ImageCache()
    {
        // Decoding is independent per file; half the cores, at most 8.
        pool_.setMaxThreadCount(std::clamp(QThread::idealThreadCount() / 2, 1, 8));
        // Decodes still running at exit finish before Qt's image plugins go.
        qAddPostRoutine([] { instance().pool_.waitForDone(); });
    }

    QString ImageCache::keyOf(const QString &path, const QColor &tint)
    {
        return tint.isValid() ? path + QLatin1Char('#') + tint.name() : path;
    }

    QImage ImageCache::image(const QString &path, const QColor &tint)
    {
        const QFileInfo info(path);
        if (!info.exists())
        {
            return {};
        }
        // In UTC: only compared, and local time would go through the C
        // library's time zone code on every lookup.
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        const qint64 modified = info.lastModified(QTimeZone::UTC).toMSecsSinceEpoch();
#else
        const qint64 modified = info.lastModified().toMSecsSinceEpoch();
#endif
        const qint64 size = info.size();
        const QString key = keyOf(path, tint);
        {
            std::unique_lock lock(mutex_);
            // Another thread may have it under way.
            decoded_.wait(lock, [&] { return !decoding_.contains(key); });
            const auto it = entries_.find(key);
            if (it != entries_.end() && it->modified == modified && it->size == size)
            {
                it->used = ++clock_;
                return it->image;
            }
            decoding_.insert(key);
        }

        // Decoded without the lock, so several threads can decode at once.
        QImageReader reader(path);
        reader.setAutoTransform(true);
        QImage image = reader.read();
        if (!image.isNull() && tint.isValid())
        {
            scribe::tint(image, tint);
        }

        const std::lock_guard lock(mutex_);
        Entry &entry = entries_[key];
        entry.image = image;
        entry.modified = modified;
        entry.size = size;
        entry.used = ++clock_;
        decoding_.remove(key);
        decoded_.notify_all();
        return entry.image;
    }

    void ImageCache::prefetch(const std::vector<std::pair<QString, QColor>> &wanted)
    {
        QSet<QString> seen;
        const std::lock_guard lock(mutex_);
        for (const auto &[path, tint] : wanted)
        {
            const QString key = keyOf(path, tint);
            if (entries_.contains(key) || decoding_.contains(key) || seen.contains(key))
            {
                continue; // image() checks that a cached one is current
            }
            seen.insert(key);
            pool_.start([this, path, tint] { image(path, tint); });
        }
    }

    std::shared_ptr<const ImageCache::Outline>
    ImageCache::outline(const QImage &image, int opaque)
    {
        const std::pair<qint64, int> key(image.cacheKey(), opaque);
        {
            const std::lock_guard lock(mutex_);
            if (const auto it = outlines_.constFind(key); it != outlines_.cend())
            {
                return *it;
            }
        }

        auto outline = std::make_shared<Outline>();
        const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
        const auto rows = static_cast<size_t>(argb.height());
        outline->first.assign(rows, -1);
        outline->last.assign(rows, -1);
        for (int y = 0; y < argb.height(); ++y)
        {
            const auto *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
            for (int x = 0; x < argb.width(); ++x)
            {
                if (qAlpha(line[x]) >= opaque)
                {
                    if (outline->first[static_cast<size_t>(y)] < 0)
                    {
                        outline->first[static_cast<size_t>(y)] = x;
                    }
                    outline->last[static_cast<size_t>(y)] = x;
                }
            }
        }

        const std::lock_guard lock(mutex_);
        return *outlines_.insert(key, std::move(outline));
    }

    void ImageCache::trim()
    {
        const std::lock_guard lock(mutex_);
        // Idle: no layout holds a copy (QImage shares its pixels), so only
        // this cache keeps them.
        std::vector<std::pair<quint64, QString>> idle; // last use, key
        qint64 idleBytes = 0;
        for (auto it = entries_.begin(); it != entries_.end();)
        {
            if (it->image.isNull())
            {
                it = entries_.erase(it); // cheap to look for again
                continue;
            }
            if (it->image.isDetached())
            {
                idle.emplace_back(it->used, it.key());
                idleBytes += it->image.sizeInBytes();
            }
            ++it;
        }
        std::sort(idle.begin(), idle.end());
        for (const auto &[used, key] : idle)
        {
            if (idleBytes <= kIdleBytes)
            {
                break;
            }
            idleBytes -= entries_.value(key).image.sizeInBytes();
            entries_.remove(key);
        }

        // Outlines of images that are gone.
        QSet<qint64> kept;
        for (const Entry &entry : std::as_const(entries_))
        {
            kept.insert(entry.image.cacheKey());
        }
        for (auto it = outlines_.begin(); it != outlines_.end();)
        {
            it = kept.contains(it.key().first) ? std::next(it) : outlines_.erase(it);
        }
    }
} // namespace scribe
