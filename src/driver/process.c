#include "process.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int driver_run_process(char *const argv[], const char *working_directory) {
  fflush(NULL);
  pid_t child = fork();
  if (child < 0) {
    fprintf(stderr, "error: cannot start '%s': %s\n", argv[0],
            strerror(errno));
    return 1;
  }
  if (child == 0) {
    if (working_directory && chdir(working_directory) != 0) {
      fprintf(stderr, "error: cannot enter build directory '%s': %s\n",
              working_directory, strerror(errno));
      _exit(126);
    }
    execvp(argv[0], argv);
    fprintf(stderr, "error: cannot execute '%s': %s\n", argv[0],
            strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
  }
  int status;
  while (waitpid(child, &status, 0) < 0) {
    if (errno == EINTR) continue;
    fprintf(stderr, "error: cannot wait for '%s': %s\n", argv[0],
            strerror(errno));
    return 1;
  }
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) {
    fprintf(stderr, "error: '%s' terminated by signal %d\n", argv[0],
            WTERMSIG(status));
    return 128 + WTERMSIG(status);
  }
  return 1;
}
