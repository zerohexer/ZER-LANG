/* src/safety/type_kind.c — type-kind predicates.
 *
 * See type_kind.h for design. Self-contained: no includes beyond the
 * header. Each function is pure, primitive-typed (int in, int out),
 * and matches a Coq spec in proofs/vst/verif_type_kind.v.
 *
 * These predicates are called from types.c's Type*-taking wrappers
 * (type_is_integer, type_is_signed, etc.) after type_unwrap_distinct.
 */
#include "type_kind.h"

int zer_type_kind_is_integer(int kind) {
    if (kind == ZER_TK_U8)    { return 1; }
    if (kind == ZER_TK_U16)   { return 1; }
    if (kind == ZER_TK_U32)   { return 1; }
    if (kind == ZER_TK_U64)   { return 1; }
    if (kind == ZER_TK_USIZE) { return 1; }
    if (kind == ZER_TK_I8)    { return 1; }
    if (kind == ZER_TK_I16)   { return 1; }
    if (kind == ZER_TK_I32)   { return 1; }
    if (kind == ZER_TK_I64)   { return 1; }
    if (kind == ZER_TK_ENUM)  { return 1; }
    return 0;
}

int zer_type_kind_is_signed(int kind) {
    if (kind == ZER_TK_I8)   { return 1; }
    if (kind == ZER_TK_I16)  { return 1; }
    if (kind == ZER_TK_I32)  { return 1; }
    if (kind == ZER_TK_I64)  { return 1; }
    if (kind == ZER_TK_ENUM) { return 1; }
    return 0;
}

int zer_type_kind_is_unsigned(int kind) {
    if (kind == ZER_TK_U8)    { return 1; }
    if (kind == ZER_TK_U16)   { return 1; }
    if (kind == ZER_TK_U32)   { return 1; }
    if (kind == ZER_TK_U64)   { return 1; }
    if (kind == ZER_TK_USIZE) { return 1; }
    return 0;
}

int zer_type_kind_is_float(int kind) {
    if (kind == ZER_TK_F32) { return 1; }
    if (kind == ZER_TK_F64) { return 1; }
    return 0;
}

int zer_type_kind_is_numeric(int kind) {
    /* Inlined integer + float cases for VST simplicity (no function calls
     * in verified predicates — keeps the proof pattern uniform with the
     * other type_kind predicates). If you modify this, also update the
     * bodies above and the Coq spec in verif_type_kind.v. */
    if (kind == ZER_TK_U8)    { return 1; }
    if (kind == ZER_TK_U16)   { return 1; }
    if (kind == ZER_TK_U32)   { return 1; }
    if (kind == ZER_TK_U64)   { return 1; }
    if (kind == ZER_TK_USIZE) { return 1; }
    if (kind == ZER_TK_I8)    { return 1; }
    if (kind == ZER_TK_I16)   { return 1; }
    if (kind == ZER_TK_I32)   { return 1; }
    if (kind == ZER_TK_I64)   { return 1; }
    if (kind == ZER_TK_ENUM)  { return 1; }
    if (kind == ZER_TK_F32)   { return 1; }
    if (kind == ZER_TK_F64)   { return 1; }
    return 0;
}

int zer_type_kind_is_pointer(int kind) {
    if (kind == ZER_TK_POINTER) { return 1; }
    if (kind == ZER_TK_OPAQUE)  { return 1; }
    return 0;
}

int zer_type_kind_has_fields(int kind) {
    if (kind == ZER_TK_STRUCT) { return 1; }
    if (kind == ZER_TK_UNION)  { return 1; }
    return 0;
}
