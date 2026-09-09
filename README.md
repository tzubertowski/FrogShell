# FrogShell

FrogShell is TreeFrogUI's offline VitaShell-style file manager. It manages the
SD card only and has no networking code or network features.

Controls:

- A: enter a folder or open the action menu for a file
- B: go up / cancel
- Y: mark or unmark an item for multi-select
- X: open actions for the selected item
- SELECT: paste the clipboard into the current folder
- START: create a folder
- L/R: page through entries
- START + SELECT: exit FrogShell

The action menu supports copy, cut, paste, rename, delete, new folder, and file
information. Copy/cut operations work recursively for folders.

FrogShell reads the shared TreeFrogUI `skin/skin.txt` colors and the selected
`font=` entry from `frogui/settings.txt`. It searches the same `cubegm/fonts/`
and `frogui/fonts/` directories, with a built-in 8x8 fallback.

## On-screen keyboard

The keyboard uses a physical 10-column layout with a caps key (up-arrow
glyph), a SYM page, and a bottom modifier row (CTRL ALT SPACE DEL ENTER):

- A: type the highlighted key
- X: space
- Y: backspace
- START: run (terminal) / save (rename, new folder)
- ENTER: same as START
- Caps key: toggle lower/upper case (starts lowercase)
- SYM: toggle the symbols page

## Developer Mode (hidden)

Developer Mode adds an integrated terminal and the ability to run files,
without changing any normal-mode behavior while it is off. It can be enabled
two ways:

- hold **L1 + R1 + X + Y** for about 2 seconds (a confirmation message is
  shown; the same chord toggles it back off), or
- create a `frogui/developer.flag` file on the SD card (enables it on boot).

While enabled:

- the title shows `FROGSHELL DEV` and the action menu gains
  **Run**, **Open terminal here** and **Terminal** entries
- **Run** executes the selected file: ELF binaries directly, anything else
  (scripts, text) through `/bin/sh`, passing the path as a real argument so
  spaces are safe
- the **terminal** is a full-screen view backed by `/bin/sh` (busybox ash):
  commands, scripts and binaries run in their own process group with stdout
  and stderr captured, exit codes shown as `[exit N]` / `[terminated SIGn]`,
  with built-in `cd`, `clear` and `exit`, a bounded scrollback buffer and a
  command history
- running commands never block the interface; Y interrupts the running
  process group (L1/R1 scroll the buffer)
- a physical USB keyboard (via OTG, where the kernel exposes one) types
  directly in the terminal line: characters, backspace, enter, arrows for
  history, PgUp/PgDn for scrollback, Ctrl+C to interrupt

Every path that can spawn a process is gated on Developer Mode being enabled;
with it off, FrogShell behaves exactly as before.
