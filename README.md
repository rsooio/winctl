# winctl

Command-line automation for Windows applications: locate windows and UI elements, inspect them, click, and type.

Standalone C project: a single `winctl.exe`, no runtime dependencies.

## Build

Requires a MinGW-w64 cross compiler:

```sh
make
```

Produces `winctl.exe`. Distribute the binary with this document (and LICENSE).

## Usage

```
winctl <command> [arguments...]
winctl --help
```

## Commands

| Command | Description |
|---|---|
| `list [--all]` | Element query (set semantics). No locator = top-level windows (default `/Window`, fast). Full tree via `//*`. Locator starting with `/` = global query (virtual root). `hwnd` prefix = single window; no xpath = whole tree. Large-tree processes (firefox.exe etc.) excluded by default; `--all` includes them |
| `prop <locator> [key] [value]` | Read/write properties: no value reads, value writes; no key dumps all (key: value lines) |
| `click <locator> [--mouse] [--button B] [--action A]` | Click element (exactly one match required) |
| `focus <locator>` | Set focus (exactly one match required) |

`prop`/`click`/`focus` require exactly one match (ambiguity is an error listing candidates); `list` is the only set-semantics command.

## Locator

```
<hwnd>             Root element itself (hex, 0x prefix optional); any window/element handle,
                  the hwnd column of list output can be used as a root
<hwnd>/<xpath>     Locate below the root; the first step matches a child of
                  the root; the root itself never participates in the xpath.
                  Chained locating is string concatenation (list output xpath
                  + hwnd prefix is directly reusable)
/<xpath>          list global query: xpath evaluated from the virtual root
                  (all top-level windows); /Window matches top-level windows,
                  //Window matches any depth
```

XPath syntax: `/` `//` `*` `[n]` positional predicate (in-tree), `[@Name='']` `[@Type='']` `[@Id='']` `[@Class='']` `[@Pid='']` attribute predicates (`@` optional; `@Pid` only on the top-level window step), `*=` contains `^=` prefix `$=` suffix, `and`, `!=`. Predicate values may be quoted or bare.

Sub-path predicates (existence check, top-level window step only, e.g. `list /Window[Pid=123][//Text[@Name='x']]`): `[//X]` any depth, `[X]` any depth (prefix omitted), `[/X]` direct child, `[./X]` direct child, `[.//X]` any depth. Sub-path may nest and reuse all predicate syntax. Not supported inside tree (`list <hwnd>/...`) or locate (`prop`/`click`/`focus`).

## Properties

| key | Access | Description |
|---|---|---|
| `value` | read/write | ValuePattern read/write; falls back to focus + clipboard input |
| `state` | read/write | Window state, `normal`/`maximized`/`minimized`; write `closed` to close (WM_CLOSE, async) |
| `pid` | read-only | Process ID of the element's window handle (decimal) |

## Examples

```sh
winctl list                                  # top-level windows (default /Window, fast)
winctl list //*                             # full tree of all visible windows
winctl list /Window[@Name*='Notepad']       # top-level windows by title (fast)
winctl list /Window[@Pid='1234']            # windows by process (dialog use case)
winctl list /Window[@Name*='Notepad']/Pane//Button    # query inside a window
winctl list 0x1a2b                          # single-window full tree
winctl list "0x1a2b//*[@Name^='Open']"      # filter by name prefix
winctl prop 0x1a2b value                    # read value
winctl prop 0x1a2b value text               # write value
winctl prop 0x1a2b state maximized          # maximize window
winctl prop 0x1a2b pid                      # read process ID
winctl click 0x1a2b/Pane/Button      # click
```

Top-level window positioning supports attribute predicates only (`[@Name]`/`[@Pid]`/`[@Class]`); positional `[n]` on top-level windows is rejected (Z-order unstable). In-tree `[n]` counts matched siblings (after predicate filtering) and is stable.

## Output and Exit Codes

- Default: human-readable — `list` aligned columns (element rows `hwnd xpath name`; name truncated with ellipsis on narrow terminals, xpath always complete), `prop` key: value lines. `--json` outputs full JSON (element fields `hwnd/enabled/invokable/scrollable/name/value/rect/type/xpath`).
- hwnd is hex throughout (`0x` prefix); pid is decimal.
- Errors go to stderr with non-zero exit codes: 0 success (including empty query results), 1 invalid argument/locator, 2 invalid window handle, 3 element not found, 4 operation failed.

### One Process, One Operation

Each invocation is an independent, stateless process: locate, operate, exit. Serial use is naturally safe; retries/polling are implemented by the calling script (e.g. loop `winctl list /Window[@Pid='...']` to wait for a dialog).

## Contract

The CLI contract is defined by `winctl --help` output; keep this document in sync when changing command syntax.

## License

GNU General Public License v3.0. See LICENSE.
