const vscode = require('vscode');
const fs = require('fs');
const path = require('path');
const { LanguageClient, TransportKind, State } = require('vscode-languageclient/node');

let client;
let operation = Promise.resolve();
let statusItem;
let connectionState = 'Stopped';

function updateStatus() {
  if (!statusItem) return;
  const editor = vscode.window.activeTextEditor;
  if (!editor || (editor.document.languageId !== 'dotk3' && path.extname(editor.document.uri.fsPath) !== '.k3')) {
    statusItem.hide(); return;
  }
  const wrongMode = editor.document.languageId !== 'dotk3';
  statusItem.text = wrongMode ? '$(warning) DotK 3: Language Mode' : `$(code) DotK 3: ${connectionState}`;
  statusItem.tooltip = wrongMode ? 'Select DotK 3 as this file\'s language mode to attach diagnostics and outlines.' :
    'Click to inspect the language server and active-document diagnostics.';
  statusItem.command = 'dotk3.showLanguageServerStatus';
  statusItem.show();
}

function state(value) { connectionState = value; updateStatus(); }

async function showStatus() {
  const editor = vscode.window.activeTextEditor;
  const mode = editor ? editor.document.languageId : '(no editor)';
  const count = editor ? vscode.languages.getDiagnostics(editor.document.uri).filter(item => item.source === 'dotk3').length : 0;
  const selected = await vscode.window.showInformationMessage(
    `DotK 3: ${connectionState}; language mode: ${mode}; ${count} diagnostics. Features: diagnostics, Outline, member hover and dot-completion.`,
    'Open Problems', 'Open Outline', 'Test Integration', 'Restart Server');
  if (selected === 'Open Problems') await vscode.commands.executeCommand('workbench.actions.view.problems');
  if (selected === 'Open Outline') await vscode.commands.executeCommand('outline.focus');
  if (selected === 'Test Integration') await vscode.commands.executeCommand('dotk3.testEditorIntegration');
  if (selected === 'Restart Server') await vscode.commands.executeCommand('dotk3.restartLanguageServer');
}

function waitForDiagnostic(uri) {
  return new Promise(resolve => {
    let finished = false;
    const finish = diagnostic => {
      if (finished) return;
      finished = true; clearTimeout(timer); listener.dispose(); resolve(diagnostic);
    };
    const check = () => {
      const diagnostic = vscode.languages.getDiagnostics(uri).find(item => item.source === 'dotk3');
      if (diagnostic) finish(diagnostic);
    };
    const listener = vscode.languages.onDidChangeDiagnostics(check);
    const timer = setTimeout(() => finish(undefined), 5000);
    check();
  });
}

async function testIntegration(context) {
  await enqueue(context, false);
  if (!client || connectionState !== 'Running') {
    await vscode.window.showErrorMessage('DotK 3 integration test requires a running language server in a trusted workspace.');
    return;
  }
  const document = await vscode.workspace.openTextDocument({
    language: 'dotk3', content: "# Temporary LSP test; this file is not executed or saved.\nlet lsp_probe: Int = 'change this to 1 to clear the error'\n"
  });
  const diagnostic = waitForDiagnostic(document.uri);
  await vscode.window.showTextDocument(document, { preview: true });
  const observed = await diagnostic;
  if (observed) await vscode.window.showInformationMessage(`DotK 3 editor integration is working: ${observed.message}. Open Problems or replace the string with 1 to clear the squiggle.`);
  else await vscode.window.showErrorMessage('DotK 3 did not publish a diagnostic for the test buffer. Check language mode and the DotK 3 Language Server output.');
}

class DotKLanguageClient extends LanguageClient {
  constructor(...parameters) {
    super(...parameters);
    this.expectedServerExit = false;
  }

  error(message, data, showNotification) {
    if (this.expectedServerExit && message === 'Server process exited with code 0.') {
      this.info('DotK 3 language server stopped normally.');
      return;
    }
    super.error(message, data, showNotification);
  }

  async stopExpected() {
    this.expectedServerExit = true;
    return this.stop();
  }
}

function serverPath(context) {
  const configured = vscode.workspace.getConfiguration('dotk3').get('serverPath', '');
  const folders = (vscode.workspace.workspaceFolders || []).map(folder => folder.uri.fsPath);
  if (configured) {
    if (!path.isAbsolute(configured)) throw new Error('dotk3.serverPath must be an absolute path');
    return configured;
  }
  const binary = process.platform === 'win32' ? 'dotk3-lsp.exe' : 'dotk3-lsp';
  const candidates = [];
  for (const folder of folders) {
    candidates.push(path.join(folder, 'dotk3', 'build', 'release', binary));
    candidates.push(path.join(folder, 'build', 'release', binary));
  }
  candidates.push(path.join(context.extensionPath, '..', 'build', 'release', binary));
  for (const candidate of candidates) {
    try { fs.accessSync(candidate, fs.constants.X_OK); return candidate; } catch (_) {}
  }
  throw new Error('Build dotk3-lsp with make -C dotk3 lsp, or set dotk3.serverPath');
}

async function start(context, restart) {
  if (!vscode.workspace.isTrusted) { state('Trust Required'); return; }
  if (client && !restart) return;
  if (client) { const previous = client; client = undefined; await previous.stopExpected(); }
  try {
    state('Starting');
    const command = serverPath(context);
    fs.accessSync(command, fs.constants.X_OK);
    const next = new DotKLanguageClient('dotk3', 'DotK 3 Language Server',
      { command, args: [], transport: TransportKind.stdio },
      { documentSelector: [{ language: 'dotk3' }] });
    context.subscriptions.push(next.onDidChangeState(event => {
      if (next.expectedServerExit) return;
      state(event.newState === State.Running ? 'Running' : event.newState === State.Starting ? 'Starting' : 'Stopped');
    }));
    next.info(`Starting DotK 3 language server: ${command}`);
    await next.start(); client = next; state('Running');
  } catch (error) {
    state('Failed');
    await vscode.window.showErrorMessage(`DotK 3 language server: ${error.message}`);
  }
}

function enqueue(context, restart) {
  operation = operation.catch(() => {}).then(() => start(context, restart));
  return operation;
}

async function activate(context) {
  statusItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 40);
  context.subscriptions.push(statusItem);
  context.subscriptions.push(vscode.window.onDidChangeActiveTextEditor(updateStatus));
  context.subscriptions.push(vscode.workspace.onDidOpenTextDocument(() => queueMicrotask(updateStatus)));
  context.subscriptions.push(vscode.workspace.onDidCloseTextDocument(() => queueMicrotask(updateStatus)));
  context.subscriptions.push(vscode.commands.registerCommand('dotk3.showLanguageServerStatus', showStatus));
  context.subscriptions.push(vscode.commands.registerCommand('dotk3.testEditorIntegration', () => testIntegration(context)));
  context.subscriptions.push(vscode.commands.registerCommand('dotk3.restartLanguageServer', () => enqueue(context, true)));
  context.subscriptions.push(vscode.workspace.onDidGrantWorkspaceTrust(() => enqueue(context, false)));
  await enqueue(context, false);
}

async function deactivate() {
  await operation;
  if (client) { const previous = client; client = undefined; await previous.stopExpected(); }
  state('Stopped');
}

module.exports = { activate, deactivate };