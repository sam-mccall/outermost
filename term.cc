#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <pty.h>
#include <unistd.h>
#include <cerrno>
#include <sys/wait.h>
#include <sys/select.h>
#include <array>
#include <deque>

#define PCHECK(x) do { if (!(x)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, strerror(errno)); \
  exit(1); \
} } while(0)
#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
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

template <int N>
class History {
 public:
  History() {
    memset(data_, 0, N);
  }

  void Write(const char* src, int count) {
    while (count >= 2*N) {
      src += N;
      count -= N;
    }
    if (pos_ + count >= N) {
      memcpy(&data_[pos_], src, N - pos_);
      count -= N - pos_;
      src += N - pos_;
      pos_ = 0;
    }
    memcpy(data_, src, count);
    pos_ += count;
  }

 private:
  char data_[N];
  int pos_ = 0;
};

template <int N>
class WriteQueue {
 public:
  WriteQueue() : blocks_(1) {}

  void Push(const char* data, int n) {
    while (n > 0) {
      int count = std::min(n, N - limit_);
      memcpy(&blocks_.back()[limit_], data, count);
      limit_ += count;
      if (limit_ == N) {
        limit_ = 0;
        blocks_.emplace_back();
      }
      n -= count;
    }
  }

  void Shift(int n) {
    start_ += n;
    if (start_ == N) {
      start_ = 0;
      blocks_.pop_front();
    }
  }

  bool HasBlock() { return blocks_.size() > 1 || start_ != limit_; }
  int GetBlock(char** data) {
    *data = &blocks_.front()[start_];
    return blocks_.size() == 1 ? limit_ - start_ : N - start_;
  }

 private:
  std::deque<std::array<char, N>> blocks_;
  int start_ = 0; // in first block
  int limit_ = 0; // in last block
};

class Shell {
 public:
   Shell(int tty) : tty_(tty) {
     int tty_flags = fcntl(tty_, F_GETFL);
     PCHECK(tty_flags >= 0);
     PCHECK(fcntl(tty_, F_SETFL, tty_flags | O_NONBLOCK) >= 0);
   }

   void Read() {
     int count = read(tty_, &read_buf_[0], read_buf_.size());
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
     read_history_.Write(&read_buf_[0], count);
     for (int i = 0; i < count; ++i) {
       fprintf(stderr, "%d %c\n", read_buf_[i], read_buf_[i]);
     }
   }

   bool NeedsWrite() { return write_queue_.HasBlock(); }
   void Write() {
     CHECK(NeedsWrite());
     char* buf;
     int count = write_queue_.GetBlock(&buf);
     count = write(tty_, buf, count);
     if (count < 0) {
       switch (errno) {
         case EAGAIN: case EINTR:
           break;
         default:
           fprintf(stderr, "writing to master: %s\n", strerror(errno));
           break;
       }
       return;
     }
     write_queue_.Shift(count);
     write_history_.Write(buf, count);
   }

   void Write(const char* data, int len) { write_queue_.Push(data, len); }

 private:
  int tty_;
  WriteQueue<1024> write_queue_;
  std::array<char, 1024> read_buf_;
  History<256> read_history_;
  History<256> write_history_;
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
  shell.Write("hello world\n", strlen("hello world\n"));
  while (1) {
    fd_set read;
    fd_set write;
    FD_ZERO(&read);
    FD_ZERO(&write);
    FD_SET(master, &read);
    if (shell.NeedsWrite()) FD_SET(master, &write);
    timeval timeout = {1, 0};
    PCHECK(select(master + 1, &read, &write, nullptr, &timeout) >= 0); 
    if (FD_ISSET(master, &write)) shell.Write();
    if (FD_ISSET(master, &read)) shell.Read();
  }
}
