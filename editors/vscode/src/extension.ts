// VS Code extension for Icon as gcon reads it: starts gcon's language
// server, `gcon lsp`.
import * as vscode from "vscode";
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
} from "vscode-languageclient/node";

let client: LanguageClient | undefined;
let running: string | undefined; // the settings the client was started with

// Starts and stops are serialised: a configuration change during startup
// must not stop a client that is still starting.
let queue: Promise<void> = Promise.resolve();
function serialise(f: () => Promise<void>): Promise<void> {
  queue = queue.then(f, f);
  return queue;
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
  context.subscriptions.push(
    vscode.commands.registerCommand("gcon.restartServer", () =>
      serialise(async () => {
        await stop();
        await start();
      }),
    ),
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (!e.affectsConfiguration("gcon")) return;
      // Restart only if the server's settings actually changed (VS Code
      // also reports changes while loading the workspace settings).
      return serialise(async () => {
        if (settingsKey() !== running) {
          await stop();
          await start();
        }
      });
    }),
  );
  await serialise(start);
}

export function deactivate(): Promise<void> {
  return serialise(stop);
}

function settings(): { command: string; ipath: string[]; icon95: boolean } {
  const config = vscode.workspace.getConfiguration("gcon");
  return {
    command: config.get<string>("path") || "gcon",
    ipath: config.get<string[]>("ipath") ?? [],
    icon95: config.get<boolean>("icon95") ?? false,
  };
}

function settingsKey(): string {
  return JSON.stringify(settings());
}

async function start(): Promise<void> {
  const { command, ipath, icon95 } = settings();
  running = settingsKey();
  // No transport is given, so the client talks over stdio without adding
  // a --stdio argument.
  const server = { command, args: ["lsp"] };
  const serverOptions: ServerOptions = { run: server, debug: server };
  const clientOptions: LanguageClientOptions = {
    documentSelector: [
      { scheme: "file", language: "icon" },
      { scheme: "untitled", language: "icon" },
    ],
    initializationOptions: { ipath, icon95 },
  };
  const c = new LanguageClient("gcon", "gcon lsp", serverOptions, clientOptions);
  try {
    await c.start();
    client = c;
  } catch (err) {
    const install = "How to install";
    vscode.window
      .showErrorMessage(
        `Could not start gcon's language server (${command} lsp). ` +
          "Build Gcon and put its bin directory on your PATH, or set gcon.path.",
        install,
      )
      .then((choice) => {
        if (choice === install) {
          vscode.env.openExternal(vscode.Uri.parse("https://github.com/phonologus/gcon#quick-start"));
        }
      });
  }
}

async function stop(): Promise<void> {
  const c = client;
  client = undefined;
  running = undefined;
  if (c) {
    try {
      await c.stop();
    } catch {
      // Already stopped (e.g. the server exited).
    }
  }
}
