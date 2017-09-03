#include "escape_parser.h"

void EscapeParser::Handle(u32 rune) {
  u8 c = (rune >= 0xa0) ? rune & 0x7f : rune;
  // Some characters are handled the same way in all modes.
  switch (__builtin_expect(c, -1)) { // None of these are very likely...
  case 0x1B:
    return Transition(ESCAPE);
  case 0x90:
    return Transition(DCS_ENTRY);
  case 0x9B:
    return Transition(CSI_ENTRY);
  case 0x9C:
    return Transition(GROUND);
  case 0x9D:
    return Transition(OSC_STRING);
  case 0x98: case 0x9E: case 0x9F:
    return Transition(SOS_PM_APC_STRING);
  case 0x18: case 0x1A:
  case 0x80: case 0x81: case 0x82: case 0x83:
  case 0x84: case 0x85: case 0x86: case 0x87:
  case 0x88: case 0x89: case 0x8a: case 0x8b:
  case 0x8c: case 0x8d: case 0x8e: case 0x8f:
  case 0x91: case 0x92: case 0x93: case 0x94:
  case 0x95: case 0x96: case 0x97: case 0x99: case 0x9a:
    return Transition(GROUND, [&]{actions_->Control(c);});
  case 0x7f:
    if (state_ != OSC_STRING) return;
  }
  // C0 control characters (not handled above) have uniform rules per state.
  if (c < 0x20) {
    switch (__builtin_expect(state_, GROUND)) {
    case GROUND: case ESCAPE: case ESCAPE_INTERMEDIATE:
    case CSI_ENTRY: case CSI_INTERMEDIATE: case CSI_PARAM: case CSI_IGNORE:
      return actions_->Control(c);
    case DCS_PASSTHROUGH:
      payload_.push_back(c);
      return;
    default:
      return;
    }
  }
  switch (state_) {
  case ESCAPE:
    switch (__builtin_expect(c, 0x5b)) {
    case 0x50:
      return Transition(DCS_ENTRY);
    case 0x5B:
      return Transition(CSI_ENTRY);
    case 0x58: case 0x5E: case 0x5F:
      return Transition(SOS_PM_APC_STRING);
    case 0x5D:
      return Transition(OSC_STRING);
    }
  /* fallthrough */
  case ESCAPE_INTERMEDIATE:
    if (UNLIKELY(c < 0x30)) return Transition(ESCAPE_INTERMEDIATE, [&]{
      command_.push_back(c);
    });
    return Transition(GROUND, [&]{
      command_.push_back(c);
      actions_->Escape(command_);
    });
  case CSI_ENTRY:
    if (UNLIKELY(c > 0x3a && c < 0x40)) return Transition(CSI_PARAM, [&]{
      command_.push_back(c);
    });
    /* fallthrough */
  case CSI_PARAM:
    if (LIKELY(ParamParse(c))) return Transition(CSI_PARAM);
    /* fallthrough */
  case CSI_INTERMEDIATE:
    command_.push_back(c);
    if (LIKELY(c >= 0x40)) return Transition(GROUND, [&]{
      actions_->CSI(command_, args_);
    });
    if (LIKELY(c < 0x30)) return Transition(CSI_INTERMEDIATE);
    return Transition(CSI_IGNORE);
  case CSI_IGNORE:
    if (c >= 0x40) return Transition(GROUND);
    return;
  case DCS_ENTRY:
    if (UNLIKELY(c > 0x3a && c < 0x40)) return Transition(CSI_PARAM, [&]{
      command_.push_back(c);
    });
    /* fallthrough */
  case DCS_PARAM:
    if (LIKELY(ParamParse(c))) return Transition(DCS_PARAM);
    /* fallthrough */
  case DCS_INTERMEDIATE:
    if (LIKELY(c >= 0x40)) return Transition(DCS_PASSTHROUGH, [&]{
      payload_.push_back(c);
    });
    if (LIKELY(c < 0x30)) return Transition(DCS_INTERMEDIATE, [&]{
      command_.push_back(c);
    });
    return Transition(DCS_IGNORE);
  case DCS_PASSTHROUGH:
    return payload_.push_back(c);
  case DCS_IGNORE:
    if (c == 0x9c) return Transition(GROUND);
    return;
  case OSC_STRING:
    // XXX: unicode encode rune instead;
    return payload_.push_back(c);
  case SOS_PM_APC_STRING:
    return;
  }
}

bool EscapeParser::ParamParse(u8 c) {
  if (c == ';') {
    arg_in_progress_ = false;
    return true;
  }
  if (c >= '0' && c <= '9') {
    if (!arg_in_progress_) {
      args_.push_back(0);
      arg_in_progress_ = true;
    }
    args_.back() *= 10;
    args_.back() += c - '0';
    return true;
  }
  return false;
}

void EscapeParser::Enter(State state) {
  state_ = state;
  // Entry actions.
  switch (state_) {
  case ESCAPE:
  case DCS_ENTRY:
  case CSI_ENTRY:
    return Clear();
  }
}

void EscapeParser::Exit(State state) {
  switch (state_) {
  case OSC_STRING:
    actions_->OSC(payload_);
    break;
  case DCS_PASSTHROUGH:
    actions_->DSC(command_, args_, payload_);
    break;
  }
}
