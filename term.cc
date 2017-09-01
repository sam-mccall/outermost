#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <pty.h>
#include <unistd.h>
#include <cerrno>
#include <sys/wait.h>
#include <sys/poll.h>
#include <array>
#include <deque>
#include <X11/Xlib.h>
#include <experimental/optional>

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

struct Keypress {
  // TODO: modifiers
  KeySym sym;
  const char* text;
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
       char c = read_buf_[i];
       if (isprint(c)) {
         fputc(c, stderr);
       } else {
         fprintf(stderr, "[%02x]", c);
       }
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

   void Key(const Keypress& key) {
     switch (key.sym) {
     default:
       Write(key.text, strlen(key.text));
     }
   }

 private:
  int tty_;
  WriteQueue<1024> write_queue_;
  std::array<char, 1024> read_buf_;
  History<256> read_history_;
  History<256> write_history_;
};

class TermWindow {
 public:
  TermWindow(Display* display) : display_(display) {
    screen_ = DefaultScreen(display_);
    window_ = XCreateSimpleWindow(display_, RootWindow(display_, screen_),
        0, 0, 100, 100, 0, 0, WhitePixel(display_, screen_));
    XSelectInput(display_, window_, KeyPressMask);
    XMapWindow(display_, window_);
    input_method_ = XOpenIM(display_, nullptr, nullptr, nullptr);
    CHECK(input_method_);
    input_context_ = XCreateIC(input_method_,
        XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
        nullptr);
    CHECK(input_context_);
  }

  std::experimental::optional<Keypress> DecodeKeypress(XEvent* event) {
    if (event->type != KeyPress) return std::experimental::nullopt;
    static char buf[16];
    Status status;
    KeySym sym;
    int len = Xutf8LookupString(
        input_context_, &event->xkey, buf, sizeof(buf) - 1, &sym, &status);
    switch (status) {
      case XLookupKeySym:
        return Keypress{sym, ""};
      case XLookupChars:
        sym = 0;
        /* fallthrough */
      case XLookupBoth:
        buf[len] = 0;
        return Keypress{sym, buf};
    }
    return std::experimental::nullopt;
  }

 private:
  Display* display_;
  int screen_;
  Window window_;
  XIM input_method_;
  XIC input_context_;
};

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
  Display* display = XOpenDisplay(nullptr);
  CHECK(display);
  TermWindow window(display);
  Shell shell(master);

  pollfd poll_fds[] = {
    {master, POLLIN, 0},
    {XConnectionNumber(display), POLLIN, 0},
  };
  pollfd& poll_master = poll_fds[0];

  while (1) {
    poll_master.events = POLLIN | (shell.NeedsWrite() ? POLLOUT : 0);
    PCHECK(poll(poll_fds, sizeof(poll_fds)/sizeof(poll_fds[0]), 1000) >= 0);
    if (poll_master.revents & POLLIN) shell.Read();
    if (poll_master.revents & POLLOUT) shell.Write();
    while (XPending(display)) {
      XEvent event;
      XNextEvent(display, &event);
      if (auto keypress = window.DecodeKeypress(&event)) shell.Key(*keypress);
    }
  }
}
