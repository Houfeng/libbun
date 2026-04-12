import { spawnSync } from "bun";
import { existsSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, join, resolve } from "node:path";

const repoRoot = resolve(import.meta.dir, "..");
const buildDir = resolve(repoRoot, Bun.argv[2] ?? "build/shared-release");
const includeDir = resolve(repoRoot, "src/embed");
const tempDir = mkdtempSync(join(tmpdir(), "bun-libbun-example-"));

const expectedRangePasses = [
  "[PASS] bun_array_get_range/bun_array_set_range handle dense ranges",
  "[PASS] bun_array_set_range rejects out-of-bounds writes before mutating",
  "[PASS] zero-count range operations succeed with NULL buffers",
  "[PASS] bun_array_get_range preserves holes and prototype getters",
  "[PASS] bun_array_get_range rejects non-array targets",
  "[PASS] bun_array_set_range surfaces JS write failures",
];

type TestCase = {
  name: string;
  sourceFile: string;
  executableName: string;
  validateOutput?: (output: string) => void;
};

const tests: TestCase[] = [
  {
    name: "embed example regression",
    sourceFile: resolve(repoRoot, "src/embed/test/example.c"),
    executableName: "embed-example-regression",
    validateOutput(output) {
      if (output.includes("[FAIL]")) {
        throw new Error("embed example reported a failure");
      }

      for (const expected of expectedRangePasses) {
        if (!output.includes(expected)) {
          throw new Error(`embed example did not report expected regression check: ${expected}`);
        }
      }
    },
  },
];

if (process.platform !== "win32") {
  tests.push(
    {
      name: "embed eval_file class regression",
      sourceFile: resolve(repoRoot, "src/embed/test/test_eval_file_class.c"),
      executableName: "embed-eval-file-class-regression",
      validateOutput(output) {
        if (output.includes("[FAIL]")) {
          throw new Error("test_eval_file_class reported a failure");
        }
      },
    },
    {
      name: "embed minimal repro regression",
      sourceFile: resolve(repoRoot, "src/embed/test/test_minimal_repro.c"),
      executableName: "embed-minimal-repro-regression",
      validateOutput(output) {
        if (output.includes("[FAIL]") || output.includes("[CRASH]")) {
          throw new Error("test_minimal_repro reported a failure");
        }
      },
    },
  );
}

function readOutput(output: Uint8Array | null | undefined) {
  return output ? Buffer.from(output).toString("utf8") : "";
}

function sharedLibraryPath() {
  switch (process.platform) {
    case "darwin":
      return resolve(buildDir, "libbun.dylib");
    case "linux":
      return resolve(buildDir, "libbun.so");
    case "win32":
      return resolve(buildDir, "libbun.dll");
    default:
      throw new Error(`Unsupported platform: ${process.platform}`);
  }
}

function importLibraryPath() {
  const candidates = [
    resolve(buildDir, "libbun.lib"),
    resolve(buildDir, "bun.lib"),
    resolve(buildDir, "libbun.dll.lib"),
  ];

  for (const candidate of candidates) {
    if (existsSync(candidate)) return candidate;
  }

  throw new Error(`Could not find a Windows import library in ${buildDir}`);
}

function compilerPath() {
  if (process.platform === "win32") {
    return Bun.which("cl.exe") ?? Bun.which("cl");
  }

  return Bun.which("clang") ?? Bun.which("gcc") ?? Bun.which("cc");
}

function executablePath(test: TestCase) {
  const suffix = process.platform === "win32" ? ".exe" : "";
  return join(tempDir, `${test.executableName}${suffix}`);
}

function compileCommand(test: TestCase) {
  const compiler = compilerPath();
  if (!compiler) {
    throw new Error(`No C compiler found for ${process.platform}`);
  }

  const outputFile = executablePath(test);

  if (process.platform === "win32") {
    const importLib = importLibraryPath();
    return [
      compiler,
      "/nologo",
      `/I${includeDir}`,
      `/Fe:${outputFile}`,
      test.sourceFile,
      "/link",
      `/LIBPATH:${buildDir}`,
      basename(importLib),
    ];
  }

  return [
    compiler,
    "-o",
    outputFile,
    test.sourceFile,
    `-I${includeDir}`,
    `-L${buildDir}`,
    "-lbun",
    `-Wl,-rpath,${buildDir}`,
  ];
}

function runtimeEnv() {
  if (process.platform === "darwin") {
    return {
      DYLD_LIBRARY_PATH: [buildDir, process.env.DYLD_LIBRARY_PATH].filter(Boolean).join(":"),
    };
  }

  if (process.platform === "linux") {
    return {
      LD_LIBRARY_PATH: [buildDir, process.env.LD_LIBRARY_PATH].filter(Boolean).join(":"),
    };
  }

  if (process.platform === "win32") {
    return {
      PATH: [buildDir, process.env.PATH].filter(Boolean).join(";"),
    };
  }

  return {};
}

function runCommand(label: string, cmd: string[], env: Record<string, string> = {}) {
  console.log(`$ ${cmd.join(" ")}`);

  const proc = spawnSync({
    cmd,
    cwd: tempDir,
    env: {
      ...process.env,
      ...env,
    },
    stdout: "pipe",
    stderr: "pipe",
  });

  const stdout = readOutput(proc.stdout);
  const stderr = readOutput(proc.stderr);

  if (stdout) process.stdout.write(stdout);
  if (stderr) process.stderr.write(stderr);

  if (proc.exitCode !== 0) {
    throw new Error(`${label} failed with exit code ${proc.exitCode}`);
  }

  return `${stdout}${stderr}`;
}

try {
  const libPath = sharedLibraryPath();
  if (!existsSync(libPath)) {
    throw new Error(`Shared library not found: ${libPath}`);
  }

  for (const test of tests) {
    if (!existsSync(test.sourceFile)) {
      throw new Error(`Source file not found: ${test.sourceFile}`);
    }
  }

  for (const test of tests) {
    console.log(`\n=== ${test.name} ===`);
    runCommand(`compile ${test.name}`, compileCommand(test));
    const output = runCommand(`run ${test.name}`, [executablePath(test)], runtimeEnv());
    test.validateOutput?.(output);
    console.log(`${test.name} passed`);
  }

  console.log("libbun embed regression suite passed");
} finally {
  rmSync(tempDir, { force: true, recursive: true });
}
