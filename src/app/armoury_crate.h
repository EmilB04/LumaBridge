// Handing the lights back to Armoury Crate.
//
// The Aura USB controller has no "give control back" command, and ASUS's lighting service
// doesn't re-apply its effect on its own. What does work (seen on a B550-F) is Armoury
// Crate's app starting: it re-applies the current Aura Sync effect. So handing back means
// "stop sending colors, then start Armoury Crate".
#pragma once

#include <string>

namespace luma::app {

// Finds Armoury Crate in Windows' app list (Store or classic install) and starts it.
// Returns false (and logs why) when it isn't installed or couldn't be started.
bool LaunchArmouryCrate();

}  // namespace luma::app
