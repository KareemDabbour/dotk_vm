#include "dotk3.h"
#include <stdio.h>
#include <stdlib.h>

static void output(void *userdata, const char *bytes, size_t length) {
    (void)userdata; fwrite(bytes, 1, length, stdout);
}

static DK3Status records(DK3Call *call, void *userdata) {
    (void)userdata;
    DK3Value *rows = NULL, *record = NULL, *key = NULL, *value = NULL;
    DK3Status result = dk3_native_list(call, 0, NULL, &rows);
    if (result != DK3_OK) return result;
    for (int64_t index = 1; index <= 3 && result == DK3_OK; index++) {
        result = dk3_native_map(call, &record);
        if (result == DK3_OK) result = dk3_native_make_string(call, "value", 5, &key);
        if (result == DK3_OK) result = dk3_native_make_int(call, index, &value);
        if (result == DK3_OK) result = dk3_native_map_set(call, record, key, value);
        if (result == DK3_OK) result = dk3_native_list_append(call, rows, record);
        if (record) dk3_native_release(call, record);
        if (key) dk3_native_release(call, key);
        if (value) dk3_native_release(call, value);
        record = key = value = NULL;
    }
    if (result == DK3_OK) result = dk3_native_return_value(call, rows);
    dk3_native_release(call, rows);
    return result;
}

static void dispose_counter(void *userdata, void *payload) {
    (void)userdata; free(payload);
}
static DK3Status create_counter(DK3Call *call, void *userdata) {
    (void)userdata;
    if (dk3_native_count(call)) return dk3_native_error(call, "create expects no arguments");
    int64_t *counter = (int64_t *)malloc(sizeof(*counter));
    if (!counter) return DK3_OUT_OF_MEMORY;
    *counter = 0;
    DK3Resource descriptor; dk3_resource_init(&descriptor);
    descriptor.type_name = "Counters.Counter"; descriptor.payload = counter; descriptor.dispose = dispose_counter;
    DK3Value *value = NULL; DK3Status result = dk3_native_resource(call, &descriptor, &value);
    if (result != DK3_OK) { free(counter); return result; }
    result = dk3_native_return_value(call, value); dk3_native_release(call, value); return result;
}
static DK3Status next_counter(DK3Call *call, void *userdata) {
    (void)userdata; DK3Value *value = NULL; void *payload = NULL;
    if (dk3_native_count(call) != 1) return dk3_native_error(call, "next expects one resource");
    DK3Status result = dk3_native_argument(call, 0, &value);
    if (result == DK3_OK) result = dk3_native_resource_get(call, value, "Counters.Counter", &payload);
    if (result == DK3_OK) {
        int64_t *counter = (int64_t *)payload;
        result = *counter == INT64_MAX ? dk3_native_error(call, "Counter overflow") : dk3_native_return_int(call, ++*counter);
    }
    if (value) dk3_native_release(call, value);
    return result;
}
static DK3Status close_counter(DK3Call *call, void *userdata) {
    (void)userdata; DK3Value *value = NULL;
    if (dk3_native_count(call) != 1) return dk3_native_error(call, "close expects one resource");
    DK3Status result = dk3_native_argument(call, 0, &value);
    if (result == DK3_OK) result = dk3_native_resource_close(call, value);
    if (value) dk3_native_release(call, value);
    return result == DK3_OK ? dk3_native_return_null(call) : result;
}

int main(void) {
    DK3Config config; dk3_config_init(&config); config.write = output;
    DK3VM *vm = dk3_create(&config);
    if (!vm) return 1;
    DK3Status result = dk3_define_native(vm, "host.records", "() -> List<Map<String, Int>>", records, NULL);
    if (result == DK3_OK)
        result = dk3_eval(vm, "let rows: List<Map<String, Int>> = host.records()\nGC.collect()\nrows[0]['value'] += 10\nSystem.print(Json.stringify(rows))", "structured-host.k3");
    DK3NativeExport exports[] = {{"create", "() -> Resource", create_counter, NULL, "Create a host-owned Counter."},
        {"next", "(counter: Resource) -> Int", next_counter, NULL, "Increment a live Counter."}, {"close", "(counter: Resource) -> Null", close_counter, NULL, "Close an opaque resource explicitly."}};
    DK3NativeModule module; dk3_module_init(&module);
    module.name = "Counters"; module.documentation = "A native resource adapter."; module.count = 3; module.exports = exports;
    if (result == DK3_OK) result = dk3_register_module(vm, &module);
    if (result == DK3_OK)
        result = dk3_eval(vm, "from Counters import create, next, close\nlet counter: Resource = create()\nGC.collect()\nSystem.print(counter.type_name, next(counter), next(counter), counter.closed)\nclose(counter)\nclose(counter)\nSystem.print(counter.closed)\ntry { next(counter) } catch error { System.print(error.kind) }", "native-resource.k3");
    if (result != DK3_OK) fprintf(stderr, "%s\n", dk3_diagnostic(vm));
    DK3Status destroyed = dk3_destroy(vm);
    if (destroyed != DK3_OK) return 1;
    return result == DK3_OK ? 0 : 1;
}