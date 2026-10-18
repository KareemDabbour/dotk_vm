const assert = require('assert');
const path = require('path');
const { spawn } = require('child_process');
const { createMessageConnection, StreamMessageReader, StreamMessageWriter } = require('../editor/node_modules/vscode-jsonrpc/node');
const binary = process.argv[2] || path.resolve(__dirname, '../build/release/dotk3-lsp');
const processServer = spawn(binary, [], { stdio: ['pipe', 'pipe', 'pipe'] });
let errors = '';
processServer.stderr.on('data', chunk => { errors += chunk; });
const exited = new Promise((resolve, reject) => {
  processServer.on('error', reject);
  processServer.on('exit', code => { if (code === 0) resolve(); else reject(new Error(`Server exit ${code}: ${errors}`)); });
});
const connection = createMessageConnection(new StreamMessageReader(processServer.stdout), new StreamMessageWriter(processServer.stdin));
connection.listen();
const timer = setTimeout(() => { processServer.kill(); }, 15000);

(async () => {
  const initialized = await connection.sendRequest('initialize', { capabilities: {} });
  assert.strictEqual(initialized.capabilities.textDocumentSync.change, 1);
  const diagnostic = new Promise(resolve => connection.onNotification('textDocument/publishDiagnostics', resolve));
  await connection.sendNotification('initialized', {});
  await connection.sendNotification('textDocument/didOpen', {
    textDocument: { uri: 'file:///client.k3', languageId: 'dotk3', version: 1, text: 'enum Event { Quit }\nlet value = Event()' }
  });
  const published = await diagnostic;
  assert(published.diagnostics[0].message.includes('Enum namespace is not callable'));
  const symbols = await connection.sendRequest('textDocument/documentSymbol', { textDocument: { uri: 'file:///client.k3' } });
  assert.deepStrictEqual(symbols.map(symbol => symbol.name), ['Event', 'value']);
  const mapDiagnostic = new Promise(resolve => connection.onNotification('textDocument/publishDiagnostics', resolve));
  await connection.sendNotification('textDocument/didChange', {
    textDocument: { uri: 'file:///client.k3', version: 2 },
    contentChanges: [{ text: "let scores: Map[String, Int] = {'Ada': 'bad'}" }]
  });
  assert((await mapDiagnostic).diagnostics[0].message.includes('Expected Int, inferred String'));
  const mapFixed = new Promise(resolve => connection.onNotification('textDocument/publishDiagnostics', resolve));
  await connection.sendNotification('textDocument/didChange', {
    textDocument: { uri: 'file:///client.k3', version: 3 },
    contentChanges: [{ text: "let scores: Map[String, Int] = {'Ada': 1}" }]
  });
  assert.deepStrictEqual((await mapFixed).diagnostics, []);
  const recordUpdated = new Promise(resolve => connection.onNotification('textDocument/publishDiagnostics', resolve));
  await connection.sendNotification('textDocument/didChange', {
    textDocument: { uri: 'file:///client.k3', version: 4 },
    contentChanges: [{ text: 'record Point { x: Int }\nlet point = Point(1)\npoint.' }]
  });
  await recordUpdated;
  const completion = await connection.sendRequest('textDocument/completion', { textDocument: { uri: 'file:///client.k3' }, position: { line: 2, character: 6 } });
  assert.deepStrictEqual(completion.map(item => item.label), ['x']);
  const docUpdated = new Promise(resolve => connection.onNotification('textDocument/publishDiagnostics', resolve));
  await connection.sendNotification('textDocument/didChange', {
    textDocument: { uri: 'file:///client.k3', version: 5 },
    contentChanges: [{ text: 'record Person { """Documented custom type."""\nname: String }\nlet person = Person("Ada")\nperson.name' }]
  });
  assert.deepStrictEqual((await docUpdated).diagnostics, []);
  const hover = await connection.sendRequest('textDocument/hover', { textDocument: { uri: 'file:///client.k3' }, position: { line: 0, character: 8 } });
  assert(hover.contents.value.includes('Documented custom type.'));
  const classUpdated = new Promise(resolve => connection.onNotification('textDocument/publishDiagnostics', resolve));
  await connection.sendNotification('textDocument/didChange', {
    textDocument: { uri: 'file:///client.k3', version: 6 },
    contentChanges: [{ text: 'class Counter { var value: Int = 0; fn add() -> Int { self.value += 1; return self.value } }\nlet counter = Counter()\ncounter.' }]
  });
  await classUpdated;
  const classCompletion = await connection.sendRequest('textDocument/completion', { textDocument: { uri: 'file:///client.k3' }, position: { line: 2, character: 8 } });
  assert.deepStrictEqual(classCompletion.map(item => item.label), ['add', 'value']);
  const traitUpdated = new Promise(resolve => connection.onNotification('textDocument/publishDiagnostics', resolve));
  await connection.sendNotification('textDocument/didChange', {
    textDocument: { uri: 'file:///client.k3', version: 7 },
    contentChanges: [{ text: 'from Collections import Sized\nfn size(value: Sized) {\nvalue.\n}' }]
  });
  await traitUpdated;
  const traitCompletion = await connection.sendRequest('textDocument/completion', { textDocument: { uri: 'file:///client.k3' }, position: { line: 2, character: 6 } });
  assert.deepStrictEqual(traitCompletion.map(item => item.label), ['length']);
  const embeddedUpdated = new Promise(resolve => connection.onNotification('textDocument/publishDiagnostics', resolve));
  await connection.sendNotification('textDocument/didChange', {
    textDocument: { uri: 'file:///client.k3', version: 8 },
    contentChanges: [{ text: 'class Bag implements Collections.Sized { fn length() -> Int { return 0 } }\nlet bag: Collections.Sized = Bag()\nbag.length()' }]
  });
  assert.deepStrictEqual((await embeddedUpdated).diagnostics, []);
  const embeddedHover = await connection.sendRequest('textDocument/hover', { textDocument: { uri: 'file:///client.k3' }, position: { line: 0, character: 7 } });
  assert(embeddedHover.contents.value.includes('class Bag implements Collections.Sized'));
  const expandedUpdated = new Promise(resolve => connection.onNotification('textDocument/publishDiagnostics', resolve));
  await connection.sendNotification('textDocument/didChange', {
    textDocument: { uri: 'file:///client.k3', version: 9 },
    contentChanges: [{ text: 'trait Named { fn name() -> String { return "record" } }\nrecord Point implements Named { x: Int; fn read() -> Int { return self.x } }\nlet point = Point(1)\npoint.' }]
  });
  await expandedUpdated;
  const expandedCompletion = await connection.sendRequest('textDocument/completion', { textDocument: { uri: 'file:///client.k3' }, position: { line: 3, character: 6 } });
  assert.deepStrictEqual(expandedCompletion.map(item => item.label), ['name', 'read', 'x']);
  const mapUpdated = new Promise(resolve => connection.onNotification('textDocument/publishDiagnostics', resolve));
  await connection.sendNotification('textDocument/didChange', {
    textDocument: { uri: 'file:///client.k3', version: 10 },
    contentChanges: [{ text: 'let data: Map[String, Int] = {}\ndata.' }]
  });
  await mapUpdated;
  const mapCompletion = await connection.sendRequest('textDocument/completion', { textDocument: { uri: 'file:///client.k3' }, position: { line: 1, character: 5 } });
  assert.deepStrictEqual(mapCompletion.map(item => item.label), ['clear', 'compute', 'computeIfAbsent', 'computeIfPresent', 'contains', 'get', 'keys', 'length', 'merge', 'putIfAbsent', 'remove', 'values']);
  const listUpdated = new Promise(resolve => connection.onNotification('textDocument/publishDiagnostics', resolve));
  await connection.sendNotification('textDocument/didChange', {
    textDocument: { uri: 'file:///client.k3', version: 11 },
    contentChanges: [{ text: 'let data: List[Int] = []\ndata.' }]
  });
  await listUpdated;
  const listCompletion = await connection.sendRequest('textDocument/completion', { textDocument: { uri: 'file:///client.k3' }, position: { line: 1, character: 5 } });
  assert.deepStrictEqual(listCompletion.map(item => item.label), ['all', 'any', 'append', 'clear', 'contains', 'extend', 'filter', 'find', 'findIndex', 'indexOf', 'insert', 'length', 'map', 'reduce', 'removeAt', 'reverse']);
  const accessUri = 'untitled:AccessClient';
  const accessUpdated = new Promise(resolve => connection.onNotification('textDocument/publishDiagnostics', resolve));
  await connection.sendNotification('textDocument/didOpen', { textDocument: { uri: accessUri, languageId: 'dotk3', version: 1,
    text: 'class Vault { private var secret: Int = 1; protected var shared: Int = 2; public fn read() -> Int { return self.secret } }\nlet vault = Vault()\nvault.' } });
  await accessUpdated;
  const accessCompletion = await connection.sendRequest('textDocument/completion', { textDocument: { uri: accessUri }, position: { line: 2, character: 6 } });
  assert.deepStrictEqual(accessCompletion.map(item => item.label), ['read']);
  const randomUri = 'untitled:RandomClient';
  const randomUpdated = new Promise(resolve => connection.onNotification('textDocument/publishDiagnostics', resolve));
  await connection.sendNotification('textDocument/didOpen', { textDocument: { uri: randomUri, languageId: 'dotk3', version: 1, text: 'Random.' } });
  await randomUpdated;
  const randomCompletion = await connection.sendRequest('textDocument/completion', { textDocument: { uri: randomUri }, position: { line: 0, character: 7 } });
  assert.deepStrictEqual(randomCompletion.map(item => item.label), ['choice', 'float', 'int', 'seed', 'shuffle', 'uuid']);
  const callbackUpdated = new Promise(resolve => connection.onNotification('textDocument/publishDiagnostics', resolve));
  const callbackSource = 'record Item { value: Int }\nlet values: List[Int] = []\nlet mapped = values.map(fn(value: Int) -> Item => Item(value))\nmapped[0].';
  await connection.sendNotification('textDocument/didChange', { textDocument: { uri: randomUri, version: 2 }, contentChanges: [{ text: callbackSource }] });
  await callbackUpdated;
  const callbackCompletion = await connection.sendRequest('textDocument/completion', { textDocument: { uri: randomUri }, position: { line: 3, character: 10 } });
  assert.deepStrictEqual(callbackCompletion.map(item => item.label), ['value']);
  const anonUpdated = new Promise(resolve => connection.onNotification('textDocument/publishDiagnostics', resolve));
  await connection.sendNotification('textDocument/didChange', {
    textDocument: { uri: 'file:///client.k3', version: 12 },
    contentChanges: [{ text: 'let add: Function = fn(value: Int) -> Int => value + 1\nlet result: Int = add(2)' }]
  });
  assert.deepStrictEqual((await anonUpdated).diagnostics, []);
  const anonHover = await connection.sendRequest('textDocument/hover', { textDocument: { uri: 'file:///client.k3' }, position: { line: 0, character: 5 } });
  assert(anonHover.contents.value.includes('fn add(value: Int) -> Int'));
  const stringUpdated = new Promise(resolve => connection.onNotification('textDocument/publishDiagnostics', resolve));
  await connection.sendNotification('textDocument/didChange', {
    textDocument: { uri: 'file:///client.k3', version: 13 },
    contentChanges: [{ text: "'abc'." }]
  });
  await stringUpdated;
  const stringCompletion = await connection.sendRequest('textDocument/completion', { textDocument: { uri: 'file:///client.k3' }, position: { line: 0, character: 6 } });
  assert.deepStrictEqual(stringCompletion.map(item => item.label), ['contains', 'endsWith', 'find', 'join', 'length', 'split', 'startsWith']);
  const filesUpdated = new Promise(resolve => connection.onNotification('textDocument/publishDiagnostics', resolve));
  await connection.sendNotification('textDocument/didChange', {
    textDocument: { uri: 'file:///client.k3', version: 14 },
    contentChanges: [{ text: "Files." }]
  });
  await filesUpdated;
  const filesCompletion = await connection.sendRequest('textDocument/completion', { textDocument: { uri: 'file:///client.k3' }, position: { line: 0, character: 6 } });
  assert.deepStrictEqual(filesCompletion.map(item => item.label), ['append', 'close', 'exists', 'flush', 'isDirectory', 'list', 'mkdir', 'open', 'read', 'readBytes', 'readLine', 'readText', 'remove', 'rename', 'seek', 'size', 'tell', 'write', 'writeText']);
  const filesValid = new Promise(resolve => connection.onNotification('textDocument/publishDiagnostics', resolve));
  await connection.sendNotification('textDocument/didChange', {
    textDocument: { uri: 'file:///client.k3', version: 15 },
    contentChanges: [{ text: "let value: String = Files.readText('not-executed.txt')" }]
  });
  assert.deepStrictEqual((await filesValid).diagnostics, []);
  const jsonUpdated = new Promise(resolve => connection.onNotification('textDocument/publishDiagnostics', resolve));
  await connection.sendNotification('textDocument/didChange', {
    textDocument: { uri: 'file:///client.k3', version: 16 },
    contentChanges: [{ text: 'Json.' }]
  });
  await jsonUpdated;
  const jsonCompletion = await connection.sendRequest('textDocument/completion', { textDocument: { uri: 'file:///client.k3' }, position: { line: 0, character: 5 } });
  assert.deepStrictEqual(jsonCompletion.map(item => item.label), ['parse', 'stringify']);
  await connection.sendRequest('shutdown', null);
  await connection.sendNotification('exit');
  await exited;
  clearTimeout(timer); connection.dispose();
  console.log('language-client transport: PASS');
})().catch(async error => {
  console.error(error); clearTimeout(timer); connection.dispose(); processServer.kill();
  await exited.catch(() => {}); process.exitCode = 1;
});