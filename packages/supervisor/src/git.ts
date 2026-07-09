import { execFileSync } from "node:child_process";
import type { GitSnapshot } from "./types.js";

export type CaptureGitSnapshotOptions = {
  runId: string;
  phase: GitSnapshot["phase"];
  cwd: string;
};

export function captureGitSnapshot(options: CaptureGitSnapshotOptions): GitSnapshot {
  const createdAt = new Date().toISOString();

  try {
    execFileSync("git", ["rev-parse", "--is-inside-work-tree"], {
      cwd: options.cwd,
      encoding: "utf8",
      stdio: ["ignore", "pipe", "ignore"],
      timeout: 2000
    });
    const statusText = execFileSync("git", ["status", "--short"], {
      cwd: options.cwd,
      encoding: "utf8",
      stdio: ["ignore", "pipe", "ignore"],
      timeout: 3000
    });

    return {
      id: `${options.runId}_${options.phase}`,
      runId: options.runId,
      phase: options.phase,
      cwd: options.cwd,
      isRepo: true,
      statusText,
      createdAt
    };
  } catch {
    return {
      id: `${options.runId}_${options.phase}`,
      runId: options.runId,
      phase: options.phase,
      cwd: options.cwd,
      isRepo: false,
      statusText: "",
      createdAt
    };
  }
}

export function addWorktree(repoPath: string, runId: string): string {
  const worktreePath = `${repoPath}/.mico/worktrees/${runId}`;
  execFileSync("git", ["-C", repoPath, "worktree", "add", "-b", `mico/${runId}`, worktreePath, "HEAD"], {
    encoding: "utf8",
    stdio: ["ignore", "pipe", "pipe"],
    timeout: 10000
  });
  return worktreePath;
}

export function removeWorktree(repoPath: string, worktreePath: string, runId: string): void {
  try {
    execFileSync("git", ["-C", repoPath, "worktree", "remove", "--force", worktreePath], {
      encoding: "utf8",
      stdio: ["ignore", "pipe", "pipe"],
      timeout: 10000
    });
  } catch {
    // best effort — the worktree may already be gone
  }
  try {
    execFileSync("git", ["-C", repoPath, "branch", "-D", `mico/${runId}`], {
      encoding: "utf8",
      stdio: ["ignore", "pipe", "pipe"],
      timeout: 5000
    });
  } catch {
    // best effort
  }
}

export function diffText(cwd: string, file?: string): string {
  let diff = "";
  try {
    diff = execFileSync("git", ["-C", cwd, "diff", "--color=always", "HEAD", ...(file ? ["--", file] : [])], {
      encoding: "utf8",
      stdio: ["ignore", "pipe", "ignore"],
      maxBuffer: 16 * 1024 * 1024,
      timeout: 8000
    });
  } catch {
    return "";
  }

  // `git diff HEAD` misses untracked files; append pseudo-diffs for them.
  try {
    const status = execFileSync("git", ["-C", cwd, "status", "--short"], {
      encoding: "utf8",
      stdio: ["ignore", "pipe", "ignore"],
      timeout: 5000
    });
    const untracked = status
      .split(/\r?\n/)
      .filter((line) => line.startsWith("??"))
      .map((line) => line.slice(3).trim())
      .filter((name) => name.length > 0 && (!file || name === file))
      .slice(0, 50);

    for (const name of untracked) {
      try {
        execFileSync("git", ["-C", cwd, "diff", "--color=always", "--no-index", "/dev/null", name], {
          encoding: "utf8",
          stdio: ["ignore", "pipe", "ignore"],
          timeout: 5000
        });
      } catch (error) {
        const stdout = (error as { stdout?: string }).stdout;
        if (typeof stdout === "string") {
          diff += stdout;
        }
      }
    }
  } catch {
    // status failed — return what we have
  }

  return diff;
}

export function mergeWorktree(repoPath: string, worktreePath: string, runId: string): void {
  const status = execFileSync("git", ["-C", worktreePath, "status", "--short"], {
    encoding: "utf8",
    stdio: ["ignore", "pipe", "pipe"],
    timeout: 5000
  });

  if (status.trim().length > 0) {
    execFileSync("git", ["-C", worktreePath, "add", "-A"], { stdio: ["ignore", "pipe", "pipe"], timeout: 10000 });
    execFileSync("git", ["-C", worktreePath, "commit", "-m", `mico: ${runId}`], { stdio: ["ignore", "pipe", "pipe"], timeout: 10000 });
  }

  execFileSync("git", ["-C", repoPath, "merge", "--no-edit", `mico/${runId}`], {
    encoding: "utf8",
    stdio: ["ignore", "pipe", "pipe"],
    timeout: 15000
  });

  removeWorktree(repoPath, worktreePath, runId);
}

export function changedFilesFromSnapshots(before: GitSnapshot | null, after: GitSnapshot | null): string[] {
  if (!after?.isRepo) {
    return [];
  }

  const files = new Set<string>();

  for (const line of after.statusText.split(/\r?\n/)) {
    const file = fileFromStatusLine(line);
    if (file) {
      files.add(file);
    }
  }

  if (before?.isRepo) {
    for (const line of before.statusText.split(/\r?\n/)) {
      const file = fileFromStatusLine(line);
      if (file) {
        files.add(file);
      }
    }
  }

  return [...files].sort();
}

function fileFromStatusLine(line: string): string | null {
  if (line.trim().length === 0) {
    return null;
  }

  const value = line.slice(3).trim();
  if (value.length === 0) {
    return null;
  }

  const renameParts = value.split(" -> ");
  return renameParts.at(-1) ?? value;
}
