const vscode = require('vscode');
const childProcess = require('child_process');
const path = require('path');

let diagnostics;

function activate(context) {
  diagnostics = vscode.languages.createDiagnosticCollection('caster');
  context.subscriptions.push(diagnostics);

  const runForDocument = (document) => {
    if (document.languageId !== 'caster' || document.uri.scheme !== 'file') return;
    if (!vscode.workspace.getConfiguration('caster').get('diagnostics.enabled')) {
      diagnostics.delete(document.uri);
      return;
    }
    runDiagnostics(document);
  };

  context.subscriptions.push(vscode.workspace.onDidSaveTextDocument(runForDocument));
  context.subscriptions.push(vscode.workspace.onDidOpenTextDocument(runForDocument));
  context.subscriptions.push(vscode.workspace.onDidCloseTextDocument((document) => {
    diagnostics.delete(document.uri);
  }));

  for (const document of vscode.workspace.textDocuments) runForDocument(document);
}

function deactivate() {
  if (diagnostics) diagnostics.dispose();
}

function runDiagnostics(document) {
  const config = vscode.workspace.getConfiguration('caster');
  const compilerPath = config.get('compilerPath') || 'caster';
  const workspaceFolder = vscode.workspace.getWorkspaceFolder(document.uri);
  const cwd = workspaceFolder ? workspaceFolder.uri.fsPath : path.dirname(document.uri.fsPath);
  const args = ['check', '--json', document.uri.fsPath];

  childProcess.execFile(
    compilerPath,
    args,
    { cwd, timeout: 10000, maxBuffer: 1024 * 1024 },
    (error, stdout, stderr) => {
      const parsed = parseDiagnostics(stdout);
      if (parsed) {
        diagnostics.set(document.uri, parsed.map((item) => toVsCodeDiagnostic(item)));
        return;
      }

      if (error) {
        diagnostics.set(document.uri, [fallbackDiagnostic(stderr || stdout || error.message)]);
        return;
      }

      diagnostics.delete(document.uri);
    }
  );
}

function parseDiagnostics(stdout) {
  const text = stdout.trim();
  if (!text) return null;
  try {
    const value = JSON.parse(text);
    return Array.isArray(value) ? value : null;
  } catch (_err) {
    return null;
  }
}

function toVsCodeDiagnostic(item) {
  const startLine = Math.max(0, Number(item.line || 1) - 1);
  const startCol = Math.max(0, Number(item.col || 1) - 1);
  const endLine = Math.max(startLine, Number(item.endLine || item.line || 1) - 1);
  const endCol = Math.max(startCol + 1, Number(item.endCol || item.col || 2) - 1);
  const range = new vscode.Range(startLine, startCol, endLine, endCol);
  const diagnostic = new vscode.Diagnostic(
    range,
    String(item.message || 'Caster check failed'),
    severityFromString(item.severity)
  );
  diagnostic.source = 'caster';
  return diagnostic;
}

function severityFromString(value) {
  switch (String(value || '').toLowerCase()) {
    case 'hint':
      return vscode.DiagnosticSeverity.Hint;
    case 'info':
      return vscode.DiagnosticSeverity.Information;
    case 'warning':
      return vscode.DiagnosticSeverity.Warning;
    default:
      return vscode.DiagnosticSeverity.Error;
  }
}

function fallbackDiagnostic(message) {
  const range = new vscode.Range(0, 0, 0, 1);
  const diagnostic = new vscode.Diagnostic(
    range,
    String(message || 'Caster check failed').trim(),
    vscode.DiagnosticSeverity.Error
  );
  diagnostic.source = 'caster';
  return diagnostic;
}

module.exports = {
  activate,
  deactivate
};
