# XenoScript

XenoScript is a strongly-typed, bytecode-compiled scripting language built for secure, controlled modding environments.

It runs entirely inside a custom virtual machine and has **no direct access to the host system unless explicitly exposed by the developer**.

XenoScript is being developed for use in a custom game engine and is designed from the ground up to provide safe, predictable, and high-performance modding.

---

## ✨ Design Goals

- Strong static typing
- Deterministic bytecode execution
- Explicit module linking
- Controlled host capability exposure
- No implicit system access
- Predictable runtime behavior

XenoScript is not intended to be a general-purpose replacement for existing languages.  
It is purpose-built for embedding.

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

- Classes, interfaces, enums
- Generics (`List<T>`, `Dictionary<K, V>`)
- Access modifiers (`public`, `private`, `protected`)
- Static classes and static functions
- Strongly-typed casting (`as` throws on failure)
- Type checks (`is`, `typeof`)
- Match expressions
- Structured control flow (`if`, `for`, `foreach`)
- String interpolation
- Module imports
- Bytecode compilation with linker stage

---

## 📦 Imports & Modules

```xeno
import <System>
import "local.xeno"
```

- `<System>` imports standard library modules
- `"local.xeno"` imports project-local files (resolved from project root)
- Circular imports are not allowed

Standard libraries are compiled to bytecode and linked during build.

---

## 🛠️ Building

Using Make:

```
make
```

Compile a script:

```
./bin/xenoc examples/example.xeno
```

Dump bytecode:

```
./bin/xenoc examples/example.xeno --dump
```

---

## 📁 Repository Structure

```
source/             Compiler and VM source
includes/           Shared headers
examples/           Example XenoScript files
bin/                Compiled binaries
build/              Intermediate build files (ignored)
xenoscript-vscode/  VS Code extension
```

---

## 🎮 Intended Use

XenoScript is designed for:

- Game engines
- Modding platforms
- Controlled runtime scripting
- Embedded VM environments

The host defines what scripts are allowed to do.

---

## 🚧 Project Status

Actively in development.

The language, VM, and standard library are evolving together.

---

## 📜 License

XenoScript Community & Commercial License (XCCL)
Version 1.0
Copyright (c) 2026 DoItBetter Studio
All Rights Reserved.

1. Ownership

XenoScript, including its compiler, virtual machine, standard libraries,
and associated tooling (collectively, the "Software"), is the exclusive
property of DoItBetter Studio.

This license grants limited rights to use the Software under the terms below.
All rights not expressly granted are reserved.

---

2. Non-Commercial Use

You are granted a non-exclusive, non-transferable, revocable license to:

- Use the Software for personal, educational, hobby, or non-commercial projects.
- Modify the Software for internal use.
- Distribute games or applications created using the Software,
  provided they are not sold or monetized.

No fee is required for non-commercial use.

---

3. Commercial Use

A Commercial License from DoItBetter Studio is required if:

- You sell, license, or otherwise monetize a product that includes or depends
  on XenoScript.
- The Software is used in a product that generates revenue.

Commercial terms, pricing, and agreements are provided separately
by DoItBetter Studio.

---

4. Modding Access Requirement

If you distribute a product using XenoScript:

- You may not require an additional fee, subscription, DLC purchase,
  or other monetary charge to enable scripting or modding functionality
  powered by XenoScript.
- Access to XenoScript-based modding features must not be artificially
  restricted behind a paywall separate from the base product.

You may charge for your game.
You may not charge users specifically to unlock XenoScript modding.

---

5. Redistribution

You may not:

- Redistribute the Software source code.
- Rebrand the Software.
- Claim authorship of the Software.
- Sell the Software independently.

Distribution of compiled products that embed the Software is permitted
under the terms of this license.

---

6. Warranty Disclaimer

The Software is provided "AS IS", without warranty of any kind,
express or implied, including but not limited to the warranties
of merchantability, fitness for a particular purpose, and noninfringement.

In no event shall DoItBetter Studio be liable for any claim, damages,
or other liability arising from the use of the Software.

---

7. Termination

Failure to comply with the terms of this license automatically
terminates your rights under it.

Upon termination, you must cease use of the Software.

---

8. Contact

For commercial licensing inquiries, contact:
sportsnut2020@gmail.com

---