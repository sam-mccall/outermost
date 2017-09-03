#include "base.h"
#include "buffers.h"
#include "escape_parser.h"

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
#include <vector>

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

struct Cell {
  u32 rune;
};
class Grid {
 public:
  Grid(int w, int h) {
    Resize(w, h);
    for (auto& row : cells_) row.resize(w);
  }

  void Resize(int w, int h) {
    CHECK(w > 0 && h > 0);
    if (int dh = h - h_) {
      if (dh > 0) {
        // Insert rows at the start: insert them at the end and then swap.
        cells_.resize(h);
        for (int i = h_ - 1; i >= 0; --i) {
          swap(cells_[i], cells_[i + dh]);
        }
      }
      if (h < h_ ) {
        // Delete rows from start: swap first and then delete from end.
        for (int i = 0; i < h; ++i) {
          swap(cells_[i], cells_[i - dh]);
        }
        cells_.resize(h);
      }
      y_ += dh;
      h_ = h;
    }
    // TODO: rewrapping
    for (auto& row : cells_) if (row.size() > w) row.resize(w);
    if (x_ > w) x_ = w;
    w_ = w;
  }

  void ShiftUp() {
    // Maybe we should have a different memory representation to make this fast.
    cells_[0].clear();
    for (int i = 1; i < h_; ++i) {
      swap(cells_[i - 1], cells_[i]);
    }
  }

  Cell& cell(int x, int y) {
    return cells_[y][x];
  }

  void Dump() {
    for (const auto& row : cells_) {
      for (const auto& cell : row) {
        fputc(isprint(cell.rune) ? cell.rune : ' ', stderr);
      }
      fputc('\n', stderr);
    }
  }

  void Put(u8 c) {
    fprintf(stderr, "Put(%02x)\n", c);
    // TODO: wide characters
    if (x_ == w_) {
      // TODO record soft-wrap
      CarriageReturn();
      LineFeed();
    }
    auto& row = cells_[y_];
    if (x_ == row.size()) row.emplace_back();
    row[x_++].rune = c;
  }

  void CarriageReturn() {
    x_ = 0;
  }

  void LineFeed() {
    if (y_ + 1 == h_) ShiftUp(); else ++y_;
    FixWidth();
  }

  int x() const { return x_; }
  int y() const { return y_; }
  void Move(int x, int y) {
    y_ = y;
    x_ = x;
    FixWidth();
  }

 private:
  void FixWidth() {
    auto& row = cells_[y_];
    if (row.size() <= x_) row.resize(std::min(x_ + 1, w_));
  }

  std::vector<std::vector<Cell>> cells_;
  int w_ = 0, h_ = 0;
  int x_ = 0, y_ = -1; // x_ may equal w_;
};

struct Keypress {
  // TODO: modifiers
  KeySym sym;
  const char* text;
};

class Shell : public DebugActions {
 public:
   Shell(int tty) : tty_(tty), parser_(this), grid_(80, 25) {
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
       u8 c = read_buf_[i];
       // XXX: unicode decode instead
       if (parser_.Consume(c)) continue;
       if (isprint(c)) {
         fputc(c, stderr);
         grid_.Put(c);
       } else {
         fprintf(stderr, "[%02x]", c);
       }
     }
   }

  void Update() {
    fprintf(stderr, "=====\n");
    grid_.Dump();
    fprintf(stderr, "-----\nRead:\n");
    read_history_.Dump();
    fprintf(stderr, "Write:\n");
    write_history_.Dump();
    fprintf(stderr, "=====\n");
  }

   bool NeedsWrite() { return write_queue_.HasBlock(); }
   void Write() {
     CHECK(NeedsWrite());
     u8* buf;
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

   void Write(const u8* data, int len) { write_queue_.Push(data, len); }

   void Key(const Keypress& key) {
     switch (key.sym) {
     default:
       Write(reinterpret_cast<const u8*>(key.text), strlen(key.text));
     }
   }

  void Control(u8 command) override {
    DebugActions::Control(command);
    switch(command) {
      case '\r':
        return grid_.CarriageReturn();
      case '\n':
        return grid_.LineFeed();
    }
  }

 private:
  Grid grid_;
  EscapeParser parser_;
  int tty_;
  WriteQueue<1024> write_queue_;
  std::array<u8, 1024> read_buf_;
  History<192> read_history_;
  History<192> write_history_;
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
    shell.Update();
  }
}
