# vsc — VS Code look & keys: ~0.4 MB real memory (PSS)

A single-file C++17 terminal editor laid out like VS Code: an **Explorer sidebar** with the folder's file
tree on the left, the open files **tiled** on the right, and the status bar (with git branch) at the
bottom, with VS Code keybindings. The theme is all black, with thin boundary lines between the panes in btop's default box colours (green sidebar edge, purple above the status bar, grey under the explorer header) (the focused tile is boxed in blue and the other tiles in grey, the selected explorer row is outlined in blue rather than filled, with its name in a lighter blue, and the other tiled files' rows are outlined in grey) and VS Code Dark+ text colours. No ncurses, no GUI toolkit, no dependencies. PDFs open in a tile, rendered
by Okular's engine, in terminals with kitty graphics (Ghostty, kitty, [foot with a small patch](https://github.com/Person-26/foot)),
and in Okular elsewhere (stock foot).

```
make            # ./vsc and ./vsc-pdf
make vsc        # just the editor (~225 KB; libstdc++ linked in, libc shared; needs: sudo dnf install libstdc++-static)
make vsc-pdf    # the PDF renderer (Qt + Okular's core library; needs: sudo dnf install okular-devel kf6-kcoreaddons-devel kf6-kconfig-devel kf6-kxmlgui-devel)
make static     # fully static; lower RSS figure but higher real memory. needs: sudo dnf install glibc-static libstdc++-static
make install    # -> ~/.local/bin/vsc and ~/.local/bin/vsc-pdf
vsc                  # open the current folder (like `code .`)
vsc ~/project        # open a folder
vsc ~/project a.cpp  # open a folder and a file
vsc . a.cpp b.cpp    # open two files side by side
```

## Tiling

Opening a file normally replaces the file in the focused tile (asking first if it has unsaved changes).
To open it in a new tile instead, Alt+click (or Ctrl+click) it in the Explorer, press Shift+Enter or
Ctrl+Enter on it, or accept the Ctrl+O prompt with Shift+Enter. Shift+click works too in terminals that pass it
on, but foot keeps Shift+click for its own text selection (`selection-override-modifiers`). Tiles fill the editor area in a grid (2 side by side, 3–4 in
2×2, 5–6 in 3×2, …; a short last row's tiles share its width). With more than one tile each is boxed with
its file name (and `●` when unsaved) in the top edge: blue round the focused tile, grey round the rest. With
a single tile there is no box and its name is in the window title. Click a tile, or use Ctrl+PgUp /
Ctrl+PgDn or Alt+1…9, to focus it; click the `×` in a tile's top-right corner, or press Ctrl+W, to close it (twice
if it has unsaved changes). Opening a file that is already in a
tile just focuses that tile.

Shift/Ctrl+Enter need a terminal that reports them (foot does, as `CSI 27;2;13~` / `CSI 27;5;13~`; the
`CSI 13;2u` form is also understood).

| Measured (Fedora 44)                   | vsc          |
|----------------------------------------|--------------|
| binary                                 | 205 KB       |
| RSS, small file                        | 2.2 MB (of which **348 KB private**, rest shared libc) |
| RSS, 13 MB / 250k-line C file          | 27 MB        |

## Explorer

| Keys | Action |
|---|---|
| Ctrl+E (or Ctrl+Shift+E) | Focus Explorer ↔ editor |
| Ctrl+B | Show / hide the sidebar |
| ↑ ↓ PgUp PgDn Home End | Move selection |
| → / ← | Expand folder / collapse or go to parent |
| Enter · Shift/Ctrl+Enter · Space | Open file in the focused tile · in a new tile (both focus the editor) · toggle folder |
| Ctrl+N · Alt+N | New file · new folder in the selected folder (`a/b/c.txt` creates the folders) |
| F2 · Delete | Rename · move to Trash (asks before a permanent delete if Trash isn't available) |
| letter | Jump to the next item starting with that letter |
| Esc | Back to the editor |

Header buttons: `+` new file, `⊞` new folder, `↻` refresh, `⊟` collapse all. Click a folder to toggle it,
click a file to open it (Alt+click: in a new tile), scroll with the wheel, and drag the sidebar's right edge to resize it. The
active editor's file is revealed and highlighted automatically. Folders load lazily, only when expanded.

## Editor keys

| Keys | Action |
|---|---|
| Ctrl+S / Ctrl+O / Ctrl+N | Save (prompts “Save as” for new files) / open / new file in the focused tile |
| Ctrl+W / Ctrl+Q | Close tile / quit — press twice to discard unsaved changes |
| Ctrl+PgUp / Ctrl+PgDn, Alt+1…9 | Focus previous / next tile, tile N |
| Ctrl+F, then Enter/↓/F3 · ↑/Shift+F3 · Esc | Find next · previous · close |
| Ctrl+G | Go to line (`42` or `42:7`) |
| Ctrl+Z / Ctrl+Y | Undo / redo |
| Ctrl+A, Ctrl+L, Shift+arrows/Home/End | Select all, select line, extend selection |
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / cut / paste (whole line when nothing is selected; also sets the system clipboard via OSC 52) |
| Ctrl+/ | Toggle line comment |
| Alt+↑/↓ · Shift+Alt+↑/↓ · Ctrl+D | Move lines · copy lines · duplicate line |
| Ctrl+←/→, Ctrl+Backspace/Delete | Word jump / delete |
| Ctrl+Home/End, Ctrl+↑/↓ | File start/end, scroll without moving |
| Tab / Shift+Tab | Indent / outdent (selection-aware) |

Auto-indent, bracket/quote auto-close and type-over, bracket-pair colours, and CRLF preservation are built in.

Long lines always wrap to the tile's width, after the last space that fits (mid-word only when there is none);
there is no horizontal scrolling. The line number is shown on a line's first row only, and ↑/↓, PgUp/PgDn, the
wheel and Ctrl+↑/↓ move by wrapped rows, keeping the cursor's column within the row.

**Mouse:** click to place the cursor, drag to select, double-click for a word, triple-click or click the
gutter for a line, and use the wheel to scroll. Click `Ln, Col` in the status bar to go to a line.

**PDF:** opening a PDF (explorer click/Enter, Ctrl+O or the command line) shows it in a tile when the terminal has
kitty graphics with shared memory (Ghostty, kitty, or foot built from the `kitty-graphics` branch of
[Person-26/foot](https://github.com/Person-26/foot); vsc asks it at startup) and `vsc-pdf` is installed next to vsc or
on PATH. `vsc-pdf` runs Okular's own engine (libOkular6Core: the same generators, fonts and `okularpartrc` settings)
with no window and renders pages into shared memory; vsc passes them to the terminal, which keeps them, so scrolling
only moves the images: pixel-smooth, with Okular's animation (Up/Down/j/k or a wheel notch 100 px in 100 ms,
PgUp/PgDn/Space/Backspace a screen in 200 ms, Shift 10x, eased like Okular's QScroller). Pages fit the width, laid
out like Okular's continuous mode; +/- zoom, 0 fits the width again, Home/End, Ctrl+G (or a click on "Page N of M")
goes to a page. Only the pages on screen and one either side are rendered, and the terminal drops any more than three
pages away. `vsc-pdf` starts with the first PDF tile and exits with the last. A touchpad scrolls in wheel notches,
since terminals get no finer scroll events. A page the terminal refuses is shown in the status bar and left out
(not placed again) until the view next changes.

Elsewhere (stock foot, or without `vsc-pdf`) a PDF launches Okular instead. Okular runs detached, in its own session with no
terminal output, so it stays open after vsc exits. A second open of the same file within a second (a double click) is
ignored.

**LaTeX:** saving a `.tex` file builds it in the background with `latexmk` (or `pdflatex` if latexmk is
missing); the status bar shows `⟳ LaTeX` while it runs, then the time taken or the first error with its
line. Saving again restarts the build; a PDF tile showing the output reloads in place when the build succeeds, and
Okular reloads it by itself.
Saving a file without `\documentclass` (a chapter) rebuilds the last main file built this session. Magic
comments in the first 20 lines are honoured: `% !TEX root = ../main.tex` builds that file instead, and
`% !TEX program = xelatex` (or `lualatex`) picks the engine; otherwise a `latexmkrc` that sets `$pdf_mode`
is respected. Only one build runs at a time.

**Konsole note:** Konsole binds Shift+←/→ to switching its own tabs by default. Unbind them under
Settings → Configure Keyboard Shortcuts if you want Shift+arrow selection.

Highlighting covers C/C++/Java/C#/Rust/Go/Swift/Kotlin, Python, JavaScript/TypeScript, shell/Makefile,
JSON and LaTeX (`.tex`/`.sty`/`.cls`: commands, environments, references, math, comments). Indent guides
(`│` at each 4-column indent level) are drawn in every file, and run through blank lines inside a block.

Undo history is a ring buffer capped at 1 MB per tile: each new step that takes it over drops just enough
of the oldest steps (in O(1) each) to get back under, and a single change over 1 MB can't be undone. Files are loaded
straight into lines with no whole-file copy, so opening a file peaks at about its final size.
