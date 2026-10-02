// Handing the lights back to Armoury Crate.
//
// The Aura USB controller has no "give control back" command, and neither Armoury Crate's
// services nor its app re-apply the effect on their own. Restarting the controller's USB
// device does: it reloads the effect Armoury Crate saved in it (confirmed on a B550-F with
// `pnputil /restart-device`). That needs admin rights, so a scheduled task set up once by
// scripts/Install-HandbackTask.ps1 does it; without the task, LumaBridge falls back to
// opening Armoury Crate (where one click on an effect restores it).
#pragma once

#include <string>

namespace luma::app {

// Finds Armoury Crate in Windows' app list (Store or classic install) and starts it.
// Returns false (and logs why) when it isn't installed or couldn't be started.
bool LaunchArmouryCrate();

// Is the silent hand-back task installed? (Runs `schtasks /query`, ~100 ms.)
bool HandbackTaskInstalled();

// Hands the lights back: runs the hand-back task if installed, else opens Armoury Crate.
void HandBackLighting();

// Is Armoury Crate's window open (visible)? Then the user is working in it, and its lighting
// service shouldn't be kept paused.
bool ArmouryCrateWindowOpen();

}  // namespace luma::app
