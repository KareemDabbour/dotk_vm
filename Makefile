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

MODE ?= release

CFLAGS_COMMON = -g
LDFLAGS_COMMON = -lm -lsqlite3 -lX11 -ldl

CFLAGS_RELEASE = -O3 -DNDEBUG -flto -march=native -fno-plt
LDFLAGS_RELEASE = -flto

CFLAGS_PGO_GEN = -fprofile-generate
LDFLAGS_PGO_GEN = -fprofile-generate
CFLAGS_PGO_USE = -fprofile-use -fprofile-correction
LDFLAGS_PGO_USE = -fprofile-use -fprofile-correction

CFLAGS_DEBUG = -O0 -g3 -fno-omit-frame-pointer -fsanitize=address
LDFLAGS_DEBUG = -fsanitize=address -static-libasan
CFLAGS_LEGACY = -O3
LDFLAGS_LEGACY =

ifeq ($(MODE),debug)
	CFLAGS = $(CFLAGS_COMMON) $(CFLAGS_DEBUG)
	LDFLAGS = $(LDFLAGS_COMMON) $(LDFLAGS_DEBUG)
else
	CFLAGS = $(CFLAGS_COMMON) $(CFLAGS_RELEASE)
	LDFLAGS = $(LDFLAGS_COMMON) $(LDFLAGS_RELEASE)
endif

all: dir $(exec)

release:
	$(MAKE) MODE=release all

debug:
	$(MAKE) MODE=debug all

pgo-gen: clean dir
	$(MAKE) MODE=release CFLAGS="$(CFLAGS_COMMON) $(CFLAGS_RELEASE) $(CFLAGS_PGO_GEN)" LDFLAGS="$(LDFLAGS_COMMON) $(LDFLAGS_RELEASE) $(LDFLAGS_PGO_GEN)" all

pgo-use: dir clean-objects
	$(MAKE) MODE=release CFLAGS="$(CFLAGS_COMMON) $(CFLAGS_RELEASE) $(CFLAGS_PGO_USE)" LDFLAGS="$(LDFLAGS_COMMON) $(LDFLAGS_RELEASE) $(LDFLAGS_PGO_USE)" all

pgo-train: pgo-gen
	@echo "[pgo-train] running workload to generate profiles..."
	python3 tools/bench_compare.py ./$(exec) ./$(exec) $(workloads_file) 1 > /dev/null
	$(MAKE) pgo-use

$(exec): $(objects)
	$(CC) $^ $(LDFLAGS) -o $@

$(objects): $(out_dir)/%.o : $(sources_dir)/%.c
	$(CC) -c $(CFLAGS) $< -o $@

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
