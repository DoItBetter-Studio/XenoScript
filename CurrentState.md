## The Toolchain
- `xenoc` — compiler, takes `.xeno` source or a `xeno.project` file, outputs `.xbc` bytecode
- `xenovm` — virtual machine, runs `.xbc` files or `.xar` archives
- `xar` — packer, bundles compiled XenoScript into distributable `.xar` archives

## The Language
- Statically typed, class-based OOP with single inheritance
- Primitive types: `int`, `long`, `float`, `double`, `bool`, `string`, `char`, `byte`, `sbyte`, `short`, `ushort`, `uint`, `ulong`
- Arrays, including typed arrays(`int[]`, `string[]`, etc.)
- `public` / `private` / `protected` access modifiers
- `static` members, `static final` constants
- `final` instance fields with definite-assignment analysis (checker enforces every `final` field is assigned before the constructor exists)
- Interfaces with implementation checking
- Generics (`class Foo<T>`)
- Enums with `match` expressions
- Constructor overloading with default parameter values
- `is` / `as` casting
- `import` for multi-file projects

## The Attribute System
- Generic annotations on classes and methods — `@AnyAttribute(args)`
- Annotation arguments can be: string/int/float/bool literals, enum members, static field references, or arrays of any of the above
- Enum-keyed event attributes: `@PlayerEvent(PlayerEvent.Join)`
- `@Mod("name", "version", "author", "description")` — entry point declaration
- Attributes are baked into XNC at compile time, readable by the host at load time with zero runtime overhead

## The Stdlib (pure XenoScript, distributable as `.xar`)
- `core.xar` — `Attribute`, `AttributeUsage`, `AttributeTarget`, `Mod`, plus `int` / `float` / `bool` / `string` extension methods
- `math.xar` — `Math` class
- `collections.xar` — `List<T>`, `Dictionary<K, V>`, `Queue<T>`, `Stack<T>`

## The Project System
- `xeno.project` TOML file declares name, version, entry point, source files, dependencies
- `xenoc build` resolves deps from `.xar` files, compiles, strips stdlib before packing
- Dependency classes are type-checked at compile time even without source

## The Bytecode Format
- XBC v13, binary, platform-independent
- Per-class: fields (with `is_statis`, `is_final`), methods (with return types, attributes), class-level attributes, parent index, constructor index
- Static field initializer values baked in
- Enum member name tables for `toString`

## What the host gets
- At load time: `ModMetadata` (name/version/author/description derived from `@Mod`)
- Full `ClassDef` / `MethodDef` / `FieldDef` tables with all attribute instances
- `MathodDef.attributes[]` — the event dispatch table. A host iterates the entry class's methods, finds `@PlayerEvent(PlayerEvent.Join)`, reads `args[0].i == 0`, and wires it up. All static, no reflections at runtime.