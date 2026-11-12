# RedBaboon C++ style

RedBaboon follows the [OXOS C++ style guide](https://github.com/kooscode/oxos/blob/main/docs/cpp_style.md)
(sections 1, 4, 5, 6, 7, 8, 9), with the kernel-only rules replaced by the ones below. Read this file before writing
code, and check every diff against it before each commit.

## Language

- C++20, `.cpp` / `.h` only, no `.c` files. nxdk builds with `NXDK_CXX = y`.
- libc++ is allowed (`<vector>`, `<array>`, `<algorithm>`, `<memory>`), and so is `new`.
- No exceptions (`throw`) and no RTTI (`dynamic_cast`, `typeid`): return errors.
- `constexpr` constants, never `#define` for values. `nullptr`. No comma operator. `auto` only where the type is on
  the same line.
- Global objects stay trivial: nothing touches hardware, files or SDL before `main`.
- Big buffers go on the heap or in static storage, never on the stack: the main thread has 256 KB (`xbox.mk`),
  other threads less.
- Memory: 64 MB for everything (nxdk's XBE asks for the 64 MB limit).

## Names

- Each folder has a namespace (`render`, `audio`, `content`, `input`, `game`).
- Types, functions and member functions: PascalCase (`Aircraft`, `UpdateFlight`). Locals, parameters and data
  members: camelCase. Constants and enumerators: UPPER_SNAKE_CASE.
- Files and folders: lower case with underscores, plain English (`flight_model.cpp`).
- Something only one file uses is `static` or in an anonymous namespace.

## Files

1. File comment: what the file does (and the docs/design.md section it implements).
2. Includes: the file's own header, then other project headers (`"core/vmath.h"`), then system headers.
3. File-wide constants and types, then file-wide state (each with a comment).
4. Sections under `// --- Title` headers, in dependency order.

Each folder has one header named for it (`render/render.h`) with what other folders use.

## Functions and statements

- **No one-line bodies anywhere, lambdas included.** `int F() { return x; }` and `[&](int x) { return x + 1; }` are
  violations; write the body on its own lines. A constructor whose work is all in its member initialisers may end
  in `{}`.
- The body of `if` / `for` / `while` is always on its own line. A single-line body may omit braces; a body over
  several lines has them, and when one branch of an if/else chain has braces, all have them.
- An empty loop body is `{}` with a comment saying what it waits for.
- Prefer early returns to deep nesting. A function does one thing.

## Comments

- A comment describes the code directly below it, in plain words; it says why where the code cannot.
- Comments are true: a change updates them. No history ("used to", "now", "fixed"), no commented-out code.
- Generated files say so in their first line, with the tool that writes them.

## Text on screen

The fewest words that say it: a title, then a list or one line.

## Formatting

Four spaces, no tabs; lines up to 120 characters; K&R braces, `} else {` on one line; one blank line between
functions, never two in a row.

## Commits

One commit per feature, with a message saying what, why and how it was checked.

## nxdk notes

- nxdk ships a C++ runtime with exception support and thread-safe statics, but exceptions and RTTI are untested
  on the console: code as if they were off.
- The runtime runs global constructors before `main`; keep them trivial (see Language).
- nxdk builds with `-fno-builtin`, so `sinf`, `sqrtf`, `floorf` and friends are x87 library calls. Hot loops use
  the fast versions in `content/fast_math.h` and `audio/synth.h`.
