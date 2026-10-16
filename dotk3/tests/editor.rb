require 'json'

root = File.expand_path('../editor', __dir__)
manifest = JSON.parse(File.read(File.join(root, 'package.json')))
language = manifest.fetch('contributes').fetch('languages').first
raise 'Wrong language extension' unless language.fetch('extensions') == ['.k3']
configuration = JSON.parse(File.read(File.join(root, language.fetch('configuration'))))
raise 'Wrong comment marker' unless configuration.fetch('comments').fetch('lineComment') == '#'
grammar_entry = manifest.fetch('contributes').fetch('grammars').first
grammar = JSON.parse(File.read(File.join(root, grammar_entry.fetch('path'))))
raise 'Scope mismatch' unless grammar.fetch('scopeName') == grammar_entry.fetch('scopeName')

visit = lambda do |node|
  case node
  when Hash
    node.each do |key, value|
      Regexp.new(value) if ['match', 'begin', 'end', 'increaseIndentPattern', 'decreaseIndentPattern'].include?(key)
      if key == 'include' && value.start_with?('#')
        raise "Missing grammar include #{value}" unless grammar.fetch('repository').key?(value[1..-1])
      end
      visit.call(value)
    end
  when Array
    node.each { |child| visit.call(child) }
  end
end
visit.call(grammar)
visit.call(configuration)
patterns = grammar.fetch('patterns')
raise 'Missing record declaration highlighting' unless patterns.any? { |entry| entry['match'] && Regexp.new(entry['match']).match?('record Point') && entry['captures'] }
raise 'Missing enum declaration highlighting' unless patterns.any? { |entry| entry['match'] && Regexp.new(entry['match']).match?('enum Event') && entry['captures'] }
raise 'Missing enum variant highlighting' unless patterns.any? { |entry| entry['match'] && Regexp.new(entry['match']).match?('Event.Move') && entry['captures'] }
raise 'Missing numeric highlighting' unless patterns.any? { |entry| entry['name'] == 'constant.numeric.dotk3' && Regexp.new(entry.fetch('match')).match?('9_000') }
puts 'editor assets: PASS'