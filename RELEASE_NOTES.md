# CoreAutoclicker 1.0

## Release Files

- `CoreAutoclicker.exe`

## Verification

- Built with `clang-cl`.
- Warning-clean with `clang-cl /Wall /WX`.
- Static-analysis clean with `cl /analyze /W4 /WX`.
- System DLL dependencies only: `KERNEL32.dll`, `USER32.dll`, `GDI32.dll`, and `COMCTL32.dll` for the main app.
