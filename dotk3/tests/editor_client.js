const assert = require('assert');
const path = require('path');
const Module = require('module');
const original = Module._load;
const instances = [];
const errors = [];
const clientErrors = [];
const clientInfo = [];
const commands = new Map();
let grantTrust;
let configured = '';
const status = { text: '', tooltip: '', shown: false, show() { this.shown = true; }, hide() { this.shown = false; }, dispose() {} };
const information = [];
let editorChanged;
const probeUri = { fsPath: '', toString: () => 'untitled:DotK3Probe' };
let opened;
let documentOpened;
const vscode = {
  workspace: {
    isTrusted: false,
    workspaceFolders: [{ uri: { fsPath: path.resolve(__dirname, '../..') } }],
    getConfiguration: () => ({ get: () => configured }),
    onDidOpenTextDocument: callback => { documentOpened = callback; return { dispose() {} }; },
    onDidCloseTextDocument: () => ({ dispose() {} }),
    openTextDocument: async options => { opened = options; return { uri: probeUri, languageId: options.language }; },
    onDidGrantWorkspaceTrust: callback => { grantTrust = callback; return { dispose() {} }; }
  },
  commands: { registerCommand: (name, callback) => { commands.set(name, callback); return { dispose() {} }; } },
  StatusBarAlignment: { Right: 2 },
  languages: {
    getDiagnostics: uri => uri === probeUri ? [{ source: 'dotk3', message: 'Expected Int, inferred String' }] : [],
    onDidChangeDiagnostics: () => ({ dispose() {} })
  },
  window: {
    activeTextEditor: { document: { languageId: 'dotk3', uri: { fsPath: '/workspace/example.k3' } } },
    createStatusBarItem: () => status,
    onDidChangeActiveTextEditor: callback => { editorChanged = callback; return { dispose() {} }; },
    showInformationMessage: async message => { information.push(message); },
    showTextDocument: async document => { vscode.window.activeTextEditor = { document }; },
    showErrorMessage: async message => { errors.push(message); }
  }
};
class Client {
  constructor(id, name, server, options) { this.server = server; this.options = options; this.started = false; this.stopped = false; instances.push(this); }
  async start() { this.started = true; }
  onDidChangeState(callback) { this.stateChanged = callback; return { dispose() {} }; }
  async stop() { this.stopped = true; this.error('Server process exited with code 0.', undefined, false); }
  error(message) { clientErrors.push(message); }
  info(message) { clientInfo.push(message); }
}
Module._load = function(request, parent, main) {
  if (request === 'vscode') return vscode;
  if (request === 'vscode-languageclient/node') return { LanguageClient: Client, TransportKind: { stdio: 0 }, State: { Stopped: 1, Running: 2, Starting: 3 } };
  return original.call(this, request, parent, main);
};
const extension = require('../editor/extension');
Module._load = original;

(async () => {
  const context = { extensionPath: path.resolve(__dirname, '../editor'), subscriptions: [] };
  await extension.activate(context);
  assert.strictEqual(instances.length, 0);
  assert(status.shown && status.text.includes('Trust'));
  vscode.workspace.isTrusted = true;
  await grantTrust();
  assert.strictEqual(instances.length, 1);
  assert(instances[0].started);
  assert(status.text.includes('Running'));
  await commands.get('dotk3.showLanguageServerStatus')();
  assert(information.some(message => message.includes('0 diagnostics')));
  vscode.window.activeTextEditor.document.languageId = 'plaintext'; editorChanged();
  assert(status.text.includes('Language Mode'));
  vscode.window.activeTextEditor.document.languageId = 'dotk3'; documentOpened();
  await Promise.resolve(); assert(status.text.includes('Running'));
  const dotkEditor = vscode.window.activeTextEditor;
  vscode.window.activeTextEditor = { document: { languageId: 'javascript', uri: { fsPath: '/workspace/example.js' } } };
  editorChanged(); assert(!status.shown);
  vscode.window.activeTextEditor = dotkEditor; editorChanged();
  assert(instances[0].server.command.endsWith(path.join('build', 'release', process.platform === 'win32' ? 'dotk3-lsp.exe' : 'dotk3-lsp')));
  assert.deepStrictEqual(instances[0].options.documentSelector, [{ language: 'dotk3' }]);
  instances[0].stateChanged({ newState: 1 }); assert(status.text.includes('Stopped'));
  instances[0].stateChanged({ newState: 2 }); assert(status.text.includes('Running'));
  await commands.get('dotk3.testEditorIntegration')();
  assert(opened.language === 'dotk3' && opened.content.includes('lsp_probe: Int'));
  assert(information.some(message => message.includes('editor integration is working')));
  instances[0].error('Server process exited with code 0.', undefined, false);
  assert.strictEqual(clientErrors.length, 1);
  await commands.get('dotk3.restartLanguageServer')();
  assert(instances[0].stopped && instances[1].started);
  assert.strictEqual(clientErrors.length, 1);
  assert(clientInfo.some(message => message.includes('stopped normally')));
  assert(clientInfo.some(message => message.includes('Starting DotK 3 language server:')));
  instances[0].error('Server process exited with code 2.', undefined, false);
  assert(clientErrors.includes('Server process exited with code 2.'));
  assert(status.text.includes('Running'));
  configured = 'relative/server';
  await commands.get('dotk3.restartLanguageServer')();
  assert(errors.some(error => error.includes('absolute path')));
  await extension.deactivate();
  console.log('editor client: PASS');
})().catch(error => { console.error(error); process.exitCode = 1; });