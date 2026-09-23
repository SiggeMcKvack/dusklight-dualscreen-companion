#pragma once

// On-disk store for converted guides.
//
// Layout under the mod's data directory (see set_guides_root):
//
//   guides/
//     index.json          catalogue: id, title, source, section list
//     <id>.guide          one converted document (see guide_doc.hpp)
//     images/<id>/...     converted images for that document
//     import/             DROP ZONE: user-saved .html goes here
//     import/done/        sources that have been converted, kept for re-import
//
// The import folder is what makes sites that block automated requests usable:
// pages are saved by a real browser and dropped in. Nothing here talks to the
// network.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "dusk/guide/guide_doc.hpp"

namespace dusk::guide {

// One catalogue entry. Section ids/titles are duplicated here so the reader
// and the progress mapper can answer "which section covers this stage?"
// without loading and parsing every document.
struct IndexSection {
    std::string id;
    std::string title;
};

struct IndexEntry {
    std::string id;        // slug, also the .guide filename stem
    std::string title;
    std::string sourceUrl;
    std::vector<IndexSection> sections;
};

struct Index {
    std::vector<IndexEntry> entries;
    // Converter revision that produced these entries; a lower value than the
    // build's triggers a one-off re-convert from import/done.
    int converter = 0;
};

// Paths. guides_root() creates nothing; ensure_dirs() creates the tree.
// set_guides_root() must be called (with the mod's data dir + "guides") before any other call.
void set_guides_root(const std::filesystem::path& root);
std::filesystem::path guides_root();
std::filesystem::path import_dir();
std::filesystem::path images_dir(const std::string& id);
bool ensure_dirs();

// Index I/O. A missing or corrupt index reads back as an empty one rather
// than an error — a bad index must never block the reader from starting.
Index load_index();
bool save_index(const Index& index);

// Document I/O. save_document() writes through a temp file and renames, so a
// crash mid-write cannot leave a half-parsed guide behind.
bool save_document(const std::string& id, const Document& doc);
std::optional<Document> load_document(const std::string& id);

// Supplies the bytes for one image. Returning false (or leaving the buffer
// empty) means "not available" and the reader falls back to alt text.
//
// A callback so this file stays free of network dependencies: the import path
// reads from the browser's saved _files folder, with an optional fallback.
using ImageSource = std::function<bool(const std::string& url, std::string& outBytes)>;

// Converts raw HTML into the store and returns the id it was filed under, or
// empty on failure.
// `fallbackName` seeds the id when the page has no usable title.
// `images` is optional; without it the guide is text-only.
std::string import_html_content(const std::string& html, const std::string& sourceUrl,
    const std::string& fallbackName, const ImageSource& images = {});

// Same, reading the markup from a file on disk.
// `netFallback` is tried for images the saved page did not bring with it —
// a page saved by the in-app browser has no _files folder, so its pictures
// only exist as URLs.
std::string import_html_file(const std::filesystem::path& file, const std::string& sourceUrl,
    const ImageSource& netFallback = {});

// Leading "13.1" of a numbered section title, empty when it has none. The one
// definition of what "numbered" means: the store names that section's images
// with it, and the reader orders chapters by it, and those two must not drift.
std::string section_number(const std::string& title);

// True when the stored catalogue was written by an older converter and the
// archived sources should be run through it again. Exposed because the reader
// has to kick that off: a version bump otherwise only reaches installs with an
// empty store, i.e. exactly the ones with nothing to re-convert.
bool index_needs_reconvert();

// Converts every *.html / *.htm sitting in import/, moving each source into
// import/done/ afterwards so it is not converted twice. Returns the number
// imported. Safe to call on every open.
int scan_import_folder(const ImageSource& netFallback = {});

}  // namespace dusk::guide
