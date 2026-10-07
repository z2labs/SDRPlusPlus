#pragma once
#include <string>

namespace sourcemenu {
    void init();
    void draw(void* ctx);

    // Select a source by name and remember it (used by the Android USB auto-start)
    bool selectSourceByName(const std::string& name);
}