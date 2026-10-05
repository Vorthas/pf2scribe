cmake_minimum_required(VERSION 3.21)
project(pf2scribe VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)

add_executable(pf2scribe
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/Editor/CodeEditor.cpp
    src/Editor/CodeEditor.h
    src/Editor/FindBar.cpp
    src/Editor/FindBar.h
    src/Editor/ScribeHighlighter.cpp
    src/Editor/ScribeHighlighter.h
    src/Rendering/Fonts.cpp
    src/Rendering/Fonts.h
    src/Rendering/ImageCache.cpp
    src/Rendering/ImageCache.h
    src/Rendering/PageLayout.cpp
    src/Rendering/PageLayout.h
    src/Rendering/PdfOutline.cpp
    src/Rendering/PdfOutline.h
    src/Rendering/ScribeParser.cpp
    src/Rendering/ScribeParser.h
    src/Rendering/Theme.cpp
    src/Rendering/Theme.h
    src/Rendering/TraitBadge.cpp
    src/Rendering/TraitBadge.h
    src/VimEngine/VimEngine.cpp
    src/VimEngine/VimEngine.h
    src/VimEngine/VimEx.cpp
    src/Widgets/FileTree.cpp
    src/Widgets/FileTree.h
    src/Widgets/IconBrowser.cpp
    src/Widgets/IconBrowser.h
    src/Widgets/PreviewFindBar.cpp
    src/Widgets/PreviewFindBar.h
    src/Widgets/PreviewWidget.cpp
    src/Widgets/PreviewWidget.h
    src/Widgets/ShortcutsDialog.cpp
    src/Widgets/ShortcutsDialog.h
    src/Widgets/TocPanel.cpp
    src/Widgets/TocPanel.h
)

# Includes are written relative to src/ ("Rendering/PageLayout.h").
target_include_directories(pf2scribe PRIVATE src)
target_link_libraries(pf2scribe PRIVATE Qt6::Widgets)

# Fontconfig (Linux): to fix the metadata of installed fonts as well, see
# fixInstalledFonts() in src/Rendering/Fonts.cpp.
find_package(Fontconfig)
if(Fontconfig_FOUND)
    target_link_libraries(pf2scribe PRIVATE Fontconfig::Fontconfig)
    target_compile_definitions(pf2scribe PRIVATE SCRIBE_HAVE_FONTCONFIG)
endif()

# Fonts baked into the executable (available at runtime as :/fonts/...).
# By default only fonts/free (free fonts, shipped with their licenses), so a
# build can be shared. SCRIBE_EMBED_ALL_FONTS=ON embeds everything in fonts/,
# commercial fonts included: for personal builds only, never to be shared.
# Either way, the app also loads fonts/ from beside the executable or the
# current directory when it starts. Re-run cmake after adding or removing font
# files.
option(SCRIBE_EMBED_ALL_FONTS
       "Embed every font in fonts/, commercial ones too (personal builds only)" OFF)
if(SCRIBE_EMBED_ALL_FONTS)
    set(SCRIBE_FONT_DIR "${CMAKE_SOURCE_DIR}/fonts")
else()
    set(SCRIBE_FONT_DIR "${CMAKE_SOURCE_DIR}/fonts/free")
endif()
file(GLOB_RECURSE SCRIBE_FONT_FILES CONFIGURE_DEPENDS
    "${SCRIBE_FONT_DIR}/*.ttf" "${SCRIBE_FONT_DIR}/*.otf")
# fonts/Unused holds fonts set aside (the app doesn't load them either).
list(FILTER SCRIBE_FONT_FILES EXCLUDE REGEX "/[Uu]nused/")
if(SCRIBE_FONT_FILES)
    list(LENGTH SCRIBE_FONT_FILES SCRIBE_FONT_COUNT)
    if(SCRIBE_EMBED_ALL_FONTS)
        message(STATUS "Embedding all ${SCRIBE_FONT_COUNT} fonts in fonts/, "
                       "commercial ones included: don't share this build")
    else()
        message(STATUS "Embedding the ${SCRIBE_FONT_COUNT} free fonts in fonts/free "
                       "(-DSCRIBE_EMBED_ALL_FONTS=ON for all of fonts/)")
    endif()
    # Paths stay relative to fonts/, so :/fonts/free/... with the default.
    qt_add_resources(pf2scribe "scribe_fonts"
        PREFIX "/fonts"
        BASE "${CMAKE_SOURCE_DIR}/fonts"
        FILES ${SCRIBE_FONT_FILES})
endif()

if(MSVC)
    target_compile_options(pf2scribe PRIVATE /W4)
else()
    target_compile_options(pf2scribe PRIVATE -Wall -Wextra)
endif()
