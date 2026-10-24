# FileEncryptor

**English** | [中文](README.md)

> 🔐 Lightweight file encryption tool | Windows GUI + CLI | AES-256-GCM
> A Windows file encryption tool with both a graphical interface (Win32) and a command-line version (CLI).

A streaming file encryption tool built on AES-256-GCM and PBKDF2-HMAC-SHA256. Large files are processed in chunks, with a simple and safe workflow.

## ✨ Features (shared)

* **Authenticated encryption**: AES-256-GCM with PBKDF2-HMAC-SHA256 key derivation. The ciphertext carries its own auth tag, so a wrong password or a tampered file is reliably detected
* **Large files**: streaming chunked processing keeps memory usage low
* **Secure delete**: a standalone feature that overwrites with random data 1-3 times and then deletes (four entry points: button / drag-and-drop / context menu / CLI); deleting a folder removes the directories too
* **Configurable deletion**: keep or overwrite-delete the source after encrypting; the encrypted file can be deleted directly after decrypting
* **No third-party dependencies**: all cryptography uses the CNG (bcrypt) built into Windows — nothing else to install
* **Format compatible**: the GUI and CLI use exactly the same file format and can decrypt each other's output

> **Platform**: Windows only. Cryptography is based on the CNG built into Windows;
> the algorithms and file format are standard and platform-independent, but have not been
> compiled or verified on other platforms.

---

## 💻 Requirements

### GUI version

| Item | Notes |
|---|---|
| OS | **Windows 10 / 11 (x64)** — the recommended and tested environment |
| Architecture | x64 only (cryptography uses system CNG; switching to 32-bit is just a platform change) |
| Runtime | **Nothing to install** — static CRT, cryptography via Windows CNG (bcrypt), a single EXE with no external DLLs |
| DPI | Declared system-DPI aware; crisp at 125% / 150% scaling |
| Privileges | `asInvoker` — no administrator rights needed |

**Older systems**: the PE header minimum version is 6.00 (Vista) and the import table contains no
Win8/Win10-only APIs, so the program **should** start on Windows 7 SP1. The following effects only
work on newer systems:

| Effect | Minimum |
|---|---|
| Rounded window corners | Windows 11 |
| Dark title bar | Windows 10 1809 (build 17763) |
| Custom title bar colors | Windows 11 |
| Light/dark following the system | Windows 10 |

On unsupported systems these calls are simply ignored (no error) and the UI falls back to square
corners with the default title bar. **Only Windows 10 / 11 have been fully verified.**

> Note: the program does not embed the `Microsoft.Windows.Common-Controls 6.0` manifest, so standard
> controls render in the classic style. Since all UI is hand-drawn (and calls
> `SetWindowTheme(h, L"", L"")` to disable themes), this has no visual impact; APIs such as
> `SetWindowSubclass` are resolved through comctl32 v5's ordinal exports.

### CLI version

| Item | Notes |
|---|---|
| OS | Windows (x64) |
| Architecture | x64 (source is architecture-independent; 32-bit compiles as well) |
| Runtime | Statically linked, one file, no external dependencies (cryptography uses system CNG) |
| Non-ASCII paths | Handled: paths are kept as UTF-8 internally and converted to UTF-16 for file APIs, so non-ASCII names work under any locale |

---

## 🔐 Algorithms and file format

| Item | Notes |
|---|---|
| Cipher | AES-256-GCM (AEAD) |
| Key derivation | PBKDF2-HMAC-SHA256, iteration count configurable |
| Strength | Fast 150,000 / Standard 300,000 (default) / Secure 450,000 / Extreme 600,000 |
| Randomness | Windows CNG `BCryptGenRandom` (16-byte salt, 12-byte IV) |

File format (v3, current). **The two slots are completely symmetric** — no header field says which is real:

```
Offset 0    magic[8]        "FENC\r\n\x1a\n"   distinguishes from old formats
Offset 8    version[1]      = 3
Offset 9    flags[1]        bit0 anti-brute-force on, bit1 mode (0 = delete, 1 = lock)
Offset 10   iterations[4]   PBKDF2 iterations (little endian)
Offset 14   maxTries[2]
Offset 16   lockDays[2]
Offset 18   saltA[16]
Offset 34   IV_A[12]        96-bit GCM nonce
Offset 46   verifierA[8]
Offset 54   slotA length[8] little endian
Offset 62   saltB[16]
Offset 78   IV_B[12]
Offset 90   verifierB[8]
Offset 98   remaining[2]    ← mutable (not authenticated)
Offset 100  lockUntil[8]    ← mutable (Unix seconds, 0 = not locked)
Offset 108  ciphertext A[slotA length]
then        ciphertext B (until end of file)
```

> **Bytes 8..98 are authenticated as GCM AAD**: if either slot's salt / IV / verifier, the iteration
> count, or the protection policy is modified, decryption fails authentication. An attacker therefore
> cannot disable protection or swap a slot by editing the header.
> Bytes 98..108 must be writable without the password, so they are not authenticated (see
> "Anti-brute-force" below for the limits this implies).

> The iteration count is stored in the header and **read automatically on decrypt**, so raising the
> strength does not affect decryption of older files.

**The two slots are symmetric and assigned randomly**:

* The header contains **no** field indicating which slot is real, or whether a decoy exists
* Encryption **randomly decides** which slot holds the real payload, removing statistical tells like "the real file is always in A"
* With no decoy, both slots are still written; the other slot is filled with random data
  (50%-150% of the real ciphertext length), so **even the existence of a second payload cannot be determined**
* Cost: **the file is always about 2x the plaintext**

**Format support**:

* The program supports **only the v3 format** (the table above) and no longer reads legacy formats
  (v2 / v1 / the oldest CBC)
* The version field is kept for future evolution, but only `ver == 3` is accepted; other versions
  produce an explicit error instead of failing silently or crashing

---

## 🖥️ GUI version

For Windows 10/11, with a clean graphical interface.

### Highlights

* **Three peer features**: Encrypt file / Decrypt file / **Secure delete**
* **Settings panel** (gear button, top right)
  * Language: 中文 / English
  * Theme: System / Light / Dark
  * Output location: same folder / fixed folder / ask each time
  * **Secure delete overwrite passes: 1 / 2 / 3** (segmented control)
  * When done: close window / show dialog
  * Encryption strength: Fast / Standard / Secure / Extreme
  * **Keep source files after encrypting/decrypting**
  * **Disable decoy**: writes a single slot, ~1x the size instead of 2x — the trade-off is that the size reveals "no decoy"
* **Modern UI**: hand-drawn WinUI 3 style interface, rounded controls, hover and progress animations, dark mode
* **Password strength meter**: live color grading (very weak / weak / good / strong)
* **Fully portable**: a single EXE, no extra DLLs

> Settings are stored in the registry under `HKCU\Software\FileEncryptor`.

### Secure delete

A standalone delete feature alongside encrypt/decrypt. Four entry points:

| Entry point | How |
|---|---|
| Main window button | Click **Secure delete** → select files → confirm |
| Drag and drop | Drag files onto the **Secure delete** button; it turns red and reads "Release to delete" → release → confirm |
| Explorer context menu | Select files → right-click → **Securely delete with FileEncryptor** |
| Command line | `enc --delete <files...>` (multiple allowed; pass count via `-w`) |

```bat
:: overwrite 3 times, then delete one file
enc --delete secret.txt

:: delete several at once
enc --delete a.txt b.txt c.txt

:: delete directly without overwriting (faster, weaker guarantee)
enc --delete temp.log -w 0
```

* **Every delete in the GUI asks for confirmation first**, stating the file count and pass count; it cannot be undone
  (the CLI, as usual for command-line tools, does not prompt)
* Shares the same task queue as encryption, so **batching, the progress bar, the tray icon and the queue counter** all apply
* The pass count comes from the settings panel (1-3); on the command line use `-w` (0-10)

> ⚠️ **About overwriting**: in full-disk encryption (BitLocker), SSD wear leveling, copy-on-write
> file systems (ReFS / Btrfs / ZFS) or cloud-synced folders, overwriting **does not guarantee** the
> original data is erased — those mechanisms may write the new data elsewhere and leave old blocks
> behind. Overwriting is best-effort, **not physical destruction**.

### Explorer context menu

On every start the program registers two menu items under `HKCU\Software\Classes\*\shell`
(**no administrator rights needed**):

| Menu item | Actually runs |
|---|---|
| Encrypt with FileEncryptor | `FileEncryptor.exe --encrypt "%1"` |
| Securely delete with FileEncryptor | `FileEncryptor.exe --secure-delete "%1"` |

* Uses `MultiSelectModel=Player`: for a multi-selection Explorer starts **one process per file**;
  the program forwards them to the running window via **single-instance forwarding**, so a
  multi-selection is still processed as **one batch**
* Menu titles follow the program language and are rewritten on every start (so moving the exe is fine)
* **Can be turned off in Settings**: uncheck "Show in Explorer context menu" and click OK — the
  registered items are **deleted immediately** (re-check to restore)
* To clean up manually:

```bat
reg delete "HKCU\Software\Classes\*\shell\FileEncryptor.Encrypt" /f
reg delete "HKCU\Software\Classes\*\shell\FileEncryptor.SecureDelete" /f
```

> ⚠️ **Windows 11 users**: the items appear in the **classic (legacy) context menu**. Click
> "Show more options" or press `Shift+F10` to see them — they do **not** appear in the new compact
> Windows 11 menu. Reaching the new menu requires packaging the shell extension as an
> **MSIX / sparse package** with **code signing**, which is a separate piece of work (it needs a
> signing certificate) and is not done in this version.

### Download and usage

1. Download `FileEncryptor.exe` from the [Releases](https://github.com/yvziyao/FileEncryptor/releases) page
2. Run it, then click **Encrypt**, **Decrypt** or **Secure delete**
3. Pick the target files and enter the password (you can also drag files straight into the window)

---

## ⌨️ CLI version

For Windows; suitable for servers, remote environments or automation.

### Build

**Windows (MSVC; cryptography uses system CNG, no third-party libraries)**

```bat
cl /EHsc /std:c++17 /MT /O2 /GL /DNOMINMAX /utf-8 ^
   enc.cpp file_crypto.cpp /Fe"enc.exe" ^
   /link /LTCG ^
   ws2_32.lib gdi32.lib crypt32.lib advapi32.lib user32.lib
```

> `/utf-8` is recommended: the sources are UTF-8 (with BOM) so the compiler detects them correctly,
> but MSVC's execution character set follows the system ANSI code page by default. With `/utf-8`
> the Chinese messages are UTF-8 under any locale. The source also uses
> `#pragma execution_character_set("utf-8")` as a fallback.

> **Dependencies**: none — cryptography (AES-256-GCM / PBKDF2-HMAC-SHA256 / HMAC-SHA256 / RNG)
> all uses the CNG (bcrypt) built into Windows; the Windows SDK is enough to build.
> The Windows-only libraries (`ws2_32` / `gdi32` / `crypt32` / `advapi32` / `user32`) ship with the OS.

### Non-ASCII and path encoding

* Sources are **UTF-8 (with BOM)** and also compile correctly on machines whose ANSI code page is
  not UTF-8 (e.g. the default Simplified Chinese Windows, code page 936)
* Internally paths are kept as **UTF-8** and converted to UTF-16 before file API calls, so
  **non-ASCII file names and output folders work under any locale**
* The console output code page is switched to UTF-8 on startup (and restored on exit) so messages
  are never garbled

### Usage

```bash
# interactive encryption (prompts for the password and confirms)
enc file.txt

# password on the command line (encryption still confirms)
enc file.txt -p yourpassword

# specify strength (fast | standard | secure | extreme, default standard)
enc file.txt -p yourpassword -s extreme

# write to a specific folder (produces D:\Encrypted\file.txt.enc)
enc file.txt -p yourpassword -o D:\Encrypted

# keep the source file
enc file.txt -p yourpassword -k

# interactive decryption
enc file.txt.enc

# password on the command line (decryption, good for batches)
enc file.txt.enc -p yourpassword

# decrypt to a specific folder and keep the encrypted file
enc file.txt.enc -p yourpassword -o D:\Decrypted -k
```

### Options

| Option | Notes |
|---|---|
| (auto-detected) | Based on the extension: `.enc` is decrypted, anything else is encrypted |
| `<file>` | Input file (required, a positional argument) |
| `-o <dir>` | Output folder, default is the input file's folder. Must already exist |
| `-p <password>` | Password; if omitted you are prompted (input is not echoed) |
| `-s <strength>` | `fast` / `standard` / `secure` / `extreme`, default `standard` |
| `-w <count>` | Source handling: `0` = **keep the source** (same as `-k`), `1`-`10` = overwrite passes before deleting, default `3` |
| `-k` / `--keep` | Keep the source (when encrypting) / the encrypted file (when decrypting); no deletion |
| `-t <count>` | **Enable anti-brute-force**: allowed wrong attempts (1-9999). Not enabled if omitted |
| `-x <action>` | When exhausted: `delete` = permanently delete (default), `lock` = block decryption for a period |
| `-l <days>` | Lock duration in `lock` mode (0-3650, default 7) |
| `--decoy <file>` | **Deniable encryption**: specify a decoy file to hand over under coercion |
| `--decoy-pass <pwd>` | **Deniable encryption**: the fake password that opens the decoy |
| `--no-decoy` | Write a single slot, ~1x the size (default 2x). The trade-off is that the size reveals "no decoy" |
| `--delete` / `-D <files...>` | **Secure delete**: only deletes, no encrypt/decrypt; multiple files allowed, pass count via `-w` |
| `--lang <zh\|en>` | Output language, default `en`. Use `--lang zh` for Chinese |
| `-h` / `--help` | Show help |

> Output names are derived automatically: `.enc` is appended when encrypting and removed when decrypting.
>
> As in the GUI, `-w 0` corresponds to "keep source files after encrypting/decrypting" — it means
> **keep**, not delete.
>
> `--decoy` and `--decoy-pass` **must be given together**, and the fake password must differ from the
> real one; the fake password only exists to be handed over under coercion.
>
> `--no-decoy` **sacrifices deniability** (the file size itself reveals there is no decoy);
> only use it when you are sure you do not need the decoy feature.

### Anti-brute-force

Enabled when encrypting; the policy is written into the header and **enforced automatically by the
decrypting side with no configuration** (shared by GUI and CLI).

* **GUI**: in the password dialog, check `after [N] wrong passwords` and choose `delete permanently`
  or `block decryption` (the latter also takes a lock duration in days). **Off by default.**
* **CLI**: enable with `-t`:

```bash
# delete the file permanently after 3 wrong attempts
enc secret.txt -p mypass -t 3

# block decryption for 7 days after 5 wrong attempts
enc secret.txt -p mypass -t 5 -x lock -l 7
```

* In **GUI decryption** the password dialog shows `X attempts left` in red; a wrong password
  decrements the counter and re-prompts
* In **CLI decryption** the interactive prompt reads `Password (X attempts left)`; with `-p`, a
  failure prints the remaining count
* While locked, **even the correct password is rejected**, with the remaining lock time shown
* After the lock expires, the first decryption resets the counter to its initial value

When decrypting a batch, **each file is verified and retried independently** if you choose
"enter a password per file"; if you choose "same password for all files", verification and retry are
based on the **first file** only, and any file with a different password decrements its own counter
when it actually fails.

#### ⚠️ Limits of this mechanism (please read)

The remaining count and lock time **must be writable without knowing the password** (otherwise one
typo would lock the file forever), so they **cannot be cryptographically authenticated**. Anyone can:

* **Copy the `.enc` before trying and restore the copy after each failure** — the counter never reaches zero
* Reset the counter and the lock time with a hex editor

So it is an **effective deterrent and speed bump**, but it **will not stop a determined attacker**.
Do not treat it as reliable protection.

The rest of the header (salt, IV, iteration count and **the policy itself**) is covered by the GCM
AAD, so an attacker **cannot disable protection by editing the header** — that would fail
authentication and the file still would not open.

> "Delete permanently" is irreversible: if you forget the password and exhaust the attempts, the file
> really is deleted and cannot be recovered.

### Deniable encryption (decoy file)

When encrypting you can additionally specify a **decoy file** and a **fake password**. Under
coercion you hand over the fake password and the other side decrypts the decoy; the real password
opens the real content.

* **CLI**: `--decoy <decoy file> --decoy-pass <fake password>`
* **GUI**: when setting the password, check "Use a decoy file (deniable encryption)", then pick the
  decoy file and set the fake password (single-file encryption only; doing this per file in a batch
  would be too tedious)

#### ⚠️ Limits of this mechanism (please read)

**What it does**:

* Handing over the fake password reveals only the decoy, never the real content
* Looking at the file alone, **you cannot tell which slot is real, nor whether a decoy exists** —
  the structure is identical either way

**What it does not do**:

* **Limited "existence" deniability**: the file is about 2x the plaintext, so structurally there are
  two ciphertext segments. A knowledgeable person can infer "there may be a second payload here".
  A true hidden volume stores the second payload in unallocated space; **this is not a hidden volume**
* If the attacker **knows both passwords**, both payloads can be decrypted
* It cannot resist sustained coercion (rubber-hose)
* It shares the anti-brute-force counter: a coercer entering the wrong fake password also consumes
  attempts, and with "delete when exhausted" configured there is a risk of accidental deletion

> **If you are sure you do not need the decoy**, check "Disable decoy" in Settings (CLI:
> `--no-decoy`). Only one slot is written and **the file drops to about 1x the size**.
> The trade-off is exactly the first point above — a single-slot file is 1x and a dual-slot file is
> 2x, so **the size itself reveals that there is no decoy**. You trade indistinguishability for size.
> With this enabled the password dialog no longer offers the decoy option.

### Batch examples

```bat
:: Windows batch (CMD): decrypt everything into a folder
for %f in (*.enc) do enc "%f" -p mypass -o D:\Decrypted
```

```powershell
# PowerShell: encrypt everything and keep the sources
Get-ChildItem *.txt | ForEach-Object { .\enc.exe $_.FullName -p mypass -o .\encrypted -k }
```

---

## 🔒 Security notes

* The CLI and GUI use **exactly the same algorithms and file format** (AES-256-GCM + PBKDF2), so
  files encrypted by one can be **decrypted by the other**
* **Deletion policy** (GUI: in Settings; CLI: `-k` / `-w`)
  * Encrypt: the source contains plaintext, so it is overwritten 1 or 3 times and deleted; choosing
    "off", `-k` or `-w 0` **keeps the source**
  * Decrypt: the encrypted file is ciphertext with no plaintext residue, so **no overwriting** — it
    is deleted directly; "off", `-k` or `-w 0` **keeps it**
* On SSDs, cloud-synced folders and similar media, overwriting cannot guarantee 100% unrecoverability
* Prefer local, non-synced folders and assess the risk yourself
* **Keep your password safe — without it the file cannot be decrypted** (AES-256-GCM has no backdoor
  and no recovery mechanism)

---

## 🔏 Code signing

Release builds of this project use the free code signing service from
[SignPath Foundation](https://signpath.org/), with certificates issued by
[SignPath.io](https://about.signpath.io/).

[![Signed by SignPath.io](https://about.signpath.io/images/badges/signed-by-signpath-badge.svg)](https://about.signpath.io/)

## 📄 License

This project is open source under the MIT License.

## 🤝 Contributing

Issues and pull requests are welcome.
