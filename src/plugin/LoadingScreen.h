#pragma once

#include <cstdint>

namespace dd::plugin
{
    // Finds the game's LoadingMenu class through its RTTI in the executable and hooks two of
    // its virtual functions: AdvanceMovie (runs every frame the loading screen is up) and the
    // destructor. Uses only the class name and the vtable slot, both stable across runtimes.
    bool InstallLoadingScreenWatch();

    // Called by the heartbeat: a loading screen that stopped advancing while the game keeps
    // running tasks has closed.
    void CheckLoadingScreenClosed(std::uint64_t now);

    struct VtableSearch
    {
        void** vtable = nullptr;
        int    typeDescriptors = 0;
        int    locators = 0;
    };

    // RTTI search in a loaded image: ".?AVName@@" -> type descriptor -> complete object
    // locator (offset 0) -> vtable. Exposed for the tests.
    VtableSearch FindVtable(const std::uint8_t* imageBase, const char* mangledName);
}
