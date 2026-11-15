#pragma once

// After a freeze report is written: "It looks like your game is frozen. Quit game and open freeze
// report?" A window of its own, on its own thread, so it never holds up a capture. It goes away by
// itself once the game is back or has exited, and only a click on its button quits the game.

#include <windows.h>

#include <filesystem>
#include <functional>

namespace dd::helper
{
    // True while the game still looks stuck; asked a few times a second while the prompt is up.
    using StillStuck = std::function<bool()>;

    // image: an optional picture shown above the question (a PNG next to the helper).
    void ShowFreezePrompt(DWORD pid, const std::filesystem::path& report, const std::filesystem::path& image, StillStuck stillStuck);

    // The same on the calling thread; returns once the prompt has closed.
    void RunFreezePrompt(DWORD pid, const std::filesystem::path& report, const std::filesystem::path& image,
                         const StillStuck& stillStuck);

    // Draws the prompt into a PNG without showing it (to check the layout and the picture).
    void PreviewFreezePrompt(const std::filesystem::path& image, const std::filesystem::path& out);
}
