# XenoScript

XenoScript is a strongly-typed, bytecode-compiled scripting language built for secure, controlled modding environments.

It runs entirely inside a custom virtual machine and has **no direct access to the host system unless explicitly exposed by the developer**.

XenoScript is being developed for use in a custom game engine and is designed from the ground up to provide safe, predictable, and high-performance modding.

---

## ✨ Design Goals

XenoScript is not intended to be a general-purpose replacement for existing languages.  
It is a focused, domain-specific language engineered for one purpose: **safe, deterministic, and accessible modding**.

Only the virtual machine is embedded into the host application.  
Scripts, packages, and projects remain fully external — loaded from the filesystem, sandboxed, and isolated from the host unless explicitly granted capabilities. This separation is intentional: it ensures that modding remains open, transparent, and under the control of both the developer and the community, not hidden inside the engine or locked behind proprietary systems.

XenoScript's design emphasizes:

- **Strong static typing** for clarity and safety  
- **Deterministic bytecode execution** for predictable behavior  
- **Explicit module linking** to avoid hidden dependencies  
- **Controlled capability exposure** so the host decides what scripts can do  
- **No implicit system access** — nothing is allowed unless explicitly granted  
- **Predictable runtime behavior** through a minimal, well-defined VM  

XenoScript is built for environments where trust, safety, and determinism matter more than raw power.  
It is a language for creators, modders, and developers who want expressive scripting without sacrificing control or security.

---

## 🔐 Security Model

XenoScript runs inside a dedicated virtual machine.

By default, scripts:

- ❌ Cannot access the file system  
- ❌ Cannot perform HTTP requests  
- ❌ Cannot execute native code  
- ❌ Cannot access the host machine  

Capabilities must be explicitly exposed by the host application.

If a feature is not allowed, it simply does not exist in the VM.

This makes XenoScript ideal for secure modding environments.

---

## 🧠 Language Features

- Primitive types (`int`, `long`, `float`, `double`, `bool`, `string`, `char`, `byte`, `sbyte`, `short`, `ushort`, `uint`, `ulong`), arrays, classes, single inheritance, interfaces, and enums
- **Erased generics** (`List<T>`, `Dictionary<K, V>`, user-defined generic classes) — distributed as compiled `.xar` binaries, no source shipping required  
- Access modifiers (`public`, `private`, `protected`)  
- Virtual dispatch (`virtual` / `override`)  
- Constructor overloading and default parameter values
- Enum `match` expressions
- Nullable types (`string?`, `int?`, `??`, `!`, `?.`)  
- Exception handling (`try` / `catch` / `finally` / `throw`)  
- Events with static and **bound delegate** handlers (`EventName += this.method`)  
- `foreach` over arrays and any `IEnumerable<T>` implementation  
- Static fields and methods  
- `final` fields (compile-time immutability)  
- User-defined annotations with `@AttributeUsage` enforcement  
- Strongly-typed casting (`as` throws on failure)  
- Type checks (`is`, `typeof`)  
- Structured control flow (`if`, `for`, `foreach`, `while`)  
- String interpolation  
- Module imports and `.xar` package archives  
- Project model (`xeno.project`) with dependency resolution and SemVer matching  
- Bytecode compilation (XBC v19) with linker stage
- API documentation via **XDocs** (`.xdoc` sidecars) for hover, stubs, and parameter names

---

## 📦 Imports & Modules

```xeno
import <core>
import <collections>
import "local_file.xeno"
```

- `<name>` imports a package from the global package cache
  (core stdlib, game-extended stdlib, or any resolved dependency `.xar`)  
- `"file.xeno"` imports project-local files (resolved from project root)  
- Circular imports are not allowed  

The standard library — including both the core language packages and the game's extended API — is embedded into the compiler, VM, packer, and LSP.
External packages are resolved from `.xar` archives at build time and loaded into the global package cache.

### API Documentation (XDocs)

Document declarations in `.xeno` source with `#Docs` blocks:

```xeno
/* #Docs
 * # title: add
 * # summary: "Append an item to the list."
 * # param: $item "Element to append."
 * # returns: "void"
 * #!Docs
 */
function add(T item): void {
    // ...
}
```

Supported tags: `title`, `summary`, `description`, `param`, `returns`, `throws`, `example`, `see`, `link`, `since`, `deprecated`, `note`, `warning`, `important`, `remarks`.

Use `$name` in `# param:` / `# throws:` so stubs and hover show real parameter names instead of `arg0`, `arg1`, …

Docs bind to the **next declaration**. If `# title:` does not match that declaration’s name, the LSP warns and still binds. A `#Docs` block with no following declaration is dropped.

When `xar pack` packages documented source files, it writes a sibling `.xdoc` sidecar next to the `.xar`. XDocs are IDE-only metadata and are **not** included in the runtime package.

#### Dependency XDocs

To make a packaged dependency’s XDocs available to the LSP:

1. Add `#Docs` blocks to the dependency’s `.xeno` declarations.
2. Pack the source into the project’s dependency directory. For example, for a dependency named `utilities`:

   ```sh
   ./bin/xar pack utilities/src/ -o my_mod/deps/utilities.xar -n utilities -v 1.0.0
   ```

   The packer writes `my_mod/deps/utilities.xdoc` alongside the archive when the source contains documentation.
3. Declare the dependency in `my_mod/xeno.project`:

   ```toml
   [dependencies]
   utilities = "1.0.0"
   ```

The dependency key (`utilities`) must match both filenames: `deps/utilities.xar` and `deps/utilities.xdoc`. The LSP reads the sidecar for each declared dependency; there is no separate doc-store registration step. If `[project] deps_dir` is configured, put both files in that directory instead of `deps/`.

#### Standard-library XDocs

XDocs for the embedded standard library (`core`, `math`, `collections`) are produced when you run `make stdlib` and are loaded by `xenolsp` from the embedded blobs (with an optional disk fallback under the toolchain root). Hover, go-to-definition stubs, and parameter names for stdlib APIs work when the language server binary is the one built with those embeds.

### Standard Library Packages

| Package | Contents |
|--------|----------|
| `<core>` | `Exception`, `Attribute`, `IEnumerable<T>`, `IEnumerator<T>`, `string`, `int`, `float`, `bool` helpers |
| `<math>` | `Math` static class |
| `<collections>` | `List<T>`, `Dictionary<K,V>`, `Stack<T>`, `Queue<T>` |

**Note:** Floating-point values (`float` and `double`) are rounded to 4 decimal places for deterministic behavior and to prevent floating-point precision issues in modding environments.

---

## 🛠️ Toolchain

### `xenoc` — Compiler

Compile a single file:
```
./bin/xenoc source.xeno -o output.xbc
```

Dump bytecode disassembly:
```
./bin/xenoc source.xeno --dump
```

Build a project:
```
./bin/xenoc build [project-dir] -o output.xar
```

### `xenovm` — Virtual Machine

Run compiled bytecode:
```
./bin/xenovm output.xbc
```

Run a `.xar` package:
```
./bin/xenovm output.xar
```

Run a project directory (source + `xeno.project`):
```
./bin/xenovm path/to/project/
```

### `xar` — Package Tool

Pack a directory into a `.xar` archive (and sibling `.xdoc` when sources contain `#Docs`):
```
./bin/xar pack src/ -o output.xar -n name -v 1.0.0
```

### `xenolsp` — Language Server

Run the LSP server for IDE integration:
```
./bin/xenolsp
```

Supported LSP features:

- Live diagnostics (parse + type check) with accurate line/column ranges  
- Hover (types + XDocs markdown when available)  
- Go-to-definition (project sources and generated stubs for `.xar` classes)  
- Find references  
- Custom `xeno/getStub` for read-only stub views of archived classes  

### Building from Source

```
make
```

### Rebuilding stdlib from Source

```
make stdlib
```

Rebuilds stdlib `.xar` / `.xdoc` packages and re-embeds them into the toolchain binaries.

### Running the test suite

```
make xeno_tests
```

---

## 📁 Repository Structure

```
source/             Compiler and VM source
  core/             Lexer, parser, checker, bytecode
  compiler/         xenoc compiler, XBC serialization, XDocs, module merge
  vm/               xenovm virtual machine
  tools/            Test runner and utilities
  lsp/              xenolsp language server
includes/           Shared headers
stdlib/             Standard library sources (packed + embedded)
  core/             Exception, Attribute, IEnumerable, primitives
  math/             Math
  collections/      List, Dictionary, Stack, Queue
build/              Intermediate build files (XARs, XDocs, object files)
bin/                Compiled binaries
test/               Language test suite
xenoscript.vscode-xenoscript/  VS Code extension
```

---

## 🧩 Project Model

A `xeno.project` file defines a project:

```toml
[project]
name = "my_mod"
version = "1.0.0"
description = "My mod"
# deps_dir = "deps"   # optional; default search includes deps/, dependencies/, libraries/, xars/

[dependencies]
utilities = "1.0.0"           # exact
playerInteractions = "^1.2.0"   # compatible (>=1.2.0, <2.0.0)
sharedLib = "~1.4.0"          # approximate (>=1.4.0, <1.5.0)
toolkit = ">=1.0.0"           # minimum
optionalExtra = "*"           # any version
```

Dependency versions use **SemVer** (optional leading `v`/`V` is accepted). Supported constraints:

| Constraint | Meaning |
|------------|---------|
| `1.2.3` | Exact match |
| `^1.2.3` | Compatible: `>=1.2.3` and `<2.0.0` |
| `~1.2.3` | Approximate: `>=1.2.3` and `<1.3.0` |
| `>=1.2.3` | Minimum version |
| `*` or empty | Any version |

The compiler resolves dependencies from `.xar` archives, checks the archive version against the constraint, and links them at build time. At runtime, `xenovm` loads declared dependency `.xar` files from the same directory as the mod package (or from the project’s dependency directory when running from source).

XenoScript uses a layered standard library model:

- **Core standard library** — foundational language features (`core`, `math`, `collections`).  
  This layer is shared across games and embedded into the compiler, VM, packer, and LSP.

- **Game-extended standard library** — additional `.xeno` files written by the game developer.  
  Each game builds its own customized XenoScript toolchain with the extended stdlib embedded directly into the binaries (xenoc, xenovm, xar, xenolsp), defining engine APIs, events, host-facing types, and helpers. This creates a game-specific XenoScript dialect where game APIs are available via standard `<name>` imports, but embedded in the toolchain rather than distributed as external `.xar` files.

- **External mod packages** — community or project-local `.xar` files.  
  These live outside the game (for example, in a `/mods` directory) and are resolved from
  the project's dependency list. Mods are never embedded into the game; they remain external.

Every mod must define an entrypoint class annotated with `@Mod`.  
The toolchain looks for this annotation when identifying a mod’s entrypoint and verifies the id against the project name when building.

The `@Mod` attribute itself is defined in the game-extended standard library as a normal
XenoScript class derived from `Attribute`. The attribute supports both positional and
named arguments, with defaults provided by the game's implementation. Developers may
extend or modify the fields of this attribute as needed. While the structure of the
attribute is customizable, the annotation name (`@Mod`) is the default entrypoint
mechanism unless the toolchain is intentionally modified.

#### Positional arguments

```xeno
@Mod("my_mod", "1.0.0", "Author", "Description")
class MyMod {
    public:
        MyMod() {
            // entry point
        }
}
```

#### Named arguments (optional)

```xeno
@Mod("my_mod", version="1.0.0")
class MyMod {
    public:
        MyMod() {
            // entry point
        }
}
```

### Exposing C host functions to scripts

Host functions are the explicit bridge from XenoScript into the game. Register them on the VM before loading its stdlib or compiling/running scripts that call them. Use `xeno_register_fn_typed` so the compiler can check the script-visible parameter and return types:

```c
#include "vm.h"

static XenoResult game_add_one(XenoVM *vm, int argc, Value *argv, Value *out)
{
    if (argc != 1) {
        xeno_vm_error(vm, "game_add_one expects one argument");
        return XENO_RUNTIME_ERROR;
    }

    *out = xeno_int((int64_t)argv[0].i + 1);
    return XENO_OK;
}

static int register_game_api(XenoVM *vm)
{
    int param_types[] = { TYPE_INT };
    return xeno_register_fn_typed(
        vm, "game_add_one", game_add_one, TYPE_INT, 1, param_types);
}
```

The script can then call the registered name as a normal function:

```xeno
int answer = game_add_one(41);
```

Initialize the VM and register every host function before loading stdlib modules and running scripts:

```c
XenoVM vm;
xeno_vm_init(&vm);

if (register_game_api(&vm) < 0) {
    /* Handle a full host-function registry. */
}

xeno_vm_load_stdlib(&vm);
/* Compile/run scripts with this VM, then call xeno_vm_free(&vm). */
```

`TYPE_*` values come from the type definitions included by `vm.h`. Use `TYPE_VOID` for a function that has no return value (leave `out` untouched), and `TYPE_ANY` for a parameter that should accept any script value. Return `XENO_OK` on success; on runtime failure, set a useful message with `xeno_vm_error` and return `XENO_RUNTIME_ERROR`. Registration exposes a capability to scripts, so only register callbacks the game intends to make available and enforce game-specific permissions inside those callbacks.

---

## 🎮 Intended Use

XenoScript is designed for:

- Game engines  
- Modding platforms  
- Controlled runtime scripting  
- Embedded VM environments  

Each game defines its own XenoScript environment through its extended standard library and exposed capabilities.

---

## 🏗️ Game Development Setup

XenoScript treats its standard library as a **dialect foundation** rather than a fixed API. Game developers are expected to extend and customize the core stdlib (`core`, `math`, `collections`) with game-specific types, events, and APIs. This creates a layered stdlib model:

- **Core stdlib** (embedded in toolchain): Fundamental language features  
- **Game-extended stdlib** (embedded in customized toolchain): Game-specific additions, becoming part of the game's XenoScript dialect  
- **Mod packages**: Community or project-local extensions, distributed as `.xar` archives  

Each game builds its own version of the XenoScript toolchain (xenoc, xenovm, xar, xenolsp) with the extended stdlib embedded directly into the binaries, just like the core stdlib. This makes game APIs available via standard `<name>` imports without needing external `.xar` resolution.  

### Recommended Directory Structure

For optimal modding workflow, games should follow this structure:

```
game_root/
├── game.exe / game          # Main game executable (Windows/Linux)
├── tools/                    # XenoScript toolchain
│   ├── xenoc                 # Compiler
│   ├── xar                   # Package tool
│   └── xenolsp               # Language server
└── mods/                     # Mods directory
    ├── my_mod/               # Development mod project (directory)
    ├── another_mod.xar       # Compiled mod package
    └── utilities.xar         # Shared mod dependency
```

The game loads mods from the `mods/` directory. Directories are treated as development projects (enabling faster iteration with hot-reloading), while `.xar` files are compiled packages.

### VS Code Extension Configuration

The extension supports a configured language-server path (recommended for game-specific toolchains) and falls back to `XENOLSP`, a bundled binary, or `PATH` when unset.

Settings (under **XenoScript**):

| Setting | Purpose |
|--------|---------|
| `xenoscript.lspPath` | Path to `xenolsp` / `xenolsp.exe` |
| `xenoscript.lspPathWindows` / `xenoscript.lspPathLinux` | Optional platform-specific overrides |
| `xenoscript.trace.server` | LSP communication tracing |

Point `lspPath` at the **game’s** (or this repo’s) `xenolsp` binary so hover, diagnostics, and navigation use that binary’s embedded stdlib and XDocs.

#### Settings scope (important)

VS Code only applies settings from the scope that was actually opened:

| Where you put the setting | When it applies |
|---------------------------|-----------------|
| **User settings** | Always, for every folder and workspace |
| **`.vscode/settings.json`** in the opened folder | When you open that folder |
| **`.code-workspace` `settings`** | Only when you open that workspace file (“Open Workspace from File…”) |

Opening a folder without opening the matching `.code-workspace` file will **ignore** workspace-file settings. If diagnostics, hover, or XDocs seem missing, confirm that `xenoscript.lspPath` is set in a scope that is active for the current window.

**Recommended for daily use — User settings:**

```json
{
  "xenoscript.lspPath": "D:/Projects/XenoScript/bin/xenolsp.exe"
}
```

**Or per-mod project** (`.vscode/settings.json` inside the mod folder):

```json
{
  "xenoscript.lspPath": "D:/Projects/XenoScript/bin/xenolsp.exe"
}
```

**Workspace file example** (only applies when that workspace is opened):

```json
{
  "folders": [
    { "path": "." }
  ],
  "settings": {
    "xenoscript.lspPath": "D:/Projects/XenoScript/bin/xenolsp.exe"
  }
}
```

Once `lspPath` points at a correctly built `xenolsp`:

- Live diagnostics, hover (including XDocs), go-to-definition, and references work for project sources  
- Stdlib symbols and their XDocs come from the embedded toolchain  
- Dependency classes resolve via project `deps/*.xar` and XDocs via sibling `deps/*.xdoc`  

---

## 🚧 Project Status

Actively in development.

The language, VM, and standard library are evolving together.

**Implemented:**
- ✅ Full type system (primitives, classes, interfaces, enums, generics)  
- ✅ Virtual dispatch (`virtual` / `override`)  
- ✅ Nullable types (`string?`, `int?`, `??`, `!`, `?.`)  
- ✅ Exception handling (`try` / `catch` / `finally` / `throw`)  
- ✅ Events with static and bound delegate handlers  
- ✅ `foreach` over arrays and `IEnumerable<T>` implementations  
- ✅ Annotations and `@AttributeUsage`  
- ✅ Standard library (`core`, `math`, `collections`)  
- ✅ Erased generics — stdlib generic classes distributed as compiled `.xar` binaries  
- ✅ `.xar` packaging and project model with dependency resolution  
- ✅ SemVer dependency constraints (`exact`, `^`, `~`, `>=`, `*`)  
- ✅ `@Mod` entrypoint detection and project id verification  
- ✅ Attribute reflection (reading annotation data at runtime)  
- ✅ LSP server (`xenolsp`): live diagnostics, hover, go-to-definition, find-references  
- ✅ Diagnostic ranges with accurate line and column spans  
- ✅ XDocs: `#Docs` extraction, `.xdoc` sidecars, hover markdown, named params in stubs  
- ✅ XDocs for project dependencies (`deps/*.xdoc`) and embedded standard-library XDocs in `xenolsp`  
- ✅ VS Code extension: syntax highlighting, diagnostics, hover, navigation, configurable `lspPath`  
- ✅ Language test suite  

**Known limitations / planned:**
- 🔲 Completion provider  
- 🔲 Automatic game-environment / toolchain detection in the extension  

---

## 📜 License

# XenoScript Community & Commercial License (XCCL)  
**Version 1.1**  
Copyright (c) 2026  
DoItBetter Studio  
All Rights Reserved  

---

## 1. Ownership

XenoScript, including its compiler, virtual machine, standard libraries, extended standard libraries, and associated tooling (collectively, the "Software"), is the exclusive property of DoItBetter Studio.  

This license grants limited rights to use the Software under the terms below. All rights not expressly granted are reserved.

---

## 2. Non-Commercial Use

You are granted a non-exclusive, non-transferable, revocable license to:

- Use the Software for personal, educational, hobby, or non-commercial projects.  
- Modify the Software for internal use.  
- Distribute games or applications created using the Software, provided they are not sold or monetized.  

No fee is required for non-commercial use.

---

## 3. Commercial Use

A Commercial License from DoItBetter Studio is required if:

- You sell, license, or otherwise monetize a product that includes or depends on XenoScript.  
- The Software is used in a product that generates revenue.  

Commercial terms, pricing, and agreements are provided separately by DoItBetter Studio.

---

## 4. Modding Access Requirement

If you distribute a product using XenoScript:

- You may not require an additional fee, subscription, DLC purchase, or other monetary charge to enable scripting or modding functionality powered by XenoScript.  
- Access to XenoScript-based modding features must not be artificially restricted behind a paywall separate from the base product.  

You may charge for your game, but you may not charge users specifically to unlock XenoScript modding.

---

## 5. Redistribution

**You may:**

- Clone, fork, modify, and build the Software from source for personal, educational, hobby, or internal use.  
- Distribute products (games, tools, applications, or mods) that embed or depend on the XenoScript virtual machine, compiler, XAR packer, standard library, extended standard library, or compiled bytecode. This is the normal and intended use of the Software.  

**You may not:**

- Redistribute the Software, in whole or in part, in source or binary form as a standalone product.  
- Distribute modified versions of the Software under a different name or brand.  
- Claim authorship of the Software or present it as your own work.  
- Sell, license, or otherwise monetize the Software independently of a larger product that embeds or depends on it.  

**Note:** Embedding the XenoScript virtual machine, compiler, XAR packer, or standard library into a product **does not** count as redistribution under this section.

---

## 6. Warranty Disclaimer

The Software is provided "AS IS," without warranty of any kind, express or implied, including but not limited to the warranties of merchantability, fitness for a particular purpose, and noninfringement.  

In no event shall DoItBetter Studio be liable for any claim, damages, or other liability arising from the use of the Software.

---

## 7. Termination

Failure to comply with the terms of this license automatically terminates your rights under it.  

Upon termination, you must cease use of the Software.

---

## 8. Contact

For commercial licensing inquiries, contact:  
sportsnut2020@gmail.com
