#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <pty.h>
#include <unistd.h>
#include <cerrno>
#include <sys/wait.h>

#define PCHECK(x) do { if (!(x)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, strerror(errno)); \
  exit(1); \
} } while(0)

static void ExecShell(int slave) {
  setsid();
  PCHECK(ioctl(slave, TIOCSCTTY, nullptr) >= 0);
  PCHECK(dup2(slave, 0) >= 0);
  PCHECK(dup2(slave, 1) >= 0);
  PCHECK(dup2(slave, 2) >= 0);
  close(slave);
  const char* shell = getenv("SHELL");
  if (!shell) shell = "/bin/sh";
  execl(shell, shell, nullptr);
}

static void HandleSIGCHLD(int) {
  int status;
  pid_t pid = waitpid(-1, &status, WNOHANG);
  PCHECK(pid > 0);
  fprintf(stderr, "Shell process %d finished with status %d\n", pid, status);
  exit(WIFEXITED(status) ? WEXITSTATUS(status) : 128);
}

int main(int argc, char** argv) {
  int master, slave;
  PCHECK(!openpty(&master, &slave, nullptr, nullptr, nullptr));
  int shell_pid = fork();
  if (shell_pid == 0) {
    close(master);
    ExecShell(slave);
    PCHECK(0);
  }
  PCHECK(shell_pid > 0);
  signal(SIGCHLD, HandleSIGCHLD);
  sleep(5);
  printf("hello, world\n");
}
