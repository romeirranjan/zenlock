# ZenLock 🔒
### A Context-Aware, AI-Powered Cognitive Firewall

ZenLock is a native macOS Menu Bar application that physically suspends distracting applications the moment you lose focus. Rather than relying on simple timers or static blocklists, ZenLock uses **Google Gemini AI** to understand *what* you are doing in real-time, enforcing productivity through kernel-level process freezing.

---

## ✨ Features

- **Gemini AI Contextual Firewall:** ZenLock doesn't just block "YouTube." It reads the active window title. If you are watching a "C++ Tutorial", Gemini allows it. If you switch to a "Funny Cat Compilation", Gemini flags it and freezes the app.
- **Native macOS Menu Bar UI:** Completely controls the daemon, tracks focus score, and manages settings without needing a terminal open.
- **Dynamic Custom Blocklists:** Add or remove specific distractor apps directly from the menu bar UI.
- **Strict Pomodoro Mode:** 25-minute work / 5-minute break sessions that automatically enforce locks.
- **Graceful Lockouts:** A 10-second warning countdown allows you to return to work before your apps are physically frozen in memory.

---

## ⚙️ How It Works (The Hybrid Architecture)

To bridge the gap between high-level LLM context analysis and low-level Unix systems programming, ZenLock utilizes a two-part hybrid architecture:

```text
┌─────────────────────────┐         Named Pipe           ┌──────────────────────────┐
│  monitor.py (Python)    │ ── "LOCK" / "UNLOCK" ──▶     │  zenlock_daemon (C)   │
│  macOS Menu Bar App     │      /tmp/zenlock.pipe    │  (System Muscle)         │
│                         │                              │                          │
│ 1. Extracts Window Data │                              │ 1. pgrep → PIDs          │
│ 2. Asks Gemini API      │                              │ 2. kill(SIGSTOP)         │
│ 3. Freezes Custom Apps  │                              │ 3. kill(SIGCONT)         │
└─────────────────────────┘                              └──────────────────────────┘
```

---

## 🚀 Quick Start

### 1. Install dependencies

```bash
make install-deps
# or manually:
pip3 install rumps google-genai
```

### 2. Set your Gemini API key

```bash
export GEMINI_API_KEY="your_key_here"
```

Get a free key at [aistudio.google.com](https://aistudio.google.com).
ZenLock works without it too — it falls back to a smart keyword-based system.

### 3. Build the C daemon

```bash
make
```

### 4. Run the daemon (Terminal 1)

```bash
make run-daemon
# or: ./zenlock_daemon
```

### 5. Run the monitor (Terminal 2)

```bash
make run-monitor
# or: python3 monitor.py
```

ZenLock will appear in your macOS menu bar. Click it to control everything.

---

## 🏗️ Project Structure

```
zenlock/
├── daemon.c       — C process: listens on named pipe, sends SIGSTOP/SIGCONT
├── monitor.py     — Python menu bar app: AI analysis, Pomodoro, UI
├── Makefile       — build & run shortcuts
└── README.md
```

---

## 🧠 Systems Concepts

| Concept | Where Used |
|---|---|
| Named Pipes (FIFO) | IPC between Python monitor and C daemon |
| POSIX Signals | `SIGSTOP` / `SIGCONT` to freeze/unfreeze processes |
| `pgrep` + `kill()` | Finding and signaling PIDs by name |
| `mkfifo`, `open`, `read` | Low-level file I/O on the pipe |
| Threading | Python monitor uses threads for UI, polling, and Pomodoro |
| `osascript` | macOS AppleScript to read active window titles |

---

## ⚙️ Configuration

Edit the top of `monitor.py` to adjust:

| Variable | Default | Description |
|---|---|---|
| `POLL_INTERVAL_SEC` | 6 | How often to check the active app |
| `LOCK_THRESHOLD` | 40 | Focus score below this triggers a warning |
| `UNLOCK_THRESHOLD` | 60 | Focus score above this unlocks apps |
| `WARNING_COUNTDOWN` | 10 | Seconds of warning before locking |
| `POMODORO_WORK_MIN` | 25 | Length of a Pomodoro work session |
| `POMODORO_BREAK_MIN` | 5 | Length of a Pomodoro break |

Edit `daemon.c` to change the default blocked apps (requires recompile):

```c
static const char *DISTRACTOR_APPS[] = {
    "MacOS/Discord",
    "MacOS/Spotify",
    "MacOS/WhatsApp",
    "MacOS/steam_osx",
    NULL
};
```

---

## 📋 Requirements

- macOS (uses AppleScript, macOS menu bar APIs)
- Python 3.8+
- GCC or Clang
- `rumps` (`pip3 install rumps`)
- `google-genai` (`pip3 install google-genai`) — optional, for AI mode
- Accessibility permissions (System Settings → Privacy → Accessibility)
