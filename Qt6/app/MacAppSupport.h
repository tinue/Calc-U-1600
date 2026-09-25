#pragma once

#ifdef __APPLE__
// Turns off AppKit's window restoration for this app (registers
// ApplePersistenceIgnoreState = YES). Call before QApplication exists.
//
// The app restores nothing itself, and AppKit's own "reopen windows?"
// prompt -- shown after a run was killed -- is fatal here: it arrives with
// the launch's open-application event, which the startup default preset's
// synchronous load delivers while it pumps events (user input excluded).
// The prompt's modal loop then waits for a click that can't arrive, and the
// load waits for the prompt: a beachball.
void macDisableWindowRestoration();
#endif
