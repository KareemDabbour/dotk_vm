#ifndef DOTK3_H
#define DOTK3_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct DK3VM DK3VM;
typedef struct DK3Value DK3Value;
typedef struct DK3Call DK3Call;
typedef enum { DK3_OK, DK3_ERROR, DK3_INVALID, DK3_BUSY, DK3_LIMIT, DK3_OUT_OF_MEMORY } DK3Status;
typedef enum { DK3_NULL, DK3_BOOL, DK3_INT, DK3_FLOAT, DK3_STRING, DK3_BYTES,
               DK3_LIST, DK3_TUPLE, DK3_MAP, DK3_FUNCTION, DK3_TYPE,
               DK3_OBJECT, DK3_MODULE, DK3_GENERATOR, DK3_TASK, DK3_RESOURCE, DK3_SET } DK3Type;
typedef enum { DK3_TASK_READY, DK3_TASK_RUNNING, DK3_TASK_CANCELLING,
               DK3_TASK_DONE, DK3_TASK_CANCELLED, DK3_TASK_FAILED } DK3TaskState;
typedef void (*DK3Write)(void *userdata, const char *bytes, size_t length);
typedef const char *(*DK3Resolve)(void *userdata, const char *module);
typedef DK3Status (*DK3Native)(DK3Call *call, void *userdata);
typedef struct {
    size_t size;
    void *userdata;
    DK3Write write;
    DK3Resolve resolve;
    uint64_t step_limit;
    size_t object_limit;
} DK3Config;

typedef DK3Status (*DK3ReadFile)(void *userdata, const char *path, size_t limit, const char **bytes, size_t *length);
typedef DK3Status (*DK3FileSize)(void *userdata, const char *path, int *exists, uint64_t *size);
#define DK3_FILES_ABI_VERSION 1u
typedef struct {
    size_t size;
    uint32_t abi_version;
    void *userdata;
    DK3ReadFile read;
    DK3FileSize info;
    size_t max_bytes;
} DK3Files;

typedef void (*DK3Dispose)(void *userdata, void *payload);
#define DK3_RESOURCE_ABI_VERSION 1u
typedef struct {
    size_t size;
    uint32_t abi_version;
    const char *type_name;
    void *payload;
    void *userdata;
    DK3Dispose dispose;
    const DK3Value *parent;
} DK3Resource;

typedef struct {
    const char *name;
    DK3Native function;
    void *userdata;
    const char *documentation;
} DK3NativeExport;
#define DK3_MODULE_ABI_VERSION 1u
typedef struct {
    size_t size;
    uint32_t abi_version;
    const char *name;
    const char *documentation;
    size_t count;
    const DK3NativeExport *exports;
} DK3NativeModule;

void dk3_module_init(DK3NativeModule *module);
DK3Status dk3_register_module(DK3VM *vm, const DK3NativeModule *module);
void dk3_resource_init(DK3Resource *resource);
DK3Status dk3_resource(DK3VM *vm, const DK3Resource *resource, DK3Value **result);
DK3Status dk3_resource_get(const DK3Value *value, const char *type_name, void **payload);
DK3Status dk3_resource_closed(const DK3Value *value, int *closed);
DK3Status dk3_resource_close(DK3Value *value);
DK3Status dk3_close_resources(DK3VM *vm);
DK3Status dk3_native_resource(DK3Call *call, const DK3Resource *resource, DK3Value **result);
DK3Status dk3_native_resource_get(DK3Call *call, const DK3Value *value, const char *type_name, void **payload);
DK3Status dk3_native_resource_closed(DK3Call *call, const DK3Value *value, int *closed);
DK3Status dk3_native_resource_close(DK3Call *call, DK3Value *value);

void dk3_config_init(DK3Config *config);
void dk3_files_init(DK3Files *files);
DK3Status dk3_set_files(DK3VM *vm, const DK3Files *files);
DK3VM *dk3_create(const DK3Config *config);
DK3Status dk3_destroy(DK3VM *vm);
const char *dk3_diagnostic(const DK3VM *vm);
DK3Status dk3_eval(DK3VM *vm, const char *source, const char *file);
DK3Status dk3_get(DK3VM *vm, const char *name, DK3Value **result);
DK3Status dk3_int(DK3VM *vm, int64_t number, DK3Value **result);
DK3Status dk3_null(DK3VM *vm, DK3Value **result);
DK3Status dk3_bool(DK3VM *vm, int boolean, DK3Value **result);
DK3Status dk3_float(DK3VM *vm, double number, DK3Value **result);
DK3Status dk3_bytes(DK3VM *vm, const char *bytes, size_t length, DK3Value **result);
DK3Status dk3_string(DK3VM *vm, const char *bytes, size_t length, DK3Value **result);
DK3Status dk3_list(DK3VM *vm, size_t count, const DK3Value *const *items, DK3Value **result);
DK3Status dk3_tuple(DK3VM *vm, size_t count, const DK3Value *const *items, DK3Value **result);
DK3Status dk3_sequence_size(const DK3Value *sequence, size_t *result);
DK3Status dk3_sequence_get(const DK3Value *sequence, size_t index, DK3Value **result);
DK3Status dk3_list_append(DK3Value *list, const DK3Value *value);
DK3Status dk3_list_set(DK3Value *list, size_t index, const DK3Value *value);
DK3Status dk3_map(DK3VM *vm, DK3Value **result);
DK3Status dk3_map_size(const DK3Value *map, size_t *result);
DK3Status dk3_map_get(const DK3Value *map, const DK3Value *key, int *found, DK3Value **result);
DK3Status dk3_map_set(DK3Value *map, const DK3Value *key, const DK3Value *value);
DK3Status dk3_map_remove(DK3Value *map, const DK3Value *key, int *removed);
DK3Status dk3_map_entry(const DK3Value *map, size_t index, DK3Value **key, DK3Value **value);
DK3Status dk3_get_int(const DK3Value *value, int64_t *result);
DK3Status dk3_get_type(const DK3Value *value, DK3Type *result);
DK3Status dk3_get_bool(const DK3Value *value, int *result);
DK3Status dk3_get_float(const DK3Value *value, double *result);
DK3Status dk3_get_bytes(const DK3Value *value, const char **bytes, size_t *length);
DK3Status dk3_get_string(const DK3Value *value, const char **bytes, size_t *length);
DK3Status dk3_call(DK3VM *vm, const DK3Value *function, size_t count,
                   const DK3Value *const *arguments, DK3Value **result);
DK3Status dk3_resume(DK3VM *vm, DK3Value *generator, int *available, DK3Value **result);
DK3Status dk3_close(DK3VM *vm, DK3Value *generator);
DK3Status dk3_spawn(DK3VM *vm, const DK3Value *generator, DK3Value **task);
DK3Status dk3_pump(DK3VM *vm, size_t turns, size_t *completed);
DK3Status dk3_cancel(DK3VM *vm, const DK3Value *task);
DK3Status dk3_task_state(const DK3Value *task, DK3TaskState *state);
DK3Status dk3_task_value(DK3VM *vm, const DK3Value *task, int *available, DK3Value **result);
DK3Status dk3_release(DK3Value *value);
DK3Status dk3_collect(DK3VM *vm);
DK3Status dk3_define_native(DK3VM *vm, const char *name, DK3Native function, void *userdata);
size_t dk3_native_count(const DK3Call *call);
DK3VM *dk3_native_vm(const DK3Call *call);
DK3Status dk3_native_argument(DK3Call *call, size_t index, DK3Value **result);
DK3Status dk3_native_release(DK3Call *call, DK3Value *value);
DK3Status dk3_native_make_int(DK3Call *call, int64_t number, DK3Value **result);
DK3Status dk3_native_make_null(DK3Call *call, DK3Value **result);
DK3Status dk3_native_make_bool(DK3Call *call, int boolean, DK3Value **result);
DK3Status dk3_native_make_float(DK3Call *call, double number, DK3Value **result);
DK3Status dk3_native_make_bytes(DK3Call *call, const char *bytes, size_t length, DK3Value **result);
DK3Status dk3_native_make_string(DK3Call *call, const char *bytes, size_t length, DK3Value **result);
DK3Status dk3_native_list(DK3Call *call, size_t count, const DK3Value *const *items, DK3Value **result);
DK3Status dk3_native_tuple(DK3Call *call, size_t count, const DK3Value *const *items, DK3Value **result);
DK3Status dk3_native_map(DK3Call *call, DK3Value **result);
DK3Status dk3_native_value_type(DK3Call *call, const DK3Value *value, DK3Type *result);
DK3Status dk3_native_value_int(DK3Call *call, const DK3Value *value, int64_t *result);
DK3Status dk3_native_value_bool(DK3Call *call, const DK3Value *value, int *result);
DK3Status dk3_native_value_float(DK3Call *call, const DK3Value *value, double *result);
DK3Status dk3_native_value_bytes(DK3Call *call, const DK3Value *value, const char **bytes, size_t *length);
DK3Status dk3_native_value_string(DK3Call *call, const DK3Value *value, const char **bytes, size_t *length);
DK3Status dk3_native_sequence_size(DK3Call *call, const DK3Value *sequence, size_t *result);
DK3Status dk3_native_sequence_get(DK3Call *call, const DK3Value *sequence, size_t index, DK3Value **result);
DK3Status dk3_native_list_append(DK3Call *call, DK3Value *list, const DK3Value *value);
DK3Status dk3_native_list_set(DK3Call *call, DK3Value *list, size_t index, const DK3Value *value);
DK3Status dk3_native_map_size(DK3Call *call, const DK3Value *map, size_t *result);
DK3Status dk3_native_map_get(DK3Call *call, const DK3Value *map, const DK3Value *key, int *found, DK3Value **result);
DK3Status dk3_native_map_set(DK3Call *call, DK3Value *map, const DK3Value *key, const DK3Value *value);
DK3Status dk3_native_map_remove(DK3Call *call, DK3Value *map, const DK3Value *key, int *removed);
DK3Status dk3_native_map_entry(DK3Call *call, const DK3Value *map, size_t index, DK3Value **key, DK3Value **value);
DK3Status dk3_native_int(const DK3Call *call, size_t index, int64_t *result);
DK3Status dk3_native_bool(const DK3Call *call, size_t index, int *result);
DK3Status dk3_native_float(const DK3Call *call, size_t index, double *result);
DK3Status dk3_native_string(const DK3Call *call, size_t index, const char **bytes, size_t *length);
DK3Status dk3_native_bytes(const DK3Call *call, size_t index, const char **bytes, size_t *length);
DK3Status dk3_native_return_int(DK3Call *call, int64_t result);
DK3Status dk3_native_return_null(DK3Call *call);
DK3Status dk3_native_return_bool(DK3Call *call, int result);
DK3Status dk3_native_return_float(DK3Call *call, double result);
DK3Status dk3_native_return_bytes(DK3Call *call, const char *bytes, size_t length);
DK3Status dk3_native_return_value(DK3Call *call, const DK3Value *value);
DK3Status dk3_native_return_string(DK3Call *call, const char *bytes, size_t length);
DK3Status dk3_native_error(DK3Call *call, const char *message);

#ifdef __cplusplus
}
#endif
#endif