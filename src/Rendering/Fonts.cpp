// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorthas

#include "Rendering/Fonts.h"

#include "Rendering/Theme.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QSet>
#include <QString>

#include <algorithm>
#include <optional>
#include <utility>

#ifdef SCRIBE_HAVE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#endif

namespace scribe
{
    namespace
    {
        // Offsets into the sfnt tables touched below.
        constexpr int kOs2WeightClass = 4;
        constexpr int kOs2FsSelection = 62;
        constexpr int kHeadMacStyle = 44;

        constexpr quint16 kFsItalic = 1 << 0;
        constexpr quint16 kFsBold = 1 << 5;
        constexpr quint16 kFsRegular = 1 << 6;
        constexpr quint16 kMacBold = 1 << 0;
        constexpr quint16 kMacItalic = 1 << 1;

        struct Table
        {
            int offset = 0;
            int length = 0;
        };

        quint16 read16(const QByteArray &data, int at)
        {
            return static_cast<quint16>((static_cast<uchar>(data[at]) << 8) |
                                        static_cast<uchar>(data[at + 1]));
        }

        quint32 read32(const QByteArray &data, int at)
        {
            return (quint32(read16(data, at)) << 16) | read16(data, at + 2);
        }

        void write16(QByteArray &data, int at, quint16 value)
        {
            data[at] = static_cast<char>(value >> 8);
            data[at + 1] = static_cast<char>(value & 0xff);
        }

        std::optional<Table> findTable(const QByteArray &data, const char *tag)
        {
            if (data.size() < 12)
            {
                return std::nullopt;
            }
            const int count = read16(data, 4);
            for (int i = 0; i < count; ++i)
            {
                const int record = 12 + 16 * i;
                if (record + 16 > data.size())
                {
                    break;
                }
                if (data.mid(record, 4) != QByteArray(tag, 4))
                {
                    continue;
                }
                const qint64 offset = read32(data, record + 8);
                const qint64 length = read32(data, record + 12);
                if (offset + length > data.size())
                {
                    return std::nullopt;
                }
                return Table{static_cast<int>(offset),
                             static_cast<int>(length)};
            }
            return std::nullopt;
        }

        // The typographic subfamily (name 17) if there is one, else the
        // subfamily (name 2).
        QString styleName(const QByteArray &data)
        {
            const auto name = findTable(data, "name");
            if (!name || name->length < 6)
            {
                return {};
            }
            const int count = read16(data, name->offset + 2);
            const int strings = name->offset + read16(data, name->offset + 4);

            QString found[2]; // [0]: name 17, [1]: name 2
            for (int i = 0; i < count; ++i)
            {
                const int record = name->offset + 6 + 12 * i;
                if (record + 12 > name->offset + name->length)
                {
                    break;
                }
                const quint16 platform = read16(data, record);
                const quint16 id = read16(data, record + 6);
                const int length = read16(data, record + 8);
                const int at = strings + read16(data, record + 10);
                if ((id != 17 && id != 2) || at + length > data.size())
                {
                    continue;
                }

                QString value;
                if (platform == 0 || platform == 3) // UTF-16BE
                {
                    for (int k = 0; k + 1 < length; k += 2)
                    {
                        value += QChar(read16(data, at + k));
                    }
                }
                else if (platform == 1) // Mac Roman; style names are ASCII
                {
                    value = QString::fromLatin1(data.mid(at, length));
                }

                QString &slot = found[id == 17 ? 0 : 1];
                if (slot.isEmpty())
                {
                    slot = value;
                }
            }
            return found[0].isEmpty() ? found[1] : found[0];
        }

        // CSS-style weight from a style name. Longer names are tested first,
        // so "SemiBold" isn't read as "Bold" or "UltraLight" as "Ultra".
        int weightFromStyle(QString style)
        {
            style = style.toLower().remove(' ').remove('-');
            const std::pair<const char *, int> weights[] = {
                {"hairline", 100},   {"thin", 100},      {"extralight", 200},
                {"ultralight", 200}, {"semibold", 600},  {"demibold", 600},
                {"extrabold", 800},  {"ultrabold", 800}, {"black", 900},
                {"heavy", 900},      {"ultra", 900},     {"light", 300},
                {"news", 350},       {"book", 350},      {"medium", 500},
                {"bold", 700}};
            for (const auto &[key, weight] : weights)
            {
                if (style.contains(QLatin1String(key)))
                {
                    return weight;
                }
            }
            return 400;
        }
    } // namespace

    QByteArray fixStyleMetadata(const QByteArray &fontData)
    {
        const auto os2 = findTable(fontData, "OS/2");
        const auto head = findTable(fontData, "head");
        if (!os2 || os2->length < kOs2FsSelection + 2 || !head ||
            head->length < kHeadMacStyle + 2)
        {
            return fontData;
        }

        const QString style = styleName(fontData);
        const int weight = weightFromStyle(style);
        const bool italic =
            style.contains(QLatin1String("italic"), Qt::CaseInsensitive) ||
            style.contains(QLatin1String("oblique"), Qt::CaseInsensitive);

        const int weightAt = os2->offset + kOs2WeightClass;
        const int fsAt = os2->offset + kOs2FsSelection;
        const int macAt = head->offset + kHeadMacStyle;
        const quint16 oldWeight = read16(fontData, weightAt);
        const quint16 oldFs = read16(fontData, fsAt);

        // Only step in when the font claims to be plain Regular but its name
        // says otherwise; well-made fonts are left alone.
        const bool fixWeight = oldWeight == 400 && weight != 400;
        const bool fixItalic = italic && !(oldFs & kFsItalic);
        if (!fixWeight && !fixItalic)
        {
            return fontData;
        }

        QByteArray data = fontData;
        quint16 fs = oldFs;
        quint16 mac = read16(data, macAt);
        if (fixWeight)
        {
            write16(data, weightAt, static_cast<quint16>(weight));
            if (weight >= 700)
            {
                fs = (fs | kFsBold) & ~kFsRegular;
                mac |= kMacBold;
            }
        }
        if (fixItalic)
        {
            fs = (fs | kFsItalic) & ~kFsRegular;
            mac |= kMacItalic;
        }
        if (fs & (kFsBold | kFsItalic))
        {
            fs &= ~kFsRegular;
        }
        write16(data, fsAt, fs);
        write16(data, macAt, mac);
        // Table checksums are left stale; FreeType doesn't verify them.
        return data;
    }

    namespace
    {
        // Installed fonts that need fixStyleMetadata(), found by
        // hideBrokenInstalledFonts().
        QStringList &brokenInstalledFonts()
        {
            static QStringList files;
            return files;
        }

        // Whether the font file at `path` lacks the italic (or oblique) flag
        // that FreeType, and so Qt, go by. Without it Qt slants the glyphs
        // itself, so an italic face comes out slanted twice.
        bool italicFlagMissing(const QString &path)
        {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
            {
                return false;
            }
            const QByteArray data = file.readAll();
            const auto os2 = findTable(data, "OS/2");
            if (!os2 || os2->length < kOs2FsSelection + 2)
            {
                return false;
            }
            constexpr quint16 kFsOblique = 1 << 9;
            return !(read16(data, os2->offset + kOs2FsSelection) &
                     (kFsItalic | kFsOblique));
        }

        // Escapes `text` for a fontconfig configuration (XML).
        QByteArray xmlEscaped(const QString &text)
        {
            return text.toHtmlEscaped().toUtf8();
        }
    } // namespace

    void hideBrokenInstalledFonts()
    {
#ifdef SCRIBE_HAVE_FONTCONFIG
        if (!FcInit())
        {
            return;
        }
        // The families the themes ask for.
        QSet<QString> families;
        for (const Theme::Preset &preset : Theme::presets())
        {
            const Theme theme = preset.make();
            for (const QStringList *list :
                 {&theme.bodyFamilies, &theme.goodFamilies,
                  &theme.goodCondensedFamilies, &theme.tarocaFamilies,
                  &theme.ginFamilies, &theme.sansFamilies, &theme.timesFamilies,
                  &theme.iconFamilies})
            {
                for (const QString &family : *list)
                {
                    families.insert(family);
                }
            }
        }

        // Their files whose weight or slant, as fontconfig reads them,
        // disagrees with their style name, where that makes styles of one
        // family indistinguishable: FF Good Pro's Regular, News, Bold, Black
        // and Ultra all claim to be regular weight. A family whose one
        // regular-weight style is called "Book" is fine as it is. And italic
        // styles whose files lack the italic flag (FF Good Pro's again):
        // fontconfig reads them as italic from their names, but Qt draws
        // them slanted a second time.
        QStringList &broken = brokenInstalledFonts();
        FcObjectSet *wanted =
            FcObjectSetBuild(FC_FILE, FC_STYLE, FC_WEIGHT, FC_SLANT, FC_WIDTH, nullptr);
        for (const QString &family : std::as_const(families))
        {
            FcPattern *pattern = FcPatternCreate();
            const QByteArray name = family.toUtf8();
            FcPatternAddString(pattern, FC_FAMILY,
                               reinterpret_cast<const FcChar8 *>(name.constData()));
            FcFontSet *set = FcFontList(nullptr, pattern, wanted);
            struct Face
            {
                QString file;
                QString style;
                int weight;
                int slant;
                int width;
            };
            std::vector<Face> faces;
            for (int i = 0; set && i < set->nfont; ++i)
            {
                FcChar8 *file = nullptr;
                FcChar8 *style = nullptr;
                int weight = 0;
                int slant = FC_SLANT_ROMAN;
                int width = FC_WIDTH_NORMAL;
                if (FcPatternGetString(set->fonts[i], FC_FILE, 0, &file) != FcResultMatch ||
                    FcPatternGetString(set->fonts[i], FC_STYLE, 0, &style) != FcResultMatch)
                {
                    continue;
                }
                FcPatternGetInteger(set->fonts[i], FC_WEIGHT, 0, &weight);
                FcPatternGetInteger(set->fonts[i], FC_SLANT, 0, &slant);
                FcPatternGetInteger(set->fonts[i], FC_WIDTH, 0, &width);
                faces.push_back({QString::fromUtf8(reinterpret_cast<const char *>(file)),
                                 QString::fromUtf8(reinterpret_cast<const char *>(style))
                                     .toLower(),
                                 weight, slant, width});
            }
            for (const Face &face : faces)
            {
                const bool italicName = face.style.contains(QLatin1String("italic")) ||
                                        face.style.contains(QLatin1String("oblique"));
                const bool wrong =
                    (face.weight == FC_WEIGHT_REGULAR &&
                     weightFromStyle(face.style) != 400) ||
                    (italicName && face.slant == FC_SLANT_ROMAN);
                // Another style claiming the same weight, slant and width?
                const bool clash = std::any_of(
                    faces.begin(), faces.end(), [&face](const Face &other) {
                        return other.style != face.style &&
                               other.weight == face.weight &&
                               other.slant == face.slant && other.width == face.width;
                    });
                if ((wrong && clash) ||
                    (italicName && italicFlagMissing(face.file)))
                {
                    broken << face.file;
                }
            }
            if (set)
            {
                FcFontSetDestroy(set);
            }
            FcPatternDestroy(pattern);
        }
        FcObjectSetDestroy(wanted);
        broken.removeDuplicates();
        if (broken.isEmpty())
        {
            return;
        }

        // A configuration like the system's that leaves them out, for this
        // program only; loadFonts() registers fixed copies instead.
        QByteArray xml = "<?xml version=\"1.0\"?>\n"
                         "<!DOCTYPE fontconfig SYSTEM \"urn:fontconfig:fonts.dtd\">\n"
                         "<fontconfig><selectfont><rejectfont>\n";
        for (const QString &file : std::as_const(broken))
        {
            xml += "<glob>" + xmlEscaped(file) + "</glob>\n";
        }
        xml += "</rejectfont></selectfont></fontconfig>\n";
        FcConfig *config = FcInitLoadConfig();
        if (!config)
        {
            return;
        }
        if (!FcConfigParseAndLoadFromMemory(
                config, reinterpret_cast<const FcChar8 *>(xml.constData()), FcTrue) ||
            !FcConfigBuildFonts(config) || !FcConfigSetCurrent(config))
        {
            FcConfigDestroy(config);
            broken.clear();
            return;
        }
        FcConfigDestroy(config); // the current configuration keeps it
#endif
    }

    void loadFonts()
    {
        QSet<QString> loaded; // files from the folders below
        const QString appDir = QCoreApplication::applicationDirPath();
        const QStringList dirs = {
            QStringLiteral(":/fonts"), appDir + QStringLiteral("/fonts"),
            QDir::currentPath() + QStringLiteral("/fonts")};
        const QStringList filters = {
            QStringLiteral("*.ttf"), QStringLiteral("*.otf"),
            QStringLiteral("*.TTF"), QStringLiteral("*.OTF")};

        for (const QString &dir : dirs)
        {
            if (!QDir(dir).exists())
            {
                continue;
            }
            QDirIterator it(dir, filters, QDir::Files,
                            QDirIterator::Subdirectories);
            while (it.hasNext())
            {
                const QString path = it.next();
                // fonts/Unused holds fonts set aside, not to be loaded.
                if (QDir(dir).relativeFilePath(path)
                        .split(QLatin1Char('/'))
                        .contains(QLatin1String("Unused"), Qt::CaseInsensitive))
                {
                    continue;
                }
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly))
                {
                    qWarning() << "Could not read font" << path;
                    continue;
                }
                const QByteArray original = file.readAll();
                const QByteArray data = fixStyleMetadata(original);
                loaded.insert(QFileInfo(path).canonicalFilePath());
                const int id = QFontDatabase::addApplicationFontFromData(data);
                if (id < 0)
                {
                    qWarning() << "Could not load font" << path;
                    continue;
                }
                qInfo() << "Loaded font"
                        << QFontDatabase::applicationFontFamilies(id) << "from"
                        << path
                        << (data != original ? "(style metadata fixed)" : "");
            }
        }
        // The installed fonts hideBrokenInstalledFonts() left out, fixed.
        for (const QString &path : std::as_const(brokenInstalledFonts()))
        {
            if (loaded.contains(QFileInfo(path).canonicalFilePath()))
            {
                continue; // the same file is in fonts/
            }
            QFile file(path);
            if (file.open(QIODevice::ReadOnly) &&
                QFontDatabase::addApplicationFontFromData(
                    fixStyleMetadata(file.readAll())) >= 0)
            {
                qInfo() << "Loaded font" << path << "(installed; style metadata fixed)";
            }
        }
    }
} // namespace scribe
