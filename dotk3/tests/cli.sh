#!/bin/sh
set -eu
binary=${1:?Expected CLI binary path}

output=$("$binary" --help)
case "$output" in
    *Usage:*) ;;
    *) printf '%s\n' 'CLI help missing usage' >&2; exit 1 ;;
esac
output=$("$binary" --version)
case "$output" in
    'DotK 3 '*) ;;
    *) printf '%s\n' 'CLI version missing identifier' >&2; exit 1 ;;
esac
output=$("$binary" -e 'System.print(Reflection.type(Collections.range(2)))')
case "$output" in
    List) ;;
    *) printf '%s\n' 'Core services are not namespaced' >&2; exit 1 ;;
esac
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

expect() {
    if test "$1" != "$2"; then
        printf 'Expected: %s\nActual: %s\n' "$1" "$2" >&2
        exit 1
    fi
}

output=$("$binary" -e 'System.print(System.args)' 'one argument' --literal)
expect '(one argument, --literal)' "$output"
for level in 0 1 2; do
    output=$("$binary" "-O$level" -e 'fn square(value: Int) -> Int { return value * value }; System.print(square(7))')
    expect '49' "$output"
done
if "$binary" -O3 -e 'null' >"$work/optimization-output" 2>"$work/optimization-error"; then exit 1; else exit_code=$?; fi
test "$exit_code" -eq 64
output=$("$binary" --engine bytecode -O0 -e 'fn square(value: Int) -> Int { return value * value }; System.print(square(7))')
expect '49' "$output"
if "$binary" --engine ast -e 'null' >"$work/old-engine-output" 2>"$work/old-engine-error"; then exit 1; else exit_code=$?; fi
test "$exit_code" -eq 64
printf '%s\n' 'export fn leaf() {' '    let values: List<Int> = []' '    System.print(values[1])' '}' >"$work/trace_leaf.k3"
printf '%s\n' 'from trace_leaf import leaf' 'fn main() { leaf() }' 'main()' >"$work/trace_main.k3"
if "$binary" "$work/trace_main.k3" >"$work/trace-output" 2>"$work/trace-error"; then exit 1; else exit_code=$?; fi
test "$exit_code" -eq 65
grep -Fq 'IndexError:' "$work/trace-error"
grep -Fq '3 |     System.print(values[1])' "$work/trace-error"
grep -Fq 'Call stack:' "$work/trace-error"
grep -Fq 'leaf()' "$work/trace-error"
grep -Fq 'main()' "$work/trace-error"
ruby -e 'abort "ANSI escaped into redirected diagnostics" if File.binread(ARGV[0]).include?("\e[")' "$work/trace-error"
output=$("$binary" -e 'var count = 0; while count < 200000 { count += 1; if count % 1000 == 0 { System.gc() } }; System.print(count)')
expect '200000' "$output"
if "$binary" --steps 100 -e 'while true {}' >"$work/steps-output" 2>"$work/steps-error"; then exit 1; else exit_code=$?; fi
test "$exit_code" -eq 65
grep -Fq 'Execution budget exceeded' "$work/steps-error"
if "$binary" --steps -1 -e 'null' >"$work/steps-output" 2>"$work/steps-error"; then exit 1; else exit_code=$?; fi
test "$exit_code" -eq 64
output=$(printf 'Ada\n' | "$binary" -e 'let name = IO.readLine(); IO.write(f"Hello {name}"); System.print(IO.readLine())')
expect 'Hello Adanull' "$output"
output=$("$binary" -e 'class Printed { fn toString() -> String { return "custom" } }; System.print(Printed())')
expect 'custom' "$output"
output=$(printf '%s\n' 'class Printed { fn toString() -> String { return "custom" } }' 'Printed()' ':quit' | "$binary" --repl)
case "$output" in
    *custom*) ;;
    *) printf '%s\n' 'REPL did not use toString' >&2; exit 1 ;;
esac
mkdir -p "$work/files/sub" "$work/outside"
printf 'hello\nworld' >"$work/files/sub/data.txt"
printf '\377\000' >"$work/files/binary.dat"
printf 'outside' >"$work/outside/data.txt"
ln -s "$work/outside/data.txt" "$work/files/escape.txt"
output=$("$binary" --files-root "$work/files" -e 'System.print(Files.readText("sub/data.txt"), Files.size("sub/data.txt"), Files.exists("missing"))')
expect 'hello
world 11 false' "$output"
output=$("$binary" --files-root "$work/files" -e 'let data = Files.readBytes("binary.dat"); System.print(data.length(), data[0], data[1])')
expect '2 255 0' "$output"
printf '%s' '{"value":9007199254740993,"items":[1,2]}' >"$work/files/data.json"
output=$("$binary" --files-root "$work/files" -e 'let value = Json.parse(Files.readText("data.json")) as Map<String, Object?>; System.print(value["value"], Json.stringify(value["items"]))')
expect '9007199254740993 [1,2]' "$output"
output=$("$binary" --files-root "$work/files" -e 'try { Files.readText("binary.dat") } catch error { System.print(error.kind) }')
expect 'value' "$output"
output=$("$binary" -e 'try { Files.readText("sub/data.txt") } catch error { System.print(error.kind) }')
expect 'capability' "$output"
for path in '../outside/data.txt' 'escape.txt' '/etc/passwd' '.'; do
    output=$("$binary" --files-root "$work/files" -e 'try { Files.readText(System.args[0]) } catch error { System.print(error.kind) }' "$path")
    expect 'io' "$output"
done
output=$(printf '%s\n' 'Files.size("sub/data.txt")' ':reset' 'Files.size("sub/data.txt")' ':quit' | "$binary" --files-root "$work/files" --repl)
expect '11
11' "$output"
ruby -e 'File.binwrite(ARGV[0], "x" * (4 * 1024 * 1024 + 1))' "$work/files/large.dat"
ruby -e 'File.binwrite(ARGV[0], "x" * (4 * 1024 * 1024)); File.binwrite(ARGV[1], "")' "$work/files/exact.dat" "$work/files/empty.dat"
output=$("$binary" --files-root "$work/files" -e 'System.print(Files.readBytes("exact.dat").length(), Files.readText("empty.dat").length(), Files.exists("empty.dat"))')
expect '4194304 0 true' "$output"
printf '%s\n' 'Files.readText("must-not-execute.txt")' >"$work/check-files.k3"
output=$("$binary" --files-root "$work/nonexistent-root" --check "$work/check-files.k3")
expect 'check: PASS' "$output"
"$binary" --files-root "$work/nonexistent-root" --ast "$work/check-files.k3" >"$work/file-ast"
grep -Fq 'readText' "$work/file-ast"
output=$("$binary" --files-root "$work/files" -e 'System.print(Files.size("large.dat"))')
expect '4194305' "$output"
if "$binary" --files-root "$work/files" -e 'Files.readBytes("large.dat")' >"$work/files-output" 2>"$work/files-error"; then exit 1; else exit_code=$?; fi
test "$exit_code" -eq 65
grep -Fq 'read limit' "$work/files-error"
if "$binary" --files-root >"$work/files-output" 2>"$work/files-error"; then exit 1; else exit_code=$?; fi
test "$exit_code" -eq 64
output=$("$binary" -e 'fn(value: Int) -> Null { System.print(value) }(7)')
expect '7' "$output"
output=$(printf '%s\n' 'let add = fn(value: Int) -> Int => value + 1' 'add(2)' ':quit' | "$binary" --repl)
expect '3' "$output"
output=$(printf '%s\n' 'record Point { x: Int, y: Int = 2 }' 'let point = Point(1)' 'System.print(Reflection.members(point))' ':members point' ':quit' | "$binary" --repl)
case "$output" in
    *'[x, y]'*'x: Int'*'y: Int'*) ;;
    *) printf '%s\n' 'Record member inspection is missing' >&2; exit 1 ;;
esac
output=$(printf '%s\n' 'System.print(1)' 'System.print(2)' | "$binary")
expect '1
2' "$output"
output=$(printf '%s\n' 'var kept = 7' 'kept + 5' 'fn add(value: Int) -> Int {' 'return value + kept' '}' 'add(3)' 'missing()' 'kept' ':quit' | "$binary" --repl 2>"$work/error")
expect '12
10
7' "$output"
if ! grep -q "Undefined name 'missing'" "$work/error"; then
    printf '%s\n' 'REPL error was not reported' >&2; exit 1
fi
output=$(printf '%s\n' 'defer fn() { System.gc() }(); ["retained", 2]' ':quit' | "$binary" --repl)
expect '[retained, 2]' "$output"
output=$(printf '%s\n' 'System.print("} [ (") # {' '1 +' '2' ':reset' 'let kept = 9' 'kept' ':quit' | "$binary" --repl)
expect '} [ (
3
9' "$output"
printf '%s\n' 'var fromScript = System.args[0]' >"$work/script.k3"
output=$(printf '%s\n' 'fromScript' ':quit' | "$binary" -i "$work/script.k3" 'kept argument')
expect 'kept argument' "$output"
printf '%s\n' 'export let answer = 31' >"$work/sibling.k3"
printf '%s\n' 'import sibling as sibling' 'System.print(sibling.answer, System.args[0])' >"$work/import.k3"
output=$("$binary" "$work/import.k3" argument)
expect '31 argument' "$output"
printf '%s\n' 'import "sibling.k3" as explicit' 'System.print(explicit.answer)' >"$work/explicit.k3"
output=$("$binary" "$work/explicit.k3")
expect '31' "$output"
mkdir "$work/nested"
printf '%s\n' 'export let answer = 19' >"$work/nested/library.k3"
printf '%s\n' 'import nested.library as dotted' 'System.print(dotted.answer)' >"$work/dotted.k3"
output=$("$binary" "$work/dotted.k3")
expect '19' "$output"
output=$(printf '%s\n' 'fn worker() { defer System.print("shutdown")' 'yield null' '}' 'Tasks.spawn(worker())' 'Tasks.pump(1)' ':quit' | "$binary" --repl)
case "$output" in
    *shutdown) ;;
    *) printf '%s\n' 'REPL did not clean up tasks' >&2; exit 1 ;;
esac
output=$(printf '%s\n' 'fn worker() { defer System.print("reset cleanup")' 'yield null' '}' 'Tasks.spawn(worker())' 'Tasks.pump(1)' ':reset' '1 + 2' ':quit' | "$binary" --repl)
case "$output" in
    *'reset cleanup
3') ;;
    *) printf '%s\n' 'Reset did not clean up tasks' >&2; exit 1 ;;
esac
if printf '%s\n' 'fn unfinished() {' | "$binary" --repl >"$work/output" 2>"$work/error"; then
    printf '%s\n' 'Incomplete EOF was accepted' >&2; exit 1
else
    exit_code=$?; test "$exit_code" -eq 65
fi
if "$binary" --unknown >"$work/output" 2>"$work/error"; then
    printf '%s\n' 'Unknown CLI option was accepted' >&2; exit 1
else
    exit_code=$?; test "$exit_code" -eq 64
fi
printf '%s\n' 'cli: PASS'
printf '%s\n' 'fn add(value: Int) -> Int {' 'let local = value + 1' 'return local' '}' 'var result = add(2)' 'result += 1' 'System.print(result)' >"$work/debug.k3"
printf '%s\n' 'b 3' 'breakpoints' 'c' 'locals' 'bt' 'frame 1' 'locals' 'frame 0' 'p local' 'p missing()' 'f' 'locals' 'n' 'globals' 'c' | "$binary" --debug "$work/debug.k3" >"$work/debug-output"
grep -Eq 'local[[:space:]]+Int[[:space:]]+3' "$work/debug-output"
grep -Eq '#0[[:space:]]+add' "$work/debug-output"
grep -Fq 'local = 3' "$work/debug-output"
grep -Fq 'expressions are not executed' "$work/debug-output"
grep -Fq "Paused $work/debug.k3:6" "$work/debug-output"
grep -Eq 'result[[:space:]]+Int[[:space:]]+4' "$work/debug-output"
grep -Fq 'debug: finished' "$work/debug-output"
printf '%s\n' 'q' | "$binary" --debug "$work/debug.k3" >"$work/debug-output"
grep -Fq 'debug: stopped' "$work/debug-output"
printf '%s\n' 'fn fails() { defer fn() { throw "cleanup failure" }()' 'yield null }' 'let job = Tasks.spawn(fails())' 'Tasks.pump(1)' 'let stopHere = 0' >"$work/cleanup-debug.k3"
if printf '%s\n' 'b 5' 'c' 'q' | "$binary" --debug "$work/cleanup-debug.k3" >"$work/output" 2>"$work/error"; then
    printf '%s\n' 'Debugger quit hid a shutdown failure' >&2; exit 1
else
    exit_code=$?; test "$exit_code" -eq 65
fi
grep -Fq 'cleanup failure' "$work/error"
if "$binary" --debug - >"$work/output" 2>"$work/error"; then
    printf '%s\n' 'Debugger accepted stdin as both script and command stream' >&2; exit 1
else
    exit_code=$?; test "$exit_code" -eq 64
fi
printf '%s\n' 'debugger CLI: PASS'
printf '%s\n' 'fn worker() {' 'yield null' '}' 'let first = Tasks.spawn(worker())' 'let second = Tasks.spawn(worker())' 'Tasks.pump(1)' >"$work/task-debug.k3"
printf '%s\n' 'b 2' 'c' 'tasks' 'c' | "$binary" --debug "$work/task-debug.k3" >"$work/debug-output"
grep -Fq 'Tasks | Active scheduler entries' "$work/debug-output"
grep -Eq 'running[[:space:]]+[0-9]+[[:space:]]+worker' "$work/debug-output"
grep -Eq 'ready[[:space:]]+[0-9]+[[:space:]]+worker' "$work/debug-output"
mkdir -p "$work/checks/nested"
printf '%s\n' 'record Point { x: Int }' 'let point = Point(1)' "System.print('CHECK_MUST_NOT_EXECUTE')" >"$work/checks/good.k3"
printf '%s\n' 'enum Event { Move(Int), Quit }' 'let event = Event.Quit()' 'let result = match event { Event.Move(value) => value }' >"$work/checks/nested/missing.k3"
printf '%s\n' 'record Point { x: Int }' "let point = Point('bad')" >"$work/checks/wrong.k3"
printf '%s\n' 'invalid language text' >"$work/checks/ignored.txt"
if "$binary" --check "$work/checks" >"$work/output" 2>"$work/error"; then
    printf '%s\n' 'Project checker accepted invalid records/enums' >&2; exit 1
else
    exit_code=$?; test "$exit_code" -eq 65
fi
grep -Fq 'missing Event.Quit' "$work/error"
grep -Fq 'Expected Int, inferred String' "$work/error"
grep -Fq 'check: 1/3 files passed' "$work/output"
if grep -Fq 'CHECK_MUST_NOT_EXECUTE' "$work/output"; then
    printf '%s\n' 'Checker executed script output' >&2; exit 1
fi
output=$("$binary" --check "$work/checks/good.k3" "$work/checks/../checks/good.k3")
case "$output" in
    *'check: 1/1 files passed') ;;
    *) printf '%s\n' 'Check path deduplication failed' >&2; exit 1 ;;
esac
output=$(printf '%s\n' 'System.print(System.args)' | "$binary" --check -)
expect 'check: PASS' "$output"
mkdir "$work/empty-check"
if "$binary" --check "$work/empty-check" >"$work/output" 2>"$work/error"; then
    printf '%s\n' 'Empty project check passed silently' >&2; exit 1
else
    exit_code=$?; test "$exit_code" -eq 64
fi
printf '%s\n' 'project checker CLI: PASS'
printf '%s\n' 'record Loaded { value: Int }' 'let loadedValue = Loaded(17)' >"$work/load sample.k3"
output=$(printf ':check "%s"\n:load "%s"\nloadedValue.value\n:quit\n' "$work/load sample.k3" "$work/load sample.k3" | "$binary" --repl)
expect 'check: PASS
17' "$output"
printf '%s\n' "System.print('AST_MUST_NOT_EXECUTE')" >"$work/ast.k3"
output=$(printf ':ast "%s"\n:quit\n' "$work/ast.k3" | "$binary" --repl)
case "$output" in
    *'call '* ) ;;
    *) printf '%s\n' 'Shell AST command did not parse source' >&2; exit 1 ;;
esac
if printf '%s\n' "$output" | grep -qx 'AST_MUST_NOT_EXECUTE'; then
    printf '%s\n' 'Shell AST command executed source' >&2; exit 1
fi
output=$(printf ':load "%s"\n:load\n:check -\n1 + 2\n:quit\n' "$work/nonexistent.k3" | "$binary" --repl 2>"$work/error")
expect '3' "$output"
grep -Fq 'Cannot read' "$work/error"
printf '%s\n' 'shell file commands: PASS'
output=$(printf '%s\n' 'fn documented() {' '"""Multiple' 'lines of documentation."""' 'return 2' '}' 'Reflection.doc(documented)' ':quit' | "$binary" --repl)
case "$output" in
    *'Multiple'*'lines of documentation.'*) ;;
    *) printf '%s\n' 'Multiline REPL docstring failed' >&2; exit 1 ;;
esac
printf '%s\n' "System.print('BAD_PATH_MUST_NOT_EXECUTE')" >"$work/do-not-load.k3"
output=$(printf ':load "%s" "unterminated\n1 + 2\n:quit\n' "$work/do-not-load.k3" | "$binary" --repl 2>"$work/error")
expect '3' "$output"
grep -Fq 'Invalid quoted path' "$work/error"