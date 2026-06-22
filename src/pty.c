#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#ifndef _WIN32
#include <sys/ioctl.h>
#include <sys/wait.h>

#if defined(__OpenBSD__) || defined(__APPLE__)
#include <util.h>
#elif defined(__FreeBSD__)
#include <libutil.h>
#else
#include <pty.h>
#endif

#if defined(__APPLE__)
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#else
extern char **environ;
#endif
#endif

#include "pty.h"
#include "utils.h"

#ifdef _WIN32
#include <io.h>

HRESULT (WINAPI *pCreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
HRESULT (WINAPI *pResizePseudoConsole)(HPCON, COORD);
void (WINAPI *pClosePseudoConsole)(HPCON);
#endif

static void alloc_cb(uv_handle_t *unused, size_t suggested_size, uv_buf_t *buf) {
  buf->base = xmalloc(suggested_size);
  buf->len = suggested_size;
}

static void close_cb(uv_handle_t *handle) { free(handle); }

static void async_free_cb(uv_handle_t *handle) {
  free((uv_async_t *) handle -> data);
}

pty_buf_t *pty_buf_init(char *base, size_t len) {
  pty_buf_t *buf = xmalloc(sizeof(pty_buf_t));
  buf->base = xmalloc(len);
  memcpy(buf->base, base, len);
  buf->len = len;
  return buf;
}

void pty_buf_free(pty_buf_t *buf) {
  if (buf == NULL) return;
  if (buf->base != NULL) free(buf->base);
  free(buf);
}

static void read_cb(uv_stream_t *stream, ssize_t n, const uv_buf_t *buf) {
  uv_read_stop(stream);
  pty_process *process = (pty_process *) stream->data;
  if (n <= 0) {
    if (n == UV_ENOBUFS || n == 0) return;
    process->read_cb(process, NULL, true);
    goto done;
  }
  process->read_cb(process, pty_buf_init(buf->base, (size_t) n), false);

done:
  free(buf->base);
}

static void write_cb(uv_write_t *req, int unused) {
  pty_buf_t *buf = (pty_buf_t *) req->data;
  pty_buf_free(buf);
  free(req);
}

pty_process *process_init(void *ctx, uv_loop_t *loop, char *argv[], char *envp[]) {
  pty_process *process = xmalloc(sizeof(pty_process));
  memset(process, 0, sizeof(pty_process));
  process->ctx = ctx;
  process->loop = loop;
  process->argv = argv;
  process->envp = envp;
  process->columns = 80;
  process->rows = 24;
  process->exit_code = -1;
  return process;
}

bool process_running(pty_process *process) {
  return process != NULL && process->pid > 0 && uv_kill(process->pid, 0) == 0;
}

void process_free(pty_process *process) {
  if (process == NULL) return;
#ifdef _WIN32
  if (process->si.lpAttributeList != NULL) {
    DeleteProcThreadAttributeList(process->si.lpAttributeList);
    free(process->si.lpAttributeList);
  }
  if (process->pty != NULL) pClosePseudoConsole(process->pty);
  if (process->handle != NULL) CloseHandle(process->handle);
#else
  close(process->pty);
  uv_thread_join(&process->tid);
#endif
  if (process->in != NULL) uv_close((uv_handle_t *) process->in, close_cb);
  if (process->out != NULL) uv_close((uv_handle_t *) process->out, close_cb);
  if (process->argv != NULL) free(process->argv);
  if (process->cwd != NULL) free(process->cwd);
  char **p = process->envp;
  for (; *p; p++) free(*p);
  free(process->envp);
}

void pty_pause(pty_process *process) {
  if (process == NULL) return;
  if (process->paused) return;
  uv_read_stop((uv_stream_t *) process->out);
}

void pty_resume(pty_process *process) {
  if (process == NULL) return;
  if (!process->paused) return;
  process->out->data = process;
  uv_read_start((uv_stream_t *) process->out, alloc_cb, read_cb);
}

int pty_write(pty_process *process, pty_buf_t *buf) {
  if (process == NULL) {
    pty_buf_free(buf);
    return UV_ESRCH;
  }
  uv_buf_t b = uv_buf_init(buf->base, buf->len);
  uv_write_t *req = xmalloc(sizeof(uv_write_t));
  req->data = buf;
  return uv_write(req, (uv_stream_t *) process->in, &b, 1, write_cb);
}

bool pty_resize(pty_process *process) {
  if (process == NULL) return false;
  if (process->columns <= 0 || process->rows <= 0) return false;
#ifdef _WIN32
  if (process->columns > SHRT_MAX || process->rows > SHRT_MAX) return false;
  COORD size = {(SHORT) process->columns, (SHORT) process->rows};
  HRESULT hr = pResizePseudoConsole(process->pty, size);
  if (FAILED(hr)) {
    print_hresult("ResizePseudoConsole", hr);
    return false;
  }
  return true;
#else
  struct winsize size = {process->rows, process->columns, 0, 0};
  return ioctl(process->pty, TIOCSWINSZ, &size) == 0;
#endif
}

bool pty_kill(pty_process *process, int sig) {
  if (process == NULL) return false;
#ifdef _WIN32
  return TerminateProcess(process->handle, 1) != 0;
#else
  return uv_kill(-process->pid, sig) == 0;
#endif
}

#ifdef _WIN32
bool conpty_init() {
  uv_lib_t kernel;
  if (uv_dlopen("kernel32.dll", &kernel)) {
    uv_dlclose(&kernel);
    return false;
  }
  static struct {
    char *name;
    FARPROC *ptr;
  } conpty_entry[] = {{"CreatePseudoConsole", (FARPROC *) &pCreatePseudoConsole},
                      {"ResizePseudoConsole", (FARPROC *) &pResizePseudoConsole},
                      {"ClosePseudoConsole", (FARPROC *) &pClosePseudoConsole},
                      {NULL, NULL}};
  for (int i = 0; conpty_entry[i].name != NULL && conpty_entry[i].ptr != NULL; i++) {
    if (uv_dlsym(&kernel, conpty_entry[i].name, (void **) conpty_entry[i].ptr)) {
      uv_dlclose(&kernel);
      return false;
    }
  }
  return true;
}

static WCHAR *to_utf16(char *str) {
  int len = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
  if (len <= 0) return NULL;
  WCHAR *wstr = xmalloc((len + 1) * sizeof(WCHAR));
  if (len != MultiByteToWideChar(CP_UTF8, 0, str, -1, wstr, len)) {
    free(wstr);
    return NULL;
  }
  wstr[len] = L'\0';
  return wstr;
}

// convert argv to cmdline for CreateProcessW
static WCHAR *join_args(char **argv) {
  char args[256] = {0};
  char **ptr = argv;
  for (; *ptr; ptr++) {
    char *quoted = (char *) quote_arg(*ptr);
    size_t arg_len = strlen(args) + 1;
    size_t quoted_len = strlen(quoted);
    if (arg_len == 1) memset(args, 0, 2);
    if (arg_len != 1) strcat(args, " ");
    strncat(args, quoted, quoted_len);
    if (quoted != *ptr) free(quoted);
  }
  if (args[255] != '\0') args[255] = '\0';  // truncate
  return to_utf16(args);
}

static void close_uv_file(uv_file *file) {
  if (*file == -1) return;
  _close(*file);
  *file = -1;
}

static COORD conpty_size(uint16_t columns, uint16_t rows) {
  COORD size = {80, 24};
  if (columns > 0 && columns <= SHRT_MAX) size.X = (SHORT) columns;
  if (rows > 0 && rows <= SHRT_MAX) size.Y = (SHORT) rows;
  return size;
}

static bool open_conpty_pipe(uv_pipe_t *pipe, uv_file *file, const char *name) {
  int err = uv_pipe_open(pipe, *file);
  if (err) {
    fprintf(stderr, "== uv_pipe_open(%s) failed: %s\n", name, uv_strerror(err));
    return false;
  }
  *file = -1;
  return true;
}

static bool conpty_setup(HPCON *hnd, COORD size, STARTUPINFOEXW *si_ex, uv_pipe_t *in, uv_pipe_t *out,
                         uv_file *conpty_input, uv_file *conpty_output) {
  HPCON pty = INVALID_HANDLE_VALUE;
  uv_file input_pipe[2] = {-1, -1};
  uv_file output_pipe[2] = {-1, -1};
  bool attr_list_initialized = false;
  bool ret = false;

  int err = uv_pipe(input_pipe, 0, UV_NONBLOCK_PIPE);
  if (err) {
    fprintf(stderr, "== uv_pipe(input) failed: %s\n", uv_strerror(err));
    goto failed;
  }

  err = uv_pipe(output_pipe, UV_NONBLOCK_PIPE, 0);
  if (err) {
    fprintf(stderr, "== uv_pipe(output) failed: %s\n", uv_strerror(err));
    goto failed;
  }

  HANDLE input_read = (HANDLE) _get_osfhandle(input_pipe[0]);
  HANDLE output_write = (HANDLE) _get_osfhandle(output_pipe[1]);
  if (input_read == INVALID_HANDLE_VALUE || output_write == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "== _get_osfhandle failed while preparing ConPTY pipes\n");
    goto failed;
  }

  HRESULT hr = pCreatePseudoConsole(size, input_read, output_write, 0, &pty);
  if (FAILED(hr)) {
    fprintf(stderr, "CreatePseudoConsole requested size: %dx%d\n", size.X, size.Y);
    print_hresult("CreatePseudoConsole", hr);
    goto failed;
  }

  si_ex->StartupInfo.cb = sizeof(STARTUPINFOEXW);
  size_t bytes_required;
  InitializeProcThreadAttributeList(NULL, 1, 0, &bytes_required);
  si_ex->lpAttributeList = (PPROC_THREAD_ATTRIBUTE_LIST) xmalloc(bytes_required);
  if (!InitializeProcThreadAttributeList(si_ex->lpAttributeList, 1, 0, &bytes_required)) {
    print_error("InitializeProcThreadAttributeList");
    goto failed;
  }
  attr_list_initialized = true;
  if (!UpdateProcThreadAttribute(si_ex->lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pty, sizeof(HPCON),
                                 NULL, NULL)) {
    print_error("UpdateProcThreadAttribute");
    goto failed;
  }

  if (!open_conpty_pipe(in, &input_pipe[1], "input")) goto failed;
  if (!open_conpty_pipe(out, &output_pipe[0], "output")) goto failed;

  *conpty_input = input_pipe[0];
  input_pipe[0] = -1;
  *conpty_output = output_pipe[1];
  output_pipe[1] = -1;
  *hnd = pty;
  pty = INVALID_HANDLE_VALUE;
  ret = true;
  goto done;

failed:
  ret = false;
  if (pty != INVALID_HANDLE_VALUE) pClosePseudoConsole(pty);
  if (si_ex->lpAttributeList != NULL) {
    if (attr_list_initialized) DeleteProcThreadAttributeList(si_ex->lpAttributeList);
    free(si_ex->lpAttributeList);
    si_ex->lpAttributeList = NULL;
  }
done:
  close_uv_file(&input_pipe[0]);
  close_uv_file(&input_pipe[1]);
  close_uv_file(&output_pipe[0]);
  close_uv_file(&output_pipe[1]);
  return ret;
}

static void CALLBACK conpty_exit(void *context, BOOLEAN unused) {
  pty_process *process = (pty_process *) context;
  uv_async_send(&process->async);
}

static void async_cb(uv_async_t *async) {
  pty_process *process = (pty_process *) async->data;
  UnregisterWait(process->wait);

  DWORD exit_code;
  GetExitCodeProcess(process->handle, &exit_code);
  process->exit_code = (int) exit_code;
  process->exit_signal = 1;
  process->exit_cb(process);

  uv_close((uv_handle_t *) async, async_free_cb);
  process_free(process);
}

int pty_spawn(pty_process *process, pty_read_cb read_cb, pty_exit_cb exit_cb) {
  uv_file conpty_input = -1;
  uv_file conpty_output = -1;
  DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
  COORD size = conpty_size(process->columns, process->rows);
  int status = 1;
  PROCESS_INFORMATION pi = {0};
  WCHAR *cmdline = NULL, *cwd = NULL;

  SetConsoleCtrlHandler(NULL, FALSE);

  process->in = xmalloc(sizeof(uv_pipe_t));
  process->out = xmalloc(sizeof(uv_pipe_t));
  uv_pipe_init(process->loop, process->in, 0);
  uv_pipe_init(process->loop, process->out, 0);

  if (!conpty_setup(&process->pty, size, &process->si, process->in, process->out, &conpty_input, &conpty_output))
    goto cleanup;

  cmdline = join_args(process->argv);
  if (cmdline == NULL) goto cleanup;
  if (process->envp != NULL) {
    char **p = process->envp;
    for (; *p; p++) {
      WCHAR *env = to_utf16(*p);
      if (env == NULL) goto cleanup;
      _wputenv(env);
      free(env);
    }
  }
  if (process->cwd != NULL) {
    cwd = to_utf16(process->cwd);
    if (cwd == NULL) goto cleanup;
  }

  if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, flags, NULL, cwd, &process->si.StartupInfo, &pi)) {
    print_error("CreateProcessW");
    goto cleanup;
  }
  close_uv_file(&conpty_input);
  close_uv_file(&conpty_output);

  process->pid = pi.dwProcessId;
  process->handle = pi.hProcess;
  process->paused = true;
  process->read_cb = read_cb;
  process->exit_cb = exit_cb;
  process->async.data = process;
  uv_async_init(process->loop, &process->async, async_cb);

  if (!RegisterWaitForSingleObject(&process->wait, pi.hProcess, conpty_exit, process, INFINITE, WT_EXECUTEONLYONCE)) {
    print_error("RegisterWaitForSingleObject");
    goto cleanup;
  }

  status = 0;

cleanup:
  close_uv_file(&conpty_input);
  close_uv_file(&conpty_output);
  if (pi.hThread != NULL) CloseHandle(pi.hThread);
  if (cmdline != NULL) free(cmdline);
  if (cwd != NULL) free(cwd);
  return status;
}
#else
static bool fd_set_cloexec(const int fd) {
  int flags = fcntl(fd, F_GETFD);
  if (flags < 0) return false;
  return (flags & FD_CLOEXEC) == 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != -1;
}

static bool fd_duplicate(int fd, uv_pipe_t *pipe) {
  int fd_dup = dup(fd);
  if (fd_dup < 0) return false;

  if (!fd_set_cloexec(fd_dup)) return false;

  int status = uv_pipe_open(pipe, fd_dup);
  if (status) close(fd_dup);
  return status == 0;
}

static void wait_cb(void *arg) {
  pty_process *process = (pty_process *) arg;

  pid_t pid;
  int stat;
  do
    pid = waitpid(process->pid, &stat, 0);
  while (pid != process->pid && errno == EINTR);

  if (WIFEXITED(stat)) {
    process->exit_code = WEXITSTATUS(stat);
  }
  if (WIFSIGNALED(stat)) {
    int sig = WTERMSIG(stat);
    process->exit_code = 128 + sig;
    process->exit_signal = sig;
  }

  uv_async_send(&process->async);
}

static void async_cb(uv_async_t *async) {
  pty_process *process = (pty_process *) async->data;
  process->exit_cb(process);

  uv_close((uv_handle_t *) async, async_free_cb);
  process_free(process);
}

int pty_spawn(pty_process *process, pty_read_cb read_cb, pty_exit_cb exit_cb) {
  int status = 0;

  uv_disable_stdio_inheritance();

  int master, pid;
  struct winsize size = {process->rows, process->columns, 0, 0};
  pid = forkpty(&master, NULL, NULL, &size);
  if (pid < 0) {
    status = -errno;
    return status;
  } else if (pid == 0) {
    setsid();
    if (process->cwd != NULL) chdir(process->cwd);
    if (process->envp != NULL) {
      char **p = process->envp;
      for (; *p; p++) putenv(*p);
    }
    int ret = execvp(process->argv[0], process->argv);
    if (ret < 0) {
      perror("execvp failed\n");
      _exit(-errno);
    }
  }

  int flags = fcntl(master, F_GETFL);
  if (flags == -1) {
    status = -errno;
    goto error;
  }
  if (fcntl(master, F_SETFL, flags | O_NONBLOCK) == -1) {
    status = -errno;
    goto error;
  }
  if (!fd_set_cloexec(master)) {
    status = -errno;
    goto error;
  }

  process->in = xmalloc(sizeof(uv_pipe_t));
  process->out = xmalloc(sizeof(uv_pipe_t));
  uv_pipe_init(process->loop, process->in, 0);
  uv_pipe_init(process->loop, process->out, 0);

  if (!fd_duplicate(master, process->in) || !fd_duplicate(master, process->out)) {
    status = -errno;
    goto error;
  }

  process->pty = master;
  process->pid = pid;
  process->paused = true;
  process->read_cb = read_cb;
  process->exit_cb = exit_cb;
  process->async.data = process;
  uv_async_init(process->loop, &process->async, async_cb);
  uv_thread_create(&process->tid, wait_cb, process);

  return 0;

error:
  close(master);
  uv_kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
  return status;
}
#endif
