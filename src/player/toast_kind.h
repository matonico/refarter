#pragma once

namespace refarter::player {

// The icon of the toast over the picture; set where the toast is raised, never guessed from its text.
enum class ToastKind {
    Done,     // Finished: a tick.
    Progress, // Running: a ring that fills, with the percentage.
    Notice,   // A hint or a blocked action: an "i".
    Partial,  // Done with a catch, or input refused: a "!".
    Error,    // Failed: a cross.
};

} // namespace refarter::player
