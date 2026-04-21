/* src/safety/type_kind.h
 *
 * Pure predicates on TypeKind values. Takes an int kind (after
 * type_unwrap_distinct) and returns 1 iff the type belongs to the
 * named category.
 *
 * Linked into zerc. Verified by `make check-vst` via CompCert clightgen
 * + VST. See docs/formal_verification_plan.md Level 3.
 *
 * The integer values MUST match TypeKind in types.h. Changing the
 * enum order requires updating the constants below AND re-running
 * make check-vst. VST fails if the Coq spec constants in
 * proofs/vst/verif_type_kind.v don't match.
 */
#ifndef ZER_SAFETY_TYPE_KIND_H
#define ZER_SAFETY_TYPE_KIND_H

/* TypeKind constants — must match types.h enum order. */
#define ZER_TK_VOID        0
#define ZER_TK_BOOL        1
#define ZER_TK_U8          2
#define ZER_TK_U16         3
#define ZER_TK_U32         4
#define ZER_TK_U64         5
#define ZER_TK_USIZE       6
#define ZER_TK_I8          7
#define ZER_TK_I16         8
#define ZER_TK_I32         9
#define ZER_TK_I64        10
#define ZER_TK_F32        11
#define ZER_TK_F64        12
#define ZER_TK_POINTER    13
#define ZER_TK_OPTIONAL   14
#define ZER_TK_SLICE      15
#define ZER_TK_ARRAY      16
#define ZER_TK_STRUCT     17
#define ZER_TK_ENUM       18
#define ZER_TK_UNION      19
#define ZER_TK_FUNC_PTR   20
#define ZER_TK_OPAQUE     21
#define ZER_TK_POOL       22
#define ZER_TK_RING       23
#define ZER_TK_ARENA      24
#define ZER_TK_BARRIER    25
#define ZER_TK_HANDLE     26
#define ZER_TK_SLAB       27
#define ZER_TK_SEMAPHORE  28
#define ZER_TK_DISTINCT   29

/* Returns 1 iff kind is an integer type (unsigned or signed ints, usize,
 * or enum — enums are i32 internally).
 *
 * Callers: types.c:type_is_integer (after type_unwrap_distinct). */
int zer_type_kind_is_integer(int kind);

/* Returns 1 iff kind is a signed integer (i8/i16/i32/i64 or enum). */
int zer_type_kind_is_signed(int kind);

/* Returns 1 iff kind is an unsigned integer (u8/u16/u32/u64/usize). */
int zer_type_kind_is_unsigned(int kind);

/* Returns 1 iff kind is floating-point (f32 or f64). */
int zer_type_kind_is_float(int kind);

/* Returns 1 iff kind is a numeric type (integer or float). */
int zer_type_kind_is_numeric(int kind);

/* Returns 1 iff kind is a pointer type (*T or *opaque). */
int zer_type_kind_is_pointer(int kind);

/* Returns 1 iff kind is a compound type with fields (struct or union). */
int zer_type_kind_has_fields(int kind);

#endif
