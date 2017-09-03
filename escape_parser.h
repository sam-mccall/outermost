#ifndef ESCAPE_PARSER_H_
#define ESCAPE_PARSER_H_

#include <vector>

#include "base.h"

// Parser for terminal escape sequences.
// Based on the state machine described at http://vt100.net/emu/dec_ansi_parser
// Pluggable parsers for OSC and DSC are not implemented, strings are passed.
class EscapeParser {
 public:
  // Interface for callbacks when we encounter various escape sequences.
  class Actions {
   public:
    virtual ~Actions() = default;
    virtual void Control(u8 control) = 0;
    virtual void Escape(const std::string& command) = 0;
    virtual void CSI(const std::string& command, const std::vector<int>& args) = 0;
    virtual void DSC(const std::string& command, const std::vector<int>& args, const std::string& payload) = 0;
    virtual void OSC(const std::string& command) = 0;
  };

  EscapeParser(Actions* actions) : actions_(actions) { Clear(); }

  // Feed the parser a unicode codepoint. Returns false if it should be printed.
  inline bool Consume(u32 rune) {
    if (LIKELY(state_ == GROUND)) {
      if (LIKELY(rune >= 0x20 && rune < 0x80)) return false;
      if (rune >= 0xa0) return false;
    }
    Handle(rune);
    return true;
  }

 private:
  enum State {
    GROUND, OSC_STRING, SOS_PM_APC_STRING,
    ESCAPE, ESCAPE_INTERMEDIATE,
    CSI_ENTRY, CSI_INTERMEDIATE, CSI_PARAM, CSI_IGNORE,
    DCS_ENTRY, DCS_INTERMEDIATE, DCS_PARAM, DCS_PASSTHROUGH, DCS_IGNORE,
  };

  void Handle(u32 rune);
  bool ParamParse(u8 c);
  void Clear() {
    command_.clear();
    payload_.clear();
    args_.clear();
    arg_in_progress_ = false;
  }

  struct Ignore { void operator()() const {} };
  // Transition to another state.
  // Runs the exit, transition, and enter actions appropriately.
  template<typename Action = Ignore>
  void Transition(State state, const Action& transition_action = Action())
  __attribute__((always_inline)) {
    Exit(state_);
    transition_action();
    Enter(state);
    state_ = state;
  }

  void Enter(State state);
  void Exit(State state);

  State state_;
  Actions* actions_;
  std::string command_;
  std::string payload_;
  std::vector<int> args_;
  bool arg_in_progress_ = false;
};

class DebugActions : public EscapeParser::Actions {
 public:
  void Control(u8 control) override {
    fprintf(stdout, "Control(%02x)\n", control);
    fflush(stdout);
  }
  void Escape(const std::string& command) override {
    fprintf(stdout, "Esc(%s)\n", command.c_str());
    fflush(stdout);
  }
  void CSI(const std::string& command, const std::vector<int>& args) override {
    fprintf(stdout, "CSI(%s, %s)\n", command.c_str(), Join(args).c_str());
    fflush(stdout);
  }
  void DSC(const std::string& command, const std::vector<int>& args, const std::string& payload) override {
    fprintf(stdout, "DSC(%s, %s, %s)\n", command.c_str(), Join(args).c_str(), payload.c_str());
    fflush(stdout);
  }
  void OSC(const std::string& command) override {
    fprintf(stdout, "OSC(%s)\n", command.c_str());
    fflush(stdout);
  }
 private:
  static std::string Join(const std::vector<int>& args) {
    std::string result = "[";
    for (int i = 0; i < args.size(); ++i) {
      if (i) result.push_back(',');
      result.append(std::to_string(args[i]));
    }
    result.push_back(']');
    return result;
  }
};

#endif
