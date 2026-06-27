# CoreAutoclicker

Native Windows Autoclicker Written in C.

## Files

- `CoreAutoclicker.exe` - the autoclicker.

## Controls

- Left click toggle: `F6`
- Right click toggle: `F7`
- Emergency close: `F9`
- CPS range: `1` to `1000`
- Universal SendInput: enabled by default for real applications.
- Low CPU mode: why would you want it off. It stays.

The left and right click toggle hotkeys can be changed in the app. `F9` is fixed as the emergency close key.

## Modes

Universal SendInput is the compatibility mode and should be left enabled for normal applications. The unchecked mode posts window messages directly to the window under the cursor; it is useful for benchmarks, but many real applications ignore posted messages.

The app is capped at `1000 CPS` and drops backlog.

The clicker uses QPC-based period math, a high-resolution waitable timer when available, and a bounded final spin to reduce scheduler wake-up error. If Windows ever  wakes the thread too late, the next click is scheduled one full period later so the app doesn't compress missed clicks into a burst.

## Build

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```
