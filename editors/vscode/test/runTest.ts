// Runs the integration tests in a downloaded copy of VS Code, against the
// gcon of this checkout (its bin/gcon: run ../../build.sh first) and a
// temporary copy of the fixture project.
//
// VSCODE_TEST_VERSION picks the version of VS Code (default: the latest
// stable, which needs the network), and VSCODE_TEST_CACHE the directory
// it is downloaded to (default: .vscode-test), so that a copy already
// there is used without the network.
import * as fs from "fs";
import * as os from "os";
import * as path from "path";
import { runTests } from "@vscode/test-electron";

async function main(): Promise<void> {
  const ext = path.resolve(__dirname, "..", "..");
  const root = path.resolve(ext, "..", "..");
  const gcon = path.join(root, "bin", "gcon");
  if (!fs.existsSync(gcon)) throw new Error(`no ${gcon}: run ${root}/build.sh first`);

  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "gcon-vscode-"));
  const workspace = path.join(tmp, "project");
  fs.cpSync(path.join(ext, "test", "fixtures", "project"), workspace, { recursive: true });
  fs.mkdirSync(path.join(workspace, ".vscode"));
  fs.writeFileSync(
    path.join(workspace, ".vscode", "settings.json"),
    JSON.stringify({ "gcon.path": gcon, "gcon.icon95": true }, null, 2),
  );
  await runTests({
    version: process.env.VSCODE_TEST_VERSION || undefined,
    cachePath: process.env.VSCODE_TEST_CACHE || undefined,
    extensionDevelopmentPath: ext,
    extensionTestsPath: path.join(__dirname, "suite", "index.js"),
    launchArgs: [workspace, "--disable-extensions", "--skip-welcome", "--skip-release-notes"],
  });
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
