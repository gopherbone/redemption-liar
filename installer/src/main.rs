// SPDX-License-Identifier: GPL-2.0-or-later
//! Install Redemption: Liar on a device running Mortar.
//!
//! The loader lives in its own profile, outside the system closure. The game is
//! the user's own retail copy for the GP2X Wiz: checked here, then its files are
//! streamed over SSH straight into the session user's data directory, next to the
//! saves. They are never put in the Nix store, a cache or a repository, and
//! neither is anything from the Wiz firmware.

use std::env;
use std::fs::{self, File};
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{exit, Command, Stdio};

const USAGE: &str = "\
nix run .#install -- [--data PATH] [--host HOST] [--user NAME] [--remove]
    --data PATH  your own files: the card, or its Liar_e folder. Optional when
                 they are already on the device.
    --host HOST  the device (default: $MORTAR_HOST or mortar.local)
    --user NAME  the session user (default: $MORTAR_USER or brick)
    --remove     take the loader away; your files and saves stay";

struct Opts {
    host: String,
    user: String,
    data: Option<String>,
    remove: bool,
}

fn die(msg: &str) -> ! {
    eprintln!("liar-install: {msg}");
    exit(1)
}

fn say(msg: &str) {
    println!("\x1b[1m==>\x1b[0m {msg}");
}

fn parse() -> Opts {
    let mut o = Opts {
        host: env::var("MORTAR_HOST").unwrap_or_else(|_| "mortar.local".into()),
        user: env::var("MORTAR_USER").unwrap_or_else(|_| "brick".into()),
        data: None,
        remove: false,
    };
    let mut it = env::args().skip(1);
    while let Some(a) = it.next() {
        let mut val = || it.next().unwrap_or_else(|| die(&format!("{a} needs a value")));
        match a.as_str() {
            "--data" => o.data = Some(val()),
            "--host" => o.host = val(),
            "--user" => o.user = val(),
            "--remove" => o.remove = true,
            "-h" | "--help" => {
                println!("{USAGE}");
                exit(0)
            }
            other => die(&format!("unknown option: {other}\n{USAGE}")),
        }
    }
    o
}

fn ssh_cmd(host: &str) -> Command {
    let mut c = Command::new("ssh");
    c.args(["-o", "ConnectTimeout=10", "-o", "BatchMode=yes"]).arg(format!("root@{host}"));
    c
}

/// Run a shell command on the device, inheriting stdio. Returns success.
fn remote(host: &str, script: &str) -> bool {
    ssh_cmd(host).arg(script).status().map(|s| s.success()).unwrap_or(false)
}

/// Output of a command on the device (None if SSH or the command failed).
fn remote_output(host: &str, script: &str) -> Option<String> {
    let out = ssh_cmd(host).arg(script).stderr(Stdio::null()).output().ok()?;
    out.status.success().then(|| String::from_utf8_lossy(&out.stdout).trim().to_string())
}

fn run(cmd: &mut Command, what: &str) {
    match cmd.status() {
        Ok(s) if s.success() => {}
        Ok(s) => die(&format!("{what} failed ({s})")),
        Err(e) => die(&format!("{what}: {e}")),
    }
}

/// Single-quote for a POSIX shell.
fn sq(s: &str) -> String {
    format!("'{}'", s.replace('\'', r"'\''"))
}

const PROFILE: &str = "/nix/var/nix/profiles/liar";
const FILES: [&str; 5] = ["liar_e.gpe", "liar.dat", "liar.inx", "ext.dat", "ext.inx"];
/// FNV-1a of the executable this was developed and tested with.
const TESTED_GPE: u64 = 0x4c39f5f1aee44c76;
const TESTED_GPE_LEN: u64 = 280_732;

// Where the fields are in a decoded .inx: count, then parallel tables.
const IDX_OFFSET: usize = 0x60c04;
const IDX_PACKED: usize = 0x63c64;
const IDX_MAX: usize = (IDX_PACKED - IDX_OFFSET) / 4;

/// The game's LZSS (4096-byte window of spaces, starting at 0xfee).
fn lzss(src: &[u8]) -> Vec<u8> {
    let mut win = [b' '; 4096];
    let mut out = Vec::with_capacity(src.len() * 4);
    let (mut r, mut flags, mut i) = (0xfeeusize, 0u32, 0usize);
    loop {
        flags >>= 1;
        if flags & 256 == 0 {
            let Some(&b) = src.get(i) else { break };
            i += 1;
            flags = b as u32 | 0xff00;
        }
        if flags & 1 != 0 {
            let Some(&c) = src.get(i) else { break };
            i += 1;
            out.push(c);
            win[r] = c;
            r = (r + 1) & 0xfff;
        } else {
            let (Some(&a), Some(&b)) = (src.get(i), src.get(i + 1)) else { break };
            i += 2;
            let off = a as usize | ((b as usize & 0xf0) << 4);
            for k in 0..(b as usize & 15) + 3 {
                let c = win[(off + k) & 0xfff];
                out.push(c);
                win[r] = c;
                r = (r + 1) & 0xfff;
            }
        }
    }
    out
}

fn le32(b: &[u8], off: usize) -> u64 {
    u32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]]) as u64
}

/// Does this index describe this archive? `header` bytes of the archive come
/// before the offsets the index gives.
fn check_archive(name: &str, inx: &[u8], dat_len: u64, header: u64) -> Result<usize, String> {
    if inx.len() < 8 {
        return Err(format!("{name}.inx is too short"));
    }
    let (packed, size) = (le32(inx, 0) as usize, le32(inx, 4) as usize);
    let table = inx.get(8..8 + packed).map(lzss).ok_or_else(|| format!("{name}.inx is truncated"))?;
    if table.len() != size || size <= IDX_PACKED + IDX_MAX * 4 {
        return Err(format!("{name}.inx doesn't decode"));
    }
    let count = le32(&table, 0) as usize;
    if count == 0 || count > IDX_MAX {
        return Err(format!("{name}.inx lists {count} files"));
    }
    for i in 0..count {
        let end = le32(&table, IDX_OFFSET + i * 4) + le32(&table, IDX_PACKED + i * 4) + header;
        if end > dat_len {
            return Err(format!("{name}.dat is shorter than its index says (truncated?)"));
        }
    }
    Ok(count)
}

/// The folder holding the game, given the card or the folder itself.
fn game_dir(path: &str) -> PathBuf {
    let root = Path::new(path);
    if find(root, FILES[0]).is_some() {
        return root.to_path_buf();
    }
    if let Some(sub) = find(root, "Liar_e") {
        return sub;
    }
    root.to_path_buf()
}

/// `name` in `dir`, whatever its case (a FAT card).
fn find(dir: &Path, name: &str) -> Option<PathBuf> {
    fs::read_dir(dir).ok()?.flatten().find(|e| e.file_name().to_string_lossy().eq_ignore_ascii_case(name)).map(|e| e.path())
}

fn fnv1a(b: &[u8]) -> u64 {
    b.iter().fold(0xcbf2_9ce4_8422_2325, |h, c| (h ^ *c as u64).wrapping_mul(0x0000_0100_0000_01b3))
}

/// What the copy is, or why it can't be used. Also the paths of its files.
pub fn check_data(path: &str) -> Result<(String, Vec<PathBuf>), String> {
    let dir = game_dir(path);
    let mut files = Vec::new();
    for f in FILES {
        files.push(find(&dir, f).ok_or_else(|| format!("{f} is missing: point --data at your card or its Liar_e folder"))?);
    }
    let read = |p: &Path| fs::read(p).map_err(|e| format!("{}: {e}", p.display()));
    let gpe = read(&files[0])?;
    if gpe.len() < 52 || gpe[..4] != *b"\x7fELF" || gpe[4] != 1 || gpe[18] != 40 || gpe[16] != 2 {
        return Err("liar_e.gpe isn't a 32-bit ARM executable".into());
    }
    let len = |p: &Path| fs::metadata(p).map(|m| m.len()).map_err(|e| format!("{}: {e}", p.display()));
    let n1 = check_archive("liar", &read(&files[2])?, len(&files[1])?, 0)?;
    // ext.dat is the file the DRM wraps: a 512-byte header, or plain.
    let mut head = [0u8; 10];
    File::open(&files[3]).and_then(|mut f| f.read_exact(&mut head)).map_err(|_| "ext.dat is too short".to_string())?;
    let wrapped = &head == b"NETSYNCDRM";
    let n2 = check_archive("ext", &read(&files[4])?, len(&files[3])?, if wrapped { 512 } else { 0 })?;
    let tested = gpe.len() as u64 == TESTED_GPE_LEN && fnv1a(&gpe) == TESTED_GPE;
    let build = if tested { "the English build this was tested with" } else { "NOT the build this was tested with (it may still work)" };
    Ok((format!("{build}, {n1} + {n2} files"), files))
}

fn main() {
    let o = parse();
    let dir = format!("/home/{}/.local/share/liar", o.user);
    if o.remove {
        say(&format!("Removing the Redemption: Liar loader from {} (your files and saves stay)", o.host));
        if !remote(&o.host, &format!("rm -f {PROFILE} {PROFILE}-*-link && echo removed")) {
            die("could not remove the profile");
        }
        return;
    }
    let files = o.data.as_deref().map(|d| match check_data(d) {
        Ok((what, files)) => {
            say(&format!("Your copy: {what}"));
            files
        }
        Err(e) => die(&format!("{d}: {e}")),
    });

    // Built by `nix run .#install` (no game data); the flake puts its path here.
    let path = env::var("LIAR_LOADER").unwrap_or_else(|_| die("LIAR_LOADER is not set: run this with `nix run .#install`"));
    say(&format!("Copying the loader to {}", o.host));
    run(
        Command::new("nix").args(["copy", "--no-check-sigs", "--to", &format!("ssh-ng://root@{}", o.host), &path]),
        "nix copy",
    );
    if !remote(&o.host, &format!("nix-env -p {PROFILE} --set {p} && ls -l {PROFILE}/bin/liar", p = sq(&path))) {
        die("could not set the liar profile");
    }

    match files {
        Some(files) => {
            for (f, name) in files.iter().zip(FILES) {
                copy_file(&o.host, f, name, &o.user, &dir);
            }
        }
        None => {
            if !remote(&o.host, &format!("test -s {dir}/{}", FILES[0])) {
                say("No game files on the device yet: run this again with --data PATH (your card or its Liar_e folder)");
                return;
            }
        }
    }
    say("Redemption: Liar installed; it shows under Games in the shell");
}

/// Stream one file to the device, owned by the session user, skipped when the
/// same size is there. Saves live beside them and aren't touched.
fn copy_file(host: &str, from: &Path, name: &str, user: &str, dir: &str) {
    let len = fs::metadata(from).map(|m| m.len()).unwrap_or_else(|e| die(&format!("{}: {e}", from.display())));
    let dst = format!("{dir}/{name}");
    if remote_output(host, &format!("stat -c %s {dst}")).is_some_and(|s| s == len.to_string()) {
        return;
    }
    say(&format!("Copying {name} ({} KB)", len / 1000));
    let script = format!(
        "set -e; umask 022; runuser -u {user} -- mkdir -p {dir}; \
         cat > {dst}.part; chown {user}:users {dst}.part; chmod 0644 {dst}.part; mv -f {dst}.part {dst}"
    );
    let file = File::open(from).unwrap_or_else(|e| die(&format!("{}: {e}", from.display())));
    let ok = ssh_cmd(host).arg(script).stdin(Stdio::from(file)).status().map(|s| s.success()).unwrap_or(false);
    if !ok {
        remote(host, &format!("rm -f {dst}.part"));
        die("copying the game files failed");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn missing_folder_is_refused() {
        assert!(check_data("/nonexistent").is_err());
    }

    /// LIAR_DATA=/path/to/card cargo test real_copy -- --ignored --nocapture
    #[test]
    #[ignore]
    fn real_copy() {
        let (what, _) = check_data(&std::env::var("LIAR_DATA").unwrap()).unwrap();
        println!("{what}");
    }

    #[test]
    fn lzss_literals_and_a_match() {
        // flags 0b01: literal 'a'; then a 3-byte match at 0xfee (the 'a' + 2 of what follows it)
        assert_eq!(lzss(&[0b0000_0001, b'a', 0xee, 0xf0]), vec![b'a', b'a', b'a', b'a']);
    }

    #[test]
    fn an_index_that_overruns_the_archive_is_refused() {
        // nothing to compress here: a handmade table of literals, 8 flags at a time
        let mut table = vec![0u8; IDX_PACKED + IDX_MAX * 4 + 4];
        table[0] = 1; // one file
        table[IDX_PACKED..IDX_PACKED + 4].copy_from_slice(&100u32.to_le_bytes());
        let mut packed = Vec::new();
        for chunk in table.chunks(8) {
            packed.push(0xff);
            packed.extend_from_slice(chunk);
        }
        let mut inx = Vec::new();
        inx.extend_from_slice(&(packed.len() as u32).to_le_bytes());
        inx.extend_from_slice(&(table.len() as u32).to_le_bytes());
        inx.extend_from_slice(&packed);
        assert!(check_archive("t", &inx, 100, 0).is_ok());
        assert!(check_archive("t", &inx, 99, 0).is_err());
    }
}
