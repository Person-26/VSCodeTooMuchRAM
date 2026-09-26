# vsc — VS Code look & keys: ~0.4 MB real memory (PSS)

A single-file C++17 terminal editor laid out like VS Code: an **Explorer sidebar** with the folder's file
tree on the left, editor tabs + breadcrumbs on the right, and the blue status bar (with git branch) at the
bottom, all in the Dark+ theme with VS Code keybindings. No ncurses, no GUI toolkit, no dependencies. PDF pages are rendered by poppler
(`pdftoppm`) in short-lived child processes and shown as sixel graphics (foot, or any terminal that reports
sixel support). The editor keeps only the rows in view plus 20% as compact run-length-coded palette indices
(~100-200 KB), never a decoded page.

```
make            # ./vsc  (~217 KB; libstdc++ linked in, libc shared; needs: sudo dnf install libstdc++-static)
make static     # fully static; lower RSS figure but higher real memory. needs: sudo dnf install glibc-static libstdc++-static
make install    # -> ~/.local/bin/vsc
vsc                  # open the current folder (like `code .`)
vsc ~/project        # open a folder
vsc ~/project a.cpp  # open a folder and a file
```

| Measured (Fedora 44)                   | vsc          |
|----------------------------------------|--------------|
| binary                                 | 217 KB       |
| RSS, 3 small tabs                      | 2.4 MB (of which **368 KB private**, rest shared libc) |
| RSS, 10 tabs                           | 2.4 MB       |
| RSS, viewing a PDF page                | 4.1 MB       |
| RSS, 13 MB / 250k-line C file          | 27 MB        |

## Explorer

| Keys | Action |
|---|---|
| Ctrl+E (or Ctrl+Shift+E) | Focus Explorer ↔ editor |
| Ctrl+B | Show / hide the sidebar |
| ↑ ↓ PgUp PgDn Home End | Move selection |
| → / ← | Expand folder / collapse or go to parent |
| Enter · Space | Open file (Enter also focuses the editor) · toggle folder |
| Ctrl+N · Alt+N | New file · new folder in the selected folder (`a/b/c.txt` creates the folders) |
| F2 · Delete | Rename · move to Trash (asks before a permanent delete if Trash isn't available) |
| letter | Jump to the next item starting with that letter |
| Esc | Back to the editor |

Header buttons: `+` new file, `⊞` new folder, `↻` refresh, `⊟` collapse all. Click a folder to toggle it,
click a file to open it, scroll with the wheel, and drag the sidebar's right edge to resize it. The
active editor's file is revealed and highlighted automatically. Folders load lazily, only when expanded.

## Editor keys

| Keys | Action |
|---|---|
| Ctrl+S / Ctrl+O / Ctrl+N | Save (prompts “Save as” for new files) / open / new tab |
| Ctrl+W / Ctrl+Q | Close tab / quit — press twice to discard unsaved changes |
| Ctrl+PgUp / Ctrl+PgDn, Alt+1…9 | Switch tab |
| Ctrl+F, then Enter/↓/F3 · ↑/Shift+F3 · Esc | Find next · previous · close |
| Ctrl+G | Go to line (`42` or `42:7`), or page in a PDF |
| Ctrl+Z / Ctrl+Y | Undo / redo |
| Ctrl+A, Ctrl+L, Shift+arrows/Home/End | Select all, select line, extend selection |
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / cut / paste (whole line when nothing is selected; also sets the system clipboard via OSC 52) |
| Ctrl+/ | Toggle line comment |
| Alt+↑/↓ · Shift+Alt+↑/↓ · Ctrl+D | Move lines · copy lines · duplicate line |
| Ctrl+←/→, Ctrl+Backspace/Delete | Word jump / delete |
| Ctrl+Home/End, Ctrl+↑/↓ | File start/end, scroll without moving |
| Tab / Shift+Tab | Indent / outdent (selection-aware) |

Auto-indent, bracket/quote auto-close and type-over, bracket-pair colours, and CRLF preservation are built in.

**Mouse:** click to place the cursor, drag to select, double-click for a word, triple-click or click the
gutter for a line, and use the wheel to scroll. On the tab bar, click a tab to switch, click `×` or
middle-click to close, double-click empty space for a new tab, and use the wheel to cycle tabs. Click
`Ln, Col` in the status bar to go to a line.

**PDF:** pages are shown as one continuous column (at 100% the widest page fills the editor width) that
scrolls by the pixel and glides: the wheel and ↑/↓ move a few lines, PgUp/PgDn/Space most of a screen,
Home/End to either end; ←/→ or a click on ◀ ▶ in the status bar go to the previous/next page (←/→ pan
instead when zoomed in past the width), `+`/`-`/`0` zoom and Ctrl+G goes to a page. Only the rows and columns
in view plus 20% of the view height are rendered and kept, that 20% ahead in the direction you are
scrolling, so a pdftoppm run (~20-80 ms) happens about once per fifth of a screen. The one exception is a
glide longer than that 20% (PgUp/PgDn, Home/End, ←/→ within two screens): the rows it passes through are
rendered up front in one run, since rendering them frame by frame would drop to ~8 fps, and the cache
shrinks back when it ends. Longer jumps go straight there. **Ctrl+T** toggles a text view extracted with
`pdftotext`, which you can select, copy and search; switching back keeps your place unless you moved to
another page in the text view. The text view is used automatically if the terminal has no sixel support;
set `VSC_GFX=1` (or `sixel`) or `VSC_GFX=0` to force either. Pages are drawn with a 232-colour palette (16
greys plus a dithered colour cube), so text is sharp and colour figures are close but not exact.

`make sixtest` builds a test tool that runs vsc in a pseudo-terminal, decodes its sixel output and
compares it with `pdftoppm` renders (usage at the top of `sixtest.cpp`).

**LaTeX:** saving a `.tex` file builds it in the background with `latexmk` (or `pdflatex` if latexmk is
missing); the status bar shows `⟳ LaTeX` while it runs, then the time taken or the first error with its
line. A tab showing the output PDF reloads when the build finishes, and saving again restarts the build.
Saving a file without `\documentclass` (a chapter) rebuilds the last main file built this session. Magic
comments in the first 20 lines are honoured: `% !TEX root = ../main.tex` builds that file instead, and
`% !TEX program = xelatex` (or `lualatex`) picks the engine; otherwise a `latexmkrc` that sets `$pdf_mode`
is respected. Only one build runs at a time, and an `$out_dir` in latexmkrc stops the PDF tab reloading.

**Konsole note:** Konsole binds Shift+←/→ to switching its own tabs by default. Unbind them under
Settings → Configure Keyboard Shortcuts if you want Shift+arrow selection.

Highlighting covers C/C++/Java/C#/Rust/Go/Swift/Kotlin, Python, JavaScript/TypeScript, shell/Makefile
and JSON.
