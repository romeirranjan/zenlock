/*
 * zenlock.c  —  ZenLock: AI-Powered Cognitive Firewall
 * ============================================================
 *
 * A single-binary C implementation of the full ZenLock system.
 * Replaces both monitor.py and daemon.c into one cohesive C program.
 *
 * Architecture (all in one process, multiple threads):
 *
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │                    zenlock (main process)                 │
 *   │                                                             │
 *   │  [monitor_thread]   polls active window via osascript       │
 *   │       │             computes rolling focus score            │
 *   │       │             calls Gemini AI via libcurl (HTTP)      │
 *   │       ▼                                                     │
 *   │  [shared state]     focus_score, is_locked, app/window      │
 *   │       │             protected by pthread_mutex              │
 *   │       ▼                                                     │
 *   │  [pomodoro_thread]  25min work / 5min break timer           │
 *   │  [warning_thread]   10s countdown, re-checks focus          │
 *   │  [logger_thread]    writes structured log to ~/.zenlock/ │
 *   │       │                                                     │
 *   │       ▼                                                     │
 *   │  [process control]  pgrep PIDs → kill(SIGSTOP/SIGCONT)      │
 *   │  [config I/O]       ~/.zenlock_config  (plain text)      │
 *   │  [notifications]    osascript display notification          │
 *   └─────────────────────────────────────────────────────────────┘
 *
 * Systems concepts demonstrated:
 *   - POSIX threads (pthread) with mutex + condition variables
 *   - POSIX signals: SIGSTOP, SIGCONT, SIGTERM, SIGINT handler
 *   - Process inspection via pgrep (popen) and kill()
 *   - HTTP/HTTPS via libcurl for Gemini AI API
 *   - File I/O: config persistence, structured logging
 *   - pipe() + fork() + exec() for osascript subprocess IPC
 *   - Rolling average with circular buffer for focus scoring
 *   - Named pipe (FIFO) for optional external control
 *
 * Build:
 *   gcc -Wall -Wextra -std=c11 -O2 -pthread \
 *       $(curl-config --cflags) \
 *       zenlock.c -o zenlock \
 *       $(curl-config --libs)
 *
 *   Or without AI (no libcurl needed):
 *   gcc -Wall -Wextra -std=c11 -O2 -pthread -DNO_CURL \
 *       zenlock.c -o zenlock
 *
 * Usage:
 *   export GEMINI_API_KEY="your_key"   # optional
 *   ./zenlock                        # runs in foreground, logs to stderr
 *   ./zenlock --pomodoro             # start immediately in Pomodoro mode
 *   ./zenlock --pause                # start paused
 *
 * Requires macOS (uses osascript for window detection + notifications).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <stdarg.h>

#ifndef NO_CURL
#include <curl/curl.h>
#endif

/* ═══════════════════════════════════════════════════════════════
 * Configuration constants
 * ═══════════════════════════════════════════════════════════════ */

#define POLL_INTERVAL_SEC    6
#define LOCK_THRESHOLD       40
#define UNLOCK_THRESHOLD     60
#define WARNING_COUNTDOWN    10
#define WINDOW_SIZE          5       /* rolling score history length  */

#define POMODORO_WORK_SEC    (25 * 60)
#define POMODORO_BREAK_SEC   (5  * 60)

#define MAX_APPS             64
#define MAX_NAME             256
#define MAX_KEYWORDS         64
#define MAX_CACHE            128
#define PIPE_PATH            "/tmp/zenlock.pipe"

/* ═══════════════════════════════════════════════════════════════
 * Default app lists
 * ═══════════════════════════════════════════════════════════════ */

static const char *DEFAULT_DISTRACTORS[] = {
    "MacOS/Discord", "MacOS/Spotify", "MacOS/WhatsApp",
    "MacOS/steam_osx", "MacOS/Netflix",
    NULL
};

static const char *PRODUCTIVE_KEYWORDS[] = {
    "code", "cursor", "xcode", "terminal", "iterm2", "pycharm",
    "intellij", "eclipse", "sublime", "vim", "emacs",
    "notion", "obsidian", "word", "pages", "excel", "numbers",
    "zoom", "teams", "slack", "tutorial", "lecture", "programming",
    "documentation", "research", "study",
    NULL
};

static const char *DISTRACTOR_KEYWORDS[] = {
    "discord", "spotify", "steam", "netflix", "youtube",
    "tiktok", "instagram", "twitter", "reddit", "twitch",
    "messages", "facetime", "whatsapp",
    NULL
};

static const char *EDUCATIONAL_KW[] = {
    "tutorial", "lecture", "course", "learn", "how to", "howto",
    "programming", "coding", "python", "javascript", "c++", "cpp",
    "math", "science", "physics", "chemistry", "biology",
    "explained", "guide", "study", "exam", "university",
    "khan", "homework", "research", "documentary", "mit", "stanford",
    NULL
};

static const char *ENTERTAINMENT_KW[] = {
    "mrbeast", "mr beast", "funny", "meme", "compilation",
    "prank", "vlog", "reaction", "gaming", "fortnite",
    "minecraft", "comedy", "roast", "drama", "music video",
    "celebrity", "movie", "trailer", "shorts",
    NULL
};

/* ═══════════════════════════════════════════════════════════════
 * Gemini AI cache entry
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    char key[MAX_NAME * 2];  /* "app::window_title" */
    int  is_productive;      /* 1 = yes, 0 = no     */
} CacheEntry;

/* ═══════════════════════════════════════════════════════════════
 * Shared state  (protected by g_mutex)
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  warning_done;

    /* focus scoring */
    int     history[WINDOW_SIZE];   /* circular buffer of 0/5/10     */
    int     history_idx;
    double  focus_score;

    /* current window */
    char    active_app[MAX_NAME];
    char    window_title[MAX_NAME];

    /* state flags */
    int     is_locked;
    int     is_paused;
    int     in_warning;
    int     running;            /* set to 0 to shut down all threads */

    /* pomodoro */
    int     pomo_active;
    int     pomo_on_break;
    int     pomo_secs_left;
    int     pomo_sessions;

    /* distractor app list */
    char    distractors[MAX_APPS][MAX_NAME];
    int     distractor_count;

    /* Gemini cache */
    CacheEntry cache[MAX_CACHE];
    int        cache_count;

    /* Gemini API key */
    char    gemini_key[256];

} State;

static State g;

/* ═══════════════════════════════════════════════════════════════
 * Utility: string to lowercase in-place
 * ═══════════════════════════════════════════════════════════════ */

static void str_lower(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

/* case-insensitive substring search */
static int str_contains_ci(const char *haystack, const char *needle) {
    char h[MAX_NAME * 2], n[MAX_NAME];
    strncpy(h, haystack, sizeof(h) - 1); h[sizeof(h)-1] = '\0';
    strncpy(n, needle,   sizeof(n) - 1); n[sizeof(n)-1] = '\0';
    str_lower(h);
    str_lower(n);
    return strstr(h, n) != NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * Logging
 * ═══════════════════════════════════════════════════════════════ */

static void fg_log(const char *level, const char *fmt, ...) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", tm);

    fprintf(stderr, "[%s] [%s] ", ts, level);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

#define LOG_INFO(...)  fg_log("INFO ", __VA_ARGS__)
#define LOG_WARN(...)  fg_log("WARN ", __VA_ARGS__)
#define LOG_ERR(...)   fg_log("ERROR", __VA_ARGS__)

/* ═══════════════════════════════════════════════════════════════
 * Config persistence  (~/.zenlock_config)
 * ═══════════════════════════════════════════════════════════════ */

static char cfg_path[512];

static void config_path_init(void) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(cfg_path, sizeof(cfg_path), "%s/.zenlock_config", home);
}

/* Save distractor list: one app name per line */
static void config_save(void) {
    FILE *f = fopen(cfg_path, "w");
    if (!f) { LOG_ERR("Cannot write config: %s", strerror(errno)); return; }

    pthread_mutex_lock(&g.mutex);
    for (int i = 0; i < g.distractor_count; i++)
        fprintf(f, "%s\n", g.distractors[i]);
    pthread_mutex_unlock(&g.mutex);

    fclose(f);
    LOG_INFO("Config saved → %s", cfg_path);
}

/* Load distractor list from config file, fall back to defaults */
static void config_load(void) {
    FILE *f = fopen(cfg_path, "r");
    if (!f) {
        LOG_INFO("No config found, using defaults");
        for (int i = 0; DEFAULT_DISTRACTORS[i] && g.distractor_count < MAX_APPS; i++) {
            strncpy(g.distractors[g.distractor_count++],
                    DEFAULT_DISTRACTORS[i], MAX_NAME - 1);
        }
        return;
    }

    char line[MAX_NAME];
    while (fgets(line, sizeof(line), f) && g.distractor_count < MAX_APPS) {
        line[strcspn(line, "\n\r")] = '\0';
        if (strlen(line) > 0)
            strncpy(g.distractors[g.distractor_count++], line, MAX_NAME - 1);
    }
    fclose(f);
    LOG_INFO("Config loaded: %d apps from %s", g.distractor_count, cfg_path);
}

/* ═══════════════════════════════════════════════════════════════
 * macOS notifications via osascript
 * ═══════════════════════════════════════════════════════════════ */

static void send_notification(const char *title, const char *message) {
    LOG_INFO("NOTIFY: %s — %s", title, message);

    char script[1024];
    snprintf(script, sizeof(script),
        "display notification \"%s\" with title \"%s\" sound name \"Funk\"",
        message, title);

    pid_t pid = fork();
    if (pid == 0) {
        /* child */
        execlp("osascript", "osascript", "-e", script, (char *)NULL);
        _exit(1);
    } else if (pid > 0) {
        /* parent: don't block — let notification fire in background */
        waitpid(pid, NULL, WNOHANG);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * Active window detection via osascript + pipe()
 * ═══════════════════════════════════════════════════════════════ */

/*
 * run_osascript()
 * Forks a child running osascript -e <script>, reads its stdout
 * into `out` (max `out_sz` bytes), waits for child to finish.
 * Returns 0 on success, -1 on error.
 */
static int run_osascript(const char *script, char *out, size_t out_sz) {
    int pipefd[2];
    if (pipe(pipefd) == -1) return -1;

    pid_t pid = fork();
    if (pid == -1) { close(pipefd[0]); close(pipefd[1]); return -1; }

    if (pid == 0) {
        /* child: redirect stdout → write end of pipe */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        /* suppress stderr */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) dup2(devnull, STDERR_FILENO);
        execlp("osascript", "osascript", "-e", script, (char *)NULL);
        _exit(1);
    }

    /* parent: read from read end */
    close(pipefd[1]);
    size_t total = 0;
    ssize_t n;
    while (total < out_sz - 1 &&
           (n = read(pipefd[0], out + total, out_sz - 1 - total)) > 0)
        total += (size_t)n;
    out[total] = '\0';
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/*
 * get_active_window()
 * Fills `app` and `title` with the frontmost app name and window title.
 * For Chrome, fetches the actual tab title.
 */
static void get_active_window(char *app, size_t app_sz,
                               char *title, size_t title_sz) {
    app[0] = title[0] = '\0';

    static const char *script =
        "tell application \"System Events\"\n"
        "  set frontApp to name of first application process whose frontmost is true\n"
        "  set wTitle to \"\"\n"
        "  try\n"
        "    set wTitle to name of front window of application process frontApp\n"
        "  end try\n"
        "  return frontApp & \"|||\" & wTitle\n"
        "end tell";

    char buf[1024] = {0};
    if (run_osascript(script, buf, sizeof(buf)) != 0) return;

    /* trim trailing whitespace */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' || buf[len-1] == ' '))
        buf[--len] = '\0';

    /* split on ||| */
    char *sep = strstr(buf, "|||");
    if (!sep) {
        strncpy(app, buf, app_sz - 1);
        return;
    }
    *sep = '\0';
    strncpy(app,   buf,       app_sz   - 1);
    strncpy(title, sep + 3,   title_sz - 1);

    /* lowercase app name for matching */
    str_lower(app);

    /* Chrome special case: get real tab title */
    if (strstr(app, "chrome")) {
        static const char *chrome_script =
            "tell application \"Google Chrome\" to get title of active tab of front window";
        char ctitle[512] = {0};
        if (run_osascript(chrome_script, ctitle, sizeof(ctitle)) == 0) {
            size_t clen = strlen(ctitle);
            while (clen > 0 && (ctitle[clen-1] == '\n' || ctitle[clen-1] == '\r'))
                ctitle[--clen] = '\0';
            if (clen > 0) strncpy(title, ctitle, title_sz - 1);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 * Process management: freeze / unfreeze via SIGSTOP / SIGCONT
 * ═══════════════════════════════════════════════════════════════ */

/*
 * get_pids_for_app()
 * Uses popen(pgrep -f ...) to find all PIDs of a process by name.
 * Returns count of PIDs found (up to max_pids).
 */
static int get_pids_for_app(const char *app_name, pid_t *pids, int max_pids) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "pgrep -f \"%s\" 2>/dev/null", app_name);

    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;

    int count = 0;
    while (count < max_pids) {
        pid_t pid;
        if (fscanf(fp, "%d", &pid) != 1) break;
        /* skip our own PID and parent */
        if (pid == getpid() || pid == getppid()) continue;
        pids[count++] = pid;
    }
    pclose(fp);
    return count;
}

/*
 * signal_app()
 * Sends sig (SIGSTOP or SIGCONT) to every PID matching app_name.
 */
static void signal_app(const char *app_name, int sig) {
    pid_t pids[64];
    int count = get_pids_for_app(app_name, pids, 64);

    if (count == 0) {
        LOG_INFO("'%s' not running — skipped", app_name);
        return;
    }

    for (int i = 0; i < count; i++) {
        if (kill(pids[i], sig) == 0) {
            LOG_INFO("Sent %s to '%s' (PID %d)",
                     sig == SIGSTOP ? "SIGSTOP" : "SIGCONT",
                     app_name, pids[i]);
        } else {
            LOG_WARN("kill(%d, %d) failed: %s", pids[i], sig, strerror(errno));
        }
    }
}

/* Lock / unlock all configured distractors */
static void lock_all(void) {
    LOG_INFO("── LOCKING all distractors ──");
    pthread_mutex_lock(&g.mutex);
    int count = g.distractor_count;
    char apps[MAX_APPS][MAX_NAME];
    for (int i = 0; i < count; i++)
        strncpy(apps[i], g.distractors[i], MAX_NAME - 1);
    pthread_mutex_unlock(&g.mutex);

    for (int i = 0; i < count; i++)
        signal_app(apps[i], SIGSTOP);
}

static void unlock_all(void) {
    LOG_INFO("── UNLOCKING all distractors ──");
    pthread_mutex_lock(&g.mutex);
    int count = g.distractor_count;
    char apps[MAX_APPS][MAX_NAME];
    for (int i = 0; i < count; i++)
        strncpy(apps[i], g.distractors[i], MAX_NAME - 1);
    pthread_mutex_unlock(&g.mutex);

    for (int i = 0; i < count; i++)
        signal_app(apps[i], SIGCONT);
}

/* ═══════════════════════════════════════════════════════════════
 * Gemini AI via libcurl  (HTTP POST to v1beta/models/...)
 * ═══════════════════════════════════════════════════════════════ */

#ifndef NO_CURL

/* libcurl write callback: appends received data into a heap buffer */
typedef struct { char *data; size_t size; } CurlBuf;

static size_t curl_write_cb(void *ptr, size_t sz, size_t nmemb, void *userdata) {
    size_t total = sz * nmemb;
    CurlBuf *buf = (CurlBuf *)userdata;
    char *tmp = realloc(buf->data, buf->size + total + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

/*
 * gemini_ask()
 * Sends the app+title context to Gemini 2.0 Flash and returns:
 *   1  → productive
 *   0  → distraction
 *  -1  → API error / no key
 */
static int gemini_ask(const char *app_name, const char *window_title) {
    pthread_mutex_lock(&g.mutex);
    char key[256];
    strncpy(key, g.gemini_key, sizeof(key) - 1);
    pthread_mutex_unlock(&g.mutex);

    if (strlen(key) == 0 || strcmp(key, "YOUR_GEMINI_API_KEY_HERE") == 0)
        return -1;

    /* Build cache key */
    char cache_key[MAX_NAME * 2];
    snprintf(cache_key, sizeof(cache_key), "%s::%s", app_name, window_title);

    /* Check cache */
    pthread_mutex_lock(&g.mutex);
    for (int i = 0; i < g.cache_count; i++) {
        if (strcmp(g.cache[i].key, cache_key) == 0) {
            int cached = g.cache[i].is_productive;
            pthread_mutex_unlock(&g.mutex);
            LOG_INFO("[Gemini] Cache hit: '%s' → %s",
                     window_title, cached ? "productive" : "distraction");
            return cached;
        }
    }
    pthread_mutex_unlock(&g.mutex);

    /* Build JSON request body */
    char prompt[1024];
    snprintf(prompt, sizeof(prompt),
        "App: '%s'. Window title: '%s'. "
        "Is this productive or a distraction? "
        "Reply NO for: YouTube entertainment, music videos, vlogs, gaming, "
        "memes, Netflix, social media, sports highlights, funny videos. "
        "Reply YES for: coding tutorials, documentation, work, studying, research, news. "
        "ONE WORD ONLY: YES or NO.",
        app_name, window_title);

    /* Escape quotes in prompt */
    char escaped[2048] = {0};
    size_t ei = 0;
    for (size_t i = 0; prompt[i] && ei < sizeof(escaped) - 2; i++) {
        if (prompt[i] == '"') escaped[ei++] = '\\';
        escaped[ei++] = prompt[i];
    }

    char body[2500];
    snprintf(body, sizeof(body),
        "{\"contents\":[{\"parts\":[{\"text\":\"%s\"}]}]}", escaped);

    /* Build URL */
    char url[512];
    snprintf(url, sizeof(url),
        "https://generativelanguage.googleapis.com/v1beta/models/"
        "gemini-2.0-flash:generateContent?key=%s", key);

    /* libcurl setup */
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    CurlBuf response = { .data = NULL, .size = 0 };

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    int result = -1;
    if (res == CURLE_OK && response.data) {
        /* Simple text scan: look for "YES" or "NO" in the response text field */
        char *text_pos = strstr(response.data, "\"text\":");
        if (text_pos) {
            char *q1 = strchr(text_pos + 7, '"');
            if (q1) {
                char *q2 = strchr(q1 + 1, '"');
                if (q2) {
                    size_t len = (size_t)(q2 - q1 - 1);
                    char answer[64] = {0};
                    if (len < sizeof(answer)) {
                        strncpy(answer, q1 + 1, len);
                        /* toupper for comparison */
                        for (size_t i = 0; i < strlen(answer); i++)
                            answer[i] = (char)toupper((unsigned char)answer[i]);
                        result = strstr(answer, "YES") ? 1 : 0;
                    }
                }
            }
        }
    } else {
        LOG_WARN("[Gemini] curl error: %s", curl_easy_strerror(res));
    }

    if (response.data) free(response.data);

    /* Store in cache if we got a valid answer */
    if (result >= 0) {
        pthread_mutex_lock(&g.mutex);
        if (g.cache_count < MAX_CACHE) {
            strncpy(g.cache[g.cache_count].key, cache_key,
                    sizeof(g.cache[0].key) - 1);
            g.cache[g.cache_count].is_productive = result;
            g.cache_count++;
        }
        pthread_mutex_unlock(&g.mutex);
        LOG_INFO("[Gemini] '%s' → %s",
                 window_title, result ? "productive ✓" : "distraction ✗");
    }

    return result;
}

#else
/* Stub when compiled without libcurl */
static int gemini_ask(const char *a, const char *b) { (void)a; (void)b; return -1; }
#endif /* NO_CURL */

/* ═══════════════════════════════════════════════════════════════
 * Focus scoring: rolling circular buffer + keyword fallback
 * ═══════════════════════════════════════════════════════════════ */

/*
 * keyword_match()
 * Returns 1 if `context` contains any string from the NULL-terminated
 * array `keywords` (case-insensitive).
 */
static int keyword_match(const char *context, const char **keywords) {
    for (int i = 0; keywords[i]; i++)
        if (str_contains_ci(context, keywords[i])) return 1;
    return 0;
}

/*
 * compute_focus_score()
 * Called with g.mutex HELD.
 * Appends a sample (0=distracted, 5=neutral, 10=focused) to the
 * circular history and returns the score as 0–100.
 */
static double compute_focus_score(const char *app, const char *title) {
    char context[MAX_NAME * 2];
    snprintf(context, sizeof(context), "%s %s", app, title);

    /* Ask Gemini (releases + re-acquires mutex around the HTTP call) */
    pthread_mutex_unlock(&g.mutex);
    int ai = gemini_ask(app, title);
    pthread_mutex_lock(&g.mutex);

    int sample;
    if (ai == 1) {
        sample = 10;
        LOG_INFO("[score] Gemini: productive");
    } else if (ai == 0) {
        sample = 0;
        LOG_INFO("[score] Gemini: distraction");
    } else {
        /* Keyword fallback */
        int is_youtube       = str_contains_ci(context, "youtube");
        int is_educational   = keyword_match(context, EDUCATIONAL_KW);
        int is_entertainment = keyword_match(context, ENTERTAINMENT_KW);

        if (is_youtube) {
            if (is_educational && !is_entertainment) {
                LOG_INFO("[score] Fallback: YouTube educational");
                sample = 10;
            } else {
                LOG_INFO("[score] Fallback: YouTube entertainment");
                sample = 0;
            }
        } else if (keyword_match(context, DISTRACTOR_KEYWORDS)) {
            LOG_INFO("[score] Fallback: distractor app");
            sample = 0;
        } else if (keyword_match(context, PRODUCTIVE_KEYWORDS)) {
            LOG_INFO("[score] Fallback: productive app");
            sample = 10;
        } else {
            LOG_INFO("[score] Fallback: neutral");
            sample = 5;
        }
    }

    /* Write into circular buffer */
    g.history[g.history_idx % WINDOW_SIZE] = sample;
    g.history_idx++;

    /* Compute average */
    int sum = 0;
    for (int i = 0; i < WINDOW_SIZE; i++) sum += g.history[i];
    double score = (double)sum / (WINDOW_SIZE * 10) * 100.0;

    LOG_INFO("[score] sample=%d  history avg → %.1f/100", sample, score);
    return score;
}

/* ═══════════════════════════════════════════════════════════════
 * Warning countdown thread
 * ═══════════════════════════════════════════════════════════════ */

static void *warning_thread(void *arg) {
    (void)arg;

    send_notification("ZenLock — Distraction Detected",
                      "Switch back to work or apps will lock in 10s...");

    char context[MAX_NAME * 2] = {0};

    for (int remaining = WARNING_COUNTDOWN; remaining > 0; remaining--) {
        fprintf(stderr, "\r[zenlock] ⚠️  Locking in %ds…   ", remaining);
        fflush(stderr);
        sleep(1);

        /* Check if focus recovered */
        char app[MAX_NAME] = {0}, title[MAX_NAME] = {0};
        get_active_window(app, sizeof(app), title, sizeof(title));
        snprintf(context, sizeof(context), "%s %s", app, title);

        int productive = keyword_match(context, PRODUCTIVE_KEYWORDS);

        pthread_mutex_lock(&g.mutex);
        int still_distracted = keyword_match(context,
                                   (const char **)DISTRACTOR_KEYWORDS);
        /* Also check custom list */
        for (int i = 0; i < g.distractor_count && !still_distracted; i++)
            if (str_contains_ci(context, g.distractors[i]))
                still_distracted = 1;
        pthread_mutex_unlock(&g.mutex);

        if (productive && !still_distracted) {
            fprintf(stderr, "\n");
            LOG_INFO("Focus recovered — lock cancelled!");
            send_notification("ZenLock", "Focus recovered, lock cancelled.");
            pthread_mutex_lock(&g.mutex);
            g.in_warning = 0;
            pthread_mutex_unlock(&g.mutex);
            pthread_cond_signal(&g.warning_done);
            return NULL;
        }
    }

    fprintf(stderr, "\n");

    /* Final check */
    pthread_mutex_lock(&g.mutex);
    int still_bad = keyword_match(context, (const char **)DISTRACTOR_KEYWORDS);
    for (int i = 0; i < g.distractor_count && !still_bad; i++)
        if (str_contains_ci(context, g.distractors[i])) still_bad = 1;

    if (still_bad) {
        g.is_locked  = 1;
        g.in_warning = 0;
        pthread_mutex_unlock(&g.mutex);

        lock_all();
        LOG_INFO("LOCKED!");
        send_notification("ZenLock — Locked",
                          "Distractor apps frozen. Get back to work!");
    } else {
        g.in_warning = 0;
        pthread_mutex_unlock(&g.mutex);
    }

    pthread_cond_signal(&g.warning_done);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * Monitor thread — polls active window every POLL_INTERVAL_SEC
 * ═══════════════════════════════════════════════════════════════ */

static void *monitor_thread(void *arg) {
    (void)arg;
    LOG_INFO("Monitor thread started (poll every %ds)", POLL_INTERVAL_SEC);

    while (1) {
        pthread_mutex_lock(&g.mutex);
        int running    = g.running;
        int paused     = g.is_paused;
        int on_break   = g.pomo_on_break;
        int pomo_active= g.pomo_active;
        pthread_mutex_unlock(&g.mutex);

        if (!running) break;

        if (paused || on_break) {
            sleep(POLL_INTERVAL_SEC);
            continue;
        }

        /* Get active window */
        char app[MAX_NAME] = {0}, title[MAX_NAME] = {0};
        get_active_window(app, sizeof(app), title, sizeof(title));

        pthread_mutex_lock(&g.mutex);

        strncpy(g.active_app,    app,   MAX_NAME - 1);
        strncpy(g.window_title,  title, MAX_NAME - 1);

        double score = compute_focus_score(app, title);
        g.focus_score = score;

        int locked     = g.is_locked;
        int in_warning = g.in_warning;
        pthread_mutex_unlock(&g.mutex);

        /* Print status line */
        const char *icon = locked ? "🔒" : (in_warning ? "⚠️ " : "🔓");
        fprintf(stderr,
            "[monitor] %s %.0f/100  app=%-20s  title=%.35s\n",
            icon, score, app, title);

        /* Trigger warning if focus dropped */
        pthread_mutex_lock(&g.mutex);
        if (!locked && !in_warning && !pomo_active && score < LOCK_THRESHOLD) {
            g.in_warning = 1;
            pthread_mutex_unlock(&g.mutex);

            pthread_t wt;
            pthread_create(&wt, NULL, warning_thread, NULL);
            pthread_detach(wt);

        } else if (locked && score > UNLOCK_THRESHOLD && !pomo_active) {
            g.is_locked  = 0;
            g.in_warning = 0;
            pthread_mutex_unlock(&g.mutex);

            unlock_all();
            send_notification("ZenLock — Unlocked",
                              "Focus recovered. Apps are back!");
        } else {
            pthread_mutex_unlock(&g.mutex);
        }

        sleep(POLL_INTERVAL_SEC);
    }

    LOG_INFO("Monitor thread exiting");
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * Pomodoro timer thread
 * ═══════════════════════════════════════════════════════════════ */

static void *pomodoro_thread(void *arg) {
    (void)arg;
    LOG_INFO("Pomodoro thread started");

    while (1) {
        pthread_mutex_lock(&g.mutex);
        int running  = g.running;
        int active   = g.pomo_active;
        int secs     = g.pomo_secs_left;
        int on_break = g.pomo_on_break;
        int sessions = g.pomo_sessions;
        pthread_mutex_unlock(&g.mutex);

        if (!running) break;

        if (!active) { sleep(1); continue; }

        if (secs > 0) {
            int m = secs / 60, s = secs % 60;
            fprintf(stderr,
                "\r[pomo] %s %02d:%02d  (session %d)   ",
                on_break ? "☕ Break" : "🍅 Work ", m, s, sessions + 1);
            fflush(stderr);

            pthread_mutex_lock(&g.mutex);
            g.pomo_secs_left--;
            pthread_mutex_unlock(&g.mutex);
            sleep(1);

        } else {
            fprintf(stderr, "\n");
            if (!on_break) {
                /* Work session ended → start break */
                pthread_mutex_lock(&g.mutex);
                g.pomo_sessions++;
                g.pomo_on_break   = 1;
                g.pomo_secs_left  = POMODORO_BREAK_SEC;
                g.is_locked       = 0;
                sessions          = g.pomo_sessions;
                pthread_mutex_unlock(&g.mutex);

                unlock_all();
                char msg[128];
                snprintf(msg, sizeof(msg),
                    "Session %d done! Take a 5-min break.", sessions);
                send_notification("ZenLock 🍅 Session Complete!", msg);

            } else {
                /* Break ended → start next work session */
                pthread_mutex_lock(&g.mutex);
                g.pomo_on_break  = 0;
                g.pomo_secs_left = POMODORO_WORK_SEC;
                g.is_locked      = 1;
                sessions         = g.pomo_sessions;
                pthread_mutex_unlock(&g.mutex);

                lock_all();
                char msg[128];
                snprintf(msg, sizeof(msg),
                    "Back to work! Session %d — apps locked again.", sessions + 1);
                send_notification("ZenLock ▶ Break Over", msg);
            }
        }
    }

    LOG_INFO("Pomodoro thread exiting");
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * Named pipe control thread (optional external commands)
 * Commands: LOCK, UNLOCK, PAUSE, RESUME, POMO_START, POMO_STOP, QUIT
 * ═══════════════════════════════════════════════════════════════ */

static void *pipe_thread(void *arg) {
    (void)arg;

    if (mkfifo(PIPE_PATH, 0666) == -1 && errno != EEXIST) {
        LOG_WARN("mkfifo failed: %s (pipe control disabled)", strerror(errno));
        return NULL;
    }

    LOG_INFO("Pipe control listening on %s", PIPE_PATH);

    while (1) {
        pthread_mutex_lock(&g.mutex);
        int running = g.running;
        pthread_mutex_unlock(&g.mutex);
        if (!running) break;

        int fd = open(PIPE_PATH, O_RDONLY);
        if (fd == -1) continue;

        char buf[64] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;

        buf[strcspn(buf, "\n\r")] = '\0';
        LOG_INFO("[pipe] Received: '%s'", buf);

        if (strcmp(buf, "LOCK") == 0) {
            pthread_mutex_lock(&g.mutex);
            g.is_locked = 1;
            pthread_mutex_unlock(&g.mutex);
            lock_all();
        } else if (strcmp(buf, "UNLOCK") == 0) {
            pthread_mutex_lock(&g.mutex);
            g.is_locked = 0;
            pthread_mutex_unlock(&g.mutex);
            unlock_all();
        } else if (strcmp(buf, "PAUSE") == 0) {
            pthread_mutex_lock(&g.mutex);
            g.is_paused = 1;
            pthread_mutex_unlock(&g.mutex);
            send_notification("ZenLock", "Monitoring paused.");
        } else if (strcmp(buf, "RESUME") == 0) {
            pthread_mutex_lock(&g.mutex);
            g.is_paused = 0;
            pthread_mutex_unlock(&g.mutex);
            send_notification("ZenLock", "Monitoring resumed.");
        } else if (strcmp(buf, "POMO_START") == 0) {
            pthread_mutex_lock(&g.mutex);
            g.pomo_active    = 1;
            g.pomo_on_break  = 0;
            g.pomo_secs_left = POMODORO_WORK_SEC;
            g.pomo_sessions  = 0;
            g.is_locked      = 1;
            pthread_mutex_unlock(&g.mutex);
            lock_all();
            send_notification("ZenLock 🍅 Session Started",
                              "Apps locked. Work for 25 minutes!");
        } else if (strcmp(buf, "POMO_STOP") == 0) {
            pthread_mutex_lock(&g.mutex);
            g.pomo_active = 0;
            g.is_locked   = 0;
            pthread_mutex_unlock(&g.mutex);
            unlock_all();
            send_notification("ZenLock", "Pomodoro stopped. Apps unlocked.");
        } else if (strcmp(buf, "QUIT") == 0) {
            pthread_mutex_lock(&g.mutex);
            g.running = 0;
            pthread_mutex_unlock(&g.mutex);
            break;
        }
    }

    unlink(PIPE_PATH);
    LOG_INFO("Pipe thread exiting");
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * SIGTERM / SIGINT handler — graceful shutdown
 * ═══════════════════════════════════════════════════════════════ */

static void handle_signal(int sig) {
    (void)sig;
    LOG_INFO("Signal received — shutting down gracefully");
    pthread_mutex_lock(&g.mutex);
    g.running = 0;
    pthread_mutex_unlock(&g.mutex);
    /* Always unfreeze apps on exit */
    unlock_all();
    unlink(PIPE_PATH);
    _exit(0);
}

/* ═══════════════════════════════════════════════════════════════
 * State init
 * ═══════════════════════════════════════════════════════════════ */

static void state_init(void) {
    memset(&g, 0, sizeof(g));
    pthread_mutex_init(&g.mutex, NULL);
    pthread_cond_init(&g.warning_done, NULL);
    g.running     = 1;
    g.focus_score = 50.0;

    /* Pre-fill history with neutral samples */
    for (int i = 0; i < WINDOW_SIZE; i++) g.history[i] = 5;

    /* Gemini API key from environment */
    const char *env_key = getenv("GEMINI_API_KEY");
    if (env_key && strlen(env_key) > 0)
        strncpy(g.gemini_key, env_key, sizeof(g.gemini_key) - 1);
    else
        strncpy(g.gemini_key, "YOUR_GEMINI_API_KEY_HERE",
                sizeof(g.gemini_key) - 1);
}

/* ═══════════════════════════════════════════════════════════════
 * main()
 * ═══════════════════════════════════════════════════════════════ */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [--pomodoro] [--pause] [--help]\n"
        "\n"
        "  --pomodoro   Start a Pomodoro session immediately (locks apps)\n"
        "  --pause      Start with monitoring paused\n"
        "  --help       Show this message\n"
        "\n"
        "Environment:\n"
        "  GEMINI_API_KEY   Google Gemini API key (optional; enables AI mode)\n"
        "\n"
        "Pipe control (echo COMMAND > /tmp/zenlock.pipe):\n"
        "  LOCK, UNLOCK, PAUSE, RESUME, POMO_START, POMO_STOP, QUIT\n",
        prog);
}

int main(int argc, char *argv[]) {
    int start_pomodoro = 0, start_paused = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pomodoro") == 0) start_pomodoro = 1;
        else if (strcmp(argv[i], "--pause") == 0) start_paused = 1;
        else if (strcmp(argv[i], "--help") == 0) { print_usage(argv[0]); return 0; }
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }

    /* Signal handlers */
    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* Init */
    state_init();
    config_path_init();
    config_load();

#ifndef NO_CURL
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (strlen(g.gemini_key) > 0 &&
        strcmp(g.gemini_key, "YOUR_GEMINI_API_KEY_HERE") != 0)
        LOG_INFO("Gemini AI: ENABLED");
    else
        LOG_INFO("Gemini AI: DISABLED (set GEMINI_API_KEY to enable)");
#else
    LOG_INFO("Gemini AI: DISABLED (compiled without libcurl)");
#endif

    LOG_INFO("ZenLock starting — %d distractor apps configured",
             g.distractor_count);

    /* Apply startup flags */
    if (start_paused) {
        g.is_paused = 1;
        LOG_INFO("Starting paused");
    }
    if (start_pomodoro) {
        g.pomo_active    = 1;
        g.pomo_secs_left = POMODORO_WORK_SEC;
        g.is_locked      = 1;
        LOG_INFO("Starting Pomodoro session immediately");
        lock_all();
        send_notification("ZenLock 🍅 Session Started",
                          "Apps locked. Work for 25 minutes!");
    }

    /* Launch threads */
    pthread_t t_monitor, t_pomo, t_pipe;
    pthread_create(&t_monitor, NULL, monitor_thread,  NULL);
    pthread_create(&t_pomo,    NULL, pomodoro_thread, NULL);
    pthread_create(&t_pipe,    NULL, pipe_thread,     NULL);

    LOG_INFO("All threads running. Ctrl-C to quit.");
    LOG_INFO("Send commands via:  echo COMMAND > %s", PIPE_PATH);

    /* Wait for threads */
    pthread_join(t_monitor, NULL);
    pthread_join(t_pomo,    NULL);
    pthread_join(t_pipe,    NULL);

    /* Cleanup */
    unlock_all();
    config_save();

#ifndef NO_CURL
    curl_global_cleanup();
#endif

    pthread_mutex_destroy(&g.mutex);
    pthread_cond_destroy(&g.warning_done);

    LOG_INFO("ZenLock exited cleanly.");
    return 0;
}
