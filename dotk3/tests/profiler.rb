require 'open3'
require 'json'
require 'tmpdir'
require 'timeout'

binary = File.expand_path(ARGV.fetch(0))
Timeout.timeout(30) do
  Dir.mktmpdir('dotk3-profiler') do |directory|
    source = "fn work(limit) { var total = 0; var index = 0; while index < limit { total += index; index += 1 }; return total }; System.print(work(2000)); System.gc(); System.delay(10)"
    output, errors, result = Open3.capture3(binary, '--profile-vm', 'simple', '-e', source)
    raise 'Simple VM profile failed' unless result.success? && output == "1999000\n" && errors.include?('VM profile (simple)') && errors.include?('GC:') && !errors.include?('Opcodes:') && !errors.include?('Allocation sites')
    speed = File.join(directory, 'source.speedscope.json')
    trace = File.join(directory, 'vm.trace.json')
    output, errors, result = Open3.capture3(binary, '--profile', '--profile-vm', 'verbose', '--profile-output', speed, '--profile-trace', trace, '-O1', '-e', source)
    raise 'Combined profiles changed execution' unless result.success? && output == "1999000\n" && errors.include?('source CPU profile') && errors.include?('Opcodes:') && errors.include?('optimization O1') && errors.include?('1 slot-bytecode calls, 0 lexical-bytecode calls') && errors.include?('System.delay')
    profile = JSON.parse(File.read(speed))
    sampled = profile.fetch('profiles').first
    raise 'Speedscope schema incorrect' unless profile['$schema'].include?('speedscope') && sampled['type'] == 'sampled' && sampled['unit'] == 'nanoseconds' && sampled['samples'].size == sampled['weights'].size && sampled['weights'].sum == sampled['endValue']
    raise 'Source sampler returned no stacks' unless sampled['samples'].any? && profile['shared']['frames'].any? { |frame| frame['name'] == 'work' }
    sampled['samples'].each { |stack| raise 'Invalid sample frame' unless stack.all? { |frame| frame >= 0 && frame < profile['shared']['frames'].size } }
    events = JSON.parse(File.read(trace)).fetch('traceEvents')
    raise 'Perfetto events missing' unless events.any? { |event| event['name'] == 'GC' } && events.any? { |event| event['name'] == 'Compile' } && events.any? { |event| event['name'] == 'native:System.delay' && event['dur'] >= 5000 }
    %w[simple verbose].each do |mode|
      output, errors, result = Open3.capture3(binary, '--profile-vm', mode, '-e', 'fn failed() { missing }; failed()')
      raise 'Failed script lost its exit/profile' unless result.exitstatus == 65 && errors.include?('NameError') && errors.include?('VM profile') && output.empty?
    end
    _, _, result = Open3.capture3(binary, '--profile-vm', 'verbose', '--profile-output', speed, '--profile-trace', speed, '-e', 'null')
    raise 'Conflicting export paths accepted' unless result.exitstatus == 64
    [['--profile-vm', 'bad'], ['--profile-interval-ms', '0'], ['--profile-interval-ms', '1001'], ['--profile-trace', trace]].each do |arguments|
      _, _, result = Open3.capture3(binary, *arguments, '-e', 'null')
      raise 'Invalid profiler options accepted' unless result.exitstatus == 64
    end
    _, errors, result = Open3.capture3(binary, '--profile-output', File.join(directory, 'missing', 'out.json'), '-e', 'null')
    raise 'Output failure was hidden' unless result.exitstatus == 70 && errors.include?('Cannot write profile output')
    output, errors, result = Open3.capture3(binary, '-e', 'System.print(42)')
    raise 'Disabled profiler changed output' unless result.success? && output == "42\n" && errors.empty?
  end
end
puts 'profiler CLI: PASS (simple/verbose/source/exports/errors/disabled)'

if ARGV[1] == '--benchmark'
  source = 'fn step(value) { return value + 1 }; var value = 0; var index = 0; while index < 200000 { value = step(value); index += 1 }; System.print(value)'
  modes = [['off', []], ['source', ['--profile']], ['vm-simple', ['--profile-vm', 'simple']], ['vm-verbose', ['--profile-vm', 'verbose']]]
  samples = modes.map { [] }
  7.times do |iteration|
    order = (0...modes.size).to_a
    order.reverse! if iteration.odd?
    order.each do |index|
      began = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      output, errors, result = Open3.capture3(binary, *modes[index][1], '-e', source)
      elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - began
      raise "Benchmark mode failed: #{modes[index][0]}: #{errors}" unless result.success? && output == "200000\n"
      samples[index] << elapsed * 1000 if iteration >= 2
    end
  end
  report = { binary: binary, source: source, method: 'Whole-process alternating order, two warmups, five recorded samples; native profiling/output overhead included', modes: {} }
  modes.each_with_index { |mode, index| report[:modes][mode[0]] = { median_ms: samples[index].sort[samples[index].size / 2], samples_ms: samples[index] } }
  File.write(ARGV.fetch(2), JSON.pretty_generate(report) + "\n")
  puts JSON.pretty_generate(report[:modes])
end