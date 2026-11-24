//! The per-item report (FORMAT-SPEC / guide step 5): every payload item ends up rebuilt, approximate,
//! failed or carried as geometry, with the reason. The user sees a plain-words summary only when there is
//! a serious problem; the full report is saved as a file for anyone helping them.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    /// Built natively and as the source had it.
    Rebuilt,
    /// Built another way, or with something left out; the reason says what.
    Approximate,
    /// The feature's own carried shape (STEP) was used: right shape, no live parameters.
    Result,
    /// Could not be built.
    Failed,
    /// A part or document that came in as the file's plain geometry.
    Fallback,
    /// Information only.
    Note,
}

impl Status {
    pub fn word(self) -> &'static str {
        match self {
            Status::Rebuilt => "rebuilt",
            Status::Approximate => "approximate",
            Status::Result => "as shape",
            Status::Failed => "failed",
            Status::Fallback => "plain geometry",
            Status::Note => "note",
        }
    }
}

#[derive(Clone, Debug)]
pub struct Item {
    pub document: String,
    pub name: String,
    pub kind: String,
    pub status: Status,
    pub reason: String,
}

#[derive(Default, Debug)]
pub struct Report {
    pub title: String,
    pub items: Vec<Item>,
    /// Problems the user must hear about (a part whose shape does not match, a cancelled run).
    pub serious: Vec<String>,
    pub notes: Vec<String>,
}

impl Report {
    pub fn add(&mut self, document: &str, name: &str, kind: &str, status: Status, reason: impl Into<String>) {
        self.items.push(Item {
            document: document.to_string(),
            name: name.to_string(),
            kind: kind.to_string(),
            status,
            reason: reason.into(),
        });
    }

    pub fn count(&self, status: Status) -> usize {
        self.items.iter().filter(|i| i.status == status).count()
    }

    /// The report file's text.
    pub fn text(&self) -> String {
        let mut s = String::new();
        let _ = writeln!(s, "{}", self.title);
        let _ = writeln!(s, "{}", "=".repeat(self.title.chars().count().max(10)));
        let _ = writeln!(
            s,
            "rebuilt {}, approximate {}, as shape {}, failed {}, plain geometry {}",
            self.count(Status::Rebuilt),
            self.count(Status::Approximate),
            self.count(Status::Result),
            self.count(Status::Failed),
            self.count(Status::Fallback)
        );
        if !self.serious.is_empty() {
            let _ = writeln!(s, "\nProblems:");
            for p in &self.serious {
                let _ = writeln!(s, "  - {p}");
            }
        }
        if !self.notes.is_empty() {
            let _ = writeln!(s, "\nNotes:");
            for p in &self.notes {
                let _ = writeln!(s, "  - {p}");
            }
        }
        let _ = writeln!(s, "\nItems:");
        let mut doc = "";
        for i in &self.items {
            if i.document != doc {
                doc = &i.document;
                let _ = writeln!(s, "\n[{doc}]");
            }
            let reason = if i.reason.is_empty() { String::new() } else { format!(" - {}", i.reason) };
            let _ = writeln!(s, "  {:<14} {:<16} {}{}", i.status.word(), i.kind, i.name, reason);
        }
        s
    }

    /// Saves the report beside `output` as `<output name>.ncx-report.txt`; returns its path.
    pub fn save_beside(&self, output: &Path) -> std::io::Result<PathBuf> {
        let name = output.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| "ncx".into());
        let path = output.with_file_name(format!("{name}.ncx-report.txt"));
        std::fs::write(&path, self.text())?;
        Ok(path)
    }
}
