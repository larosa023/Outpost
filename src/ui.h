// Outpost - the window
#pragma once
#include "common.h"
#include "selfupdate.h"

struct UiStart {
    selfupdate::AfterUpdate after;
    bool tray = false;          // start in the tray (Start with Windows)
    bool boot = false;          // also start the servers marked "start on boot"
    std::wstring restoreState;  // Hangar live update: state file to restore
};
HWND UiCreate(HINSTANCE inst, int show, const UiStart& start);
