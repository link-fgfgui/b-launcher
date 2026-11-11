#pragma once

#include "app/downloads/Addons.h"

#include <utility>
#include <vector>

// The list the mod-downloader browser extension posts to 127.0.0.1:18801 while the mod page is open, shown in place
// of its search results. An entry is {"p": "mr" | "cf", "id": project id or null, "slug": slug, "file": file or ""};
// the file is not used yet.
namespace PluginFeed {

    // Called as the mod page is drawn: the port is open while it is, and closed once it stops being drawn.
    void shown(bool visible);
    void poll();
    void shutdown();

    // A list was received and has not been cleared: it is being resolved, or is ready.
    bool active();
    bool loading();
    const std::vector<std::pair<Addons::Source, Addons::Hit>>& items();
    // A new list was received, so the page knows the list it shows is no longer the one it has.
    unsigned revision();
    // Back to the search results, stopping whatever is still being resolved.
    void clear();
}
