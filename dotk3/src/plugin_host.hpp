#pragma once
#include "../include/dotk3.h"

namespace dk3 { class Runtime; }

// Lets the CLI expose DK3NativeModule plugins on a Runtime it owns. Destroying the handle
// with dk3_destroy disposes plugin resources but leaves the Runtime alone.
extern "C" {
DK3VM *dk3_attach_runtime(dk3::Runtime *runtime);
DK3Status dk3_install_native_module(DK3VM *vm, const DK3NativeModule *module);
}
