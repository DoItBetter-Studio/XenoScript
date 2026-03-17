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

- Classes, single inheritance, interfaces, enums  
- **Erased generics** (`List<T>`, `Dictionary<K, V>`, user-defined generic classes) — distributed as compiled `.xar` binaries, no source shipping required  
- Access modifiers (`public`, `private`, `protected`)  
- Virtual dispatch (`virtual` / `override`)  
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
- Project model (`xeno.project`) with dependency resolution  
- Bytecode compilation (XBC v16) with linker stage  

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

### Standard Library Packages

| Package | Contents |
|--------|----------|
| `<core>` | `Exception`, `Attribute`, `IEnumerable<T>`, `IEnumerator<T>`, `string`, `int`, `float`, `bool` helpers |
| `<math>` | `Math` static class |
| `<collections>` | `List<T>`, `Dictionary<K,V>`, `Stack<T>`, `Queue<T>` |

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

### `xar` — Package Tool

Pack a directory into a `.xar` archive:
```
./bin/xar pack src/ -o output.xar -n name -v 1.0.0
```

### `xenolsp` — Language Server

Run the LSP server for IDE integration:
```
./bin/xenolsp
```

### Building from Source

```
make
```

### Rebuilding stdlib from Source

```
make stdlib
```

### Running the test suite

```
make xeno_tests
```

---

## 📁 Repository Structure

```
source/             Compiler and VM source
  core/             Lexer, parser, checker, bytecode
  compiler/         xenoc compiler, XBC serialization, XAR packer
  vm/               xenovm virtual machine
  stdlib/           Core stdlib bootstrap sources (embedded)
  tools/            Test runner and utilities
  lsp/              xenolsp language server
includes/           Shared headers
stdlib/             Example game-extended stdlib sources
  core/             Exception, Attribute, IEnumerable, primitives
  math/             Math
  collections/      List, Dictionary, Stack, Queue
build/              Intermediate build files (XARs, object files)
bin/                Compiled binaries
test/               Language test suite (27 tests)
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

[dependencies]
utilities = "v3.5.1"
playerInteractions = "v1.2.0"
```

The compiler resolves dependencies from `.xar` archives and links them at build time.

XenoScript uses a layered standard library model:

- **Core standard library** — foundational language features (`core`, `math`, `collections`).  
  This layer is shared across games and embedded into the compiler, VM, packer, and LSP.

- **Game-extended standard library** — additional `.xeno` files written by the game developer.  
  Each game builds its own customized XenoScript toolchain with the extended stdlib embedded directly into the binaries (xenoc, xenovm, xar, xenolsp), defining engine APIs, events, host-facing types, and helpers. This creates a game-specific XenoScript dialect where game APIs are available via standard `<name>` imports, but embedded in the toolchain rather than distributed as external `.xar` files.

- **External mod packages** — community or project-local `.xar` files.  
  These live outside the game (for example, in a `/mods` directory) and are resolved from
  the project's dependency list. Mods are never embedded into the game; they remain external.

Every mod must define an entrypoint class annotated with `@Mod`.  
The current XenoScript toolchain (compiler, VM, and packer) is configured to look for
this specific annotation name when identifying a mod's entrypoint.

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

This structure enables the VS Code extension to automatically detect the LSP server and game-specific stdlib. However, the extension is designed to be customizable — developers can configure paths to match their project's needs.

The game loads mods from the `mods/` directory. Directories are treated as development projects (enabling faster iteration with hot-reloading), while `.xar` files are compiled packages.

### VS Code Extension Configuration

The VS Code extension currently uses a static LSP path but is intended to be game-aware. For now:

- Install the extension from source or marketplace  
- Configure the LSP server path in VS Code settings if your game uses a non-standard location  
- The extension provides syntax highlighting, basic diagnostics, and will gain full IntelliSense as LSP features mature  

Future versions will automatically detect game environments and load appropriate stdlibs.

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
- ✅ `.xar` packaging and project model  
- ✅ Attribute reflection (reading annotation data at runtime)  
- ✅ LSP server (`xenolsp`) for IDE integration  
- ✅ VS Code extension with diagnostics, hover, and completions  
- ✅ 27-test suite passing  

**Partially implemented / in progress:**
- 🔄 LSP server: Basic diagnostics working; go-to-definition, find-references, and full IntelliSense autocomplete are under development  
- 🔄 VS Code extension: Error squigglies appear at line start (not precise positioning); extension detects static LSP path but may need customization for game-specific stdlibs  

**Planned:**
- 🔲 Enhanced LSP features (precise error positioning, full symbol navigation)  
- 🔲 Game-aware VS Code extension configuration for dynamic stdlib detection  

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
