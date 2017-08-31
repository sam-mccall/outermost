#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <pty.h>
#include <unistd.h>
#include <cerrno>
#include <sys/wait.h>
#include <sys/select.h>

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

class Shell {
 public:
   Shell(int tty) : tty_(tty) {
     int tty_flags = fcntl(tty_, F_GETFL);
     PCHECK(tty_flags >= 0);
     PCHECK(fcntl(tty_, F_SETFL, tty_flags | O_NONBLOCK) >= 0);
   }

   void Read() {
     int count = read(tty_, &buf_, sizeof(buf_));
     if (count < 0) {
       switch (errno) {
         case EAGAIN: case EINTR:
           break;
         default:
           fprintf(stderr, "reading from master: %s\n", strerror(errno));
           break;
       }
       return;
     }
     fprintf(stderr, "read from master\n");
     for (int i = 0; i < count; ++i) {
       fprintf(stderr, "%d %c\n", buf_[i], buf_[i]);
     }
   }

 private:
  int tty_;
  char buf_[1024];
};
static void ReadFromShell(int fd) {
  static char buf[1024];
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
  close(slave);
  Shell shell(master);
  while (1) {
    fd_set read;
    FD_ZERO(&read);
    FD_SET(master, &read);
    timeval timeout = {1, 0};
    PCHECK(select(master + 1, &read, nullptr, nullptr, &timeout) >= 0); 
    if (FD_ISSET(master, &read)) {
      shell.Read();
    }
  }
}
