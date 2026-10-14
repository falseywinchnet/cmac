# Requests to GUI.Forms

Reusable capabilities cmac needs that belong in GUI.Forms, not here. Each is
coordinated with the GUI.Forms owner and stays listed until it lands in a
pinned revision.

| Request | Why cmac needs it | Status |
|---|---|---|
| A ribbon control (tab stack and command groups, as the default theme already specifies for ribbon buttons) | cmac's commands; Plan Paint carries its own today | waiting on decision D1 |
| A host call to show an item in the platform file manager (Finder on macOS, Explorer on Windows, the desktop's file manager on Linux) | "Show in Finder" for the selection | not yet raised; cmac uses its own `src/platform/macos/` adapter meanwhile |
