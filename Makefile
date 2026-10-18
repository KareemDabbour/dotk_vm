CC = gcc
exec = dotk.out
exec_glob = dotk
baseline_exec = benchmarks/bin/dotk-baseline.out
candidate_exec = benchmarks/bin/dotk-candidate.out
nan_exec = benchmarks/bin/dotk-nan.out
workloads_file = benchmarks/workloads.txt
sources_dir = src
out_dir = out/$(MODE)
sources = $(wildcard $(sources_dir)/*.c)
objects = $(patsubst $(sources_dir)/%.c,$(out_dir)/%.o, $(sources))
library_sources = $(filter-out $(sources_dir)/main.c,$(sources))
library_objects = $(patsubst $(sources_dir)/%.c,$(out_dir)/library/%.o,$(library_sources))
library_exec = $(out_dir)/libdotk.a
ifeq ($(shell uname -s),Darwin)
shared_library_exec = $(out_dir)/libdotk.dylib
shared_library_flags = -dynamiclib
else
shared_library_exec = $(out_dir)/libdotk.so
shared_library_flags = -shared
endif

MODE ?= release

CFLAGS_COMMON = -g
LDFLAGS_COMMON = -lm -lpthread -lsqlite3 -ldl #-lX11

CFLAGS_RELEASE = -O3 -DNDEBUG -flto -march=native -fno-plt
LDFLAGS_RELEASE = -flto

CFLAGS_PGO_GEN = -fprofile-generate
LDFLAGS_PGO_GEN = -fprofile-generate
CFLAGS_PGO_USE = -fprofile-use -fprofile-correction
LDFLAGS_PGO_USE = -fprofile-use -fprofile-correction

CFLAGS_DEBUG = -O0 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined
LDFLAGS_DEBUG = -fsanitize=address,undefined
CFLAGS_STRESS = -O0 -g3 -fno-omit-frame-pointer -DDEBUG_STRESS_GC=1 -fsanitize=undefined -fno-sanitize-recover=all
LDFLAGS_STRESS = -fsanitize=undefined
CFLAGS_LEGACY = -O3
LDFLAGS_LEGACY =

ifeq ($(MODE),debug)
	CFLAGS = $(CFLAGS_COMMON) $(CFLAGS_DEBUG)
	LDFLAGS = $(LDFLAGS_COMMON) $(LDFLAGS_DEBUG)
else ifeq ($(MODE),stress)
	CFLAGS = $(CFLAGS_COMMON) $(CFLAGS_STRESS)
	LDFLAGS = $(LDFLAGS_COMMON) $(LDFLAGS_STRESS)
else
	CFLAGS = $(CFLAGS_COMMON) $(CFLAGS_RELEASE)
	LDFLAGS = $(LDFLAGS_COMMON) $(LDFLAGS_RELEASE)
endif

.PHONY: all dir release debug stress library shared-library test-embedding bench-embedding examples test test-fast test-stress FORCE

all: dir $(exec)

library: $(library_exec)

shared-library: $(shared_library_exec)

$(shared_library_exec): $(library_objects)
	$(CC) $(shared_library_flags) $(library_objects) $(LDFLAGS) -o $@

test-embedding: $(library_exec)
	$(CC) $(CFLAGS) -UNDEBUG -std=c11 -Wall -Wextra -I src/include tests/embedding/host.c $(library_exec) $(LDFLAGS) -o $(out_dir)/embedding-host
	$(out_dir)/embedding-host
	$(CXX) $(CFLAGS) -UNDEBUG -x c++ -std=c++17 -Wall -Wextra -I src/include -c tests/embedding/host.c -o $(out_dir)/embedding-host-cpp.o
	$(CXX) $(out_dir)/embedding-host-cpp.o $(library_exec) $(LDFLAGS) -o $(out_dir)/embedding-host-cpp
	$(out_dir)/embedding-host-cpp

bench-embedding: $(library_exec)
	$(CC) $(CFLAGS) -std=c11 -I src/include benchmarks/embedding_calls.c $(library_exec) $(LDFLAGS) -o $(out_dir)/embedding-bench
	$(out_dir)/embedding-bench

examples: $(library_exec)
	$(CC) $(CFLAGS) -std=c11 -Wall -Wextra -I src/include examples/updates/host_demo.c $(library_exec) $(LDFLAGS) -o $(out_dir)/updates-demo
	$(out_dir)/updates-demo all

$(library_exec): $(library_objects)
	$(AR) rcs $@ $(library_objects)

$(library_objects): $(out_dir)/library/%.o : $(sources_dir)/%.c Makefile
	mkdir -p $(dir $@)
	$(CC) -MMD -MP -fPIC -fvisibility=hidden -c $(CFLAGS) $< -o $@

release:
	$(MAKE) MODE=release all

debug:
	$(MAKE) MODE=debug all

stress:
	$(MAKE) MODE=stress all

test-stress: stress
	./$(exec) --test-all tests

pgo-gen: clean dir
	$(MAKE) MODE=release CFLAGS="$(CFLAGS_COMMON) $(CFLAGS_RELEASE) $(CFLAGS_PGO_GEN)" LDFLAGS="$(LDFLAGS_COMMON) $(LDFLAGS_RELEASE) $(LDFLAGS_PGO_GEN)" all

pgo-use: dir clean-objects
	$(MAKE) MODE=release CFLAGS="$(CFLAGS_COMMON) $(CFLAGS_RELEASE) $(CFLAGS_PGO_USE)" LDFLAGS="$(LDFLAGS_COMMON) $(LDFLAGS_RELEASE) $(LDFLAGS_PGO_USE)" all

pgo-train: pgo-gen
	@echo "[pgo-train] running workload to generate profiles..."
	python3 tools/bench_compare.py ./$(exec) ./$(exec) $(workloads_file) 1 > /dev/null
	$(MAKE) pgo-use

$(exec): $(objects) FORCE
	$(CC) $(objects) $(LDFLAGS) -o $@

FORCE:

$(objects): $(out_dir)/%.o : $(sources_dir)/%.c | dir
	$(CC) -MMD -MP -c $(CFLAGS) $< -o $@

-include $(objects:.o=.d)
-include $(library_objects:.o=.d)

rebuild:
	make clean
	make all

dir:
	mkdir -p $(out_dir)
	mkdir -p benchmarks/bin

install:
	make all
	cp ./$(exec) /usr/local/bin/$(exec_glob)

test: all
	./$(exec) --test-all tests

test-fast: all
	./$(exec) --test-all tests --stop-on-fail

save-baseline: all
	cp ./$(exec) $(baseline_exec)
	@echo "Saved baseline binary to $(baseline_exec)"

save-legacy-baseline: clean dir
	$(MAKE) MODE=release CFLAGS="$(CFLAGS_COMMON) $(CFLAGS_LEGACY)" LDFLAGS="$(LDFLAGS_COMMON) $(LDFLAGS_LEGACY)" all
	cp ./$(exec) $(baseline_exec)
	@echo "Saved legacy-style baseline binary to $(baseline_exec)"
	$(MAKE) MODE=release all

save-candidate: all
	cp ./$(exec) $(candidate_exec)
	@echo "Saved candidate binary to $(candidate_exec)"

snapshot: all
	@ts=$$(date +%Y%m%d-%H%M%S); \
	cp ./$(exec) benchmarks/bin/dotk-$$ts.out; \
	echo "Snapshot saved to benchmarks/bin/dotk-$$ts.out"

bench: all
	python3 tools/bench_compare.py $(baseline_exec) ./$(exec) $(workloads_file) 5

bench-long: all
	python3 tools/bench_compare.py $(baseline_exec) ./$(exec) $(workloads_file) 25

bench-pool: all
	python3 tools/bench_pool_tuning.py ./$(exec)

bench-pool-long: all
	python3 tools/bench_pool_tuning.py ./$(exec) --iters 10 --tasks 1000 --loops 5000 --limits 8,16,32,64,128,256

bench-legacy: save-legacy-baseline
	python3 tools/bench_compare.py $(baseline_exec) ./$(exec) $(workloads_file) 5

bench-candidate: all
	python3 tools/bench_compare.py $(baseline_exec) $(candidate_exec) $(workloads_file) 5

nan: dir clean-objects
	$(MAKE) MODE=release CFLAGS="$(CFLAGS_COMMON) $(CFLAGS_RELEASE) -DNAN_BOXING" LDFLAGS="$(LDFLAGS_COMMON) $(LDFLAGS_RELEASE)" all

nan-test-fast: nan
	./$(exec) --test-all tests --stop-on-fail

save-nan-candidate: nan
	cp ./$(exec) $(nan_exec)
	@echo "Saved NaN-boxing binary to $(nan_exec)"

bench-nan: all save-baseline save-nan-candidate
	python3 tools/bench_compare.py $(baseline_exec) $(nan_exec) $(workloads_file) 5

clean:
	-rm *.out
	-rm -r out

clean-objects:
	-rm -f out/release/*.o

json-module:
	mkdir -p modules
	$(CC) -shared -fPIC -O3 -DNDEBUG -I./src/include modules/json_module.c -o modules/json_module.so

raylib-module:
	mkdir -p modules
	$(CC) -shared -fPIC -O3 -DNDEBUG -I./src/include modules/raylib_module.c -o modules/raylib_module.so $$(pkg-config --cflags --libs raylib 2>/dev/null || echo "-lraylib -lm -lpthread -ldl -lrt -lX11")

sdl-module:
	mkdir -p modules
	$(CC) -shared -fPIC -O3 -DNDEBUG -I./src/include modules/sdl_module.c -o modules/sdl_module.so $$(pkg-config --cflags --libs sdl2 2>/dev/null || echo "-lSDL2")

x11-module:
	mkdir -p modules
	$(CC) -shared -fPIC -O3 -DNDEBUG -I./src/include modules/x11_module.c -o modules/x11_module.so -lX11

ndarray-module:
	mkdir -p modules
	$(CC) -shared -fPIC -O3 -DNDEBUG -I./src/include modules/ndarray_module.c -o modules/ndarray_module.so
