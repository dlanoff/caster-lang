// Caster system adapter.
//
// This file is included into generated C only when OS/FS/PATH/IO/PROC native
// namespaces are used. It depends on the generated FLX_STR, array helpers, and
// fixed MAP structs for FileStat and ProcResult.

#include <errno.h>
#include <limits.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static int flx_sys_argc = 0;
static char **flx_sys_argv = NULL;

// Captures process arguments once, which makes OS.args available anywhere because C main receives
// argc/argv only at startup.
static FLX_UNUSED void flx_sys_init(int argc, char **argv) {
    flx_sys_argc = argc;
    flx_sys_argv = argv;
}

// Copies FLX_STR into a null-terminated C string, which is needed because OS, sockets, H2O, and
// sqlite expect C APIs.
static FLX_UNUSED char *flx_sys_cstr(FLX_STR text) {
    char *out = malloc((size_t)text.len + 1);
    if (!out) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
    for (int64_t i = 0; i < text.len; i++) out[i] = text.data[i];
    out[text.len] = 0;
    return out;
}

// Bridges C strings into FLX_STR, which keeps adapter code isolated because generated C uses
// Caster string values.
static FLX_UNUSED FLX_STR flx_sys_str_from_c(const char *text) {
    if (!text) text = "";
    int64_t len = (int64_t)strlen(text);
    FLX_STR out = flx_str_alloc(len);
    for (int64_t i = 0; i < len; i++) out.data[i] = text[i];
    return out;
}

// Bridges C strings into FLX_STR, which keeps adapter code isolated because generated C uses
// Caster string values.
static FLX_UNUSED FLX_STR flx_sys_str_from_bytes(const char *data, int64_t len) {
    FLX_STR out = flx_str_alloc(len);
    for (int64_t i = 0; i < len; i++) out.data[i] = data[i];
    return out;
}

// Reports OS errors with context, which keeps adapter failures readable because C errno alone
// loses the operation and path.
static FLX_UNUSED void flx_sys_fail_errno(const char *op, const char *path) {
    fprintf(stderr, "caster: %s failed", op);
    if (path && *path) fprintf(stderr, " for %s", path);
    fprintf(stderr, ": %s\n", strerror(errno));
    exit(1);
}

// Exposes OS.args, which wraps process state as typed Caster values because compiler-like programs
// need portable OS access.
static FLX_UNUSED FLX_ARR_STR OS_args(void) {
    FLX_ARR_STR out = flx_arr_str_make(NULL, 0);
    for (int i = 0; i < flx_sys_argc; i++) {
        out = flx_arr_str_add(out, flx_sys_str_from_c(flx_sys_argv[i]));
    }
    return out;
}

// Exposes OS.exit, which wraps process state as typed Caster values because compiler-like programs
// need portable OS access.
static FLX_UNUSED void OS_exit(int64_t code) {
    exit((int)code);
}

// Exposes OS.cwd, which wraps process state as typed Caster values because compiler-like programs
// need portable OS access.
static FLX_UNUSED FLX_STR OS_cwd(void) {
#if defined(_WIN32)
    char *cwd = _getcwd(NULL, 0);
#else
    char *cwd = getcwd(NULL, 0);
#endif
    if (!cwd) flx_sys_fail_errno("cwd", "");
    FLX_STR out = flx_sys_str_from_c(cwd);
    free(cwd);
    return out;
}

// Exposes OS.chdir, which wraps process state as typed Caster values because compiler-like
// programs need portable OS access.
static FLX_UNUSED void OS_chdir(FLX_STR path) {
    char *raw = flx_sys_cstr(path);
#if defined(_WIN32)
    int rc = _chdir(raw);
#else
    int rc = chdir(raw);
#endif
    if (rc != 0) flx_sys_fail_errno("chdir", raw);
    free(raw);
}

// Exposes OS.env, which wraps process state as typed Caster values because compiler-like programs
// need portable OS access.
static FLX_UNUSED FLX_STR OS_env(FLX_STR name) {
    char *raw = flx_sys_cstr(name);
    char *value = getenv(raw);
    FLX_STR out = flx_sys_str_from_c(value ? value : "");
    free(raw);
    return out;
}

// Exposes OS.setEnv, which wraps process state as typed Caster values because compiler-like
// programs need portable OS access.
static FLX_UNUSED void OS_setEnv(FLX_STR name, FLX_STR value) {
    char *raw_name = flx_sys_cstr(name);
    char *raw_value = flx_sys_cstr(value);
#if defined(_WIN32)
    int rc = _putenv_s(raw_name, raw_value);
#else
    int rc = setenv(raw_name, raw_value, 1);
#endif
    if (rc != 0) flx_sys_fail_errno("setEnv", raw_name);
    free(raw_name);
    free(raw_value);
}

// Exposes OS.platform, which wraps process state as typed Caster values because compiler-like
// programs need portable OS access.
static FLX_UNUSED FLX_STR OS_platform(void) {
#if defined(_WIN32)
    return flx_sys_str_from_c("windows");
#elif defined(__APPLE__)
    return flx_sys_str_from_c("macos");
#elif defined(__linux__)
    return flx_sys_str_from_c("linux");
#elif defined(__unix__)
    return flx_sys_str_from_c("unix");
#else
    return flx_sys_str_from_c("unknown");
#endif
}

// Exposes OS.arch, which wraps process state as typed Caster values because compiler-like programs
// need portable OS access.
static FLX_UNUSED FLX_STR OS_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return flx_sys_str_from_c("x86_64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    return flx_sys_str_from_c("arm64");
#elif defined(__arm__) || defined(_M_ARM)
    return flx_sys_str_from_c("arm");
#elif defined(__i386__) || defined(_M_IX86)
    return flx_sys_str_from_c("x86");
#else
    return flx_sys_str_from_c("unknown");
#endif
}

// Exposes OS.isWindows, which wraps process state as typed Caster values because compiler-like
// programs need portable OS access.
static FLX_UNUSED bool OS_isWindows(void) {
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

// Exposes OS.isMac, which wraps process state as typed Caster values because compiler-like
// programs need portable OS access.
static FLX_UNUSED bool OS_isMac(void) {
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

// Exposes OS.isLinux, which wraps process state as typed Caster values because compiler-like
// programs need portable OS access.
static FLX_UNUSED bool OS_isLinux(void) {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

// Exposes FS.exists, which wraps filesystem calls as typed Caster values because self-hosted
// tooling needs file IO without hand-written C.
static FLX_UNUSED bool FS_exists(FLX_STR path) {
    char *raw = flx_sys_cstr(path);
#if defined(_WIN32)
    bool exists = _access(raw, 0) == 0;
#else
    bool exists = access(raw, F_OK) == 0;
#endif
    free(raw);
    return exists;
}

// Exposes FS.read, which wraps filesystem calls as typed Caster values because self-hosted tooling
// needs file IO without hand-written C.
static FLX_UNUSED FLX_STR FS_read(FLX_STR path) {
    char *raw = flx_sys_cstr(path);
    FILE *file = fopen(raw, "rb");
    if (!file) flx_sys_fail_errno("read", raw);
    if (fseek(file, 0, SEEK_END) != 0) flx_sys_fail_errno("read", raw);
    long size = ftell(file);
    if (size < 0) flx_sys_fail_errno("read", raw);
    if (fseek(file, 0, SEEK_SET) != 0) flx_sys_fail_errno("read", raw);
    FLX_STR out = flx_str_alloc((int64_t)size);
    if (size > 0 && fread(out.data, 1, (size_t)size, file) != (size_t)size) flx_sys_fail_errno("read", raw);
    fclose(file);
    free(raw);
    return out;
}

// Exposes FS.readBytes, which wraps filesystem calls as typed Caster values because self-hosted
// tooling needs file IO without hand-written C.
static FLX_UNUSED FLX_ARR_INT FS_readBytes(FLX_STR path) {
    FLX_STR text = FS_read(path);
    FLX_ARR_INT out = flx_arr_int_make(NULL, 0);
    for (int64_t i = 0; i < text.len; i++) {
        out = flx_arr_int_add(out, (int64_t)(unsigned char)text.data[i]);
    }
    flx_drop_str(text);
    return out;
}

// Exposes FS.write, which wraps filesystem calls as typed Caster values because self-hosted
// tooling needs file IO without hand-written C.
static FLX_UNUSED void FS_write(FLX_STR path, FLX_STR data) {
    char *raw = flx_sys_cstr(path);
    FILE *file = fopen(raw, "wb");
    if (!file) flx_sys_fail_errno("write", raw);
    if (data.len > 0 && fwrite(data.data, 1, (size_t)data.len, file) != (size_t)data.len) flx_sys_fail_errno("write", raw);
    fclose(file);
    free(raw);
}

// Exposes FS.append, which wraps filesystem calls as typed Caster values because self-hosted
// tooling needs file IO without hand-written C.
static FLX_UNUSED void FS_append(FLX_STR path, FLX_STR data) {
    char *raw = flx_sys_cstr(path);
    FILE *file = fopen(raw, "ab");
    if (!file) flx_sys_fail_errno("append", raw);
    if (data.len > 0 && fwrite(data.data, 1, (size_t)data.len, file) != (size_t)data.len) flx_sys_fail_errno("append", raw);
    fclose(file);
    free(raw);
}

// Exposes FS.remove, which wraps filesystem calls as typed Caster values because self-hosted
// tooling needs file IO without hand-written C.
static FLX_UNUSED void FS_remove(FLX_STR path) {
    char *raw = flx_sys_cstr(path);
    if (remove(raw) != 0) flx_sys_fail_errno("remove", raw);
    free(raw);
}

// Exposes FS.rename, which wraps filesystem calls as typed Caster values because self-hosted
// tooling needs file IO without hand-written C.
static FLX_UNUSED void FS_rename(FLX_STR from, FLX_STR to) {
    char *raw_from = flx_sys_cstr(from);
    char *raw_to = flx_sys_cstr(to);
    if (rename(raw_from, raw_to) != 0) flx_sys_fail_errno("rename", raw_from);
    free(raw_from);
    free(raw_to);
}

// Exposes FS.copy, which wraps filesystem calls as typed Caster values because self-hosted tooling
// needs file IO without hand-written C.
static FLX_UNUSED void FS_copy(FLX_STR from, FLX_STR to) {
    char *raw_from = flx_sys_cstr(from);
    char *raw_to = flx_sys_cstr(to);
    FILE *src = fopen(raw_from, "rb");
    if (!src) flx_sys_fail_errno("copy", raw_from);
    FILE *dst = fopen(raw_to, "wb");
    if (!dst) flx_sys_fail_errno("copy", raw_to);
    char buffer[16384];
    size_t n = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, n, dst) != n) flx_sys_fail_errno("copy", raw_to);
    }
    if (ferror(src)) flx_sys_fail_errno("copy", raw_from);
    fclose(src);
    fclose(dst);
    free(raw_from);
    free(raw_to);
}

// Exposes FS.mkdir, which wraps filesystem calls as typed Caster values because self-hosted
// tooling needs file IO without hand-written C.
static FLX_UNUSED void FS_mkdir(FLX_STR path) {
    char *raw = flx_sys_cstr(path);
#if defined(_WIN32)
    int rc = _mkdir(raw);
#else
    int rc = mkdir(raw, 0777);
#endif
    if (rc != 0 && errno != EEXIST) flx_sys_fail_errno("mkdir", raw);
    free(raw);
}

// Exposes FS.list, which wraps filesystem calls as typed Caster values because self-hosted tooling
// needs file IO without hand-written C.
static FLX_UNUSED FLX_ARR_STR FS_list(FLX_STR path) {
    char *raw = flx_sys_cstr(path);
    FLX_ARR_STR out = flx_arr_str_make(NULL, 0);
#if defined(_WIN32)
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", raw);
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) flx_sys_fail_errno("list", raw);
    do {
        if (strcmp(data.cFileName, ".") != 0 && strcmp(data.cFileName, "..") != 0) {
            out = flx_arr_str_add(out, flx_sys_str_from_c(data.cFileName));
        }
    } while (FindNextFileA(h, &data));
    FindClose(h);
#else
    DIR *dir = opendir(raw);
    if (!dir) flx_sys_fail_errno("list", raw);
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            out = flx_arr_str_add(out, flx_sys_str_from_c(entry->d_name));
        }
    }
    closedir(dir);
#endif
    free(raw);
    return out;
}

// Exposes FS.stat, which wraps filesystem calls as typed Caster values because self-hosted tooling
// needs file IO without hand-written C.
static FLX_UNUSED FLX_FileStat FS_stat(FLX_STR path) {
    char *raw = flx_sys_cstr(path);
    FLX_FileStat out = flx_value_FileStat();
    out.path = flx_str_copy(path);
#if defined(_WIN32)
    struct _stat st;
    if (_stat(raw, &st) == 0) {
        out.exists = true;
        out.isFile = (st.st_mode & _S_IFREG) != 0;
        out.isDir = (st.st_mode & _S_IFDIR) != 0;
        out.size = (int64_t)st.st_size;
    }
#else
    struct stat st;
    if (stat(raw, &st) == 0) {
        out.exists = true;
        out.isFile = S_ISREG(st.st_mode);
        out.isDir = S_ISDIR(st.st_mode);
        out.size = (int64_t)st.st_size;
    }
#endif
    free(raw);
    return out;
}

// Checks path separators, which keeps normalization portable because Windows and POSIX separators
// can both appear in input.
static FLX_UNUSED bool flx_path_is_sep(char ch) {
    return ch == '/' || ch == '\\';
}

// Checks absolute paths, which lets PATH.absolute avoid double-prefixing because callers may
// already pass rooted paths.
static FLX_UNUSED bool flx_path_is_absolute_c(const char *path) {
    if (!path || !*path) return false;
    if (flx_path_is_sep(path[0])) return true;
    return isalpha((unsigned char)path[0]) && path[1] == ':';
}

// Exposes PATH.normalize, which centralizes path rules because compiler code should not manually
// splice platform separators.
static FLX_UNUSED FLX_STR PATH_normalize(FLX_STR path) {
    char *raw = flx_sys_cstr(path);
    bool absolute = flx_path_is_absolute_c(raw);
    char drive[3] = {0, 0, 0};
    int start = 0;
    if (isalpha((unsigned char)raw[0]) && raw[1] == ':') {
        drive[0] = raw[0];
        drive[1] = ':';
        start = 2;
        if (flx_path_is_sep(raw[start])) {
            absolute = true;
            start++;
        }
    } else {
        while (flx_path_is_sep(raw[start])) start++;
    }

    char **parts = NULL;
    int len = 0;
    int cap = 0;
    for (int i = start; raw[i]; ) {
        while (flx_path_is_sep(raw[i])) i++;
        int part_start = i;
        while (raw[i] && !flx_path_is_sep(raw[i])) i++;
        int part_len = i - part_start;
        if (part_len == 0) break;
        char *part = malloc((size_t)part_len + 1);
        if (!part) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
        memcpy(part, raw + part_start, (size_t)part_len);
        part[part_len] = 0;
        if (strcmp(part, ".") == 0) {
            free(part);
            continue;
        }
        if (strcmp(part, "..") == 0 && len > 0 && strcmp(parts[len - 1], "..") != 0) {
            free(parts[--len]);
            free(part);
            continue;
        }
        if (len == cap) {
            cap = cap ? cap * 2 : 8;
            parts = realloc(parts, sizeof(char *) * (size_t)cap);
            if (!parts) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
        }
        parts[len++] = part;
    }

    int64_t out_len = (int64_t)strlen(drive) + (absolute ? 1 : 0);
    for (int i = 0; i < len; i++) out_len += (int64_t)strlen(parts[i]) + (i > 0 ? 1 : 0);
    if (out_len == 0) out_len = 1;
    FLX_STR out = flx_str_alloc(out_len);
    int64_t pos = 0;
    if (drive[0]) {
        out.data[pos++] = drive[0];
        out.data[pos++] = ':';
    }
    if (absolute) out.data[pos++] = '/';
    for (int i = 0; i < len; i++) {
        if (i > 0) out.data[pos++] = '/';
        int64_t n = (int64_t)strlen(parts[i]);
        memcpy(out.data + pos, parts[i], (size_t)n);
        pos += n;
    }
    if (pos == 0) out.data[pos++] = '.';
    out.len = pos;
    out.data[pos] = 0;
    for (int i = 0; i < len; i++) free(parts[i]);
    free(parts);
    free(raw);
    return out;
}

// Exposes PATH.join, which centralizes path rules because compiler code should not manually splice
// platform separators.
static FLX_UNUSED FLX_STR PATH_join(FLX_ARR_STR parts) {
    FLX_STR out = flx_str_lit("", 0);
    for (int64_t i = 0; i < parts.len; i++) {
        if (parts.data[i].len == 0) continue;
        if (out.len == 0) out = flx_str_copy(parts.data[i]);
        else {
            FLX_STR sep = flx_str_lit("/", 1);
            out = flx_str_concat(flx_str_concat(out, sep), parts.data[i]);
        }
    }
    return PATH_normalize(out);
}

// Exposes PATH.parent, which centralizes path rules because compiler code should not manually
// splice platform separators.
static FLX_UNUSED FLX_STR PATH_parent(FLX_STR path) {
    FLX_STR norm = PATH_normalize(path);
    int64_t end = norm.len;
    while (end > 1 && flx_path_is_sep(norm.data[end - 1])) end--;
    int64_t slash = -1;
    for (int64_t i = end - 1; i >= 0; i--) {
        if (flx_path_is_sep(norm.data[i])) { slash = i; break; }
    }
    if (slash < 0) return flx_sys_str_from_c(".");
    if (slash == 0) return flx_sys_str_from_c("/");
    return flx_sys_str_from_bytes(norm.data, slash);
}

// Exposes PATH.name, which centralizes path rules because compiler code should not manually splice
// platform separators.
static FLX_UNUSED FLX_STR PATH_name(FLX_STR path) {
    FLX_STR norm = PATH_normalize(path);
    int64_t end = norm.len;
    while (end > 1 && flx_path_is_sep(norm.data[end - 1])) end--;
    int64_t start = 0;
    for (int64_t i = end - 1; i >= 0; i--) {
        if (flx_path_is_sep(norm.data[i])) { start = i + 1; break; }
    }
    return flx_sys_str_from_bytes(norm.data + start, end - start);
}

// Exposes PATH.ext, which centralizes path rules because compiler code should not manually splice
// platform separators.
static FLX_UNUSED FLX_STR PATH_ext(FLX_STR path) {
    FLX_STR name = PATH_name(path);
    int64_t dot = -1;
    for (int64_t i = name.len - 1; i >= 0; i--) {
        if (name.data[i] == '.') { dot = i; break; }
    }
    if (dot <= 0) return flx_sys_str_from_c("");
    return flx_sys_str_from_bytes(name.data + dot, name.len - dot);
}

// Exposes PATH.absolute, which centralizes path rules because compiler code should not manually
// splice platform separators.
static FLX_UNUSED FLX_STR PATH_absolute(FLX_STR path) {
    char *raw = flx_sys_cstr(path);
    bool absolute = flx_path_is_absolute_c(raw);
    free(raw);
    if (absolute) return PATH_normalize(path);
    FLX_STR cwd = OS_cwd();
    FLX_ARR_STR parts = flx_arr_str_make(NULL, 0);
    parts = flx_arr_str_add(parts, cwd);
    parts = flx_arr_str_add(parts, path);
    return PATH_join(parts);
}

// Exposes IO.print_str, which keeps terminal IO behind typed helpers because generated programs
// should avoid raw stdio plumbing.
static FLX_UNUSED void IO_print_str(FLX_STR value) {
    printf("%.*s\n", (int)value.len, value.data);
}

// Exposes IO.print_int, which keeps terminal IO behind typed helpers because generated programs
// should avoid raw stdio plumbing.
static FLX_UNUSED void IO_print_int(int64_t value) {
    printf("%lld\n", (long long)value);
}

// Exposes IO.print_flt, which keeps terminal IO behind typed helpers because generated programs
// should avoid raw stdio plumbing.
static FLX_UNUSED void IO_print_flt(double value) {
    printf("%.17g\n", value);
}

// Exposes IO.print_bol, which keeps terminal IO behind typed helpers because generated programs
// should avoid raw stdio plumbing.
static FLX_UNUSED void IO_print_bol(bool value) {
    printf("%s\n", value ? "true" : "false");
}

// Exposes IO.error_str, which keeps terminal IO behind typed helpers because generated programs
// should avoid raw stdio plumbing.
static FLX_UNUSED void IO_error_str(FLX_STR value) {
    fprintf(stderr, "%.*s\n", (int)value.len, value.data);
}

// Exposes IO.error_int, which keeps terminal IO behind typed helpers because generated programs
// should avoid raw stdio plumbing.
static FLX_UNUSED void IO_error_int(int64_t value) {
    fprintf(stderr, "%lld\n", (long long)value);
}

// Exposes IO.error_flt, which keeps terminal IO behind typed helpers because generated programs
// should avoid raw stdio plumbing.
static FLX_UNUSED void IO_error_flt(double value) {
    fprintf(stderr, "%.17g\n", value);
}

// Exposes IO.error_bol, which keeps terminal IO behind typed helpers because generated programs
// should avoid raw stdio plumbing.
static FLX_UNUSED void IO_error_bol(bool value) {
    fprintf(stderr, "%s\n", value ? "true" : "false");
}

// Exposes IO.readLine, which keeps terminal IO behind typed helpers because generated programs
// should avoid raw stdio plumbing.
static FLX_UNUSED FLX_STR IO_readLine(void) {
    int64_t len = 0;
    int64_t cap = 128;
    char *buf = malloc((size_t)cap);
    if (!buf) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
    int ch = 0;
    while ((ch = fgetc(stdin)) != EOF && ch != '\n') {
        if (len + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, (size_t)cap);
            if (!buf) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
        }
        buf[len++] = (char)ch;
    }
    FLX_STR out = flx_sys_str_from_bytes(buf, len);
    free(buf);
    return out;
}

// Exposes IO.stdin, which keeps terminal IO behind typed helpers because generated programs should
// avoid raw stdio plumbing.
static FLX_UNUSED int64_t IO_stdin(void) { return 0; }
// Exposes IO.stdout, which keeps terminal IO behind typed helpers because generated programs
// should avoid raw stdio plumbing.
static FLX_UNUSED int64_t IO_stdout(void) { return 1; }
// Exposes IO.stderr, which keeps terminal IO behind typed helpers because generated programs
// should avoid raw stdio plumbing.
static FLX_UNUSED int64_t IO_stderr(void) { return 2; }

#if !defined(_WIN32)
// Makes process pipes nonblocking, which lets PROC.run collect stdout and stderr together because
// either pipe may be idle.
static FLX_UNUSED void flx_proc_set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Drains one process pipe into a growable buffer, which avoids deadlock because child processes
// can fill stdout or stderr independently.
static FLX_UNUSED void flx_proc_append_read(int fd, char **buf, int64_t *len, int64_t *cap, bool *open) {
    char tmp[4096];
    for (;;) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n > 0) {
            if (*len + n + 1 > *cap) {
                while (*len + n + 1 > *cap) *cap *= 2;
                *buf = realloc(*buf, (size_t)*cap);
                if (!*buf) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
            }
            memcpy(*buf + *len, tmp, (size_t)n);
            *len += n;
            (*buf)[*len] = 0;
            continue;
        }
        if (n == 0) {
            *open = false;
            close(fd);
        }
        break;
    }
}
#endif

// Exposes PROC.run, which gives compiler tooling subprocess access because builds and probes need
// command execution.
static FLX_UNUSED FLX_ProcResult PROC_run(FLX_STR command, FLX_ARR_STR args) {
    FLX_ProcResult out = flx_value_ProcResult();
#if defined(_WIN32)
    FLX_STR cmd = flx_str_copy(command);
    for (int64_t i = 0; i < args.len; i++) {
        cmd = flx_str_concat(cmd, flx_str_lit(" ", 1));
        cmd = flx_str_concat(cmd, args.data[i]);
    }
    char *raw = flx_sys_cstr(cmd);
    FILE *pipe = _popen(raw, "r");
    if (!pipe) {
        out.code = -1;
        out.err = flx_sys_str_from_c("process start failed");
        free(raw);
        return out;
    }
    char buffer[4096];
    FLX_STR stdout_text = flx_str_lit("", 0);
    while (fgets(buffer, sizeof(buffer), pipe)) {
        stdout_text = flx_str_concat(stdout_text, flx_sys_str_from_c(buffer));
    }
    int code = _pclose(pipe);
    out.code = code;
    out.out = stdout_text;
    out.err = flx_sys_str_from_c("");
    free(raw);
    return out;
#else
    int stdout_pipe[2];
    int stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        out.code = -1;
        out.err = flx_sys_str_from_c("pipe failed");
        return out;
    }

    pid_t pid = fork();
    if (pid < 0) {
        out.code = -1;
        out.err = flx_sys_str_from_c("fork failed");
        return out;
    }

    if (pid == 0) {
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);

        char **argv = calloc((size_t)args.len + 2, sizeof(char *));
        if (!argv) _exit(127);
        argv[0] = flx_sys_cstr(command);
        for (int64_t i = 0; i < args.len; i++) argv[i + 1] = flx_sys_cstr(args.data[i]);
        argv[args.len + 1] = NULL;
        execvp(argv[0], argv);
        _exit(127);
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    flx_proc_set_nonblock(stdout_pipe[0]);
    flx_proc_set_nonblock(stderr_pipe[0]);

    int64_t out_len = 0, err_len = 0;
    int64_t out_cap = 4096, err_cap = 4096;
    char *out_buf = calloc(1, (size_t)out_cap);
    char *err_buf = calloc(1, (size_t)err_cap);
    if (!out_buf || !err_buf) { fprintf(stderr, "caster: allocation failed\n"); exit(1); }
    bool out_open = true;
    bool err_open = true;
    int status = 0;
    bool child_done = false;

    while (out_open || err_open || !child_done) {
        if (out_open) flx_proc_append_read(stdout_pipe[0], &out_buf, &out_len, &out_cap, &out_open);
        if (err_open) flx_proc_append_read(stderr_pipe[0], &err_buf, &err_len, &err_cap, &err_open);
        if (!child_done) {
            pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) child_done = true;
        }
        if ((out_open || err_open) && !child_done) {
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 1000;
            select(0, NULL, NULL, NULL, &tv);
        } else if (child_done) {
            if (out_open) flx_proc_append_read(stdout_pipe[0], &out_buf, &out_len, &out_cap, &out_open);
            if (err_open) flx_proc_append_read(stderr_pipe[0], &err_buf, &err_len, &err_cap, &err_open);
        }
    }

    if (WIFEXITED(status)) out.code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) out.code = 128 + WTERMSIG(status);
    else out.code = -1;
    out.out = flx_sys_str_from_bytes(out_buf, out_len);
    out.err = flx_sys_str_from_bytes(err_buf, err_len);
    free(out_buf);
    free(err_buf);
    return out;
#endif
}
