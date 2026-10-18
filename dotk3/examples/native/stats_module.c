/*
 * A native DotK 3 module written in C.
 *
 *   make example-plugin     (builds, copies the library beside stats_demo.k3 and runs it)
 *
 * Scripts load it with `import Stats` when stats_module.<ext> (or Stats.<ext>, libStats.<ext>) sits next
 * in the script's directory; `dotk3 --native PATH` loads it from anywhere.
 *
 * A plugin exports one function, `dk3_module_entry`, that returns a DK3NativeModule: a name plus a
 * table of exports. Each export has a DK3Native callback and a declaration tail that the type checker
 * uses, so `Stats.mean(...)` is checked before the program runs. Arguments are read with
 * dk3_native_*, results are returned with dk3_native_return_*, and failures are reported with
 * dk3_native_error (they surface in DotK 3 as a catchable Error of kind Native).
 *
 * The plugin does not link against DotK 3; the dotk3 executable provides the dk3_* symbols.
 */
#include "dotk3.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

/* ---- plain functions: Ints, Strings and Lists ---- */

static DK3Status gcd(DK3Call *call, void *userdata) {
    (void)userdata;
    int64_t a, b;
    DK3Status status = dk3_native_int(call, 0, &a);
    if (status == DK3_OK) status = dk3_native_int(call, 1, &b);
    if (status != DK3_OK) return status;
    if (a == INT64_MIN || b == INT64_MIN) return dk3_native_error(call, "gcd: argument out of range");
    a = a < 0 ? -a : a; b = b < 0 ? -b : b;
    while (b) { int64_t t = a % b; a = b; b = t; }
    return dk3_native_return_int(call, a);
}

static DK3Status fnv1a(DK3Call *call, void *userdata) {
    (void)userdata;
    const char *bytes; size_t length;
    DK3Status status = dk3_native_string(call, 0, &bytes, &length);
    if (status != DK3_OK) return status;
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < length; i++) { hash ^= (unsigned char)bytes[i]; hash *= 1099511628211ull; }
    return dk3_native_return_int(call, (int64_t)hash);
}

/* Lists may hold Ints where Floats are declared, so read each element by its runtime type. */
static DK3Status number_at(DK3Call *call, const DK3Value *list, size_t index, double *out) {
    DK3Value *item = NULL; DK3Type type;
    DK3Status status = dk3_native_sequence_get(call, list, index, &item);
    if (status != DK3_OK) return status;
    status = dk3_native_value_type(call, item, &type);
    if (status == DK3_OK && type == DK3_INT) { int64_t whole; status = dk3_native_value_int(call, item, &whole); *out = (double)whole; }
    else if (status == DK3_OK) status = dk3_native_value_float(call, item, out);
    dk3_native_release(call, item);
    return status;
}

static DK3Status mean(DK3Call *call, void *userdata) {
    (void)userdata;
    DK3Value *list = NULL; size_t count = 0; double total = 0;
    DK3Status status = dk3_native_argument(call, 0, &list);
    if (status == DK3_OK) status = dk3_native_sequence_size(call, list, &count);
    if (status == DK3_OK && count == 0) status = dk3_native_error(call, "mean: empty list");
    for (size_t i = 0; status == DK3_OK && i < count; i++) {
        double value; status = number_at(call, list, i, &value); total += value;
    }
    if (status == DK3_OK) status = dk3_native_return_float(call, total / (double)count);
    if (list) dk3_native_release(call, list);
    return status;
}

/* ---- a resource: C memory owned by a DotK 3 value ---- */

typedef struct { int64_t count; double mean, m2, min, max; } Accumulator;
#define ACCUMULATOR "Stats.Accumulator"

static void dispose_accumulator(void *userdata, void *payload) { (void)userdata; free(payload); }

static DK3Status create(DK3Call *call, void *userdata) {
    (void)userdata;
    Accumulator *state = calloc(1, sizeof(*state));
    if (!state) return DK3_OUT_OF_MEMORY;
    DK3Resource descriptor; dk3_resource_init(&descriptor);
    descriptor.type_name = ACCUMULATOR; descriptor.payload = state; descriptor.dispose = dispose_accumulator;
    DK3Value *value = NULL;
    DK3Status status = dk3_native_resource(call, &descriptor, &value);
    if (status != DK3_OK) { free(state); return status; }
    status = dk3_native_return_value(call, value);
    dk3_native_release(call, value);
    return status;
}

/* Fetches argument 0 as an Accumulator; the caller releases *handle. */
static DK3Status accumulator(DK3Call *call, DK3Value **handle, Accumulator **state) {
    void *payload = NULL;
    DK3Status status = dk3_native_argument(call, 0, handle);
    if (status == DK3_OK) status = dk3_native_resource_get(call, *handle, ACCUMULATOR, &payload);
    *state = payload;
    return status;
}

static DK3Status add(DK3Call *call, void *userdata) {
    (void)userdata;
    DK3Value *handle = NULL; Accumulator *s = NULL; double x;
    DK3Status status = accumulator(call, &handle, &s);
    if (status == DK3_OK) status = dk3_native_float(call, 1, &x);
    if (status == DK3_OK) {
        if (++s->count == 1) s->min = s->max = x;
        if (x < s->min) s->min = x;
        if (x > s->max) s->max = x;
        double delta = x - s->mean; s->mean += delta / (double)s->count; s->m2 += delta * (x - s->mean);
        status = dk3_native_return_null(call);
    }
    if (handle) dk3_native_release(call, handle);
    return status;
}

static DK3Status put(DK3Call *call, DK3Value *map, const char *name, double number) {
    DK3Value *key = NULL, *value = NULL; size_t length = 0;
    while (name[length]) length++;
    DK3Status status = dk3_native_make_string(call, name, length, &key);
    if (status == DK3_OK) status = dk3_native_make_float(call, number, &value);
    if (status == DK3_OK) status = dk3_native_map_set(call, map, key, value);
    if (key) dk3_native_release(call, key);
    if (value) dk3_native_release(call, value);
    return status;
}

static DK3Status summary(DK3Call *call, void *userdata) {
    (void)userdata;
    DK3Value *handle = NULL, *map = NULL; Accumulator *s = NULL;
    DK3Status status = accumulator(call, &handle, &s);
    if (status == DK3_OK && s->count == 0) status = dk3_native_error(call, "summary: no samples were added");
    if (status == DK3_OK) status = dk3_native_map(call, &map);
    if (status == DK3_OK) status = put(call, map, "count", (double)s->count);
    if (status == DK3_OK) status = put(call, map, "mean", s->mean);
    if (status == DK3_OK) status = put(call, map, "stddev", s->count > 1 ? sqrt(s->m2 / (double)(s->count - 1)) : 0.0);
    if (status == DK3_OK) status = put(call, map, "min", s->min);
    if (status == DK3_OK) status = put(call, map, "max", s->max);
    if (status == DK3_OK) status = dk3_native_return_value(call, map);
    if (map) dk3_native_release(call, map);
    if (handle) dk3_native_release(call, handle);
    return status;
}

static DK3Status release(DK3Call *call, void *userdata) {
    (void)userdata;
    DK3Value *handle = NULL;
    DK3Status status = dk3_native_argument(call, 0, &handle);
    if (status == DK3_OK) status = dk3_native_resource_close(call, handle); /* runs dispose_accumulator once */
    if (handle) dk3_native_release(call, handle);
    return status == DK3_OK ? dk3_native_return_null(call) : status;
}

/* ---- the module table ---- */

static const DK3NativeExport exports[] = {
    {"gcd", "(a: Int, b: Int) -> Int", gcd, NULL, "Greatest common divisor."},
    {"fnv1a", "(text: String) -> Int", fnv1a, NULL, "64-bit FNV-1a hash of the UTF-8 bytes."},
    {"mean", "(values: List<Float>) -> Float", mean, NULL, "Arithmetic mean of a nonempty list."},
    {"create", "() -> Resource", create, NULL, "Create a running-statistics accumulator."},
    {"add", "(stats: Resource, sample: Float) -> Null", add, NULL, "Add one sample."},
    {"summary", "(stats: Resource) -> Map<String, Float>", summary, NULL, "count, mean, stddev, min and max so far."},
    {"close", "(stats: Resource) -> Null", release, NULL, "Free the accumulator now."},
};

const DK3NativeModule *dk3_module_entry(void) {
    static DK3NativeModule module;
    dk3_module_init(&module);
    module.name = "Stats";
    module.documentation = "Numeric helpers implemented in C.";
    module.count = sizeof(exports) / sizeof(exports[0]);
    module.exports = exports;
    return &module;
}
