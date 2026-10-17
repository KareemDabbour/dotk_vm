CXX ?= c++
MODE ?= release
BUILD = build/$(MODE)
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -g
CLI_LIBS ?= -ledit -lcurl
NETWORK_OBJECTS = $(BUILD)/network_module.o $(BUILD)/requests_module.o $(BUILD)/picohttpparser.o $(BUILD)/bytecode.o $(BUILD)/runtime_bytecode.o $(BUILD)/profiler.o $(BUILD)/native_methods.o
NETWORK_OBJECTS += $(BUILD)/native_list_methods.o $(BUILD)/native_map_methods.o $(BUILD)/native_set_methods.o $(BUILD)/native_text_methods.o
ifeq ($(OS),Windows_NT)
CLI_LIBS =
endif
ifeq ($(MODE),stress)
CXXFLAGS += -O0 -fsanitize=undefined -fno-sanitize-recover=all
LDFLAGS += -fsanitize=undefined
else
CXXFLAGS += -O2
endif

.PHONY: all library lsp example-native test test-lexer test-parser test-runtime test-profiler test-checker test-typer test-embedding test-generators test-tasks test-embedding-threads test-cli test-diagnostics test-network test-debugger test-editor test-lsp clean
all: $(BUILD)/dotk3 $(BUILD)/dotk3-lsp

$(BUILD)/dotk3: $(BUILD)/lexer.o $(BUILD)/parser.o $(BUILD)/runtime.o $(BUILD)/core_module.o $(NETWORK_OBJECTS) $(BUILD)/json_module.o $(BUILD)/checker.o $(BUILD)/debugger.o $(BUILD)/main.o
	$(CXX) $^ $(LDFLAGS) $(CLI_LIBS) -o $@

$(BUILD)/dotk3-lsp: $(BUILD)/lexer.o $(BUILD)/parser.o $(BUILD)/checker.o $(BUILD)/semantic.o $(BUILD)/lsp_main.o
	$(CXX) $^ $(LDFLAGS) -o $@

lsp: $(BUILD)/dotk3-lsp

test-lsp: $(BUILD)/dotk3-lsp
	ruby tests/lsp.rb $(BUILD)/dotk3-lsp

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -MMD -MP -I src -c $< -o $@

$(BUILD)/picohttpparser.o: vendor/picohttpparser.c vendor/picohttpparser.h | $(BUILD)
	$(CC) -std=c11 -Wall -Wextra -g $(if $(filter stress,$(MODE)),-O0 $(LDFLAGS),-O2) -c $< -o $@

$(BUILD)/lexer-test: tests/lexer.cpp $(BUILD)/lexer.o
	$(CXX) $(CXXFLAGS) -I src $^ $(LDFLAGS) -o $@

test-lexer: $(BUILD)/lexer-test
	$(BUILD)/lexer-test

$(BUILD)/parser-test: tests/parser.cpp $(BUILD)/lexer.o $(BUILD)/parser.o
	$(CXX) $(CXXFLAGS) -I src $^ $(LDFLAGS) -o $@

test-parser: $(BUILD)/parser-test
	$(BUILD)/parser-test

$(BUILD)/runtime-test: tests/runtime.cpp $(BUILD)/lexer.o $(BUILD)/parser.o $(BUILD)/runtime.o $(BUILD)/core_module.o $(NETWORK_OBJECTS) $(BUILD)/json_module.o
	$(CXX) $(CXXFLAGS) -I src $^ $(LDFLAGS) -o $@

test-runtime: $(BUILD)/runtime-test
	$(BUILD)/runtime-test

$(BUILD)/profiler-test: tests/profiler.cpp $(BUILD)/lexer.o $(BUILD)/parser.o $(BUILD)/runtime.o $(BUILD)/core_module.o $(NETWORK_OBJECTS) $(BUILD)/json_module.o
	$(CXX) $(CXXFLAGS) -I src $^ $(LDFLAGS) -o $@

test-profiler: $(BUILD)/profiler-test $(BUILD)/dotk3
	$(BUILD)/profiler-test
	ruby tests/profiler.rb $(BUILD)/dotk3

$(BUILD)/generators-test: tests/generators.cpp $(BUILD)/lexer.o $(BUILD)/parser.o $(BUILD)/runtime.o $(BUILD)/core_module.o $(NETWORK_OBJECTS) $(BUILD)/json_module.o
	$(CXX) $(CXXFLAGS) -I src $^ $(LDFLAGS) -o $@

test-generators: $(BUILD)/generators-test
	$(BUILD)/generators-test

$(BUILD)/tasks-test: tests/tasks.cpp $(BUILD)/lexer.o $(BUILD)/parser.o $(BUILD)/runtime.o $(BUILD)/core_module.o $(NETWORK_OBJECTS) $(BUILD)/json_module.o
	$(CXX) $(CXXFLAGS) -I src $^ $(LDFLAGS) -o $@

test-tasks: $(BUILD)/tasks-test
	$(BUILD)/tasks-test

$(BUILD)/checker-test: tests/checker.cpp $(BUILD)/lexer.o $(BUILD)/parser.o $(BUILD)/checker.o
	$(CXX) $(CXXFLAGS) -I src $^ $(LDFLAGS) -o $@

test-checker: $(BUILD)/checker-test
	$(BUILD)/checker-test

$(BUILD)/typer-test: tests/typer.cpp $(BUILD)/lexer.o $(BUILD)/parser.o $(BUILD)/types.o $(BUILD)/typer.o
	$(CXX) $(CXXFLAGS) -I src $^ $(LDFLAGS) -o $@

test-typer: $(BUILD)/typer-test
	$(BUILD)/typer-test

$(BUILD)/libdotk3.a: $(BUILD)/lexer.o $(BUILD)/parser.o $(BUILD)/runtime.o $(BUILD)/core_module.o $(NETWORK_OBJECTS) $(BUILD)/json_module.o $(BUILD)/debugger.o $(BUILD)/embedding.o
	$(AR) rcs $@ $^

library: $(BUILD)/libdotk3.a

$(BUILD)/structured-host.o: examples/structured_host.c include/dotk3.h | $(BUILD)
	$(CC) -std=c11 -Wall -Wextra -I include -c $< -o $@

$(BUILD)/structured-host: $(BUILD)/structured-host.o $(BUILD)/libdotk3.a
	$(CXX) $^ $(LDFLAGS) -o $@

example-native: $(BUILD)/structured-host
	$(BUILD)/structured-host

$(BUILD)/embedding-test.o: tests/embedding.c include/dotk3.h | $(BUILD)
	$(CC) -std=c11 -Wall -Wextra -I include -c $< -o $@

$(BUILD)/embedding-test: $(BUILD)/embedding-test.o $(BUILD)/libdotk3.a
	$(CXX) $^ $(LDFLAGS) -o $@

test-embedding: $(BUILD)/embedding-test
	$(BUILD)/embedding-test

$(BUILD)/embedding-threads-test: tests/embedding_threads.cpp include/dotk3.h $(BUILD)/libdotk3.a
	$(CXX) $(CXXFLAGS) -I include tests/embedding_threads.cpp $(BUILD)/libdotk3.a $(LDFLAGS) -pthread -o $@

test-embedding-threads: $(BUILD)/embedding-threads-test
	$(BUILD)/embedding-threads-test

$(BUILD)/debugger-test: tests/debugger.cpp $(BUILD)/debugger.o $(BUILD)/lexer.o $(BUILD)/parser.o $(BUILD)/runtime.o $(BUILD)/core_module.o $(NETWORK_OBJECTS) $(BUILD)/json_module.o
	$(CXX) $(CXXFLAGS) -I src $^ $(LDFLAGS) -o $@

test-debugger: $(BUILD)/debugger-test
	$(BUILD)/debugger-test

test-cli: $(BUILD)/dotk3
	sh tests/cli.sh $(BUILD)/dotk3
	ruby tests/repl_terminal.rb $(BUILD)/dotk3

test-diagnostics: $(BUILD)/dotk3 test-lexer test-runtime
	sh tests/cli.sh $(BUILD)/dotk3
	ruby tests/repl_terminal.rb $(BUILD)/dotk3 --diagnostics-only

test-network: $(BUILD)/dotk3
	ruby tests/network.rb $(BUILD)/dotk3

test-editor: $(BUILD)/dotk3-lsp
	ruby tests/editor.rb
	node --check editor/extension.js
	node --check examples/browser_ui/app.js
	node tests/editor_client.js

test: test-lexer test-parser test-runtime test-profiler test-checker test-typer test-embedding test-generators test-tasks test-embedding-threads test-cli test-network test-debugger test-editor test-lsp

clean:
	rm -rf build

-include $(wildcard $(BUILD)/*.d)