#include "dotk3.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

struct Rendezvous {
    std::mutex mutex;
    std::condition_variable changed;
    size_t entered = 0;
};

static DK3Status meet(DK3Call *call, void *userdata) {
    DK3Status wrongThread = DK3_OK;
    std::thread invalidWorker([&] {
        DK3Value *value = nullptr; wrongThread = dk3_native_map(call, &value);
        assert(dk3_native_return_string(call, "invalid thread", 14) == DK3_INVALID);
        assert(dk3_native_count(call) == 0 && dk3_native_vm(call) == nullptr);
    });
    invalidWorker.join(); assert(wrongThread == DK3_INVALID);
    auto &rendezvous = *static_cast<Rendezvous *>(userdata);
    std::unique_lock<std::mutex> lock(rendezvous.mutex);
    rendezvous.entered++; rendezvous.changed.notify_all();
    if (!rendezvous.changed.wait_for(lock, std::chrono::seconds(5), [&] { return rendezvous.entered == 2; }))
        return dk3_native_error(call, "Independent VM callbacks did not overlap");
    return dk3_native_return_int(call, 7);
}

static void dispose_resource(void *userdata, void *payload) {
    assert(payload == userdata); ++*static_cast<int *>(userdata);
}
static void resource_threads() {
    DK3VM *vm = dk3_create(nullptr); int disposed = 0;
    DK3Resource descriptor; dk3_resource_init(&descriptor);
    descriptor.type_name = "ThreadBound"; descriptor.payload = &disposed;
    descriptor.userdata = &disposed; descriptor.dispose = dispose_resource;
    DK3Value *resource = nullptr; assert(dk3_resource(vm, &descriptor, &resource) == DK3_OK);
    std::thread worker([&] {
        void *payload = nullptr; int closed = 1;
        assert(dk3_resource_get(resource, "ThreadBound", &payload) == DK3_BUSY && payload == nullptr);
        assert(dk3_resource_close(resource) == DK3_BUSY);
        assert(dk3_resource_closed(resource, &closed) == DK3_OK && closed == 0);
        assert(dk3_close_resources(vm) == DK3_BUSY && dk3_destroy(vm) == DK3_BUSY);
    });
    worker.join(); assert(disposed == 0);
    assert(dk3_resource_close(resource) == DK3_OK && disposed == 1);
    std::thread destroyer([&] { assert(dk3_destroy(vm) == DK3_OK); }); destroyer.join();
}

int main() {
    resource_threads();
    DK3VM *first = dk3_create(nullptr), *second = dk3_create(nullptr);
    assert(first && second);
    Rendezvous rendezvous;
    assert(dk3_define_native(first, "meet", meet, &rendezvous) == DK3_OK);
    assert(dk3_define_native(second, "meet", meet, &rendezvous) == DK3_OK);
    auto run = [](DK3VM *vm) {
        assert(dk3_eval(vm, "let answer = meet()", "worker.k3") == DK3_OK);
        DK3Value *value = nullptr; int64_t answer = 0;
        assert(dk3_get(vm, "answer", &value) == DK3_OK);
        assert(dk3_get_int(value, &answer) == DK3_OK && answer == 7);
        assert(dk3_release(value) == DK3_OK);
    };
    std::thread firstWorker(run, first), secondWorker(run, second);
    firstWorker.join(); secondWorker.join();
    assert(rendezvous.entered == 2);
    assert(dk3_destroy(first) == DK3_OK && dk3_destroy(second) == DK3_OK);
    std::cout << "embedding-threads: PASS\n";
}