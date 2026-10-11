// POSIX functions that VitaSDK's libc lacks but TDLib (stacktrace, NativeFd) and SQLite
// (its syscall table) reference. They only report the call and fail.
// Originally taken from v-atamanenko/tdlib-vita (vitaports/td_missing_func_placeholder.cpp).
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>

extern "C" {

int fchown(int, uid_t, gid_t) {
  fprintf(stderr, "[WARNING] fchown used\n");
  return -1;
}

uid_t geteuid(void) {
  fprintf(stderr, "[WARNING] geteuid used\n");
  return -1;
}

ssize_t readlink(const char *__restrict, char *__restrict, size_t) {
  fprintf(stderr, "[WARNING] readlink used\n");
  return -1;
}

int dup2(int, int) {
  fprintf(stderr, "[WARNING] dup2 used\n");
  return -1;
}

}
