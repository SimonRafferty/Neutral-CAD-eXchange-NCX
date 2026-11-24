//! SolidCraft, driven through its own command-line program.
//!
//! The translator never touches a SolidCraft window the user has open: it starts a private, headless
//! SolidCraft session (`solidcraft-cli mcp --headless`) and talks to it over MCP (JSON-RPC 2.0, one
//! message per line on stdin/stdout). Each command is sent on its own and answered on its own, so the
//! importer sees the ids SolidCraft gives new sketch entities and can react to a feature that fails.

use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError};
use std::sync::Arc;
use std::time::{Duration, Instant};

use anyhow::{anyhow, bail, Context, Result};
use serde_json::{json, Value};

/// Where SolidCraft's command-line program usually is. The user can also point the app at it.
pub fn find_cli(configured: Option<&Path>) -> Option<PathBuf> {
    let exe = if cfg!(windows) { "solidcraft-cli.exe" } else { "solidcraft-cli" };
    let mut candidates: Vec<PathBuf> = Vec::new();
    if let Some(p) = configured {
        candidates.push(p.to_path_buf());
    }
    if let Some(p) = std::env::var_os("NCX_SOLIDCRAFT_CLI") {
        candidates.push(PathBuf::from(p));
    }
    #[cfg(windows)]
    {
        for var in ["ProgramFiles", "ProgramW6432", "LOCALAPPDATA"] {
            if let Some(base) = std::env::var_os(var) {
                let base = PathBuf::from(base);
                candidates.push(base.join("SolidCraft").join(exe));
                candidates.push(base.join("Programs").join("SolidCraft").join(exe));
            }
        }
    }
    #[cfg(not(windows))]
    {
        candidates.push(PathBuf::from("/usr/bin").join(exe));
        candidates.push(PathBuf::from("/usr/local/bin").join(exe));
        candidates.push(PathBuf::from("/Applications/SolidCraft.app/Contents/MacOS").join(exe));
    }
    if let Some(path) = std::env::var_os("PATH") {
        for dir in std::env::split_paths(&path) {
            candidates.push(dir.join(exe));
        }
    }
    candidates.into_iter().find(|p| p.is_file())
}

/// The version line `solidcraft-cli --version` prints, e.g. "solidcraft-cli 0.47.0".
pub fn cli_version(cli: &Path) -> Result<String> {
    let out = hidden(Command::new(cli).arg("--version")).output().context("SolidCraft could not be started")?;
    Ok(String::from_utf8_lossy(&out.stdout).trim().to_string())
}

/// No console window flashes up for the child processes (Windows).
fn hidden(cmd: &mut Command) -> &mut Command {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        cmd.creation_flags(CREATE_NO_WINDOW);
    }
    cmd
}

/// A failed SolidCraft command: SolidCraft's own message, which says why.
#[derive(Debug)]
pub struct ScError {
    pub command: String,
    pub message: String,
}

impl std::fmt::Display for ScError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}: {}", self.command, self.message)
    }
}
impl std::error::Error for ScError {}

/// A private headless SolidCraft session.
pub struct Session {
    child: Child,
    stdin: ChildStdin,
    lines: Receiver<String>,
    next_id: u64,
    cancel: Arc<AtomicBool>,
    /// Every command and its answer, for the developer log (`--log`).
    pub log: Vec<String>,
    pub keep_log: bool,
}

impl Session {
    /// Starts the session. `allow` are the folders it may read and write (the input's and output's).
    pub fn start(cli: &Path, allow: &[PathBuf], cancel: Arc<AtomicBool>) -> Result<Session> {
        let mut cmd = Command::new(cli);
        cmd.arg("mcp").arg("--headless").arg("--compact");
        for dir in allow {
            cmd.arg("--allow-path").arg(dir);
        }
        let mut child = hidden(&mut cmd)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .with_context(|| format!("SolidCraft could not be started ({})", cli.display()))?;
        let stdin = child.stdin.take().ok_or_else(|| anyhow!("no stdin"))?;
        let stdout = child.stdout.take().ok_or_else(|| anyhow!("no stdout"))?;
        // A reader thread, so a wait can time out and notice a cancel.
        let (tx, rx) = mpsc::channel();
        std::thread::spawn(move || {
            for line in BufReader::new(stdout).lines() {
                match line {
                    Ok(l) => {
                        if tx.send(l).is_err() {
                            break;
                        }
                    }
                    Err(_) => break,
                }
            }
        });
        let mut s = Session { child, stdin, lines: rx, next_id: 0, cancel, log: Vec::new(), keep_log: false };
        s.request(
            "initialize",
            json!({"protocolVersion": "2025-06-18", "capabilities": {},
                   "clientInfo": {"name": "ncx-translator", "version": env!("CARGO_PKG_VERSION")}}),
            Duration::from_secs(60),
        )?;
        s.notify("notifications/initialized")?;
        Ok(s)
    }

    fn notify(&mut self, method: &str) -> Result<()> {
        let msg = json!({"jsonrpc": "2.0", "method": method, "params": {}});
        writeln!(self.stdin, "{msg}")?;
        self.stdin.flush()?;
        Ok(())
    }

    fn request(&mut self, method: &str, params: Value, timeout: Duration) -> Result<Value> {
        self.next_id += 1;
        let id = self.next_id;
        let msg = json!({"jsonrpc": "2.0", "id": id, "method": method, "params": params});
        writeln!(self.stdin, "{msg}").context("SolidCraft stopped")?;
        self.stdin.flush()?;
        let started = Instant::now();
        loop {
            if self.cancel.load(Ordering::Relaxed) {
                let _ = self.child.kill();
                bail!(Cancelled);
            }
            match self.lines.recv_timeout(Duration::from_millis(100)) {
                Ok(line) => {
                    let Ok(v) = serde_json::from_str::<Value>(&line) else { continue };
                    if v.get("id").and_then(Value::as_u64) == Some(id) {
                        return Ok(v);
                    }
                }
                Err(RecvTimeoutError::Timeout) => {
                    if started.elapsed() > timeout {
                        let _ = self.child.kill();
                        bail!("SolidCraft did not answer within {} s ({method})", timeout.as_secs());
                    }
                }
                Err(RecvTimeoutError::Disconnected) => bail!("SolidCraft stopped unexpectedly"),
            }
        }
    }

    /// Calls one MCP tool; returns its answer (JSON where the tool answers JSON).
    pub fn tool(&mut self, name: &str, args: Value) -> Result<Value> {
        let r = self.request("tools/call", json!({"name": name, "arguments": args}), Duration::from_secs(1800))?;
        if let Some(e) = r.get("error") {
            return Err(ScError { command: name.into(), message: e.to_string() }.into());
        }
        let result = r.get("result").cloned().unwrap_or(Value::Null);
        let text: String = result
            .get("content")
            .and_then(Value::as_array)
            .map(|c| c.iter().filter_map(|i| i.get("text").and_then(Value::as_str)).collect::<Vec<_>>().join("\n"))
            .unwrap_or_default();
        if self.keep_log {
            let mut t = text.clone();
            t.truncate(2000);
            self.log.push(format!("{name} {args}\n  -> {t}"));
        }
        if result.get("isError").and_then(Value::as_bool) == Some(true) {
            return Err(ScError { command: name.into(), message: text }.into());
        }
        Ok(serde_json::from_str(&text).unwrap_or(Value::String(text)))
    }

    /// Runs one SolidCraft command (`solidcraft-cli commands --id <cmd>` documents each).
    pub fn exec(&mut self, command: &str, params: Value) -> Result<Value> {
        self.tool("execute", json!({"command": command, "params": params})).map_err(|e| match e.downcast::<ScError>() {
            Ok(mut sc) => {
                sc.command = command.into();
                sc.into()
            }
            Err(other) => other,
        })
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

/// The user cancelled.
#[derive(Debug)]
pub struct Cancelled;
impl std::fmt::Display for Cancelled {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "cancelled")
    }
}
impl std::error::Error for Cancelled {}

pub fn is_cancelled(e: &anyhow::Error) -> bool {
    e.downcast_ref::<Cancelled>().is_some()
}
