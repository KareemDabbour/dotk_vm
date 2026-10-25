require 'json'
require 'open3'
require 'timeout'
require 'tmpdir'
require 'fileutils'

def send_message(writer, message)
  body = JSON.generate(message)
  writer.write("Content-Length: #{body.bytesize}\r\n\r\n#{body}")
  writer.flush
end

def receive_message(reader)
  length = nil
  while (header = reader.gets)
    break if header == "\r\n"
    length = Integer(header.split(':', 2).last.strip) if header.start_with?('Content-Length:')
  end
  raise 'Missing server frame' unless length
  body = reader.read(length)
  raise 'Truncated server frame' unless body && body.bytesize == length
  JSON.parse(body)
end

Timeout.timeout(15) do
  Open3.popen3(ARGV.fetch(0)) do |writer, reader, error, process|
    send_message(writer, { jsonrpc: '2.0', id: 'early', method: 'textDocument/documentSymbol', params: {} })
    raise 'Server accepted pre-initialize request' unless receive_message(reader).dig('error', 'code') == -32002
    send_message(writer, { jsonrpc: '2.0', id: 1, method: 'initialize', params: {} })
    initialized = receive_message(reader)
    raise 'Initialization failed' unless initialized['id'] == 1 && initialized.dig('result', 'capabilities', 'positionEncoding') == 'utf-16'
    uri = 'file:///workspace/example.k3'
    send_message(writer, { jsonrpc: '2.0', method: 'initialized', params: {} })
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: uri, languageId: 'dotk3', version: 1, text: "let value: Int = 'bad'" } } })
    diagnostic = receive_message(reader)
    raise 'Missing document diagnostics' unless diagnostic['method'] == 'textDocument/publishDiagnostics' && diagnostic.dig('params', 'uri') == uri
    raise 'Missing type error' unless diagnostic.dig('params', 'diagnostics', 0, 'message').include?('Expected Int')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: uri, version: 2 }, contentChanges: [{ text: 'let value: Int = 2' }] } })
    fixed = receive_message(reader)
    raise 'Diagnostics were not cleared' unless fixed.dig('params', 'version') == 2 && fixed.dig('params', 'diagnostics') == []
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: uri, version: 1 }, contentChanges: [{ text: 'bad stale source' }] } })
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didSave', params: { textDocument: { uri: uri } } })
    raise 'Stale edit replaced current text' unless receive_message(reader).dig('params', 'diagnostics') == []
    unicode = "let label = '\u{1F600}'; missing()"
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: uri, version: 3 }, contentChanges: [{ text: unicode }] } })
    diagnostic = receive_message(reader)
    prefix = unicode.split('missing', 2).first
    expected = prefix.encode('UTF-16LE').bytesize / 2
    raise 'Incorrect UTF-16 position' unless diagnostic.dig('params', 'diagnostics', 0, 'range', 'start', 'character') == expected
    outline = "record Point { x: Int }\r\nenum Event { Move(Point), Quit }\r\nfn name(value: Event) -> String { return 'name' }\r\nlet value = Event.Quit"
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: uri, version: 4 }, contentChanges: [{ text: outline }] } })
    raise 'Valid outline source rejected' unless receive_message(reader).dig('params', 'diagnostics') == []
    send_message(writer, { jsonrpc: '2.0', id: 'outline', method: 'textDocument/documentSymbol', params: { textDocument: { uri: uri } } })
    symbols = receive_message(reader)
    raise 'Outline missing declarations' unless symbols['id'] == 'outline' && symbols['result'].map { |entry| entry['name'] } == ['Point', 'Event', 'name', 'value']
    raise 'Outline missing enum variants' unless symbols['result'][1]['children'].map { |entry| entry['name'] } == ['Move', 'Quit']
    raise 'CRLF positions incorrect' unless symbols['result'][1].dig('selectionRange', 'start', 'line') == 1
    send_message(writer, { jsonrpc: '2.0', id: 9, method: 'unknown/request', params: {} })
    raise 'Unknown request was not rejected' unless receive_message(reader).dig('error', 'code') == -32601
    writer.write("Content-Length: 1\r\n\r\n{"); writer.flush
    raise 'Malformed JSON not handled' unless receive_message(reader).dig('error', 'code') == -32700
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didClose', params: { textDocument: { uri: uri } } })
    closed = receive_message(reader)
    raise 'Close did not clear diagnostics' unless closed.dig('params', 'diagnostics') == []
    probe = 'untitled:DotK3Integration'
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: probe, languageId: 'dotk3', version: 1, text: "let lsp_probe: Int = 'bad'" } } })
    probeError = receive_message(reader)
    raise 'Untitled diagnostic missing' unless probeError.dig('params', 'uri') == probe && probeError.dig('params', 'diagnostics', 0, 'message').include?('Expected Int')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: probe, version: 2 }, contentChanges: [{ text: 'let lsp_probe: Int = 1' }] } })
    raise 'Untitled fix did not clear error' unless receive_message(reader).dig('params', 'diagnostics') == []
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didClose', params: { textDocument: { uri: probe } } })
    raise 'Untitled close did not clear errors' unless receive_message(reader).dig('params', 'diagnostics') == []
    bindingUri = 'untitled:BindingDetails'
    bindingCases = [
      ["var path = 'object_lifecycle.k3'\npath", 'var path: String', 'Type inferred from initializer'],
      ["let source: String = Files.readText('must-not-execute')\nsource", 'let source: String', 'Declared type'],
      ["let data: Bytes = Files.readBytes('must-not-execute')\ndata", 'let data: Bytes', 'Declared type'],
      ["let count = 42\ncount", 'let count: Int = 42', 'Type inferred from initializer'],
      ["var active = true\nactive", 'var active: Bool = true', 'Type inferred from initializer'],
      ["let values: List<Int> = [1,2]\nvalues", 'let values: List<Int>', 'Declared type'],
      ["class Item {}\nlet item = Item()\nitem", 'let item: Item', 'Type inferred from initializer'],
      ["let callback = fn(value: Int) -> Int => value + 1\ncallback", 'let callback: fn(Int) -> Int', 'fn callback(value: Int) -> Int'],
      ["export let version: String = 'alpha'", 'export let version: String', 'Declared type'],
      ["let dynamic: Object = 1\ndynamic", 'let dynamic: Object', 'Declared type'],
      ["let total = 1 + 2\ntotal", 'let total: Int', 'Type inferred from initializer'],
      ["let values = [1,2]\nvalues", 'let values: List<Int>', 'Type inferred from initializer'],
      ["let scores = {'Ada': 1}\nscores", 'let scores: Map<String, Int>', 'Type inferred from initializer'],
      ["let maybe: Int? = null\nlet result = maybe ?? 3\nresult", 'let result: Int', 'Type inferred from initializer'],
      ["fn unavailable() { throw 'must not execute' }\nlet result = unavailable()\nresult", 'let result: Null', 'Type inferred from initializer'],
      ["from Files import readText as read\nread", 'from Files import readText as read', 'readText(path: String) -> String'],
      ["fn items() { yield 1 }\nlet stream = items()\nstream", 'let stream: Generator', 'Type inferred from initializer'],
      ["let maybe: Int? = null\nmaybe", 'let maybe: Int?', 'Declared type'],
      ["let original = 'name'\nlet alias = original\nalias", 'let alias: String', 'Type inferred from initializer'],
      ["let original = 'outside'\nfn local() {\nvar original: Int = 2\noriginal\n}", 'var original: Int = 2', 'Declared type'],
      ["let declaration: String = 'name'", 'let declaration: String', 'Declared type'],
      ["for (var counter: Int = 0; counter < 3; counter++) {\ncounter\n}", 'var counter: Int = 0', 'Declared type']
    ]
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: bindingUri, languageId: 'dotk3', version: 1, text: '' } } })
    receive_message(reader)
    bindingCases.each_with_index do |(text, signature, extra), index|
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: bindingUri, version: index + 2 }, contentChanges: [{ text: text }] } })
      raise "Binding hover source rejected: #{index}" unless receive_message(reader).dig('params', 'diagnostics') == []
      lines = text.split("\n")
      line = lines.last == '}' ? lines.length - 2 : lines.length - 1
      character = lines.length == 1 ? lines.last.index(/version|declaration/) + 2 : 2
      send_message(writer, { jsonrpc: '2.0', id: "bindingHover#{index}", method: 'textDocument/hover', params: { textDocument: { uri: bindingUri }, position: { line: line, character: character } } })
      detail = receive_message(reader).dig('result', 'contents', 'value')
      raise "Binding hover details missing: #{index}: #{detail}" unless detail && detail.include?(signature) && detail.include?(extra)
    end
    oopUri = 'untitled:OpenClasses'
    oopHeader = "class Parent { static var count: Int = 0; var value: Int; static fn create(value: Int) -> Parent { return Parent(value) }; fn read() -> Int { return self.value } }\nclass Child extends Parent { let label: String = 'child'; fn read() -> Int { return super.read() + 1 } }\n"
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: oopUri, languageId: 'dotk3', version: 1, text: oopHeader + 'Child.' } } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'staticMembers', method: 'textDocument/completion', params: { textDocument: { uri: oopUri }, position: { line: 2, character: 6 } } })
    raise 'Inherited static member discovery missing' unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['count', 'create']
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: oopUri, version: 2 }, contentChanges: [{ text: oopHeader + "let child = Child(2)\nchild." }] } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'inheritedMembers', method: 'textDocument/completion', params: { textDocument: { uri: oopUri }, position: { line: 3, character: 6 } } })
    raise 'Inherited instance member discovery missing' unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['label', 'read', 'value']
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: oopUri, version: 3 }, contentChanges: [{ text: "class Parent { fn read() -> Int { return 1 } }\nclass Child extends Parent { fn read() -> Int {\nsuper.\n} }" }] } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'superMembers', method: 'textDocument/completion', params: { textDocument: { uri: oopUri }, position: { line: 2, character: 6 } } })
    raise 'Super member discovery missing' unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['read']
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: oopUri, version: 4 }, contentChanges: [{ text: "class C { static fn read() -> Int { return self.value } }" }] } })
    raise 'Static self diagnostic missing' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?("Undefined name 'self'")
    collectionUri = 'untitled:CollectionClasses'
    collectionCases = [
      ["class Bag extends Collections.List<Int> { let label: String = 'bag' }\nlet bag = Bag()\nbag.", ['all', 'any', 'append', 'clear', 'contains', 'extend', 'filter', 'find', 'findIndex', 'indexOf', 'insert', 'label', 'length', 'map', 'reduce', 'removeAt', 'reverse']],
      ["let unique: Set<Int> = Set([1,2])\nunique.", ['add', 'clear', 'contains', 'difference', 'intersection', 'isSubsetOf', 'length', 'remove', 'union', 'values']],
      ["record Item { value: Int }\nlet unique: Set<Item> = Set()\nunique.values()[0].", ['value']]
    ]
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: collectionUri, languageId: 'dotk3', version: 1, text: '' } } })
    receive_message(reader)
    collectionCases.each_with_index do |(text, expected), index|
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: collectionUri, version: index + 2 }, contentChanges: [{ text: text }] } })
      receive_message(reader)
      send_message(writer, { jsonrpc: '2.0', id: "collectionClass#{index}", method: 'textDocument/completion', params: { textDocument: { uri: collectionUri }, position: { line: text.split("\n").size - 1, character: text.split("\n").last.length } } })
      raise "Collection class discovery #{index} missing" unless receive_message(reader)['result'].map { |entry| entry['label'] } == expected
    end
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: collectionUri, version: 5 }, contentChanges: [{ text: "let unique: Set<Int> = Set()\nunique.add('bad')" }] } })
    raise 'Set element diagnostic missing' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('Expected Int')
    accessUri = 'untitled:ClassAccess'
    accessHeader = "class Vault { private var secret: Int = 3; protected var shared: Int = 1; public var value: Int = 2; public fn read() -> Int { return self.secret } }\n"
    accessCases = [
      [accessHeader + "let vault = Vault()\nvault.", ['read', 'value']],
      ["class Vault { private var secret: Int = 3; protected var shared: Int = 1; public var value: Int = 2; fn read() -> Int {\nself.\n} }", ['read', 'secret', 'shared', 'value']],
      [accessHeader + "class Child extends Vault { fn check() -> Int {\nself.\n} }", ['check', 'read', 'shared', 'value']]
    ]
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: accessUri, languageId: 'dotk3', version: 1, text: '' } } })
    receive_message(reader)
    accessCases.each_with_index do |(text, expected), index|
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: accessUri, version: index + 2 }, contentChanges: [{ text: text }] } })
      receive_message(reader)
      line = text.split("\n").index { |entry| entry == 'self.' || entry == 'vault.' }
      send_message(writer, { jsonrpc: '2.0', id: "accessMembers#{index}", method: 'textDocument/completion', params: { textDocument: { uri: accessUri }, position: { line: line, character: text.split("\n")[line].length } } })
      raise "Visibility completion #{index} failed" unless receive_message(reader)['result'].map { |entry| entry['label'] } == expected
    end
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: accessUri, version: 5 }, contentChanges: [{ text: accessHeader + 'Vault().secret' }] } })
    raise 'Private access diagnostic missing' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('private in Vault')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: accessUri, version: 6 }, contentChanges: [{ text: accessHeader + 'Vault().read()' }] } })
    raise 'Private access fix did not clear error' unless receive_message(reader).dig('params', 'diagnostics') == []
    operatorUri = 'untitled:OperatorHooks'
    operatorSource = "record Vector { x: Int; fn __add__(other: Vector) -> Vector { return Vector(self.x + other.x) }; fn __mul__(amount: Int) -> Vector { return Vector(self.x * amount) }; fn toString() -> String { return Text.str(self.x) } }\nlet left = Vector(1)\nlet right = Vector(2)\nlet result = left + right\nresult."
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: operatorUri, languageId: 'dotk3', version: 1, text: operatorSource } } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'operatorResultMembers', method: 'textDocument/completion', params: { textDocument: { uri: operatorUri }, position: { line: 4, character: 7 } } })
    raise 'Operator result discovery missing' unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['__add__', '__mul__', 'toString', 'x']
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: operatorUri, version: 2 }, contentChanges: [{ text: "record Vector { x: Int; fn __mul__(amount: Int) -> Vector { return Vector(self.x * amount) } }\nVector(1) * 'bad'" }] } })
    raise 'Operator argument diagnostic missing' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('Expected Int')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: operatorUri, version: 3 }, contentChanges: [{ text: "class Bad { fn toString() -> Int { return 1 } }" }] } })
    raise 'Conversion signature diagnostic missing' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('Expected String')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: operatorUri, version: 4 }, contentChanges: [{ text: "class NoExecute { fn toString() -> String { throw 'must not run' } }; Text.str(NoExecute())" }] } })
    raise 'LSP executed conversion hook' unless receive_message(reader).dig('params', 'diagnostics') == []
    foundationUri = 'untitled:Foundations'
    foundationCases = [["Error.Kind.", errorKinds = ['Access', 'Application', 'Call', 'Cancelled', 'Capability', 'IO', 'Index', 'Internal', 'Json', 'Key', 'Limit', 'Match', 'Name', 'Native', 'Overflow', 'State', 'Syntax', 'Thrown', 'Type', 'Value']],
      ["IO.", ['Files', 'Http', 'Sockets', 'Terminal', 'readLine', 'write']], ["IO.Files.", ['append', 'close', 'exists', 'flush', 'isDirectory', 'list', 'mkdir', 'open', 'read', 'readBytes', 'readLine', 'readText', 'remove', 'rename', 'seek', 'size', 'tell', 'write', 'writeText']],
      ["Sockets.", ['accept', 'close', 'connect', 'listen', 'port', 'receive', 'send']],
      ["IO.Sockets.", ['accept', 'close', 'connect', 'listen', 'port', 'receive', 'send']],
      ["import Http as Web\nWeb.", ['readRequest', 'readResponse', 'writeRequest', 'writeResponse']],
      ["let server = IO.Sockets.listen('127.0.0.1', 0)\nIO.Http.", ['readRequest', 'readResponse', 'writeRequest', 'writeResponse']],
      ["let name = 'Ada'\nlet message = f'Hi {name}'\nmessage.", ['contains', 'endsWith', 'find', 'join', 'length', 'split', 'startsWith']],
      ["let xs = [1, 2]\nlet strings = xs.map(fn(x) => 'a' + x)\nstrings[0].", ['contains', 'endsWith', 'find', 'join', 'length', 'split', 'startsWith']],
      ["fn first<T>(items: List<T>) -> T? { return items.find(fn(i) => true) }\nlet r = first(['a'])\nr!.", ['contains', 'endsWith', 'find', 'join', 'length', 'split', 'startsWith']]]
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: foundationUri, languageId: 'dotk3', version: 1, text: '' } } })
    receive_message(reader)
    foundationCases.each_with_index do |(text, expected), index|
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: foundationUri, version: index + 2 }, contentChanges: [{ text: text }] } })
      receive_message(reader)
      send_message(writer, { jsonrpc: '2.0', id: "foundation#{index}", method: 'textDocument/completion', params: { textDocument: { uri: foundationUri }, position: { line: text.split("\n").size - 1, character: text.split("\n").last.length } } })
      raise "Foundation discovery #{index} missing" unless receive_message(reader)['result'].map { |entry| entry['label'] } == expected
    end
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: foundationUri, version: 12 }, contentChanges: [{ text: "let value = f'{missing}'" }] } })
    raise 'Interpolation name diagnostic missing' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('Undefined name')
    resourceUri = 'untitled:NativeResource'
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: resourceUri, languageId: 'dotk3', version: 1, text: "fn inspect(value: Resource) -> String { return value.type_name }" } } })
    raise 'Resource annotations/member types rejected' unless receive_message(reader).dig('params', 'diagnostics') == []
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: resourceUri, version: 2 }, contentChanges: [{ text: "fn inspect(value: Resource) {\nvalue.\n}" }] } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'resourceMembers', method: 'textDocument/completion', params: { textDocument: { uri: resourceUri }, position: { line: 1, character: 6 } } })
    raise 'Resource metadata completion missing' unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['closed', 'type_name']
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: resourceUri, version: 3 }, contentChanges: [{ text: "fn change(value: Resource) { value.closed = true }" }] } })
    raise 'Resource readonly diagnostic missing' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('immutable')
    intersectionUri = 'untitled:TraitIntersection'
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: intersectionUri, languageId: 'dotk3', version: 1, text: "trait Named { fn name() -> String }\nfn inspect(value: Collections.Sized & Named) {\nvalue.\n}" } } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'intersectionMembers', method: 'textDocument/completion', params: { textDocument: { uri: intersectionUri }, position: { line: 2, character: 6 } } })
    raise 'Intersection member discovery missing' unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['length', 'name']
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: intersectionUri, version: 2 }, contentChanges: [{ text: "let values: Map<String, Int> = {}\nvalues.compute('x', fn(key: String, old: Int) -> Int => old)" }] } })
    raise 'Compute nullable input diagnostic missing' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('Expected fn(String, Int?)')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: intersectionUri, version: 3 }, contentChanges: [{ text: "let values: Map<String, Int> = {}\nvalues.compute('x', fn(key: String, old: Int?) -> Int => (old ?? 0) + 1)" }] } })
    raise 'Compute fix did not clear diagnostics' unless receive_message(reader).dig('params', 'diagnostics') == []
    typedMap = 'untitled:TypedMap'
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: typedMap, languageId: 'dotk3', version: 1, text: "let scores: Map<String, Int> = {'Ada': 'bad'}" } } })
    mapError = receive_message(reader)
    raise 'Typed-map LSP diagnostic missing' unless mapError.dig('params', 'diagnostics', 0, 'message').include?('Expected Int, got String')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: typedMap, version: 2 }, contentChanges: [{ text: "let scores: Map<String, Int> = {'Ada': 1}\nscores[2] = 3" }] } })
    mapError = receive_message(reader)
    raise 'Map-key diagnostic missing' unless mapError.dig('params', 'diagnostics', 0, 'message').include?('Expected String, got Int')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: typedMap, version: 3 }, contentChanges: [{ text: "let scores: Map<String, Int> = {'Ada': 1}\nscores['Grace'] = 3" }] } })
    raise 'Typed-map error did not clear' unless receive_message(reader).dig('params', 'diagnostics') == []
    memberUri = 'untitled:MemberDiscovery'
    source = "record Point { x: Int, y: Int = 2 }\nlet point = Point(1)\npoint."
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: memberUri, languageId: 'dotk3', version: 1, text: source } } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'members', method: 'textDocument/completion', params: { textDocument: { uri: memberUri }, position: { line: 2, character: 6 } } })
    memberItems = receive_message(reader)
    raise 'Partial record dot completion failed' unless memberItems['result'].map { |item| item['label'] } == ['x', 'y']
    source = "record Point { x: Int, y: Int = 2 }\nlet point = Point(1)\npoint.x"
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: memberUri, version: 2 }, contentChanges: [{ text: source }] } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'hover', method: 'textDocument/hover', params: { textDocument: { uri: memberUri }, position: { line: 2, character: 6 } } })
    raise 'Record field hover missing type' unless receive_message(reader).dig('result', 'contents', 'value').include?('Point.x: Int')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: memberUri, version: 3 }, contentChanges: [{ text: 'Reflection.' }] } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'core', method: 'textDocument/completion', params: { textDocument: { uri: memberUri }, position: { line: 0, character: 11 } } })
    raise 'Core namespace completion missing' unless receive_message(reader)['result'].map { |item| item['label'] }.sort == ['annotation', 'annotations', 'binding', 'bindings', 'doc', 'get', 'has', 'id', 'isInstance', 'members', 'type']
    filesUri = 'untitled:FilesCapability'
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: filesUri, languageId: 'dotk3', version: 1, text: "Files." } } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'filesMembers', method: 'textDocument/completion', params: { textDocument: { uri: filesUri }, position: { line: 0, character: 6 } } })
    raise 'Files completion missing' unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['append', 'close', 'exists', 'flush', 'isDirectory', 'list', 'mkdir', 'open', 'read', 'readBytes', 'readLine', 'readText', 'remove', 'rename', 'seek', 'size', 'tell', 'write', 'writeText']
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: filesUri, version: 2 }, contentChanges: [{ text: 'Files.readText(1)' }] } })
    raise 'Files path diagnostic missing' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('Expected String')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: filesUri, version: 3 }, contentChanges: [{ text: "from Files import readText as read\nlet text: String = read('never-executed.txt')\ntext." }] } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'fileResultMembers', method: 'textDocument/completion', params: { textDocument: { uri: filesUri }, position: { line: 2, character: 5 } } })
    raise 'Files result type discovery missing' unless receive_message(reader)['result'].map { |entry| entry['label'] }.include?('split')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: filesUri, version: 4 }, contentChanges: [{ text: "Files.readText('never-executed.txt')" }] } })
    raise 'Files fix did not clear diagnostics without execution' unless receive_message(reader).dig('params', 'diagnostics') == []
    jsonUri = 'untitled:JsonLibrary'
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: jsonUri, languageId: 'dotk3', version: 1, text: 'Json.' } } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'jsonMembers', method: 'textDocument/completion', params: { textDocument: { uri: jsonUri }, position: { line: 0, character: 5 } } })
    raise 'Json completion missing' unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['parse', 'stringify']
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: jsonUri, version: 2 }, contentChanges: [{ text: 'Json.parse(1)' }] } })
    raise 'Json input type diagnostic missing' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('Expected String')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: jsonUri, version: 3 }, contentChanges: [{ text: "from Json import stringify\nlet output: String = stringify([1])\noutput." }] } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'jsonResultMembers', method: 'textDocument/completion', params: { textDocument: { uri: jsonUri }, position: { line: 2, character: 7 } } })
    raise 'Json serialized String discovery missing' unless receive_message(reader)['result'].map { |entry| entry['label'] }.include?('split')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: jsonUri, version: 4 }, contentChanges: [{ text: "Json.parse('invalid JSON not executed')" }] } })
    raise 'LSP executed JSON parsing' unless receive_message(reader).dig('params', 'diagnostics') == []
    stringUri = 'untitled:StringMethods'
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: stringUri, languageId: 'dotk3', version: 1, text: "'abc'." } } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'stringLiteralMembers', method: 'textDocument/completion', params: { textDocument: { uri: stringUri }, position: { line: 0, character: 6 } } })
    raise 'String literal completion missing' unless receive_message(reader)['result'].map { |entry| entry['label'] }.include?('contains')
    splitUri = 'untitled:StringSplit'
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: splitUri, languageId: 'dotk3', version: 1, text: "'a,b'.split(',')[0]." } } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'splitElementMembers', method: 'textDocument/completion', params: { textDocument: { uri: splitUri }, position: { line: 0, character: 20 } } })
    raise 'Split String element completion missing' unless receive_message(reader)['result'].map { |entry| entry['label'] }.include?('find')
    mapMethodUri = 'untitled:MapMethods'
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: mapMethodUri, languageId: 'dotk3', version: 1, text: "let data: Map<String, Int> = {}\ndata." } } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'mapMethodMembers', method: 'textDocument/completion', params: { textDocument: { uri: mapMethodUri }, position: { line: 1, character: 5 } } })
    raise 'Map membership completion missing' unless receive_message(reader)['result'].map { |entry| entry['label'] }.include?('contains')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: mapMethodUri, version: 2 }, contentChanges: [{ text: "let data: Map<String, Int> = {}\ndata.contains(1)" }] } })
    raise 'Map membership key diagnostic missing' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('Expected String')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: mapMethodUri, version: 3 }, contentChanges: [{ text: "let data: Map<String, Int> = {}\ndata.contains('x')" }] } })
    raise 'Map membership fix did not clear error' unless receive_message(reader).dig('params', 'diagnostics') == []
    getUri = 'untitled:MapGet'
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: getUri, languageId: 'dotk3', version: 1, text: "let data: Map<String, Int> = {}\nlet value: Int = data.get('x')" } } })
    raise 'Map get nullable diagnostic absent' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('Int?')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: getUri, version: 2 }, contentChanges: [{ text: "let data: Map<String, Int> = {}\nlet value: Int? = data.get('x')" }] } })
    raise 'Map get nullable fix did not clear error' unless receive_message(reader).dig('params', 'diagnostics') == []
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: mapMethodUri, version: 4 }, contentChanges: [{ text: "let data: Map<String, Int> = {}\ndata.remove(1)" }] } })
    raise 'Map removal key diagnostic missing' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('Expected String')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: mapMethodUri, version: 5 }, contentChanges: [{ text: "let data: Map<String, Int> = {}\nlet removed: Bool = data.remove('x')" }] } })
    raise 'Map removal fix did not clear error' unless receive_message(reader).dig('params', 'diagnostics') == []
    snapshotSource = "record Point { x: Int }\nlet data: Map<String, Point> = {}\ndata.values()[0]."
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: mapMethodUri, version: 6 }, contentChanges: [{ text: snapshotSource }] } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'snapshotElementMembers', method: 'textDocument/completion', params: { textDocument: { uri: mapMethodUri }, position: { line: 2, character: 17 } } })
    raise 'Typed snapshot element completion missing' unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['x']
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: mapMethodUri, version: 7 }, contentChanges: [{ text: "let data: Map<String, Int> = {}\ndata.values().append('wrong')" }] } })
    raise 'Typed snapshot append diagnostic missing' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('Expected Int')
    semanticCases = [
      ["record Point { x: Int }\nfn use(point: Point) {\npoint.\n}", 2, 6, ['x']],
      ["record Point { x: Int }\nlet values: Map<String, Point> = {}\nvalues['x'].", 2, 12, ['x']],
      ["enum Event { Move(Int), Quit }\nEvent.", 1, 6, ['Move', 'Quit']],
      ["fn counter() { yield 1 }\nlet job = Tasks.spawn(counter())\njob.", 2, 4, ['done', 'error', 'status', 'value']]
    ]
    semanticCases.each_with_index do |(text, line, character, expected), index|
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: memberUri, version: 4 + index }, contentChanges: [{ text: text }] } })
      receive_message(reader)
      send_message(writer, { jsonrpc: '2.0', id: "semantic#{index}", method: 'textDocument/completion', params: { textDocument: { uri: memberUri }, position: { line: line, character: character } } })
      raise "Member case #{index} failed" unless receive_message(reader)['result'].map { |item| item['label'] }.sort == expected.sort
    end
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: memberUri, version: 9 }, contentChanges: [{ text: 'GC.collect()' }] } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'coreHover', method: 'textDocument/hover', params: { textDocument: { uri: memberUri }, position: { line: 0, character: 7 } } })
    raise 'Core hover missing signature' unless receive_message(reader).dig('result', 'contents', 'value').include?('collect()')
    documentation = "record Documented {\n\"\"\"A custom documented record.\"\"\"\nvalue: Int\n}\nlet object = Documented(1)\nobject.value"
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: memberUri, version: 10 }, contentChanges: [{ text: documentation }] } })
    raise 'Documented record did not check' unless receive_message(reader).dig('params', 'diagnostics') == []
    send_message(writer, { jsonrpc: '2.0', id: 'recordDoc', method: 'textDocument/hover', params: { textDocument: { uri: memberUri }, position: { line: 0, character: 8 } } })
    raise 'Custom type doc hover missing' unless receive_message(reader).dig('result', 'contents', 'value').include?('A custom documented record.')
    send_message(writer, { jsonrpc: '2.0', id: 'fieldDoc', method: 'textDocument/hover', params: { textDocument: { uri: memberUri }, position: { line: 5, character: 8 } } })
    raise 'Field doc hover missing' unless receive_message(reader).dig('result', 'contents', 'value').include?('Documented.value: Int')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: memberUri, version: 11 }, contentChanges: [{ text: "from Collections import List as NewList\nlet items = NewList()\nitems." }] } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'protocolMembers', method: 'textDocument/completion', params: { textDocument: { uri: memberUri }, position: { line: 2, character: 6 } } })
    raise 'Imported constructor length completion missing' unless receive_message(reader)['result'].map { |item| item['label'] }.sort == ['all', 'any', 'append', 'clear', 'contains', 'extend', 'filter', 'find', 'findIndex', 'indexOf', 'insert', 'length', 'map', 'reduce', 'removeAt', 'reverse']
    callbackUri = 'untitled:CallbacksAndRandom'
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: callbackUri, languageId: 'dotk3', version: 1, text: 'Random.' } } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'randomMembers', method: 'textDocument/completion', params: { textDocument: { uri: callbackUri }, position: { line: 0, character: 7 } } })
    raise 'Random completion missing' unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['choice', 'float', 'int', 'seed', 'shuffle', 'uuid']
    callbackCases = [
      "record Item { value: Int }\nlet values: List<Int> = []\nlet mapped = values.map(fn(value: Int) -> Item => Item(value))\nmapped[0].",
      "record Item { value: Int }\nlet values: List<Item> = []\nlet filtered = values.filter(fn(value: Item) -> Bool => true)\nfiltered[0].",
      "record Item { value: Int }\nlet values: List<Item> = []\nlet chosen = Random.choice(values)\nchosen.",
      "record Item { value: Int }\nfrom Random import choice as pick\nlet values: List<Item> = []\nlet chosen = pick(values)\nchosen."
    ]
    callbackCases.each_with_index do |text, index|
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: callbackUri, version: index + 2 }, contentChanges: [{ text: text }] } })
      receive_message(reader)
      send_message(writer, { jsonrpc: '2.0', id: "callbackResult#{index}", method: 'textDocument/completion', params: { textDocument: { uri: callbackUri }, position: { line: text.split("\n").size - 1, character: text.split("\n").last.length } } })
      raise "Callback/Random result discovery #{index} missing" unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['value']
    end
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: callbackUri, version: 6 }, contentChanges: [{ text: "[1].filter(fn(value: Int) -> Int => value)" }] } })
    raise 'Predicate result diagnostic missing' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('Expected fn(Int) -> Bool')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: callbackUri, version: 7 }, contentChanges: [{ text: "Random.int('bad')" }] } })
    raise 'Random argument diagnostic missing' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('Expected Int')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: callbackUri, version: 8 }, contentChanges: [{ text: "Random.choice([1])\n[1].map(fn(value: Int) -> Int { throw 'must not execute' })" }] } })
    raise 'LSP executed randomness/callbacks' unless receive_message(reader).dig('params', 'diagnostics') == []
    listMethodUri = 'untitled:ListMethods'
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: listMethodUri, languageId: 'dotk3', version: 1, text: "let data: List<Int> = []\ndata.insert(0, 'bad')" } } })
    raise 'List insertion type diagnostic absent' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('Expected Int')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: listMethodUri, version: 2 }, contentChanges: [{ text: "let data: List<Int> = []\ndata.insert(0, 1)" }] } })
    raise 'List insertion fix did not clear error' unless receive_message(reader).dig('params', 'diagnostics') == []
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: listMethodUri, version: 3 }, contentChanges: [{ text: "record Item { value: Int }\nlet data: List<Item> = []\ndata.removeAt(0)." }] } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'removedElementMembers', method: 'textDocument/completion', params: { textDocument: { uri: listMethodUri }, position: { line: 2, character: 17 } } })
    raise 'Removed element completion missing' unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['value']
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: listMethodUri, version: 4 }, contentChanges: [{ text: "let data: List<Int> = []\ndata.extend([1, 'bad'])" }] } })
    raise 'List extension type diagnostic absent' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('Expected Int')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: listMethodUri, version: 5 }, contentChanges: [{ text: "let data: List<Int> = []\ndata.extend([1, 2])" }] } })
    raise 'List extension fix did not clear error' unless receive_message(reader).dig('params', 'diagnostics') == []
    classUri = 'untitled:Classes'
    classSource = "class Counter {\n\"\"\"Documented mutable counter.\"\"\"\nvar value: Int = 0\nlet name: String = 'counter'\nfn add(amount: Int) -> Int {\n\"\"\"Increment the counter.\"\"\"\nself.value += amount\nreturn self.value\n}\nfn same() -> Counter { return self }\n}\nlet counter = Counter()\ncounter.add(2)"
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: classUri, languageId: 'dotk3', version: 1, text: classSource } } })
    raise 'Valid class rejected' unless receive_message(reader).dig('params', 'diagnostics') == []
    send_message(writer, { jsonrpc: '2.0', id: 'classOutline', method: 'textDocument/documentSymbol', params: { textDocument: { uri: classUri } } })
    classOutline = receive_message(reader)['result'][0]
    raise 'Class outline missing fields/methods' unless classOutline['kind'] == 5 && classOutline['children'].map { |entry| entry['name'] } == ['value', 'name', 'add', 'same']
    send_message(writer, { jsonrpc: '2.0', id: 'classMethodDoc', method: 'textDocument/hover', params: { textDocument: { uri: classUri }, position: { line: 12, character: 9 } } })
    methodDoc = receive_message(reader).dig('result', 'contents', 'value')
    raise 'Class method hover missing signature/docs' unless methodDoc.include?('Counter.add(amount: Int) -> Int') && methodDoc.include?('Increment the counter.')
    classCases = [[classSource + "\ncounter.", 13, 8], [classSource.sub('self.value += amount', 'self.'), 6, 5], [classSource + "\ncounter.same().", 13, 15]]
    classCases.each_with_index do |(text, line, character), index|
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: classUri, version: index + 2 }, contentChanges: [{ text: text }] } })
      receive_message(reader)
      send_message(writer, { jsonrpc: '2.0', id: "classMembers#{index}", method: 'textDocument/completion', params: { textDocument: { uri: classUri }, position: { line: line, character: character } } })
      raise "Class member case #{index} failed" unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['add', 'name', 'same', 'value']
    end
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: classUri, version: 5 }, contentChanges: [{ text: classSource + "\ncounter.name = 'bad'" }] } })
    raise 'Class readonly diagnostic absent' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('immutable')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: classUri, version: 6 }, contentChanges: [{ text: classSource + "\ncounter.value = 4" }] } })
    raise 'Class write fix did not clear diagnostics' unless receive_message(reader).dig('params', 'diagnostics') == []
    initUri = 'untitled:Initializer'
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: initUri, languageId: 'dotk3', version: 1, text: "class C { fn init(value: Int) {} }; C('wrong')" } } })
    raise 'Invalid constructor argument diagnostic absent' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('Expected Int')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: initUri, version: 2 }, contentChanges: [{ text: 'class C { var value: Int = 0; fn init() -> Null { self.value += 1 } }' }] } })
    raise 'Initializer fix did not clear diagnostic' unless receive_message(reader).dig('params', 'diagnostics') == []
    anonymousUri = 'untitled:AnonymousFunctions'
    anonymousSource = "record Point { x: Int }\nlet make = fn(value: Int) -> Point => Point(value)\nlet point = make(2)\npoint.x"
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: anonymousUri, languageId: 'dotk3', version: 1, text: anonymousSource } } })
    raise 'Anonymous function source rejected' unless receive_message(reader).dig('params', 'diagnostics') == []
    send_message(writer, { jsonrpc: '2.0', id: 'anonymousHover', method: 'textDocument/hover', params: { textDocument: { uri: anonymousUri }, position: { line: 1, character: 6 } } })
    raise 'Anonymous callable signature missing' unless receive_message(reader).dig('result', 'contents', 'value').include?('fn make(value: Int) -> Point')
    anonymousCases = [[anonymousSource + "\npoint.", 4, 6], ["record Point { x: Int }\nlet use = fn(point: Point) {\npoint.\n}", 2, 6], ["record Point { x: Int }\nlet use = fn(point: Point) => point.", 1, 36], ["record Point { x: Int }\n(fn() -> Point { return Point(1) })().", 1, 38]]
    anonymousCases.each_with_index do |(text, line, character), index|
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: anonymousUri, version: index + 2 }, contentChanges: [{ text: text }] } })
      receive_message(reader)
      send_message(writer, { jsonrpc: '2.0', id: "anonymousMembers#{index}", method: 'textDocument/completion', params: { textDocument: { uri: anonymousUri }, position: { line: line, character: character } } })
      raise "Anonymous member case #{index} failed" unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['x']
    end
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: anonymousUri, version: 6 }, contentChanges: [{ text: "let use = fn(value: Int) -> Int => value\nuse('bad')" }] } })
    raise 'Anonymous call type diagnostic absent' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('Expected Int')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: anonymousUri, version: 7 }, contentChanges: [{ text: "let use = fn(value: Int) -> Int => value\nuse(1)" }] } })
    raise 'Anonymous call fix did not clear error' unless receive_message(reader).dig('params', 'diagnostics') == []
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: anonymousUri, version: 8 }, contentChanges: [{ text: "record Point { x: Int }\nlet use = fn(point: Point) => point.x\npoint." }] } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'anonymousScopeBoundary', method: 'textDocument/completion', params: { textDocument: { uri: anonymousUri }, position: { line: 2, character: 6 } } })
    raise 'Anonymous parameters leaked outside body' unless receive_message(reader)['result'] == []
    defaultUri = 'untitled:TraitDefaults'
    defaultSource = "trait Reader {\nfn read() -> Int\nfn twice() -> Int { return self.read() * 2 }\n}\nclass C implements Reader { fn read() -> Int { return 3 } }\nlet object = C()\nobject."
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: defaultUri, languageId: 'dotk3', version: 1, text: defaultSource } } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'defaultMembers', method: 'textDocument/completion', params: { textDocument: { uri: defaultUri }, position: { line: 6, character: 7 } } })
    raise 'Inherited default completion missing' unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['read', 'twice']
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: defaultUri, version: 2 }, contentChanges: [{ text: defaultSource.sub('return self.read() * 2', 'return self.').sub('object.', 'object.twice()') }] } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'defaultSelf', method: 'textDocument/completion', params: { textDocument: { uri: defaultUri }, position: { line: 2, character: 32 } } })
    raise 'Trait default self completion missing' unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['read', 'twice']
    externalDefaultUri = 'untitled:ExternalDefaults'
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: externalDefaultUri, languageId: 'dotk3', version: 1, text: "trait Named { fn name() -> String { return 'default' } }\nclass C {}\nimpl Named for C {}\nlet value = C()\nvalue." } } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'externalDefaultMembers', method: 'textDocument/completion', params: { textDocument: { uri: externalDefaultUri }, position: { line: 4, character: 6 } } })
    raise 'External default method completion missing' unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['name']
    recordMethodUri = 'untitled:RecordMethods'
    recordMethodSource = "trait Named { fn name() -> String { return 'record' } }\nrecord Point implements Named {\nx: Int\nfn sum() -> Int { return self.x }\n}\nlet point = Point(2)\npoint."
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: recordMethodUri, languageId: 'dotk3', version: 1, text: recordMethodSource } } })
    receive_message(reader)
    send_message(writer, { jsonrpc: '2.0', id: 'recordMethodMembers', method: 'textDocument/completion', params: { textDocument: { uri: recordMethodUri }, position: { line: 6, character: 6 } } })
    raise 'Record methods/default completion missing' unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['name', 'sum', 'x']
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: recordMethodUri, version: 2 }, contentChanges: [{ text: recordMethodSource + 'sum()' }] } })
    raise 'Valid record method source rejected' unless receive_message(reader).dig('params', 'diagnostics') == []
    send_message(writer, { jsonrpc: '2.0', id: 'recordMethodOutline', method: 'textDocument/documentSymbol', params: { textDocument: { uri: recordMethodUri } } })
    recordOutline = receive_message(reader)['result'][1]
    raise 'Record method outline missing' unless recordOutline['children'].map { |entry| entry['name'] } == ['x', 'sum'] && recordOutline['detail'].include?('implements Named')
    traitUri = 'untitled:Traits'
    traitSource = "trait Readable {\n\"\"\"A checked reader.\"\"\"\nfn read() -> Int\n}\nclass Box { let value: Int = 2 }\nimpl Readable for Box {\nfn read() -> Int { return self.value }\n}\nlet box = Box()\nfn use(value: Readable) -> Int { return value.read() }\nbox.read()"
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: traitUri, languageId: 'dotk3', version: 1, text: traitSource } } })
    raise 'Valid trait rejected' unless receive_message(reader).dig('params', 'diagnostics') == []
    send_message(writer, { jsonrpc: '2.0', id: 'traitOutline', method: 'textDocument/documentSymbol', params: { textDocument: { uri: traitUri } } })
    traitOutline = receive_message(reader)['result']
    raise 'Trait/impl outline absent' unless traitOutline[0]['kind'] == 11 && traitOutline[0]['children'][0]['name'] == 'read' && traitOutline[2]['name'] == 'impl Readable for Box'
    send_message(writer, { jsonrpc: '2.0', id: 'traitDoc', method: 'textDocument/hover', params: { textDocument: { uri: traitUri }, position: { line: 0, character: 8 } } })
    raise 'Trait documentation absent' unless receive_message(reader).dig('result', 'contents', 'value').include?('A checked reader.')
    traitCases = [[traitSource + "\nbox.", 11, 4, ['read', 'value']], [traitSource.sub('return self.value', 'return self.'), 6, 31, ['read', 'value']], ["from Collections import Sized\nfn size(value: Sized) {\nvalue.\n}", 2, 6, ['length']], ["trait Reader { fn read() -> Int }\nfn use(value: Reader) {\nvalue.\n}", 2, 6, ['read']], [traitSource + "\nimpl Collections.Sized for Box { fn length() -> Int { return 1 } }\nbox.", 12, 4, ['length', 'read', 'value']]]
    traitCases.each_with_index do |(text, line, character, expected), index|
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: traitUri, version: index + 2 }, contentChanges: [{ text: text }] } })
      receive_message(reader)
      send_message(writer, { jsonrpc: '2.0', id: "traitMembers#{index}", method: 'textDocument/completion', params: { textDocument: { uri: traitUri }, position: { line: line, character: character } } })
      raise "Trait member case #{index} failed" unless receive_message(reader)['result'].map { |entry| entry['label'] } == expected
    end
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: traitUri, version: 7 }, contentChanges: [{ text: traitSource.sub('fn read() -> Int { return self.value }', "fn read() -> String { return 'bad' }") }] } })
    raise 'Trait mismatch diagnostic absent' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('requires')
    send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: traitUri, version: 8 }, contentChanges: [{ text: traitSource }] } })
    raise 'Trait mismatch fix did not clear diagnostics' unless receive_message(reader).dig('params', 'diagnostics') == []
    Dir.mktmpdir('dotk3-lsp-members') do |directory|
      embeddedUri = 'untitled:EmbeddedTraits'
      embeddedSource = "trait Readable { fn read() -> Int }\nclass Box implements Collections.Sized, Readable {\nlet value: Int = 2\nfn length() -> Int { return 1 }\nfn read() -> Int { return self.value }\n}\nlet box: Readable = Box()\nbox.read()"
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: embeddedUri, languageId: 'dotk3', version: 1, text: embeddedSource } } })
      raise 'Embedded class conformance rejected' unless receive_message(reader).dig('params', 'diagnostics') == []
      send_message(writer, { jsonrpc: '2.0', id: 'embeddedHover', method: 'textDocument/hover', params: { textDocument: { uri: embeddedUri }, position: { line: 1, character: 8 } } })
      raise 'Class hover lacks implements clause' unless receive_message(reader).dig('result', 'contents', 'value').include?('class Box implements Collections.Sized, Readable')
      send_message(writer, { jsonrpc: '2.0', id: 'embeddedOutline', method: 'textDocument/documentSymbol', params: { textDocument: { uri: embeddedUri } } })
      embeddedOutline = receive_message(reader)['result'][1]
      raise 'Class outline lacks conformance detail' unless embeddedOutline['detail'].include?('implements Collections.Sized, Readable') && embeddedOutline['children'].map { |entry| entry['name'] } == ['value', 'length', 'read']
      multilineUri = 'untitled:MultilineEmbeddedTraits'
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: multilineUri, languageId: 'dotk3', version: 1, text: embeddedSource.sub('Collections.Sized, Readable', "Collections.Sized,\nReadable") } } })
      raise 'Multiline implements clause rejected' unless receive_message(reader).dig('params', 'diagnostics') == []
      send_message(writer, { jsonrpc: '2.0', id: 'multilineEmbeddedOutline', method: 'textDocument/documentSymbol', params: { textDocument: { uri: multilineUri } } })
      multilineOutline = receive_message(reader)['result'][1]
      raise 'Multiline class outline range incorrect' unless multilineOutline['children'].map { |entry| entry['name'] } == ['value', 'length', 'read'] && multilineOutline['children'][0].dig('selectionRange', 'start', 'line') == 3
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: embeddedUri, version: 2 }, contentChanges: [{ text: embeddedSource.sub('fn length() -> Int { return 1 }', "fn length() -> String { return 'bad' }") }] } })
      raise 'Embedded class mismatch diagnostic absent' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('requires')
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: embeddedUri, version: 3 }, contentChanges: [{ text: embeddedSource }] } })
      raise 'Embedded class fix did not clear diagnostics' unless receive_message(reader).dig('params', 'diagnostics') == []
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: embeddedUri, version: 4 }, contentChanges: [{ text: embeddedSource + "\nBox()." }] } })
      receive_message(reader)
      send_message(writer, { jsonrpc: '2.0', id: 'embeddedMembers', method: 'textDocument/completion', params: { textDocument: { uri: embeddedUri }, position: { line: 8, character: 6 } } })
      raise 'Embedded class completion absent' unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['length', 'read', 'value']
      File.write(File.join(directory, 'embedded.k3'), "export class Embedded implements Collections.Sized { fn length() -> Int { return 2 } }")
      importedEmbeddedUri = "file://#{directory}/embedded-main.k3"
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: importedEmbeddedUri, languageId: 'dotk3', version: 1, text: "from embedded import Embedded\nlet value = Embedded()\nvalue." } } })
      receive_message(reader)
      send_message(writer, { jsonrpc: '2.0', id: 'importedEmbedded', method: 'textDocument/completion', params: { textDocument: { uri: importedEmbeddedUri }, position: { line: 2, character: 6 } } })
      raise 'Imported embedded class members absent' unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['length']
      File.write(File.join(directory, 'reader.k3'), "export trait Readable { fn read() -> Int }\nexport class Box {}\nimpl Readable for Box { fn read() -> Int { return 2 } }")
      importedTraitUri = "file://#{directory}/trait-main.k3"
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: importedTraitUri, languageId: 'dotk3', version: 1, text: "from reader import Box\nlet box = Box()\nbox." } } })
      receive_message(reader)
      send_message(writer, { jsonrpc: '2.0', id: 'importedImpl', method: 'textDocument/completion', params: { textDocument: { uri: importedTraitUri }, position: { line: 2, character: 4 } } })
      raise 'Imported implementation method absent' unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['read']
      File.write(File.join(directory, 'counter.k3'), "export class Counter { var value: Int = 0; fn add() -> Int { return self.value } }")
      classImportUri = "file://#{directory}/classes.k3"
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: classImportUri, languageId: 'dotk3', version: 1, text: "from counter import Counter as Counting\nlet counter = Counting()\ncounter." } } })
      receive_message(reader)
      send_message(writer, { jsonrpc: '2.0', id: 'importedClass', method: 'textDocument/completion', params: { textDocument: { uri: classImportUri }, position: { line: 2, character: 8 } } })
      raise 'Imported class alias completion absent' unless receive_message(reader)['result'].map { |entry| entry['label'] } == ['add', 'value']
      File.write(File.join(directory, 'model.k3'), "export record Point { x: Int }\n")
      File.write(File.join(directory, 'settings.k3'), "export let version: String = 'alpha'\nexport var count = 1\nlet privateValue = 2\n")
      settingsUri = "file://#{directory}/settings-main.k3"
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: settingsUri, languageId: 'dotk3', version: 1, text: "import settings\nsettings.version" } } })
      receive_message(reader)
      send_message(writer, { jsonrpc: '2.0', id: 'exportedBindingHover', method: 'textDocument/hover', params: { textDocument: { uri: settingsUri }, position: { line: 1, character: 11 } } })
      raise 'Exported binding hover missing declaration' unless receive_message(reader).dig('result', 'contents', 'value').include?('export let version: String')
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: settingsUri, version: 2 }, contentChanges: [{ text: "import settings\nsettings." }] } })
      receive_message(reader)
      send_message(writer, { jsonrpc: '2.0', id: 'exportedBindingCompletion', method: 'textDocument/completion', params: { textDocument: { uri: settingsUri }, position: { line: 1, character: 9 } } })
      entries = receive_message(reader)['result']
      raise 'Exported binding completion details missing' unless entries.map { |entry| entry['label'] } == ['count', 'version'] && entries.first['detail'].include?('export var count: Int')
      moduleUri = "file://#{directory}/model.k3"
      importedUri = "file://#{directory}/main.k3"
      importedSource = "import model as model\nlet point: model.Point = model.Point(1)\npoint."
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: importedUri, languageId: 'dotk3', version: 1, text: importedSource } } })
      receive_message(reader)
      send_message(writer, { jsonrpc: '2.0', id: 'imported', method: 'textDocument/completion', params: { textDocument: { uri: importedUri }, position: { line: 2, character: 6 } } })
      raise 'Imported record completion missing' unless receive_message(reader)['result'].map { |item| item['label'] } == ['x']
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: importedUri, version: 2 }, contentChanges: [{ text: "from model import Point as Position\nlet point = Position(1)\npoint." }] } })
      receive_message(reader)
      send_message(writer, { jsonrpc: '2.0', id: 'importAlias', method: 'textDocument/completion', params: { textDocument: { uri: importedUri }, position: { line: 2, character: 6 } } })
      raise 'Imported record alias completion missing' unless receive_message(reader)['result'].map { |item| item['label'] } == ['x']
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: importedUri, version: 3 }, contentChanges: [{ text: "from model import *\nlet point = Point(1)\npoint." }] } })
      receive_message(reader)
      send_message(writer, { jsonrpc: '2.0', id: 'importWildcard', method: 'textDocument/completion', params: { textDocument: { uri: importedUri }, position: { line: 2, character: 6 } } })
      raise 'Wildcard import completion missing' unless receive_message(reader)['result'].map { |item| item['label'] } == ['x']
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: moduleUri, languageId: 'dotk3', version: 1, text: 'export record Point { unsaved: String }' } } })
      receive_message(reader)
      send_message(writer, { jsonrpc: '2.0', id: 'unsaved', method: 'textDocument/completion', params: { textDocument: { uri: importedUri }, position: { line: 2, character: 6 } } })
      raise 'Unsaved module did not override disk metadata' unless receive_message(reader)['result'].map { |item| item['label'] } == ['unsaved']
    end
    send_message(writer, { jsonrpc: '2.0', id: 2, method: 'shutdown', params: nil })
    raise 'Shutdown failed' unless receive_message(reader)['id'] == 2
    send_message(writer, { jsonrpc: '2.0', id: 3, method: 'textDocument/documentSymbol', params: { textDocument: { uri: uri } } })
    raise 'Shutdown state not enforced' unless receive_message(reader).dig('error', 'code') == -32600
    send_message(writer, { jsonrpc: '2.0', method: 'exit' })
    writer.close
    raise "Server failed: #{error.read}" unless process.value.success?
  end
end
output, error, status = Open3.capture3(ARGV.fetch(0), stdin_data: "Content-Length: 20000000\r\n\r\n")
raise 'Oversized message did not fail safely' unless !status.success? && output.empty? && error.include?('Content-Length')
# Imports resolve through DOTK3_PATH like the CLI.
Dir.mktmpdir('dotk3-lsp-path') do |directory|
  FileUtils.mkdir_p(File.join(directory, 'lib', 'std'))
  FileUtils.mkdir_p(File.join(directory, 'app'))
  File.write(File.join(directory, 'lib', 'std', 'shapes.k3'), "export record Square { side: Int }\n")
  Timeout.timeout(10) do
    Open3.popen3({ 'DOTK3_PATH' => File.join(directory, 'lib') }, ARGV.fetch(0)) do |writer, reader, _error, process|
      send_message(writer, { jsonrpc: '2.0', id: 1, method: 'initialize', params: {} })
      receive_message(reader)
      appUri = "file://#{directory}/app/main.k3"
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: appUri, languageId: 'dotk3', version: 1, text: "from std.shapes import *\nlet square = Square(2)\nSystem.print(square.side)" } } })
      raise 'DOTK3_PATH import reported errors' unless receive_message(reader).dig('params', 'diagnostics') == []
      send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: appUri, version: 2 }, contentChanges: [{ text: "from std.shapes import *\nlet square = Square(2)\nsquare." }] } })
      receive_message(reader)
      send_message(writer, { jsonrpc: '2.0', id: 'pathCompletion', method: 'textDocument/completion', params: { textDocument: { uri: appUri }, position: { line: 2, character: 7 } } })
      raise 'DOTK3_PATH import completion missing' unless receive_message(reader)['result'].map { |item| item['label'] } == ['side']
      send_message(writer, { jsonrpc: '2.0', id: 2, method: 'shutdown', params: nil })
      receive_message(reader)
      send_message(writer, { jsonrpc: '2.0', method: 'exit' })
      writer.close
      raise 'DOTK3_PATH server failed' unless process.value.success?
    end
  end
end
output, error, status = Open3.capture3(ARGV.fetch(0), stdin_data: '')
raise 'Unexpected EOF was reported as success' unless status.exitstatus == 1 && output.empty? && error.include?('shutdown/exit handshake')
Timeout.timeout(10) do
  Open3.popen3(ARGV.fetch(0)) do |writer, reader, error, process|
    send_message(writer, { jsonrpc: '2.0', id: 1, method: 'initialize', params: {} })
    raise 'Unexpected initialize response' unless receive_message(reader)['id'] == 1
    writer.close
    raise 'Initialized server hid input closure' unless process.value.exitstatus == 1 && error.read.include?('shutdown/exit handshake')
  end
end
if ARGV[1]
  Dir.mktmpdir('dotk3-lsp-native') do |directory|
    FileUtils.cp(ARGV[1], File.join(directory, "stats_module#{File.extname(ARGV[1])}"))
    uri = "file://#{directory}/main.k3"
    [[[], true], [['--no-native'], false]].each do |flags, native|
      Timeout.timeout(10) do
        Open3.popen3(ARGV.fetch(0), *flags) do |writer, reader, error, process|
          send_message(writer, { jsonrpc: '2.0', id: 1, method: 'initialize', params: {} })
          receive_message(reader)
          send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didOpen', params: { textDocument: { uri: uri, languageId: 'dotk3', version: 1, text: "import Stats\nlet n: Int = Stats.gcd(4, 6)" } } })
          diagnostics = receive_message(reader).dig('params', 'diagnostics')
          raise "Native import diagnostics wrong (native=#{native})" unless native ? diagnostics == [] : !diagnostics.empty?
          if native
            send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: uri, version: 2 }, contentChanges: [{ text: "import Stats\nlet n: String = Stats.gcd(4, 6)" }] } })
            raise 'Native signature was not type-checked' unless receive_message(reader).dig('params', 'diagnostics', 0, 'message').include?('Expected String')
            send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: uri, version: 3 }, contentChanges: [{ text: "import Stats\nStats." }] } })
            receive_message(reader)
            send_message(writer, { jsonrpc: '2.0', id: 'members', method: 'textDocument/completion', params: { textDocument: { uri: uri }, position: { line: 1, character: 6 } } })
            labels = receive_message(reader)['result'].map { |item| item['label'] }
            raise 'Native completion missing' unless labels == %w[add close create fnv1a gcd mean summary]
            send_message(writer, { jsonrpc: '2.0', method: 'textDocument/didChange', params: { textDocument: { uri: uri, version: 4 }, contentChanges: [{ text: "import Stats\nStats.gcd(1, 2)" }] } })
            receive_message(reader)
            send_message(writer, { jsonrpc: '2.0', id: 'hover', method: 'textDocument/hover', params: { textDocument: { uri: uri }, position: { line: 1, character: 8 } } })
            raise 'Native hover missing' unless receive_message(reader).dig('result', 'contents', 'value').include?('gcd')
          end
          send_message(writer, { jsonrpc: '2.0', id: 2, method: 'shutdown' })
          receive_message(reader)
          send_message(writer, { jsonrpc: '2.0', method: 'exit' })
          raise 'Native import server exit failed' unless process.value.exitstatus == 0
        end
      end
    end
  end
end
puts 'lsp: PASS'
