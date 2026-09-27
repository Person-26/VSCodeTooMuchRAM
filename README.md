# vsc — VS Code look & keys: ~0.4 MB real memory (PSS)

A single-file C++17 terminal editor laid out like VS Code: an **Explorer sidebar** with the folder's file
tree on the left, editor tabs + breadcrumbs on the right, and the blue status bar (with git branch) at the
bottom, all in the Dark+ theme with VS Code keybindings. No ncurses, no GUI toolkit, no dependencies. PDFs open in Okular
rather than in the terminal.

```
make            # ./vsc  (~205 KB; libstdc++ linked in, libc shared; needs: sudo dnf install libstdc++-static)
make static     # fully static; lower RSS figure but higher real memory. needs: sudo dnf install glibc-static libstdc++-static
make install    # -> ~/.local/bin/vsc
vsc                  # open the current folder (like `code .`)
vsc ~/project        # open a folder
vsc ~/project a.cpp  # open a folder and a file
```

| Measured (Fedora 44)                   | vsc          |
|----------------------------------------|--------------|
| binary                                 | 205 KB       |
| RSS, 3 small tabs                      | 2.4 MB (of which **368 KB private**, rest shared libc) |
| RSS, 10 tabs                           | 2.4 MB       |
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

**Mouse:** click to place the cursor, drag to select, double-click for a word, triple-click or click the
gutter for a line, and use the wheel to scroll. On the tab bar, click a tab to switch, click `×` or
middle-click to close, double-click empty space for a new tab, and use the wheel to cycle tabs. Click
`Ln, Col` in the status bar to go to a line.

**PDF:** opening a PDF (explorer click/Enter, Ctrl+O or the command line) launches Okular on it instead of
opening a tab. Okular runs detached, in its own session with no terminal output, so it stays open after vsc
exits. A second open of the same file within a second (a double click) is ignored.

**LaTeX:** saving a `.tex` file builds it in the background with `latexmk` (or `pdflatex` if latexmk is
missing); the status bar shows `⟳ LaTeX` while it runs, then the time taken or the first error with its
line. Saving again restarts the build; Okular reloads the PDF by itself when it changes.
Saving a file without `\documentclass` (a chapter) rebuilds the last main file built this session. Magic
comments in the first 20 lines are honoured: `% !TEX root = ../main.tex` builds that file instead, and
`% !TEX program = xelatex` (or `lualatex`) picks the engine; otherwise a `latexmkrc` that sets `$pdf_mode`
is respected. Only one build runs at a time.

**Konsole note:** Konsole binds Shift+←/→ to switching its own tabs by default. Unbind them under
Settings → Configure Keyboard Shortcuts if you want Shift+arrow selection.

Highlighting covers C/C++/Java/C#/Rust/Go/Swift/Kotlin, Python, JavaScript/TypeScript, shell/Makefile,
JSON and LaTeX (`.tex`/`.sty`/`.cls`: commands, environments, references, math, comments). Indent guides
(`│` at each 4-column indent level) are drawn in every file, and run through blank lines inside a block.

Undo history is a ring buffer capped at 1 MB per tab: each new step that takes it over drops just enough
of the oldest steps (in O(1) each) to get back under, and a single change over 1 MB can't be undone. Files are loaded
straight into lines with no whole-file copy, so opening a file peaks at about its final size.
