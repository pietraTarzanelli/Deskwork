/*
 * Deskwork - tiny solar dynamic wallpaper daemon for KDE Plasma
 *
 * Build:
 *   cc -O2 -Wall -Wextra -std=c11 deskwork.c -o deskwork -lm
 *
 * Runtime dependency:
 *   qdbus6 (preferred) or qdbus
 *
 * Design:
 *   - 8 solar phases
 *   - local solar calculations (offline once coordinates are known)
 *   - cached last known coordinates
 *   - sleeps until the next phase
 *   - --reset can wake a running daemon via SIGHUP
 *
 * NOTE ABOUT AUTO LOCATION:
 *   This first version deliberately has no online IP-geolocation provider
 *   hardcoded. --set-location seeds the cache; --reset reloads it.
 *   A provider can be added later without touching the solar engine.
 */

#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DESKWORK_VERSION "0.2.0"
#define PHASE_COUNT 8
#define DEG2RAD(x) ((x) * M_PI / 180.0)
#define RAD2DEG(x) ((x) * 180.0 / M_PI)

typedef struct {
    const char *name;
    double altitude_deg;
    int direction; /* +1 rising, -1 setting, 0 = midnight fallback */
} PhaseDef;

static const PhaseDef phases[PHASE_COUNT] = {
    {"night",      -18.0,   -1},
    {"dawn",       -12.0,   +1},
    {"civil-dawn",  -6.0,   +1},
    {"morning",     -0.833, +1},
    {"day",         15.0,   +1},
    {"evening",     15.0,   -1},
    {"sunset",      -0.833, -1},
    {"dusk",        -6.0,   -1},
};

typedef struct {
    char wallpaper_dir[PATH_MAX];
    char cache_file[PATH_MAX];
    char pid_file[PATH_MAX];
} Config;

typedef struct {
    double lat;
    double lon;
    bool valid;
} Location;

typedef struct {
    time_t when;
    bool valid;
} PhaseTime;

static volatile sig_atomic_t g_reset_requested = 0;
static volatile sig_atomic_t g_stop_requested = 0;

static void on_hup(int sig)  { (void)sig; g_reset_requested = 1; }
static void on_stop(int sig) { (void)sig; g_stop_requested = 1; }

static void usage(FILE *out) {
    fprintf(out,
        "Deskwork %s - tiny solar dynamic wallpaper daemon\n\n"
        "Usage:\n"
        "  deskwork\n"
        "  deskwork --status\n"
        "  deskwork --reset\n"
        "  deskwork --location LAT LON\n"
        "  deskwork --set-location LAT LON\n"
        "  deskwork --phase N|NAME\n"
        "  deskwork -h | --help\n\n"
        "Options:\n"
        "  --status               Show saved location, today's solar phases,\n"
        "                         current phase and next change.\n"
        "  --reset                Tell a running Deskwork to immediately reload\n"
        "                         location/time/config and recalculate everything.\n"
        "                         If no daemon is running, performs one recalculation.\n"
        "  --location LAT LON     Use coordinates for this process only.\n"
        "                         Does not modify the saved location.\n"
        "  --set-location LAT LON Save coordinates as the current location.\n"
        "  --phase N|NAME         Force one wallpaper immediately for debugging.\n"
        "  -h, --help             Show this help.\n\n"
        "Phases:\n"
        "  1  night\n"
        "  2  dawn\n"
        "  3  civil-dawn\n"
        "  4  morning\n"
        "  5  day\n"
        "  6  evening\n"
        "  7  sunset\n"
        "  8  dusk\n\n"
        "Config file:\n"
        "  ~/.config/deskwork/config.conf\n\n"
        "Default wallpaper directory:\n"
        "  ~/.local/share/deskwork/wallpapers/default\n\n"
        "Accepted image names include:\n"
        "  1.jpg, 01-night.jpg, night.jpg (also .png, .jpeg, .webp)\n",
        DESKWORK_VERSION);
}

static void trim(char *s) {
    char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') ||
                   (s[0] == '\'' && s[n - 1] == '\''))) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) return -1;
    size_t len = strlen(tmp);
    if (!len) return 0;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
    return 0;
}

static int ensure_parent(const char *path) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) return -1;
    char *slash = strrchr(tmp, '/');
    if (!slash) return 0;
    *slash = '\0';
    return mkdir_p(tmp);
}

static void defaults(Config *cfg) {
    const char *home = getenv("HOME");
    if (!home) home = ".";

    snprintf(cfg->wallpaper_dir, sizeof(cfg->wallpaper_dir),
             "%s/.local/share/deskwork/wallpapers/default", home);
    snprintf(cfg->cache_file, sizeof(cfg->cache_file),
             "%s/.cache/deskwork/location.cache", home);

    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime)
        snprintf(cfg->pid_file, sizeof(cfg->pid_file), "%s/deskwork.pid", runtime);
    else
        snprintf(cfg->pid_file, sizeof(cfg->pid_file), "/tmp/deskwork-%ld.pid", (long)getuid());
}

static void load_config(Config *cfg) {
    defaults(cfg);

    const char *home = getenv("HOME");
    if (!home) return;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.config/deskwork/config.conf", home);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[PATH_MAX + 128];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = '\0';
        trim(line);
        trim(eq);

        if (!strcmp(line, "wallpaper_dir")) {
            snprintf(cfg->wallpaper_dir, sizeof(cfg->wallpaper_dir), "%s", eq);
        } else if (!strcmp(line, "cache_file")) {
            snprintf(cfg->cache_file, sizeof(cfg->cache_file), "%s", eq);
        }
    }
    fclose(f);
}

static bool parse_coord(const char *s, double min, double max, double *out) {
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || end == s || *end || !isfinite(v) || v < min || v > max) return false;
    *out = v;
    return true;
}

static bool auto_location(Location *loc) {
    /* Online IP geolocation; caller falls back to the saved cache on failure. */
    FILE *p = popen("curl -fsS --max-time 4 https://ipapi.co/latlong/ 2>/dev/null", "r");
    if (!p) return false;
    char buf[128] = {0};
    bool ok = false;
    if (fgets(buf, sizeof(buf), p)) {
        double lat, lon;
        if (sscanf(buf, "%lf,%lf", &lat, &lon) == 2 &&
            lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0) {
            loc->lat = lat; loc->lon = lon; loc->valid = true; ok = true;
        }
    }
    pclose(p);
    return ok;
}

static Location load_location(const Config *cfg) {
    Location loc = {0};
    FILE *f = fopen(cfg->cache_file, "r");
    if (!f) return loc;
    if (fscanf(f, "%lf %lf", &loc.lat, &loc.lon) == 2 &&
        loc.lat >= -90 && loc.lat <= 90 && loc.lon >= -180 && loc.lon <= 180)
        loc.valid = true;
    fclose(f);
    return loc;
}

static bool save_location(const Config *cfg, Location loc) {
    if (ensure_parent(cfg->cache_file)) return false;
    FILE *f = fopen(cfg->cache_file, "w");
    if (!f) return false;
    fprintf(f, "%.8f %.8f\n", loc.lat, loc.lon);
    fclose(f);
    return true;
}

static int day_of_year(const struct tm *t) {
    return t->tm_yday + 1;
}

/* NOAA-style solar declination / equation-of-time approximation. */
static void solar_terms(int doy, double hour, double *eqtime_min, double *decl_rad) {
    double gamma = 2.0 * M_PI / 365.0 * (doy - 1 + (hour - 12.0) / 24.0);

    *eqtime_min = 229.18 * (
        0.000075 +
        0.001868 * cos(gamma) -
        0.032077 * sin(gamma) -
        0.014615 * cos(2 * gamma) -
        0.040849 * sin(2 * gamma));

    *decl_rad =
        0.006918 -
        0.399912 * cos(gamma) +
        0.070257 * sin(gamma) -
        0.006758 * cos(2 * gamma) +
        0.000907 * sin(2 * gamma) -
        0.002697 * cos(3 * gamma) +
        0.001480 * sin(3 * gamma);
}

static time_t local_midnight(time_t now, struct tm *out_tm) {
    struct tm t;
    localtime_r(&now, &t);
    t.tm_hour = 0;
    t.tm_min = 0;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    time_t midnight = mktime(&t);
    if (out_tm) localtime_r(&midnight, out_tm);
    return midnight;
}

/*
 * Returns the local epoch time when the Sun crosses altitude_deg.
 * rising=true chooses the morning crossing, false the evening crossing.
 */
static bool solar_crossing(time_t midnight, double lat_deg, double lon_deg,
                           double altitude_deg, bool rising, time_t *result) {
    struct tm date_tm;
    localtime_r(&midnight, &date_tm);
    int doy = day_of_year(&date_tm);

    /* Evaluate terms near local noon; sufficient for wallpaper scheduling. */
    double eqtime, decl;
    solar_terms(doy, 12.0, &eqtime, &decl);

    double lat = DEG2RAD(lat_deg);
    double alt = DEG2RAD(altitude_deg);

    double denom = cos(lat) * cos(decl);
    if (fabs(denom) < 1e-12) return false;

    double cosH = (sin(alt) - sin(lat) * sin(decl)) / denom;
    if (cosH < -1.0 || cosH > 1.0) return false;

    double Hdeg = RAD2DEG(acos(cosH));

    /* Solar noon in UTC minutes from UTC midnight. */
    double solar_noon_utc_min = 720.0 - 4.0 * lon_deg - eqtime;
    double crossing_utc_min = solar_noon_utc_min + (rising ? -4.0 * Hdeg : 4.0 * Hdeg);

    /*
     * Convert via the timezone offset applicable to local noon that day.
     * tm_gmtoff is not POSIX, so derive offset using localtime/gmtime+mktime.
     */
    time_t local_noon = midnight + 12 * 3600;
    struct tm lt, gt;
    localtime_r(&local_noon, &lt);
    gmtime_r(&local_noon, &gt);
    lt.tm_isdst = -1;
    gt.tm_isdst = -1;
    time_t l_epoch = mktime(&lt);
    time_t g_as_local = mktime(&gt);
    double tz_offset_sec = difftime(l_epoch, g_as_local);

    double local_min = crossing_utc_min + tz_offset_sec / 60.0;
    *result = midnight + (time_t)llround(local_min * 60.0);
    return true;
}

static void calculate_schedule(Location loc, time_t now, PhaseTime out[PHASE_COUNT]) {
    struct tm dummy;
    time_t midnight = local_midnight(now, &dummy);

    for (int i = 0; i < PHASE_COUNT; ++i) {
        out[i].valid = false;
        out[i].when = 0;
        if (phases[i].direction == 0) continue;
        time_t t;
        if (solar_crossing(midnight, loc.lat, loc.lon,
                           phases[i].altitude_deg,
                           phases[i].direction > 0, &t)) {
            out[i].valid = true;
            out[i].when = t;
        }
    }
}

static int current_phase(time_t now, const PhaseTime schedule[PHASE_COUNT]) {
    int best = 0; /* night if nothing else fits */
    time_t best_time = 0;

    for (int i = 0; i < PHASE_COUNT; ++i) {
        if (schedule[i].valid && schedule[i].when <= now &&
            schedule[i].when >= best_time) {
            best = i;
            best_time = schedule[i].when;
        }
    }

    /* Before dawn, phase 1/night. After night event, also phase 1. */
    return best;
}

static bool next_change(time_t now, Location loc, int *phase_index, time_t *when) {
    PhaseTime today[PHASE_COUNT];
    calculate_schedule(loc, now, today);

    bool found = false;
    time_t best = 0;
    int idx = -1;
    for (int i = 0; i < PHASE_COUNT; ++i) {
        if (today[i].valid && today[i].when > now &&
            (!found || today[i].when < best)) {
            found = true;
            best = today[i].when;
            idx = i;
        }
    }

    if (!found) {
        time_t tomorrow = local_midnight(now, NULL) + 36 * 3600;
        PhaseTime nextday[PHASE_COUNT];
        calculate_schedule(loc, tomorrow, nextday);
        for (int i = 0; i < PHASE_COUNT; ++i) {
            if (nextday[i].valid && nextday[i].when > now &&
                (!found || nextday[i].when < best)) {
                found = true;
                best = nextday[i].when;
                idx = i;
            }
        }
    }

    if (found) {
        *phase_index = idx;
        *when = best;
    }
    return found;
}

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static bool find_wallpaper(const Config *cfg, int idx, char out[PATH_MAX]) {
    static const char *exts[] = {"jpg", "jpeg", "png", "webp"};
    const char *name = phases[idx].name;

    for (size_t e = 0; e < sizeof(exts)/sizeof(exts[0]); ++e) {
        const char *patterns[] = {
            "%s/%d.%s",
            "%s/%02d-%s.%s",
            "%s/%d-%s.%s",
            "%s/%s.%s",
        };

        char p[PATH_MAX];
        snprintf(p, sizeof(p), patterns[0], cfg->wallpaper_dir, idx + 1, exts[e]);
        if (file_exists(p)) { snprintf(out, PATH_MAX, "%s", p); return true; }

        snprintf(p, sizeof(p), patterns[1], cfg->wallpaper_dir, idx + 1, name, exts[e]);
        if (file_exists(p)) { snprintf(out, PATH_MAX, "%s", p); return true; }

        snprintf(p, sizeof(p), patterns[2], cfg->wallpaper_dir, idx + 1, name, exts[e]);
        if (file_exists(p)) { snprintf(out, PATH_MAX, "%s", p); return true; }

        snprintf(p, sizeof(p), patterns[3], cfg->wallpaper_dir, name, exts[e]);
        if (file_exists(p)) { snprintf(out, PATH_MAX, "%s", p); return true; }
    }
    return false;
}

static bool shell_safe_path(const char *p) {
    /* We pass it inside a single-quoted shell argument and JS string. */
    for (; *p; ++p) {
        if (*p == '\'' || *p == '\n' || *p == '\r' || *p == '\\' || *p == '"')
            return false;
    }
    return true;
}

static bool set_wallpaper(const Config *cfg, int idx) {
    char image[PATH_MAX];
    if (!find_wallpaper(cfg, idx, image)) {
        fprintf(stderr, "deskwork: wallpaper for phase %d (%s) not found in %s\n",
                idx + 1, phases[idx].name, cfg->wallpaper_dir);
        return false;
    }

    char real[PATH_MAX];
    if (!realpath(image, real)) {
        perror("deskwork: realpath");
        return false;
    }
    if (!shell_safe_path(real)) {
        fprintf(stderr, "deskwork: unsupported characters in wallpaper path\n");
        return false;
    }

    const char *qdbus = NULL;
    if (system("command -v qdbus6 >/dev/null 2>&1") == 0) qdbus = "qdbus6";
    else if (system("command -v qdbus >/dev/null 2>&1") == 0) qdbus = "qdbus";
    else {
        fprintf(stderr, "deskwork: qdbus6/qdbus not found\n");
        return false;
    }

    char cmd[PATH_MAX * 2 + 1024];
    int n = snprintf(cmd, sizeof(cmd),
        "%s org.kde.plasmashell /PlasmaShell org.kde.PlasmaShell.evaluateScript "
        "'const ds=desktopsForActivity(currentActivity());"
        "for(let i=0;i<ds.length;i++){"
        "let d=ds[i];"
        "d.wallpaperPlugin=\"org.kde.image\";"
        "d.currentConfigGroup=[\"Wallpaper\",\"org.kde.image\",\"General\"];"
        "d.writeConfig(\"Image\",\"file://%s\");"
        "}' >/dev/null",
        qdbus, real);

    if (n < 0 || n >= (int)sizeof(cmd)) {
        fprintf(stderr, "deskwork: wallpaper command too long\n");
        return false;
    }

    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "deskwork: KDE wallpaper command failed\n");
        return false;
    }
    return true;
}

static int phase_from_arg(const char *s) {
    char *end = NULL;
    long n = strtol(s, &end, 10);
    if (end != s && *end == '\0' && n >= 1 && n <= PHASE_COUNT)
        return (int)n - 1;

    for (int i = 0; i < PHASE_COUNT; ++i)
        if (!strcmp(s, phases[i].name)) return i;
    return -1;
}

static void print_time(time_t t) {
    struct tm tm;
    char buf[32];
    localtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%H:%M", &tm);
    printf("%s", buf);
}

static void status(Location loc) {
    time_t now = time(NULL);
    PhaseTime schedule[PHASE_COUNT];
    calculate_schedule(loc, now, schedule);
    int cur = current_phase(now, schedule);

    struct tm tm;
    char date[32];
    localtime_r(&now, &tm);
    strftime(date, sizeof(date), "%Y-%m-%d", &tm);

    printf("Deskwork %s\n", DESKWORK_VERSION);
    printf("Location: %.6f, %.6f\n", loc.lat, loc.lon);
    printf("Date: %s\n\n", date);

    for (int i = 0; i < PHASE_COUNT; ++i) {
        printf("%02d %-12s ", i + 1, phases[i].name);
        if (schedule[i].valid) print_time(schedule[i].when);
        else printf("--:--");
        putchar('\n');
    }

    printf("\nCurrent: %02d %s\n", cur + 1, phases[cur].name);

    int next_idx;
    time_t next_t;
    if (next_change(now, loc, &next_idx, &next_t)) {
        printf("Next:    %02d %s - ", next_idx + 1, phases[next_idx].name);
        print_time(next_t);
        putchar('\n');
    } else {
        printf("Next:    unavailable at these coordinates/date\n");
    }
}

static bool write_pid(const Config *cfg) {
    if (ensure_parent(cfg->pid_file)) return false;
    FILE *f = fopen(cfg->pid_file, "w");
    if (!f) return false;
    fprintf(f, "%ld\n", (long)getpid());
    fclose(f);
    return true;
}

static pid_t read_pid(const Config *cfg) {
    FILE *f = fopen(cfg->pid_file, "r");
    if (!f) return -1;
    long p = -1;
    if (fscanf(f, "%ld", &p) != 1) p = -1;
    fclose(f);
    return (pid_t)p;
}

static int daemon_loop(Config *cfg, Location loc) {
    signal(SIGHUP, on_hup);
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);

    if (!write_pid(cfg))
        fprintf(stderr, "deskwork: warning: could not write pid file\n");

    int last_phase = -1;

    while (!g_stop_requested) {
        if (g_reset_requested) {
            g_reset_requested = 0;
            load_config(cfg);
            Location fresh = {0};
            if (auto_location(&fresh)) {
                loc = fresh;
                save_location(cfg, loc);
            } else {
                Location cached = load_location(cfg);
                if (cached.valid) loc = cached;
            }
            last_phase = -1;
        }

        time_t now = time(NULL);
        PhaseTime schedule[PHASE_COUNT];
        calculate_schedule(loc, now, schedule);
        int cur = current_phase(now, schedule);

        if (cur != last_phase) {
            if (set_wallpaper(cfg, cur))
                last_phase = cur;
        }

        int next_idx;
        time_t next_t;
        unsigned int seconds = 3600;
        if (next_change(now, loc, &next_idx, &next_t)) {
            double d = difftime(next_t, now);
            if (d < 1) d = 1;
            if (d > 60) d = 60; /* robust after suspend / clock changes */
            seconds = (unsigned int)ceil(d);
        }

        while (seconds > 0 && !g_reset_requested && !g_stop_requested)
            seconds = sleep(seconds);
    }

    unlink(cfg->pid_file);
    return 0;
}

int main(int argc, char **argv) {
    Config cfg;
    load_config(&cfg);

    if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        usage(stdout);
        return 0;
    }

    if (argc == 3 && !strcmp(argv[1], "--phase")) {
        int p = phase_from_arg(argv[2]);
        if (p < 0) {
            fprintf(stderr, "deskwork: invalid phase '%s'\n", argv[2]);
            return 2;
        }
        return set_wallpaper(&cfg, p) ? 0 : 1;
    }

    if (argc == 4 && (!strcmp(argv[1], "--location") ||
                      !strcmp(argv[1], "--set-location"))) {
        Location loc = {0};
        if (!parse_coord(argv[2], -90, 90, &loc.lat) ||
            !parse_coord(argv[3], -180, 180, &loc.lon)) {
            fprintf(stderr, "deskwork: invalid coordinates\n");
            return 2;
        }
        loc.valid = true;

        if (!strcmp(argv[1], "--set-location")) {
            if (!save_location(&cfg, loc)) {
                fprintf(stderr, "deskwork: could not save location cache\n");
                return 1;
            }
            printf("Saved location: %.6f, %.6f\n", loc.lat, loc.lon);
            return 0;
        }

        /* --location: run the normal daemon with temporary coordinates. */
        return daemon_loop(&cfg, loc);
    }

    if (argc == 2 && !strcmp(argv[1], "--reset")) {
        pid_t pid = read_pid(&cfg);
        if (pid > 1 && kill(pid, SIGHUP) == 0) {
            printf("Deskwork reset requested.\n");
            return 0;
        }

        Location loc = load_location(&cfg);
        if (!loc.valid) {
            fprintf(stderr,
                "deskwork: no running daemon and no saved location.\n"
                "Use: deskwork --set-location LAT LON\n");
            return 1;
        }

        PhaseTime schedule[PHASE_COUNT];
        time_t now = time(NULL);
        calculate_schedule(loc, now, schedule);
        int cur = current_phase(now, schedule);
        return set_wallpaper(&cfg, cur) ? 0 : 1;
    }

    if (argc == 2 && !strcmp(argv[1], "--status")) {
        Location loc = load_location(&cfg);
        if (!loc.valid) {
            fprintf(stderr,
                "deskwork: no saved location.\n"
                "Use: deskwork --set-location LAT LON\n");
            return 1;
        }
        status(loc);
        return 0;
    }

    if (argc != 1) {
        usage(stderr);
        return 2;
    }

    Location loc = {0};
    if (auto_location(&loc)) {
        save_location(&cfg, loc);
    } else {
        loc = load_location(&cfg);
    }
    if (!loc.valid) {
        fprintf(stderr,
            "deskwork: automatic location failed and no saved location exists.\n"
            "Set it once with:\n"
            "  deskwork --set-location LAT LON\n");
        return 1;
    }

    return daemon_loop(&cfg, loc);
}
