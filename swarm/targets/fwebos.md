# fwebos: a small operating system for the web, written in fut

Build the core of a desktop operating system as a fut library, with thorough tests, then put a face on it in the browser. The core is:
- an in-memory file system;
- a shell language with pipes, redirection, variables, command substitution, arithmetic, `if`/`for`/`while` and background jobs;
- about forty commands, with `grep` matching a small regular-expression language;
- a process table and a scheduler on a virtual clock;
- a window manager;
- terminals with line editing, history and tab completion.

Everything runs on the virtual clock, so it is deterministic: the same calls give the same results every time. The rules below simplify real Unix on purpose. Where they differ, follow these rules exactly: hidden tests check outputs character by character.

## Scope (roughly in order)
1. Text helpers and paths: splitting into lines and words, ASCII case, globs (`*` and `?`), path normalizing. Use `std/strings` and `std/lists` (see the fut reference) before writing your own.
2. File system: directories and text files, path resolution, and `save`/`load`.
3. Shell lexer and parser: words, quotes, expansions, redirections, pipelines, `&&`/`||`, `;`/`&`, `if`/`for`/`while`. Define the syntax tree once, in one module, as fut enums or records with fixed fields; every other module uses it.
4. The executor and the simple commands: `sh`, with the file commands, text commands and `test`.
5. `grep` and its regular expressions.
6. Arithmetic `$((...))` and command substitution `$(...)`.
7. Jobs and the clock: `&`, `sleep`, `tick`, `jobs`, `ps`, `kill`, `wait`, `uptime`.
8. Window manager: the `wm*` functions.
9. Terminals: the `term*` functions, with line editing, history and completion.
10. The desktop in the browser (`src/desktop.fut` with `std/ui`; see the last section), once 1 to 9 work.

## Public API (fixed: hidden tests are written against it)
`src/os.fut` is the package root (`root src/os.fut` in fut.pkg). It makes these visible, directly or through `pub import`:
```
pub fn boot(): map                                    // a new OS
pub fn sh(os: map, line: str): map                    // {out: str, err: str, code: num}, in session 0
pub fn tick(os: map, ms: num): map                    // {out, err}: what session 0's jobs wrote while the clock moved
pub fn save(os: map): str                             // the file system, as text
pub fn load(text: str): map                           // a new OS whose file system is the saved one
pub fn wmOpen(os: map, app: str, title: str): num     // the new window's id
pub fn wmClose(os: map, id: num)
pub fn wmFocus(os: map, id: num)
pub fn wmMove(os: map, id: num, x: num, y: num)
pub fn wmResize(os: map, id: num, w: num, h: num)
pub fn wmMinimize(os: map, id: num)
pub fn wmMaximize(os: map, id: num)                   // toggles
pub fn wmTile(os: map)
pub fn wmCycle(os: map)
pub fn wmAt(os: map, x: num, y: num)                  // a window id, or nil
pub fn wmList(os: map): list
pub fn termNew(os: map, cols: num, rows: num): num    // the new terminal's id
pub fn termType(os: map, id: num, text: str)
pub fn termKeys(os: map, id: num, keys: list)
pub fn termScreen(os: map, id: num): list             // rows strings, each exactly cols characters
pub fn termCursor(os: map, id: num): map              // {row, col}, 0-based, on the screen
```
`os` is mutable state: the functions change it in place. Don't name your own functions after fut builtins: `run`, `print` and `exec` are taken.

## File system
- There are directories and text files. A file's content is a string. A new OS has the directories `/`, `/home`, `/home/user` and `/tmp`, and no files.
- Names are any characters but `/`. `.` and `..` are not stored; they are resolved.
- Resolving a path: a path starting with `/` is absolute, anything else is relative to the session's current directory. Split on `/`, drop empty parts and `.`; `..` removes the previous part (at `/` it stays `/`). The result is a normal absolute path: `/` or `/a/b`, with no trailing slash.
- `save(os)` returns a string that `load` turns back into the same tree: the same directories, files and contents. Any format will do (JSON is easiest). Sessions, variables, jobs, windows and terminals are not saved; `load` gives them the values of a new OS.

## Sessions
A session is a shell's state: the current directory, the variables and the last exit status (`$?`).
- `sh` always uses session 0. Each terminal has its own session. All sessions share the file system, the clock, the job table and the windows.
- A new session starts in `/home/user`. Its variables are `HOME=/home/user`, `USER=user` and `PWD=/home/user`, and `$?` is 0. `cd` keeps `PWD` up to date.

## Output
- A command writes to standard output (out) and standard error (err). `sh` returns all of each stream written during the call, including what background jobs of session 0 wrote while the clock moved during the call (see Jobs), and the exit status of the last command that ran in the foreground.
- `code` and `$?`: 0 is success; failures are 1 unless stated otherwise.
- Every error message is one line, ending with `\n`, on err. In the formats below, `X` is the argument exactly as the user wrote it, after expansion (not the resolved path).
- Line-based output (`ls`, `head`, `tail`, `sort`, `uniq`, `grep`, `seq`, `find`, `env`, `jobs`, `ps`, `cut`, `tr` output lines) ends each line with `\n`.

## Shell syntax
A line is parsed completely before any of it runs. On a syntax error, nothing runs: err gets `sh: syntax error: ` and a short description, and the code (and `$?`) is 2.

Structure, loosest first:
- **list**: and-or lists separated by `;`, `&` or newlines. `&` runs the and-or list before it as a background job. A trailing `;` or `&` is allowed. An empty list (blank line or only a comment) does nothing; the code is 0 and `$?` is unchanged.
- **and-or list**: pipelines joined by `&&` and `||`, left to right with equal precedence. `a && b` runs b only if a's code is 0; `a || b` only if it isn't. The status is that of the last pipeline that ran.
- **pipeline**: commands joined by `|`. Each command's out is the next one's standard input. The pipeline's code is the last command's. Commands run one after another, left to right, and each command's err goes straight to the session's err.
- **command**: one of
  - a simple command: words and redirections in any order. If every word is an assignment (`NAME=value`, NAME being a letter or `_` then letters, digits and `_`), it sets those variables in order and its code is 0. A value is expanded like a word in double quotes: no splitting, globs or tilde (`Y=$X` copies X exactly). Otherwise the first word is the command name and the rest are its arguments; any assignment words among them are plain arguments.
  - `if LIST; then LIST; fi`, `if LIST; then LIST; else LIST; fi`, and `elif LIST; then LIST` before the else. The status of the if is the status of the branch that ran, or 0 if none ran.
  - `for NAME in WORDS; do LIST; done`. The words are expanded (with splitting and globs), then the body runs once per word with NAME set to it. `for NAME; do` is a syntax error. The status is the body's last status, or 0.
  - `while LIST; do LIST; done`. The status is the body's last status, or 0.
  - Inside these, `;` can be a newline. The keywords `if then elif else fi for in do done while` are only keywords where a command name could start (and `in` after the for's NAME). Redirections can't be applied to compound commands (a syntax error), but compound commands can be in pipelines and and-or lists, and run in the background with `&`.

Words:
- A word is a run of characters other than unquoted spaces, tabs, newlines and the operators `| & ; < >`, `&&`, `||`, `>>`. `2>` and `2>&1` are operators only where a word would start (`echo a2>f` is the word `a2`, then `>`). There are no other operators: `2>>` is `2>` then `>`, a syntax error. `(` and `)` are ordinary characters, except that inside `$(...)` the first unquoted `)` that isn't part of a nested `$(...)` or `$((...))` ends it.
- `'...'` takes every character literally. `"..."` takes characters literally except `$` expansions and the escapes `\"`, `\\` and `\$`. Outside quotes, `\` makes the next character literal. An unterminated quote is a syntax error.
- `#` at the start of a word starts a comment to the end of the line.
- Expansions, inside double quotes and unquoted: `$NAME`, `${NAME}` (an unset variable is empty), `$?`, `$(LIST)` (command substitution), `$((EXPR))` (arithmetic). `$` followed by anything else is a literal `$`.
- Command substitution runs the list in the same session, as a subshell: variable assignments and `cd` inside it don't change the session, but files do. Its err goes to the session's err. The result is its out with all trailing newlines removed.
- Splitting: the results of unquoted `$NAME`, `${NAME}`, `$?`, `$(...)` and `$((...))` are split into separate words on spaces, tabs and newlines. Quoted ones are not split. A word made only of unquoted expansions that come out empty disappears. `""` and `''` are empty words.
- Tilde: an unquoted `~` at the start of a word, followed by `/` or the end of the word, is replaced by `$HOME`.
- Globs: an unquoted `*` or `?` that appears literally in the source (not one that came from an expansion) makes the word a pattern. `*` matches any run of characters and `?` matches one character. Only the last path part may hold glob characters (`src/*.fut` works; `*/x` doesn't need to). The pattern matches the names in its directory, except names starting with `.`, unless the pattern starts with `.`. The matches replace the word, sorted, each as the directory part as written plus the name. With no match, the word stays as written.

Arithmetic, in `$((EXPR))`:
- Integers, `+ - * / %`, unary `-`, and parentheses, with the usual precedence. Operators of equal precedence group left to right.
- A name is a variable's value as an integer: a value that isn't an optional `-` and digits (unset, empty, `abc`, `1.5`) counts as 0. `$NAME`, `${NAME}` and `$?` work too.
- Anything else is malformed: `sh: arithmetic syntax error`, status 1, and the command doesn't run.
- `/` truncates toward zero, and `%` takes the sign of the left operand.
- Division or modulo by zero prints `sh: division by zero` and gives status 1; the command it was part of doesn't run.

Redirections (each takes the next word as its file):
- `> file` writes out to the file, creating or truncating it. `>> file` appends. `< file` reads the file as standard input. `2> file` writes err to the file. `2>&1` sends err wherever out goes at that point.
- They apply left to right. `cmd > f 2>&1` sends both to f; `cmd 2>&1 > f` sends only out to f.
- If a redirection's file can't be opened, err gets `sh: X: No such file or directory` (missing parent directory, or a missing input file) or `sh: X: Is a directory`, the status is 1, and the command doesn't run.

Running a command name:
- A name that is not a command prints `sh: X: command not found`, with status 127.
- `NAME=value` words that are all assignments set the variables (see above).

## Commands
`[ cat cd clear cp cut echo env export false find grep head jobs kill ls mkdir mv ps pwd rm seq sh sleep sort source tail test touch tr true uname uniq unset uptime wait wc whoami`
These are the command names, in sorted (byte) order. Tab completion offers them in this order.

Options are the arguments that start with `-` (and aren't just `-`) before the first other argument. Letters may be combined (`-rf`, `-in`). An option that takes a value takes the rest of its argument or, if nothing is left, the next argument (`-n 3`, `-n3`). Commands with `[FILE]` read only one file. Unknown options are ignored. `echo` treats only an exact first argument `-n` as an option.

Paths: a path whose parent directory doesn't exist, or goes through a file, counts as missing (`No such file or directory`).

Standard input is empty unless it comes from a pipe or `<`. "Lines" of a text are its pieces split on `\n`. If the text ends with `\n`, that last empty piece is not a line, and the empty text has no lines.

File commands:
- `pwd`: prints the current directory.
- `cd [DIR]`: goes to DIR (default `$HOME`) and sets `PWD`. Errors: `cd: X: No such file or directory`, `cd: X: Not a directory`.
- `ls [-a] [PATH...]`: with no path, lists the current directory. A directory prints its entries' names one per line, sorted (byte order). Names starting with `.` are left out without `-a` (`.` and `..` are never listed). A file prints the path as written. With two or more paths, files come first, in the order given; then each directory as `X:` followed by its entries, with an empty line before each directory's header except at the very start of output. A missing path prints `ls: X: No such file or directory` to err and the status is 1, but the others are still listed.
- `cat [FILE...]`: prints the files' contents one after another, or standard input with no files. Errors: `cat: X: No such file or directory`, `cat: X: Is a directory` (the other files still print; status 1).
- `touch FILE...`: creates empty files; existing files are left as they are. Error: `touch: X: No such file or directory` when the parent directory is missing.
- `mkdir [-p] DIR...`: creates directories. Errors: `mkdir: X: File exists`, `mkdir: X: No such file or directory` (missing parent). With `-p`, parents are created and an existing directory is not an error.
- `rm [-r] [-f] PATH...`: removes files, and directories with their contents under `-r`. Errors: `rm: X: No such file or directory` (silent with `-f`, and then not a failure), `rm: X: Is a directory` (without `-r`). Flags may be combined (`-rf`).
- `cp [-r] SRC DST`, `mv SRC DST`: copy or move SRC to DST. If DST is an existing directory, the copy goes inside it under SRC's last name. `cp` of a directory needs `-r` (`cp: X: Is a directory`, X being SRC). Errors: `cp: X: No such file or directory`, `mv: X: No such file or directory` (missing SRC, or DST's parent missing: X is the path that's missing as written, SRC or DST).

Text commands (each reads its FILE argument if given, otherwise standard input; a missing file is `NAME: X: No such file or directory` with status 1):
- `echo [-n] [ARG...]`: the args joined by one space, then `\n` (none with `-n`). No escapes are interpreted.
- `head [-n N] [FILE]`, `tail [-n N] [FILE]`: the first or last N lines (default 10).
- `wc [-l|-w|-c] [FILE]`: lines (count of `\n`), words (runs of non-whitespace) and characters (string length). With no flag it prints all three, else just the one asked for, separated by single spaces, then ` FILE` if a file was given, then `\n`. Example: `3 5 21 notes.txt`.
- `sort [-r] [-n] [-u] [FILE]`: sorts lines in byte order. `-n` sorts by each line's leading number (an optional `-`, digits, an optional `.` and digits; no leading number counts as 0), with ties in byte order of the whole line. `-r` reverses the final order. `-u` then keeps the first of each run of equal lines.
- `uniq [-c] [FILE]`: collapses runs of adjacent equal lines into one. `-c` puts the run's length and a space before each line (`3 apple`).
- `cut -d D -f N [FILE]`: the Nth field (1-based) of each line split on the one-character delimiter D. A line without D prints whole. A missing field prints an empty line. `-dD` and `-fN` (attached) work too. D defaults to a tab.
- `tr SET1 SET2`: replaces each character of standard input found in SET1 with the character of SET2 at the position of its first occurrence in SET1. If SET2 is shorter, its last character repeats. Sets may contain ranges `a-z` (a `-` first or last is literal) and the escapes `\n`, `\t` and `\\`. `tr -d SET1` deletes those characters.
- `grep [-i] [-v] [-c] [-n] PATTERN [FILE...]`: prints the lines that match the pattern (see Regular expressions). `-i` ignores ASCII case. `-v` prints the lines that don't match. `-c` prints only the count of printed lines (per file with several files). `-n` puts the line number and `:` first. With two or more files, each output line (or count) starts with the file name and `:`. The status is 0 if any line was selected, else 1. A missing file is reported and grep goes on, and then the status is 2 unless a line was selected.
- `seq [FIRST] LAST`: the integers from FIRST (default 1) to LAST, one per line. Nothing if LAST < FIRST.
- `find [PATH] [-name PATTERN] [-type f|d]`: the path itself and everything below it, depth first, children in sorted order. Each path is PATH as written (default `.`), plus `/` and the names below it. Without `/` doubling: if PATH ends in `/`, no extra `/` is added. Names starting with `.` are included. Filters: `-name` matches the glob (`*`, `?`, matching dots too) against the last name (for PATH itself: its last part, ignoring trailing slashes, so `.` for `.`); `-type f` keeps files, `-type d` directories. Error: `find: X: No such file or directory`.

Other commands:
- `true`, `false`: status 0 and 1.
- `whoami`: `user`. `uname`: `fwebos`.
- `env`: every variable as `NAME=value`, sorted by name.
- `export NAME=value...`: the same as the assignments. `export NAME` does nothing. `unset NAME...`: removes variables.
- `test EXPR` and `[ EXPR ]` (the last argument must be `]`, else `[: missing ]` with status 2). Status 0 if true, 1 if false. EXPR is one of: `-e P`, `-f P`, `-d P` (exists, is a file, is a directory); `-z S`, `-n S` (empty, not empty); `S1 = S2`, `S1 != S2`; `N1 -eq N2`, `-ne`, `-lt`, `-le`, `-gt`, `-ge` (integers); a single argument (true if not empty); no argument (false). A leading `!` negates the rest. Errors, with status 2 (NAME being `test` or `[`): `NAME: X: integer expression expected` for a non-integer operand of `-eq` and the others; `NAME: bad expression` for anything else.
- `sh FILE`, `source FILE`: run the file's text as a list in the current session (both of them: there are no subshells besides `$(...)`). Error: `sh: X: No such file or directory`.
- `clear`: in a terminal, clears the screen (see Terminals). Elsewhere it does nothing.

## Jobs and the clock
The OS has a virtual clock in milliseconds, starting at 0. Nothing moves it but `sleep`, `wait` and `tick`.
- `LIST &` starts a background job running that and-or list. Jobs are numbered 1, 2, 3, … in the order they're started, and never reused. Job N's process id is N + 1 (pid 1 is the shell). Starting one prints `[N] PID` on out. The job's text is its source text with surrounding spaces trimmed (without the `&`).
- A job has a wake time, initially the current time. A job runs its and-or list until it finishes or reaches `sleep S`, which sets its wake time to now + S seconds (S may have decimals) and pauses it there. A job's command substitutions and its commands run in a copy of its session made when it started. Its out and err go to the session that started it.
- The scheduler runs due jobs: while some live job's wake time is at or before the current time, run the one with the earliest wake time (lowest job number on a tie) until it pauses or finishes. It runs at the end of every `sh` call (and every terminal command), and whenever the clock moves.
- Moving the clock to time T: repeatedly take the earliest live wake time that is at or before T, set the clock to it, and run due jobs. Then set the clock to T.
- When a job finishes, it prints on its session's out `[N] Done\tTEXT` if its status was 0, else `[N] Exit S\tTEXT`.
- `sleep S` in the foreground moves the clock by S seconds (S·1000 rounded to whole milliseconds). `tick(os, ms)` moves it by ms and returns what session 0's jobs wrote. What session 0's jobs write outside `sh` and `tick` calls (say, while a terminal command sleeps) is dropped.
- `wait`: moves the clock to the next wake time, again and again, until no jobs are left. `wait %N` or `wait PID`: until that job is done (immediately if it already is, or never existed). In a job, `wait` does nothing.
- A job is code paused in the middle of running (inside a `for` loop, a `$(...)`, a sourced file). fut's fibers fit: `yield` works from any call depth (see the fut reference).
- `jobs`: `[N] Running\tTEXT` for each live job, by number.
- `ps`: `PID\tSTAT\tCMD`, then `1\tR\tsh`, then each live job's `PID\tSTAT\tTEXT` by pid, STAT being `R` if its wake time is at or before now and `S` if it's sleeping.
- `kill PID...` or `kill %N...`: ends each job and prints `[N] Terminated\tTEXT` on out. Error: `kill: X: No such process` (the others are still killed; status 1).
- `uptime`: `up S.mmms`: the clock in seconds with exactly three decimals (`up 2.500s`).

## Regular expressions (grep)
A pattern matches a line if it matches any substring of it.
- A literal character matches itself. `.` matches any character. `[abc]`, `[a-z0-9_]` and `[^...]` match one character in (or not in) the set. A `]` right after `[` or `[^` is literal, and so is `-` first or last.
- `*`, `+`, `?` after an atom: zero or more, one or more, zero or one. One that doesn't follow an atom (at the start, or after another quantifier) is a literal character: `a**` is `a*` then a literal `*`.
- A `[` with no closing `]` is a literal `[`. Inside a set, `\` is an ordinary character.
- `^` at the start of the pattern anchors to the line's start, and `$` at the end anchors to its end. Elsewhere they are literal.
- `\` makes the next character literal.
- With `-i`, ASCII letters match either case.

## Window manager
The screen is 1280 by 800. Windows have an id (1, 2, 3, … in the order opened, never reused), an app name, a title, a position and a size.
- `wmOpen`: a new window, 640 by 400, at (40·k, 30·k) where k is the number of windows opened before it (ever) mod 8. It goes on top and gets the focus.
- Stacking: windows are in a stack, bottom to top. The focused window is the top window that isn't minimized; when no window qualifies, nothing is focused.
- `wmFocus(id)`: un-minimizes the window and moves it to the top.
- `wmClose(id)`: removes it.
- `wmMove(id, x, y)`: moves the top-left corner, clamped so that x stays between 40 − w and 1280 − 40, and y between 0 and 800 − 24.
- `wmResize(id, w, h)`: sets the size, at least 160 by 100. The position stays.
- `wmMinimize(id)`: hides it. It stays in the stack where it is.
- `wmMaximize(id)`: if the window isn't maximized, it remembers the position and size and becomes (0, 0, 1280, 800). If it is, it goes back to what it remembered. Moving or resizing a maximized window un-maximizes it (the remembered geometry is dropped).
- `wmTile`: puts the windows that aren't minimized in a grid, in id order: c = ceil(√n) columns and r = ceil(n / c) rows. Each cell is floor(1280 / c) by floor(800 / r), filled row by row from the top left. Tiled windows aren't maximized. The stack and focus don't change.
- `wmCycle`: moves the top window that isn't minimized to the bottom of the stack (alt-tab). With fewer than two such windows, nothing changes.
- `wmAt(x, y)`: the top window that isn't minimized and contains the point (x ≤ px < x + w, y ≤ py < y + h), or nil.
- `wmList`: one map per window, bottom of the stack first: `{id, app, title, x, y, w, h, focused, minimized, maximized}`.
- Functions given an id that doesn't exist do nothing.

## Terminals
A terminal is a screen of cols by rows characters, a session, a scrollback and an input line.
- `termNew(cols, rows)`: a new terminal (ids 1, 2, 3, …) with a new session. Its scrollback is empty.
- The prompt is `user@fwebos:P$ ` where P is the current directory, with `/home/user` written as `~` (and `/home/user/x` as `~/x`).
- The screen lines: each line of the scrollback (by the rule for lines under Commands, but a last piece without `\n` is a line too), then the prompt followed by the input as one more line. Each line is cut every cols characters; an empty line is one empty screen line. A tab shows as one space. The screen shows the last rows screen lines, top-aligned when there are fewer, each padded with spaces to cols.
- The cursor is at the input's cursor position, counted from the start of the prompt line: screen row position ÷ cols (rounded down) within that line, column position mod cols. When the cursor sits just past the end of a prompt line whose length is a multiple of cols, the prompt line gets one more, empty, screen line for it.
- `termType(id, text)` sends each character as a key, with `\n` as `Enter` and `\t` as `Tab`. `termKeys(id, keys)` sends a list of keys: a one-character string is that character (`\n` and `\t` mean `Enter` and `Tab` here too); the names are `Enter`, `Backspace`, `Delete`, `Left`, `Right`, `Home`, `End`, `Up`, `Down`, `Tab`, `Ctrl-C`, `Ctrl-L`.
- Editing: a character is inserted at the cursor. `Backspace` deletes the character before the cursor, `Delete` the one at it. `Left`, `Right`, `Home` and `End` move the cursor within the input.
- `Enter`: the prompt and input go into the scrollback, followed by `\n`. Then the input runs as a command line in the terminal's session, and everything it writes on out and err goes into the scrollback in the order written. A line that isn't empty or all spaces is added to the history, unless it equals the latest entry. Then the input is empty again.
- `clear` in a terminal, and `Ctrl-L`, empty the scrollback.
- `Ctrl-C`: the prompt, the input and `^C\n` go into the scrollback, and the input is emptied.
- History: `Up` shows the previous entry (staying at the oldest), `Down` the next one. Going down past the newest entry brings back what was being typed before `Up` was first pressed, and ends browsing. The cursor goes to the end of the input. `Up` with no history and `Down` when not browsing do nothing. A typed character, `Backspace`, `Delete`, `Tab`, `Enter` or `Ctrl-C` ends browsing, keeping the shown text as the input (the history itself doesn't change).
- `Tab` completes the word that ends at the cursor (the text after the last space before the cursor).
  - If no space comes before it, the candidates are the command names starting with it.
  - Otherwise, they are the entries of the directory part (the word up to and including its last `/`, resolved in the session; the current directory if there's no `/`) whose names start with the rest, excluding names starting with `.` unless the rest starts with `.`.
  - One candidate: the word becomes the directory part plus the name, then `/` for a directory or a space otherwise. The cursor goes to the end of the inserted text; text after the cursor stays after it.
  - Several: if their longest common prefix is longer than what's typed, the word is extended to it. If not, the prompt and input go into the scrollback followed by `\n`, then the candidates (directories with `/`) joined by two spaces, then `\n`. The input stays as it was.
  - None: nothing happens.
- Jobs started in a terminal write to its scrollback, when they run.

## The desktop (scope item 10)
`src/desktop.fut` draws the OS with `std/ui`, so that `fut` can serve it in the browser:
- a desktop with a launcher and a taskbar listing windows (click to focus, or to un-minimize);
- windows from `wmList`, with title bars that can be dragged and close, minimize and maximize buttons;
- terminal windows showing `termScreen` with a cursor, taking keys;
- a file manager over the file system, and a text editor that saves with `save`-style persistence to the browser's storage.

Put the logic (layouts, hit tests, what a key does) in pure functions with tests. The view code should only draw what they return.

## Layout
- `src/*.fut` holds the modules. `src/os.fut` is the entry and makes the public API visible.
- `tests/*_test.fut` hold the tests. Each imports what it tests with a relative path (`import "../src/fs.fut"`).
- `all.fut` imports every module (`import "src/x.fut"`), so `fut map all.fut` shows the whole project. Keep it current.
