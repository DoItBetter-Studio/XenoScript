/*
 * bytecode.h — Value representation, opcodes, and the Chunk (bytecode buffer)
 *
 * This is the contract between the compiler and the VM. The compiler emits
 * Chunks; the VM executes them. Neither needs to know how the other works
 * internally — they only share this header.
 *
 * DESIGN PRINCIPLE: Static typing means the compiler encodes type information
 * INTO the instruction. ADD_INT and ADD_FLOAT are different opcodes. The VM
 * never inspects a value's type at runtime — the instruction itself IS the
 * type decision. This makes the VM fast and simple.
 */

#ifndef BYTECODE_H
#define BYTECODE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ═════════════════════════════════════════════════════════════════════════════
 * CLASS DEFINITION (compile-time metadata, lives at runtime too)
 *
 * ClassDef is built by the compiler and embedded in the Module. At runtime
 * the VM uses it to allocate objects and resolve field indices.
 *
 * Fields are stored by index — `obj->fields[2]` is the third field declared
 * in the class. The compiler resolves field names to indices at compile time
 * so field access at runtime is just an array lookup with no name lookup.
 * ═════════════════════════════════════════════════════════════════════════════*/
#define CLASS_MAX_FIELDS      64
#define CLASS_MAX_METHODS     64
#define CLASS_MAX_EVENTS      16
#define CLASS_NAME_MAX        64
#define FIELD_NAME_MAX        64
#define CLASS_MAX_ATTRIBUTES  8
#define ATTR_MAX_ARGS         16

/* ── Compile-time attribute argument value ──────────────────────────────── */
typedef enum {
    ATTR_ARG_STRING = 0,
    ATTR_ARG_INT,
    ATTR_ARG_FLOAT,
    ATTR_ARG_BOOL,
    ATTR_ARG_ARRAY,          /* nested AttrArg[] — for [Target.Class, ...] */
} AttrArgKind;

typedef struct AttrArg AttrArg;
struct AttrArg {
    AttrArgKind kind;
    union {
        char   *s;            /* ATTR_ARG_STRING — heap-allocated            */
        __int128_t i;         /* ATTR_ARG_INT (includes enum values)         */
        double  f;            /* ATTR_ARG_FLOAT                              */
        bool    b;            /* ATTR_ARG_BOOL                               */
        struct {
            AttrArg *elems;   /* heap-allocated                              */
            int      count;
        } arr;                /* ATTR_ARG_ARRAY                              */
    };
};

/* ── One applied attribute instance ─────────────────────────────────────── */
typedef struct {
    char    class_name[CLASS_NAME_MAX]; /* e.g. "Mod", "AttributeUsage"     */
    AttrArg args[ATTR_MAX_ARGS];
    int     arg_count;
} AttributeInstance;

/* Value and XenoObject need to be declared before ClassDef because
 * ClassDef contains a Value array (static_values). */
typedef struct XenoObject XenoObject;
typedef struct XenoArray  XenoArray;
typedef struct XenoType   XenoType;


/* Value: tagged union representing any XenoScript runtime value.
 * is_null == 1 means this is the null value regardless of the union contents.
 * This gives us an unambiguous null distinct from 0, false, 0.0, etc. */
typedef struct Value {
    uint8_t      is_null;   /* 1 = null literal, 0 = real value */
    union {
        __int128_t   i;
        double       f;
        bool         b;
        char        *s;
        XenoObject  *obj;
        XenoArray   *arr;
        XenoType    *type;
    };
} Value;

static inline Value val_int  (__int128_t  v) { Value r; r.is_null=0; r.i   = v; return r; }
static inline Value val_float(double      v) { Value r; r.is_null=0; r.f   = v; return r; }
static inline Value val_bool (bool        v) { Value r; r.is_null=0; r.b   = v; return r; }
static inline Value val_str  (char       *v) { Value r; r.is_null=0; r.s   = v; return r; }
static inline Value val_obj  (XenoObject *v) { Value r; r.is_null=0; r.obj = v; return r; }
static inline Value val_arr  (XenoArray  *v) { Value r; r.is_null=0; r.arr = v; return r; }
static inline Value val_type (XenoType   *v) { Value r; r.is_null=0; r.type= v; return r; }
/* Null: is_null flag set, union cleared */
static inline Value val_null (void)          { Value r; r.is_null=1; r.i   = 0; return r; }
static inline bool  is_val_null(Value v)     { return v.is_null != 0; }

/* XenoArray: heap-allocated homogeneous array.
 * Defined after Value so the flexible elements[] member works. */
typedef struct XenoArray {
    int     length;
    uint8_t elem_kind;  /* TypeKind of elements — set at allocation, used by OP_CMP_EQ_VAL */
    Value   elements[];
} XenoArray;

/* XenoType: runtime type descriptor returned by typeof().
 * Minimal by design — just enough for diagnostics and scripting. */
typedef struct XenoType {
    const char       *name;       /* e.g. "int", "MyMod", "ushort[]", "Phase" */
    bool              is_array;
    bool              is_primitive;
    bool              is_enum;
    bool              is_class;
    const struct ClassDef *class_def; /* non-NULL for class/enum types -- enables attribute reflection */
} XenoType;


typedef struct {
    char     name[FIELD_NAME_MAX];
    int      type_kind;    /* TypeKind of this field                        */
    char     class_name[CLASS_NAME_MAX]; /* if type_kind == TYPE_OBJECT     */
    bool     is_static;
    bool     is_final;
    bool     is_nullable;  /* true if declared with ?, e.g. string?         */
    int      instance_slot; /* For instance fields: slot in obj->fields[].
                             * For static fields: -1 (use static_values[]) */
} FieldDef;

#define METHOD_MAX_PARAMS 16
#define METHOD_PARAM_CLASS_MAX 32

typedef struct {
    char     name[FIELD_NAME_MAX];
    int      fn_index;     /* Index into Module->chunks                     */
    bool     is_static;
    bool     is_virtual;   /* true if declared virtual in parent            */
    int      return_type_kind;              /* TypeKind of return value      */
    bool     return_is_nullable;            /* true if return type is T?     */
    char     return_class_name[CLASS_NAME_MAX]; /* if TYPE_OBJECT            */
    /* Parameter signature — stored here so stubs can be generated from
     * the ClassDef alone without access to Chunk internals. v17+ */
    int      param_count;
    int      param_type_kinds[METHOD_MAX_PARAMS];
    char     param_class_names[METHOD_MAX_PARAMS][METHOD_PARAM_CLASS_MAX];
    bool     param_is_nullable[METHOD_MAX_PARAMS];
    AttributeInstance *attributes;          /* heap-allocated, may be NULL  */
    int                attribute_count;
} MethodDef;

/* EventDef — describes a declared event and its parameter signature.
 * param_type_kinds[i] / param_class_names[i] describe each parameter.
 * At runtime, each event has a handler list stored separately (not here). */
#define EVENT_MAX_PARAMS  8
#define EVENT_CLASS_NAME_MAX 32
typedef struct {
    char name[FIELD_NAME_MAX];
    int  param_count;
    int  param_type_kinds[EVENT_MAX_PARAMS];
    char param_class_names[EVENT_MAX_PARAMS][EVENT_CLASS_NAME_MAX];
    bool param_is_nullable[EVENT_MAX_PARAMS];
} EventDef;

typedef struct ClassDef {
    char       name[CLASS_NAME_MAX];
    int        field_count;
    FieldDef   fields[CLASS_MAX_FIELDS];
    int        method_count;
    MethodDef  methods[CLASS_MAX_METHODS];
    int        event_count;
    EventDef   events[CLASS_MAX_EVENTS];
    int        constructor_index; /* fn_index of constructor, or -1         */
    int        parent_index;      /* ClassDef index of parent, or -1        */

    /* Static field storage — one Value slot per field marked is_static.
     * static_values[i] corresponds to fields[i] when fields[i].is_static.
     * Non-static field indices are unused here (left zero). */
    Value      static_values[CLASS_MAX_FIELDS];

    /* Enum member name table — populated only for enum ClassDefs.
     * enum_member_names[i] is the string name of the i-th member. */
    bool        is_enum;
    int         enum_member_count;
    char       *enum_member_names[64];
    int         enum_member_values[64]; /* parallel array: integer value per member */

    /* Compile-time attributes applied to this class, e.g. @Mod(...) */
    AttributeInstance attributes[CLASS_MAX_ATTRIBUTES];
    int               attribute_count;

    /* Generic type parameter names, e.g. {"T"} for List<T>, {"K","V"} for Dict<K,V>.
     * Empty (type_param_count == 0) for non-generic classes.
     * Used by checker_declare_class_from_def to reconstruct TypeParamNodes
     * for classes loaded from .xar so generic substitution works correctly. */
    int  type_param_count;
    char type_param_names[8][8]; /* up to 8 params, each name up to 7 chars */

    /* Interface list — names of interfaces this class implements, including
     * any type argument suffix, e.g. "IEnumerable<int>", "IEnumerator<string>".
     * Populated by the compiler; serialized in XBC so xar-loaded classes
     * still expose their interface contracts to the checker. */
    bool is_interface;
    int  interface_count;
    char interface_names[8][64]; /* up to 8 interfaces, name+typearg up to 63 chars */
} ClassDef;


/* ═════════════════════════════════════════════════════════════════════════════
 * RUNTIME VALUE
 *
 * Value and XenoObject are defined above ClassDef because ClassDef embeds
 * a Value array. See the definitions near the top of this file.
 * ═════════════════════════════════════════════════════════════════════════════*/


/* ═════════════════════════════════════════════════════════════════════════════
 * XENOOBJECT — a heap-allocated class instance
 *
 * Every `new Foo()` allocates one of these. The `class_def` pointer ties it
 * back to its type, and `fields` is a flexible array of Values sized by
 * class_def->field_count at allocation time.
 *
 * Memory: allocated with malloc(sizeof(XenoObject) + field_count*sizeof(Value)).
 * Freed when the VM shuts down (no GC yet — acceptable for mod scripts that
 * run for a bounded time. GC is a future phase).
 * ═════════════════════════════════════════════════════════════════════════════*/
struct XenoObject {
    ClassDef *class_def;   /* Type info — NOT owned by this object          */
    Value     fields[];    /* Flexible array — sized at allocation           */
};


/* ═════════════════════════════════════════════════════════════════════════════
 * OPCODES
 *
 * Each opcode is one byte. Operands follow the opcode byte in the
 * instruction stream. The comment after each opcode shows:
 *   [operands]  —  stack effect (what it pops/pushes)
 *
 * Stack notation:  ( before -- after )
 *   LOAD_CONST_INT  ( -- int )       pushes an int
 *   ADD_INT         ( int int -- int ) pops two ints, pushes result
 *   POP             ( val -- )        discards top of stack
 * ═════════════════════════════════════════════════════════════════════════════*/
typedef enum {

    /* ── Constant loading ────────────────────────────────────────────────
     * These push a value from the constant pool onto the stack.
     * Operand: uint16_t index into the chunk's constant pool.
     *
     * LOAD_CONST_BOOL is special — the value (0 or 1) is encoded inline
     * in the single operand byte, avoiding a constant pool entry for
     * the two most common literal values. */
    OP_LOAD_CONST_INT,    /* [uint16_t idx]  ( -- int   ) */
    OP_LOAD_CONST_FLOAT,  /* [uint16_t idx]  ( -- float ) */
    OP_LOAD_CONST_BOOL,   /* [uint8_t  val]  ( -- bool  )  val: 0=false, 1=true */
    OP_LOAD_CONST_STR,    /* [uint16_t idx]  ( -- str   ) */

    /* ── Local variable access ───────────────────────────────────────────
     * Operand: uint8_t slot index in the current call frame.
     * Parameters occupy the first N slots (0..param_count-1).
     * Local variables occupy subsequent slots. */
    OP_LOAD_LOCAL,        /* [uint8_t slot]  ( -- val  ) push local[slot] */
    OP_STORE_LOCAL,       /* [uint8_t slot]  ( val -- ) pop → local[slot] */

    /* ── Integer arithmetic ──────────────────────────────────────────────
     * All pop two ints, push one int result. */
    OP_ADD_INT,           /* ( int int -- int ) */
    OP_SUB_INT,           /* ( int int -- int ) */
    OP_MUL_INT,           /* ( int int -- int ) */
    OP_DIV_INT,           /* ( int int -- int ) runtime error if divisor = 0 */
    OP_MOD_INT,           /* ( int int -- int ) runtime error if divisor = 0 */
    OP_NEGATE_INT,        /* ( int -- int )     unary minus */

    /* ── Float arithmetic ────────────────────────────────────────────────
     * All pop two floats, push one float result. */
    OP_ADD_FLOAT,         /* ( float float -- float ) */
    OP_SUB_FLOAT,         /* ( float float -- float ) */
    OP_MUL_FLOAT,         /* ( float float -- float ) */
    OP_DIV_FLOAT,         /* ( float float -- float ) */
    OP_MOD_FLOAT,         /* ( float float -- float ) */
    OP_NEGATE_FLOAT,      /* ( float -- float )       unary minus */

    /* ── String operations ───────────────────────────────────────────────*/
    OP_CONCAT_STR,        /* ( str str -- str )  string concatenation      */
    OP_TO_STR,            /* ( any     -- str )  convert any value to string */

    /* ── Boolean operations ──────────────────────────────────────────────*/
    OP_NOT_BOOL,          /* ( bool -- bool )    logical not */
    OP_AND_BOOL,          /* ( bool bool -- bool ) */
    OP_OR_BOOL,           /* ( bool bool -- bool ) */

    /* ── Integer comparisons — all push a bool ───────────────────────────*/
    OP_CMP_EQ_INT,        /* ( int int -- bool ) */
    OP_CMP_NEQ_INT,       /* ( int int -- bool ) */
    OP_CMP_LT_INT,        /* ( int int -- bool ) */
    OP_CMP_LTE_INT,       /* ( int int -- bool ) */
    OP_CMP_GT_INT,        /* ( int int -- bool ) */
    OP_CMP_GTE_INT,       /* ( int int -- bool ) */

    /* ── Float comparisons ───────────────────────────────────────────────*/
    OP_CMP_EQ_FLOAT,      /* ( float float -- bool ) */
    OP_CMP_NEQ_FLOAT,     /* ( float float -- bool ) */
    OP_CMP_LT_FLOAT,      /* ( float float -- bool ) */
    OP_CMP_LTE_FLOAT,     /* ( float float -- bool ) */
    OP_CMP_GT_FLOAT,      /* ( float float -- bool ) */
    OP_CMP_GTE_FLOAT,     /* ( float float -- bool ) */

    /* ── Bool / string equality ──────────────────────────────────────────*/
    OP_CMP_EQ_BOOL,       /* ( bool bool -- bool ) */
    OP_CMP_NEQ_BOOL,      /* ( bool bool -- bool ) */
    OP_CMP_EQ_STR,        /* ( str  str  -- bool ) */
    OP_CMP_NEQ_STR,       /* ( str  str  -- bool ) */

    /* ── Stack management ────────────────────────────────────────────────*/
    OP_POP,               /* ( val -- )  discard top of stack
                           * Used after expression statements where the
                           * result value is not needed. */

    /* ── Control flow ────────────────────────────────────────────────────
     * Operand: int16_t byte offset, relative to the instruction AFTER
     * the jump instruction itself. Positive = forward, negative = back.
     *
     * Using relative offsets (not absolute addresses) means the bytecode
     * is position-independent — you can load it anywhere in memory. */
    OP_JUMP,              /* [int16_t offset]  ( -- )      unconditional jump  */
    OP_JUMP_IF_FALSE,     /* [int16_t offset]  ( bool -- ) jump if top is false,
                           * always pops the bool regardless              */
    OP_JUMP_IF_TRUE,      /* [int16_t offset]  ( bool -- ) jump if top is true,
                           * always pops the bool regardless              */

    /* ── Function calls ──────────────────────────────────────────────────
     * OP_CALL: calls a script-defined function.
     *   Operand: uint8_t arg_count
     *   Before call: stack has [... arg0 arg1 ... argN]
     *   The function index is resolved by the compiler into a direct
     *   function table index, stored as a uint16_t before arg_count.
     *
     * OP_RETURN: returns the top-of-stack value to the caller.
     * OP_RETURN_VOID: returns from a void function (no value). */
    OP_CALL,              /* [uint16_t fn_idx][uint8_t argc]  ( args -- retval ) */
    OP_RETURN,            /* ( val -- )  return value to caller */
    OP_RETURN_VOID,       /* ( -- )      return from void function */

    OP_MATCH_FAIL,        /* ( -- )      runtime error: no match arm taken */

    /* Integer truncation opcodes: wrap arithmetic results to type width */
    OP_TRUNC_I8,    /* ( int -- int )  wrap to int8_t   */
    OP_TRUNC_U8,    /* ( int -- int )  wrap to uint8_t  */
    OP_TRUNC_I16,   /* ( int -- int )  wrap to int16_t  */
    OP_TRUNC_U16,   /* ( int -- int )  wrap to uint16_t */
    OP_TRUNC_I32,   /* ( int -- int )  wrap to int32_t  */
    OP_TRUNC_U32,   /* ( int -- int )  wrap to uint32_t */
    OP_TRUNC_U64,   /* ( int -- int )  wrap to uint64_t */
    OP_TRUNC_CHAR,  /* ( int -- int )  wrap to uint32_t codepoint */

    /* Array opcodes */
    OP_NEW_ARRAY,   /* [uint8_t elem_kind]  ( len -- arr )  alloc zero-filled array  */
    OP_CMP_EQ_VAL,  /* ( val val -- bool )  generic ==, uses vm->elem_kind_reg       */
    OP_CMP_NEQ_VAL, /* ( val val -- bool )  generic !=, uses vm->elem_kind_reg       */
    OP_ARRAY_LIT,   /* [uint8_t count]  ( v0..vN -- arr )  build from stack vals */
    OP_ARRAY_GET,   /* ( arr idx -- val )  bounds-checked element read           */
    OP_ARRAY_SET,   /* ( arr idx val -- )  bounds-checked element write          */
    OP_ARRAY_LEN,   /* ( arr -- int )  push array.length                        */

    /* ── Host API calls ──────────────────────────────────────────────────
     * Calls a C function registered by the game engine.
     * The host function index is assigned when YOU register functions —
     * modders never pick or know these indices.
     *
     * This is the ONLY way a mod script interacts with your game.
     * If you don't register it, they can't call it. Period. */
    OP_CALL_HOST,         /* [uint16_t host_fn_idx][uint8_t argc]  ( args -- retval ) */

    /* ── Object opcodes ──────────────────────────────────────────────────
     * These implement the class/object system.
     *
     * OP_NEW: allocate a new object of a given class and run its constructor.
     *   Operand: uint16_t class_index (into module->classes[])
     *            uint8_t  argc        (constructor argument count)
     *   Stack:   ( args -- obj )
     *
     * OP_GET_FIELD: push the value of a field on an object.
     *   Operand: uint8_t field_index
     *   Stack:   ( obj -- val )    obj is consumed
     *
     * OP_SET_FIELD: pop a value and store it into a field on an object.
     *   Operand: uint8_t field_index
     *   Stack:   ( obj val -- )    both consumed, obj's field updated
     *
     * OP_LOAD_THIS: push the current 'this' object onto the stack.
     *   No operands. The 'this' pointer is always in slot 0 of a method frame.
     *   Stack:   ( -- obj )
     *
     * OP_CALL_METHOD: call a method on an object.
     *   Operand: uint16_t method_fn_index  (index into module->chunks)
     *            uint8_t  argc
     *   Stack:   ( obj args -- retval )
     *            obj is passed as 'this' (slot 0) in the method's frame */
    OP_NEW,               /* [uint16_t class_idx][uint8_t argc]  ( args -- obj   ) */
    OP_GET_FIELD,         /* [uint8_t  field_idx]                ( obj  -- val   ) */
    OP_SET_FIELD,         /* [uint8_t  field_idx]                ( obj val -- )    */
    OP_LOAD_THIS,         /* (no operands)                       ( -- obj )        */
    OP_CALL_METHOD,       /* [uint16_t fn_idx][uint8_t argc]     ( obj args -- ret ) */
    OP_CALL_SUPER,        /* [uint16_t fn_idx][uint8_t argc]     ( args -- )       */
    OP_CALL_IFACE,        /* [uint16_t name_const_idx][uint8_t argc]
                           * Virtual dispatch through an interface-typed reference.
                           * name_const_idx: index of a string constant holding the
                           * method name. At runtime, looks up the method on the
                           * actual concrete class_def of the object.
                           * Stack: ( obj args -- ret ) */

    /* ── Static field access ─────────────────────────────────────────────
     * Reads/writes a static field — belongs to the class, not an instance.
     * class_idx: uint8_t index into Module->classes[]
     * field_idx: uint8_t index into ClassDef->fields[] (must be is_static) */
    OP_LOAD_STATIC,       /* [uint8_t class_idx][uint8_t field_idx]  ( -- val ) */
    OP_STORE_STATIC,      /* [uint8_t class_idx][uint8_t field_idx]  ( val -- ) */

    /* ── Type operators ────────────────────────────────────────────────── */
    OP_IS_TYPE,           /* [uint8_t type_tag]  ( val -- bool )  expr is T   */
    OP_AS_TYPE,           /* [uint8_t type_tag]  ( val -- val )   expr as T   */
    OP_TYPEOF,            /* [uint8_t type_tag][uint8_t name_len][name_bytes] */
    OP_TYPE_FIELD,        /* [uint8_t field_id]  ( Type -- val )              */
    OP_TYPE_HAS_ATTR,     /* ( Type string -- bool )  hasAttribute(name)      */
    OP_TYPE_GET_ATTR_ARG, /* ( Type string int -- string? ) getAttributeArg   */

    /* ── Nullable operators ─────────────────────────────────────────────── */
    OP_PUSH_NULL,         /*                     ( -- null )  push null value */
    OP_IS_NULL,           /*                     ( val -- bool ) null check   */
    OP_NULL_ASSERT,       /* [uint16_t line]     ( val -- val ) assert != null, RuntimeError if null */
    OP_NULL_COALESCE,     /*                     ( val val -- val ) pop right then left; push left if non-null else right */

    /* ── Exception handling ─────────────────────────────────────────────── */
    OP_TRY_BEGIN,         /* [uint16_t catch_offset]  push exception handler  */
    OP_TRY_END,           /*                           pop exception handler (normal exit) */
    OP_THROW,             /*                     ( obj -- ) throw exception; unwind to nearest handler */
    OP_LOAD_EXCEPTION,    /*                     ( -- obj ) push caught exception inside catch block */
    OP_EXCEPTION_IS_TYPE, /* [u8 name_len][name_bytes]  ( -- bool ) test caught exception class */

    /* ── Event opcodes ──────────────────────────────────────────────────── */
    OP_EVENT_SUBSCRIBE,   /* [u8 event_name_len][event_name_bytes][u8 handler_name_len][handler_name_bytes]
                             ( -- ) subscribe static/top-level function to named event */
    OP_EVENT_UNSUBSCRIBE, /* [u8 event_name_len][event_name_bytes][u8 handler_name_len][handler_name_bytes]
                             ( -- ) unsubscribe static/top-level function from named event */
    OP_EVENT_FIRE,        /* [u8 name_len][name_bytes][u8 arg_count]
                             ( arg0..argN -- ) fire top-level event */
    OP_EVENT_SUBSCRIBE_BOUND,   /* [u8 event_name_len][event_name_bytes][u8 method_name_len][method_name_bytes]
                                   ( receiver -- ) subscribe bound instance method to named event */
    OP_EVENT_UNSUBSCRIBE_BOUND, /* [u8 event_name_len][event_name_bytes][u8 method_name_len][method_name_bytes]
                                   ( receiver -- ) unsubscribe bound instance method from named event */
    OP_EVENT_SUBSCRIBE_MEMBER,   /* [u8 name_len][name_bytes][u8 handler_name_len][handler_name_bytes]
                                    ( obj -- ) subscribe fn to member event on obj */
    OP_EVENT_UNSUBSCRIBE_MEMBER, /* [u8 name_len][name_bytes][u8 handler_name_len][handler_name_bytes]
                                    ( obj -- ) unsubscribe fn from member event on obj */
    OP_EVENT_FIRE_MEMBER,        /* [u8 name_len][name_bytes][u8 arg_count]
                                    ( obj arg0..argN -- ) fire member event on obj */

} OpCode;

/* Human-readable opcode name — for the disassembler */
const char *opcode_name(OpCode op);


/* ═════════════════════════════════════════════════════════════════════════════
 * CONSTANT POOL
 *
 * A flat array of Values baked into the bytecode at compile time.
 * Literals (42, 3.14, "hello") are stored here, not inline in the
 * instruction stream (except LOAD_CONST_BOOL which is small enough to inline).
 *
 * The compiler adds constants here; the VM reads them.
 * Max 65535 constants per chunk (uint16_t index).
 * ═════════════════════════════════════════════════════════════════════════════*/
/* ConstPool grows dynamically — no fixed upper limit baked into the struct */
typedef struct {
    Value  *values;   /* heap-allocated, grown on demand */
    int     count;
    int     capacity;
} ConstPool;


/* ═════════════════════════════════════════════════════════════════════════════
 * CHUNK
 *
 * A Chunk is one compiled unit — one function's worth of bytecode.
 * The compiler produces one Chunk per function, plus one for top-level code.
 *
 * Layout:
 *   code[]       — flat array of bytes (opcodes + operands, packed)
 *   lines[]      — parallel array: lines[i] = source line for code[i]
 *                  used for runtime error messages ("error at line 42")
 *   constants    — the constant pool for this chunk
 *   local_count  — how many local variable slots this function needs
 *                  (the VM uses this to allocate the call frame)
 *
 * We grow code[] and lines[] dynamically. For a mod script, these will
 * typically be a few hundred bytes at most.
 * ═════════════════════════════════════════════════════════════════════════════*/
#define CHUNK_INIT_CAPACITY 256

typedef struct {
    /* Instruction stream */
    uint8_t *code;       /* Bytecode bytes — opcodes and operands packed   */
    int     *lines;      /* Source line per byte — parallel to code[]      */
    int      count;      /* Number of bytes written                        */
    int      capacity;   /* Allocated capacity of code[] and lines[]       */

    /* Constant pool */
    ConstPool constants;

    /* Frame info — set by the compiler, used by the VM */
    int  local_count;     /* Number of local variable slots needed          */
    int  param_count;     /* Number of parameters (occupy first N slots)    */
    bool is_constructor;  /* True if this chunk is a class constructor.
                           * RETURN_VOID from a constructor does not push a
                           * dummy value — the object was already pushed by
                           * OP_NEW before the constructor frame was set up. */

    /* Type signature — stored so declare_staging can give the checker
     * accurate types without re-parsing source. Uses TypeKind values. */
    int  return_type_kind;              /* TypeKind of the return value     */
    int  param_type_kinds[16];          /* TypeKind of each parameter       */
} Chunk;

/* Initialize an empty chunk */
void chunk_init(Chunk *chunk);

/* Free all memory owned by the chunk */
void chunk_free(Chunk *chunk);

/*
 * Write one byte to the chunk, recording its source line.
 * Grows the buffer automatically if needed.
 * Returns the byte offset of the written byte (useful for patching jumps).
 */
int chunk_write(Chunk *chunk, uint8_t byte, int line);

/*
 * Write a uint16_t operand as two bytes, big-endian.
 * Returns the offset of the FIRST byte written (used for jump patching).
 */
int chunk_write_u16(Chunk *chunk, uint16_t value, int line);

/*
 * Patch a uint16_t that was previously written at `offset`.
 * Used for forward jumps: emit the jump with a placeholder offset of 0,
 * then patch it once you know where the target is.
 */
void chunk_patch_u16(Chunk *chunk, int offset, uint16_t value);

/*
 * Add a constant to the pool. Returns the index.
 * The compiler calls this for every literal value.
 */
int chunk_add_constant(Chunk *chunk, Value value);

/*
 * Disassemble a chunk to stdout — prints each instruction as human-readable
 * text. Invaluable for debugging the compiler.
 *
 * Example output:
 *   0000  line 2   LOAD_CONST_INT    0  (42)
 *   0003  line 2   LOAD_LOCAL        1
 *   0005  line 2   ADD_INT
 *   0006  line 2   RETURN
 */
void chunk_disassemble(const Chunk *chunk, const char *name);

#endif /* BYTECODE_H */