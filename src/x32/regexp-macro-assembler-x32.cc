// Copyright 2012 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "v8.h"

#if V8_TARGET_ARCH_X32

#include "cpu-profiler.h"
#include "serialize.h"
#include "unicode.h"
#include "log.h"
#include "regexp-stack.h"
#include "macro-assembler.h"
#include "regexp-macro-assembler.h"
#include "x32/regexp-macro-assembler-x32.h"

namespace v8 {
namespace internal {

#ifndef V8_INTERPRETED_REGEXP

/*
 * This assembler uses the following register assignment convention
 * - rdx : Currently loaded character(s) as ASCII or UC16.  Must be loaded
 *         using LoadCurrentCharacter before using any of the dispatch methods.
 *         Temporarily stores the index of capture start after a matching pass
 *         for a global regexp.
 * - rdi : Current position in input, as negative offset from end of string.
 *         Please notice that this is the byte offset, not the character
 *         offset!  Is always a 32-bit signed (negative) offset, but must be
 *         maintained sign-extended to 64 bits, since it is used as index.
 * - rsi : End of input (points to byte after last character in input),
 *         so that rsi+rdi points to the current character.
 * - rbp : Frame pointer.  Used to access arguments, local variables and
 *         RegExp registers.
 * - rsp : Points to tip of C stack.
 * - rcx : Points to tip of backtrack stack.  The backtrack stack contains
 *         only 32-bit values.  Most are offsets from some base (e.g., character
 *         positions from end of string or code location from Code* pointer).
 * - r8  : Code object pointer.  Used to convert between absolute and
 *         code-object-relative addresses.
 *
 * The registers rax, rbx, r9 and r11 are free to use for computations.
 * If changed to use r12+, they should be saved as callee-save registers.
 * The macro assembler special registers r12 and r13 (kSmiConstantRegister,
 * kRootRegister) aren't special during execution of RegExp code (they don't
 * hold the values assumed when creating JS code), so no Smi or Root related
 * macro operations can be used.
 *
 * Each call to a C++ method should retain these registers.
 *
 * The stack will have the following content, in some order, indexable from the
 * frame pointer (see, e.g., kStackHighEnd):
 *    - Isolate* isolate     (address of the current isolate)
 *    - direct_call          (if 1, direct call from JavaScript code, if 0 call
 *                            through the runtime system)
 *    - stack_area_base      (high end of the memory area to use as
 *                            backtracking stack)
 *    - capture array size   (may fit multiple sets of matches)
 *    - int* capture_array   (int[num_saved_registers_], for output).
 *    - end of input         (address of end of string)
 *    - start of input       (address of first character in string)
 *    - start index          (character index of start)
 *    - String* input_string (input string)
 *    - return address
 *    - backup of callee save registers (rbx, possibly rsi and rdi).
 *    - success counter      (only useful for global regexp to count matches)
 *    - Offset of location before start of input (effectively character
 *      position -1).  Used to initialize capture registers to a non-position.
 *    - At start of string (if 1, we are starting at the start of the
 *      string, otherwise 0)
 *    - register 0  rbp[-n]   (Only positions must be stored in the first
 *    - register 1  rbp[-n-8]  num_saved_registers_ registers)
 *    - ...
 *
 * The first num_saved_registers_ registers are initialized to point to
 * "character -1" in the string (i.e., char_size() bytes before the first
 * character of the string).  The remaining registers starts out uninitialized.
 *
 * The first seven values must be provided by the calling code by
 * calling the code's entry address cast to a function pointer with the
 * following signature:
 * int (*match)(String* input_string,
 *              int start_index,
 *              Address start,
 *              Address end,
 *              int* capture_output_array,
 *              bool at_start,
 *              byte* stack_area_base,
 *              bool direct_call)
 */

#define __ ACCESS_MASM((&masm_))

RegExpMacroAssemblerX32::RegExpMacroAssemblerX32(
    Mode mode,
    int registers_to_save,
    Zone* zone)
    : NativeRegExpMacroAssembler(zone),
      masm_(zone->isolate(), NULL, kRegExpCodeSize),
      no_root_array_scope_(&masm_),
      code_relative_fixup_positions_(4, zone),
      mode_(mode),
      num_registers_(registers_to_save),
      num_saved_registers_(registers_to_save),
      entry_label_(),
      start_label_(),
      success_label_(),
      backtrack_label_(),
      exit_label_() {
  ASSERT_EQ(0, registers_to_save % 2);
  __ jmp(&entry_label_);   // We'll write the entry code when we know more.
  __ bind(&start_label_);  // And then continue from here.
}


RegExpMacroAssemblerX32::~RegExpMacroAssemblerX32() {
  // Unuse labels in case we throw away the assembler without calling GetCode.
  entry_label_.Unuse();
  start_label_.Unuse();
  success_label_.Unuse();
  backtrack_label_.Unuse();
  exit_label_.Unuse();
  check_preempt_label_.Unuse();
  stack_overflow_label_.Unuse();
}


int RegExpMacroAssemblerX32::stack_limit_slack()  {
  return RegExpStack::kStackLimitSlack;
}


void RegExpMacroAssemblerX32::AdvanceCurrentPosition(int by) {
  if (by != 0) {
    __ addq(rdi, Immediate(by * char_size()));
  }
}


void RegExpMacroAssemblerX32::AdvanceRegister(int reg, int by) {
  ASSERT(reg >= 0);
  ASSERT(reg < num_registers_);
  if (by != 0) {
    __ addl(register_location(reg), Immediate(by));
  }
}


void RegExpMacroAssemblerX32::Backtrack() {
  CheckPreemption();
  // Pop Code* offset from backtrack stack, add Code* and jump to location.
  Pop(rbx);
  __ addl(rbx, code_object_pointer());
  __ jmp(rbx);
}


void RegExpMacroAssemblerX32::Bind(Label* label) {
  __ bind(label);
}


void RegExpMacroAssemblerX32::CheckCharacter(uint32_t c, Label* on_equal) {
  __ cmpl(current_character(), Immediate(c));
  BranchOrBacktrack(equal, on_equal);
}


void RegExpMacroAssemblerX32::CheckCharacterGT(uc16 limit, Label* on_greater) {
  __ cmpl(current_character(), Immediate(limit));
  BranchOrBacktrack(greater, on_greater);
}


void RegExpMacroAssemblerX32::CheckAtStart(Label* on_at_start) {
  Label not_at_start;
  // Did we start the match at the start of the string at all?
  __ cmpl(Operand(rbp, kStartIndex), Immediate(0));
  BranchOrBacktrack(not_equal, &not_at_start);
  // If we did, are we still at the start of the input?
  __ leal(rax, Operand(rsi, rdi, times_1, 0));
  __ cmpl(rax, Operand(rbp, kInputStart));
  BranchOrBacktrack(equal, on_at_start);
  __ bind(&not_at_start);
}


void RegExpMacroAssemblerX32::CheckNotAtStart(Label* on_not_at_start) {
  // Did we start the match at the start of the string at all?
  __ cmpl(Operand(rbp, kStartIndex), Immediate(0));
  BranchOrBacktrack(not_equal, on_not_at_start);
  // If we did, are we still at the start of the input?
  __ leal(rax, Operand(rsi, rdi, times_1, 0));
  __ cmpl(rax, Operand(rbp, kInputStart));
  BranchOrBacktrack(not_equal, on_not_at_start);
}


void RegExpMacroAssemblerX32::CheckCharacterLT(uc16 limit, Label* on_less) {
  __ cmpl(current_character(), Immediate(limit));
  BranchOrBacktrack(less, on_less);
}


void RegExpMacroAssemblerX32::CheckGreedyLoop(Label* on_equal) {
  Label fallthrough;
  __ cmpl(rdi, Operand(backtrack_stackpointer(), 0));
  __ j(not_equal, &fallthrough);
  Drop();
  BranchOrBacktrack(no_condition, on_equal);
  __ bind(&fallthrough);
}


void RegExpMacroAssemblerX32::CheckNotBackReferenceIgnoreCase(
    int start_reg,
    Label* on_no_match) {
  Label fallthrough;
  __ movsxlq(rdx, register_location(start_reg));  // Offset of start of capture
  // Offset of end of capture
  __ movsxlq(rbx, register_location(start_reg + 1));
  __ subl(rbx, rdx);  // Length of capture.

  // -----------------------
  // rdx  = Start offset of capture.
  // rbx = Length of capture

  // If length is negative, this code will fail (it's a symptom of a partial or
  // illegal capture where start of capture after end of capture).
  // This must not happen (no back-reference can reference a capture that wasn't
  // closed before in the reg-exp, and we must not generate code that can cause
  // this condition).

  // If length is zero, either the capture is empty or it is nonparticipating.
  // In either case succeed immediately.
  __ j(equal, &fallthrough);

  // -----------------------
  // rdx - Start of capture
  // rbx - length of capture
  // Check that there are sufficient characters left in the input.
  __ movl(rax, rdi);
  __ addl(rax, rbx);
  BranchOrBacktrack(greater, on_no_match);

  if (mode_ == ASCII) {
    Label loop_increment;
    if (on_no_match == NULL) {
      on_no_match = &backtrack_label_;
    }

    __ leal(r9, Operand(rsi, rdx, times_1, 0));
    __ leal(r11, Operand(rsi, rdi, times_1, 0));
    __ addl(rbx, r9);  // End of capture
    // ---------------------
    // r11 - current input character address
    // r9 - current capture character address
    // rbx - end of capture

    Label loop;
    __ bind(&loop);
    __ movzxbl(rdx, Operand(r9, 0));
    __ movzxbl(rax, Operand(r11, 0));
    // al - input character
    // dl - capture character
    __ cmpb(rax, rdx);
    __ j(equal, &loop_increment);

    // Mismatch, try case-insensitive match (converting letters to lower-case).
    // I.e., if or-ing with 0x20 makes values equal and in range 'a'-'z', it's
    // a match.
    __ orl(rax, Immediate(0x20));  // Convert match character to lower-case.
    __ orl(rdx, Immediate(0x20));  // Convert capture character to lower-case.
    __ cmpb(rax, rdx);
    __ j(not_equal, on_no_match);  // Definitely not equal.
    __ subb(rax, Immediate('a'));
    __ cmpb(rax, Immediate('z' - 'a'));
    __ j(below_equal, &loop_increment);  // In range 'a'-'z'.
    // Latin-1: Check for values in range [224,254] but not 247.
    __ subb(rax, Immediate(224 - 'a'));
    __ cmpb(rax, Immediate(254 - 224));
    __ j(above, on_no_match);  // Weren't Latin-1 letters.
    __ cmpb(rax, Immediate(247 - 224));  // Check for 247.
    __ j(equal, on_no_match);
    __ bind(&loop_increment);
    // Increment pointers into match and capture strings.
    __ addl(r11, Immediate(1));
    __ addl(r9, Immediate(1));
    // Compare to end of capture, and loop if not done.
    __ cmpl(r9, rbx);
    __ j(below, &loop);

    // Compute new value of character position after the matched part.
    __ movl(rdi, r11);
    __ subq(rdi, rsi);
  } else {
    ASSERT(mode_ == UC16);
    // Save important/volatile registers before calling C function.
#ifndef _WIN64
    // Caller save on Linux and callee save in Windows.
    __ push(rsi);
    __ push(rdi);
#endif
    __ push(backtrack_stackpointer());

    static const int num_arguments = 4;
    __ PrepareCallCFunction(num_arguments);

    // Put arguments into parameter registers. Parameters are
    //   Address byte_offset1 - Address captured substring's start.
    //   Address byte_offset2 - Address of current character position.
    //   size_t byte_length - length of capture in bytes(!)
    //   Isolate* isolate
#ifdef _WIN64
    // Compute and set byte_offset1 (start of capture).
    __ leal(rcx, Operand(rsi, rdx, times_1, 0));
    // Set byte_offset2.
    __ leal(rdx, Operand(rsi, rdi, times_1, 0));
    // Set byte_length.
    __ movl(r8, rbx);
    // Isolate.
    __ LoadAddress(r9, ExternalReference::isolate_address(isolate()));
#else  // AMD64 calling convention
    // Compute byte_offset2 (current position = rsi+rdi).
    __ leal(rax, Operand(rsi, rdi, times_1, 0));
    // Compute and set byte_offset1 (start of capture).
    __ leal(rdi, Operand(rsi, rdx, times_1, 0));
    // Set byte_offset2.
    __ movl(rsi, rax);
    // Set byte_length.
    __ movl(rdx, rbx);
    // Isolate.
    __ LoadAddress(rcx, ExternalReference::isolate_address(isolate()));
#endif

    { // NOLINT: Can't find a way to open this scope without confusing the
      // linter.
      AllowExternalCallThatCantCauseGC scope(&masm_);
      ExternalReference compare =
          ExternalReference::re_case_insensitive_compare_uc16(isolate());
      __ CallCFunction(compare, num_arguments);
    }

    // Restore original values before reacting on result value.
    __ Move(code_object_pointer(), masm_.CodeObject());
    __ pop(backtrack_stackpointer());
#ifndef _WIN64
    __ pop(rdi);
    __ pop(rsi);
#endif

    // Check if function returned non-zero for success or zero for failure.
    __ testl(rax, rax);
    BranchOrBacktrack(zero, on_no_match);
    // On success, increment position by length of capture.
    // Requires that rbx is callee save (true for both Win64 and AMD64 ABIs).
    __ addq(rdi, rbx);
  }
  __ bind(&fallthrough);
}


void RegExpMacroAssemblerX32::CheckNotBackReference(
    int start_reg,
    Label* on_no_match) {
  Label fallthrough;

  // Find length of back-referenced capture.
  __ movsxlq(rdx, register_location(start_reg));
  __ movsxlq(rax, register_location(start_reg + 1));
  __ subl(rax, rdx);  // Length to check.

  // Fail on partial or illegal capture (start of capture after end of capture).
  // This must not happen (no back-reference can reference a capture that wasn't
  // closed before in the reg-exp).
  __ Check(greater_equal, kInvalidCaptureReferenced);

  // Succeed on empty capture (including non-participating capture)
  __ j(equal, &fallthrough);

  // -----------------------
  // rdx - Start of capture
  // rax - length of capture

  // Check that there are sufficient characters left in the input.
  __ movl(rbx, rdi);
  __ addl(rbx, rax);
  BranchOrBacktrack(greater, on_no_match);

  // Compute pointers to match string and capture string
  __ leal(rbx, Operand(rsi, rdi, times_1, 0));  // Start of match.
  __ addl(rdx, rsi);  // Start of capture.
  __ leal(r9, Operand(rdx, rax, times_1, 0));  // End of capture

  // -----------------------
  // rbx - current capture character address.
  // rbx - current input character address .
  // r9 - end of input to match (capture length after rbx).

  Label loop;
  __ bind(&loop);
  if (mode_ == ASCII) {
    __ movzxbl(rax, Operand(rdx, 0));
    __ cmpb(rax, Operand(rbx, 0));
  } else {
    ASSERT(mode_ == UC16);
    __ movzxwl(rax, Operand(rdx, 0));
    __ cmpw(rax, Operand(rbx, 0));
  }
  BranchOrBacktrack(not_equal, on_no_match);
  // Increment pointers into capture and match string.
  __ addl(rbx, Immediate(char_size()));
  __ addl(rdx, Immediate(char_size()));
  // Check if we have reached end of match area.
  __ cmpl(rdx, r9);
  __ j(below, &loop);

  // Success.
  // Set current character position to position after match.
  __ movl(rdi, rbx);
  __ subq(rdi, rsi);

  __ bind(&fallthrough);
}


void RegExpMacroAssemblerX32::CheckNotCharacter(uint32_t c,
                                                Label* on_not_equal) {
  __ cmpl(current_character(), Immediate(c));
  BranchOrBacktrack(not_equal, on_not_equal);
}


void RegExpMacroAssemblerX32::CheckCharacterAfterAnd(uint32_t c,
                                                     uint32_t mask,
                                                     Label* on_equal) {
  if (c == 0) {
    __ testl(current_character(), Immediate(mask));
  } else {
    __ movl(rax, Immediate(mask));
    __ andl(rax, current_character());
    __ cmpl(rax, Immediate(c));
  }
  BranchOrBacktrack(equal, on_equal);
}


void RegExpMacroAssemblerX32::CheckNotCharacterAfterAnd(uint32_t c,
                                                        uint32_t mask,
                                                        Label* on_not_equal) {
  if (c == 0) {
    __ testl(current_character(), Immediate(mask));
  } else {
    __ movl(rax, Immediate(mask));
    __ andl(rax, current_character());
    __ cmpl(rax, Immediate(c));
  }
  BranchOrBacktrack(not_equal, on_not_equal);
}


void RegExpMacroAssemblerX32::CheckNotCharacterAfterMinusAnd(
    uc16 c,
    uc16 minus,
    uc16 mask,
    Label* on_not_equal) {
  ASSERT(minus < String::kMaxUtf16CodeUnit);
  __ leal(rax, Operand(current_character(), -minus));
  __ andl(rax, Immediate(mask));
  __ cmpl(rax, Immediate(c));
  BranchOrBacktrack(not_equal, on_not_equal);
}


void RegExpMacroAssemblerX32::CheckCharacterInRange(
    uc16 from,
    uc16 to,
    Label* on_in_range) {
  __ leal(rax, Operand(current_character(), -from));
  __ cmpl(rax, Immediate(to - from));
  BranchOrBacktrack(below_equal, on_in_range);
}


void RegExpMacroAssemblerX32::CheckCharacterNotInRange(
    uc16 from,
    uc16 to,
    Label* on_not_in_range) {
  __ leal(rax, Operand(current_character(), -from));
  __ cmpl(rax, Immediate(to - from));
  BranchOrBacktrack(above, on_not_in_range);
}


void RegExpMacroAssemblerX32::CheckBitInTable(
    Handle<ByteArray> table,
    Label* on_bit_set) {
  __ Move(rax, table);
  Register index = current_character();
  if (mode_ != ASCII || kTableMask != String::kMaxOneByteCharCode) {
    __ movl(rbx, current_character());
    __ andl(rbx, Immediate(kTableMask));
    index = rbx;
  }
  __ cmpb(FieldOperand(rax, index, times_1, ByteArray::kHeaderSize),
          Immediate(0));
  BranchOrBacktrack(not_equal, on_bit_set);
}


bool RegExpMacroAssemblerX32::CheckSpecialCharacterClass(uc16 type,
                                                         Label* on_no_match) {
  // Range checks (c in min..max) are generally implemented by an unsigned
  // (c - min) <= (max - min) check, using the sequence:
  //   leal(rax, Operand(current_character(), -min)) or sub(rax, Immediate(min))
  //   cmp(rax, Immediate(max - min))
  switch (type) {
  case 's':
    // Match space-characters
    if (mode_ == ASCII) {
      // One byte space characters are '\t'..'\r', ' ' and \u00a0.
      Label success;
      __ cmpl(current_character(), Immediate(' '));
      __ j(equal, &success, Label::kNear);
      // Check range 0x09..0x0d
      __ leal(rax, Operand(current_character(), -'\t'));
      __ cmpl(rax, Immediate('\r' - '\t'));
      __ j(below_equal, &success, Label::kNear);
      // \u00a0 (NBSP).
      __ cmpl(rax, Immediate(0x00a0 - '\t'));
      BranchOrBacktrack(not_equal, on_no_match);
      __ bind(&success);
      return true;
    }
    return false;
  case 'S':
    // The emitted code for generic character classes is good enough.
    return false;
  case 'd':
    // Match ASCII digits ('0'..'9')
    __ leal(rax, Operand(current_character(), -'0'));
    __ cmpl(rax, Immediate('9' - '0'));
    BranchOrBacktrack(above, on_no_match);
    return true;
  case 'D':
    // Match non ASCII-digits
    __ leal(rax, Operand(current_character(), -'0'));
    __ cmpl(rax, Immediate('9' - '0'));
    BranchOrBacktrack(below_equal, on_no_match);
    return true;
  case '.': {
    // Match non-newlines (not 0x0a('\n'), 0x0d('\r'), 0x2028 and 0x2029)
    __ movl(rax, current_character());
    __ xorl(rax, Immediate(0x01));
    // See if current character is '\n'^1 or '\r'^1, i.e., 0x0b or 0x0c
    __ subl(rax, Immediate(0x0b));
    __ cmpl(rax, Immediate(0x0c - 0x0b));
    BranchOrBacktrack(below_equal, on_no_match);
    if (mode_ == UC16) {
      // Compare original value to 0x2028 and 0x2029, using the already
      // computed (current_char ^ 0x01 - 0x0b). I.e., check for
      // 0x201d (0x2028 - 0x0b) or 0x201e.
      __ subl(rax, Immediate(0x2028 - 0x0b));
      __ cmpl(rax, Immediate(0x2029 - 0x2028));
      BranchOrBacktrack(below_equal, on_no_match);
    }
    return true;
  }
  case 'n': {
    // Match newlines (0x0a('\n'), 0x0d('\r'), 0x2028 and 0x2029)
    __ movl(rax, current_character());
    __ xorl(rax, Immediate(0x01));
    // See if current character is '\n'^1 or '\r'^1, i.e., 0x0b or 0x0c
    __ subl(rax, Immediate(0x0b));
    __ cmpl(rax, Immediate(0x0c - 0x0b));
    if (mode_ == ASCII) {
      BranchOrBacktrack(above, on_no_match);
    } else {
      Label done;
      BranchOrBacktrack(below_equal, &done);
      // Compare original value to 0x2028 and 0x2029, using the already
      // computed (current_char ^ 0x01 - 0x0b). I.e., check for
      // 0x201d (0x2028 - 0x0b) or 0x201e.
      __ subl(rax, Immediate(0x2028 - 0x0b));
      __ cmpl(rax, Immediate(0x2029 - 0x2028));
      BranchOrBacktrack(above, on_no_match);
      __ bind(&done);
    }
    return true;
  }
  case 'w': {
    if (mode_ != ASCII) {
      // Table is 128 entries, so all ASCII characters can be tested.
      __ cmpl(current_character(), Immediate('z'));
      BranchOrBacktrack(above, on_no_match);
    }
    __ movl(rbx, ExternalReference::re_word_character_map());
    ASSERT_EQ(0, word_character_map[0]);  // Character '\0' is not a word char.
    __ testb(Operand(rbx, current_character(), times_1, 0),
             current_character());
    BranchOrBacktrack(zero, on_no_match);
    return true;
  }
  case 'W': {
    Label done;
    if (mode_ != ASCII) {
      // Table is 128 entries, so all ASCII characters can be tested.
      __ cmpl(current_character(), Immediate('z'));
      __ j(above, &done);
    }
    __ movl(rbx, ExternalReference::re_word_character_map());
    ASSERT_EQ(0, word_character_map[0]);  // Character '\0' is not a word char.
    __ testb(Operand(rbx, current_character(), times_1, 0),
             current_character());
    BranchOrBacktrack(not_zero, on_no_match);
    if (mode_ != ASCII) {
      __ bind(&done);
    }
    return true;
  }

  case '*':
    // Match any character.
    return true;
  // No custom implementation (yet): s(UC16), S(UC16).
  default:
    return false;
  }
}


void RegExpMacroAssemblerX32::Fail() {
  STATIC_ASSERT(FAILURE == 0);  // Return value for failure is zero.
  if (!global()) {
    __ Set(rax, FAILURE);
  }
  __ jmp(&exit_label_);
}


Handle<HeapObject> RegExpMacroAssemblerX32::GetCode(Handle<String> source) {
  Label return_rax;
  // Finalize code - write the entry point code now we know how many
  // registers we need.
  // Entry code:
  __ bind(&entry_label_);

  // Tell the system that we have a stack frame.  Because the type is MANUAL, no
  // is generated.
  FrameScope scope(&masm_, StackFrame::MANUAL);

  // Actually emit code to start a new stack frame.
  __ push(rbp);
  __ movl(rbp, rsp);
  // Save parameters and callee-save registers. Order here should correspond
  //  to order of kBackup_ebx etc.
#ifdef _WIN64
  // MSVC passes arguments in rcx, rdx, r8, r9, with backing stack slots.
  // Store register parameters in pre-allocated stack slots,
  __ movq(Operand(rbp, kInputString), rcx);
  __ movq(Operand(rbp, kStartIndex), rdx);  // Passed as int32 in edx.
  __ movq(Operand(rbp, kInputStart), r8);
  __ movq(Operand(rbp, kInputEnd), r9);
  // Callee-save on Win64.
  __ push(rsi);
  __ push(rdi);
  __ push(rbx);
#else
  // GCC passes arguments in rdi, rsi, rdx, rcx, r8, r9 (and then on stack).
  // Push register parameters on stack for reference.
  ASSERT_EQ(kInputString, -1 * kRegisterSize);
  ASSERT_EQ(kStartIndex, -2 * kRegisterSize);
  ASSERT_EQ(kInputStart, -3 * kRegisterSize);
  ASSERT_EQ(kInputEnd, -4 * kRegisterSize);
  ASSERT_EQ(kRegisterOutput, -5 * kRegisterSize);
  ASSERT_EQ(kNumOutputRegisters, -6 * kRegisterSize);
  __ push(rdi);
  __ push(rsi);
  __ push(rdx);
  __ push(rcx);
  __ push(r8);
  __ push(r9);

  __ push(rbx);  // Callee-save
#endif

  __ Push(Immediate(0));  // Number of successful matches in a global regexp.
  __ Push(Immediate(0));  // Make room for "input start - 1" constant.

  // Check if we have space on the stack for registers.
  Label stack_limit_hit;
  Label stack_ok;

  ExternalReference stack_limit =
      ExternalReference::address_of_stack_limit(isolate());
  __ movl(rcx, rsp);
  __ movl(kScratchRegister, stack_limit);
  __ subl(rcx, Operand(kScratchRegister, 0));
  // Handle it if the stack pointer is already below the stack limit.
  __ j(below_equal, &stack_limit_hit);
  // Check if there is room for the variable number of registers above
  // the stack limit.
  __ cmpl(rcx, Immediate(num_registers_ * kPointerSize));
  __ j(above_equal, &stack_ok);
  // Exit with OutOfMemory exception. There is not enough space on the stack
  // for our working registers.
  __ Set(rax, EXCEPTION);
  __ jmp(&return_rax);

  __ bind(&stack_limit_hit);
  __ Move(code_object_pointer(), masm_.CodeObject());
  CallCheckStackGuardState();  // Preserves no registers beside rbp and rsp.
  __ testl(rax, rax);
  // If returned value is non-zero, we exit with the returned value as result.
  __ j(not_zero, &return_rax);

  __ bind(&stack_ok);

  // Allocate space on stack for registers.
  __ subl(rsp, Immediate(num_registers_ * kPointerSize));
  // Load string length.
  __ movl(rsi, Operand(rbp, kInputEnd));
  // Load input position.
  __ movl(rdi, Operand(rbp, kInputStart));
  // Set up rdi to be negative offset from string end.
  __ subq(rdi, rsi);
  // Set rax to address of char before start of the string
  // (effectively string position -1).
  __ movl(rbx, Operand(rbp, kStartIndex));
  __ neg(rbx);
  if (mode_ == UC16) {
    __ leal(rax, Operand(rdi, rbx, times_2, -char_size()));
  } else {
    __ leal(rax, Operand(rdi, rbx, times_1, -char_size()));
  }
  // Store this value in a local variable, for use when clearing
  // position registers.
  __ movl(Operand(rbp, kInputStartMinusOne), rax);

#if V8_OS_WIN
  // Ensure that we have written to each stack page, in order. Skipping a page
  // on Windows can cause segmentation faults. Assuming page size is 4k.
  const int kPageSize = 4096;
  const int kRegistersPerPage = kPageSize / kPointerSize;
  for (int i = num_saved_registers_ + kRegistersPerPage - 1;
      i < num_registers_;
      i += kRegistersPerPage) {
    __ movl(register_location(i), rax);  // One write every page.
  }
#endif  // V8_OS_WIN

  // Initialize code object pointer.
  __ Move(code_object_pointer(), masm_.CodeObject());

  Label load_char_start_regexp, start_regexp;
  // Load newline if index is at start, previous character otherwise.
  __ cmpl(Operand(rbp, kStartIndex), Immediate(0));
  __ j(not_equal, &load_char_start_regexp, Label::kNear);
  __ Set(current_character(), '\n');
  __ jmp(&start_regexp, Label::kNear);

  // Global regexp restarts matching here.
  __ bind(&load_char_start_regexp);
  // Load previous char as initial value of current character register.
  LoadCurrentCharacterUnchecked(-1, 1);
  __ bind(&start_regexp);

  // Initialize on-stack registers.
  if (num_saved_registers_ > 0) {
    // Fill saved registers with initial value = start offset - 1
    // Fill in stack push order, to avoid accessing across an unwritten
    // page (a problem on Windows).
    if (num_saved_registers_ > 8) {
      __ Set(rcx, kRegisterZero);
      Label init_loop;
      __ bind(&init_loop);
      __ movl(Operand(rbp, rcx, times_1, 0), rax);
      __ subq(rcx, Immediate(kPointerSize));
      __ cmpq(rcx,
              Immediate(kRegisterZero - num_saved_registers_ * kPointerSize));
      __ j(greater, &init_loop);
    } else {  // Unroll the loop.
      for (int i = 0; i < num_saved_registers_; i++) {
        __ movl(register_location(i), rax);
      }
    }
  }

  // Initialize backtrack stack pointer.
  __ movl(backtrack_stackpointer(), Operand(rbp, kStackHighEnd));

  __ jmp(&start_label_);

  // Exit code:
  if (success_label_.is_linked()) {
    // Save captures when successful.
    __ bind(&success_label_);
    if (num_saved_registers_ > 0) {
      // copy captures to output
      __ movl(rdx, Operand(rbp, kStartIndex));
      __ movl(rbx, Operand(rbp, kRegisterOutput));
      __ movl(rcx, Operand(rbp, kInputEnd));
      __ subl(rcx, Operand(rbp, kInputStart));
      if (mode_ == UC16) {
        __ leal(rcx, Operand(rcx, rdx, times_2, 0));
      } else {
        __ addl(rcx, rdx);
      }
      for (int i = 0; i < num_saved_registers_; i++) {
        __ movsxlq(rax, register_location(i));
        if (i == 0 && global_with_zero_length_check()) {
          // Keep capture start in rdx for the zero-length check later.
          __ movl(rdx, rax);
        }
        __ addl(rax, rcx);  // Convert to index from start, not end.
        if (mode_ == UC16) {
          __ sarl(rax, Immediate(1));  // Convert byte index to character index.
        }
        __ movl(Operand(rbx, i * kIntSize), rax);
      }
    }

    if (global()) {
      // Restart matching if the regular expression is flagged as global.
      // Increment success counter.
      __ incl(Operand(rbp, kSuccessfulCaptures));
      // Capture results have been stored, so the number of remaining global
      // output registers is reduced by the number of stored captures.
      __ movsxlq(rcx, Operand(rbp, kNumOutputRegisters));
      __ subl(rcx, Immediate(num_saved_registers_));
      // Check whether we have enough room for another set of capture results.
      __ cmpl(rcx, Immediate(num_saved_registers_));
      __ j(less, &exit_label_);

      __ movl(Operand(rbp, kNumOutputRegisters), rcx);
      // Advance the location for output.
      __ addl(Operand(rbp, kRegisterOutput),
              Immediate(num_saved_registers_ * kIntSize));

      // Prepare rax to initialize registers with its value in the next run.
      __ movl(rax, Operand(rbp, kInputStartMinusOne));

      if (global_with_zero_length_check()) {
        // Special case for zero-length matches.
        // rdx: capture start index
        __ cmpl(rdi, rdx);
        // Not a zero-length match, restart.
        __ j(not_equal, &load_char_start_regexp);
        // rdi (offset from the end) is zero if we already reached the end.
        __ testl(rdi, rdi);
        __ j(zero, &exit_label_, Label::kNear);
        // Advance current position after a zero-length match.
        if (mode_ == UC16) {
          __ addq(rdi, Immediate(2));
        } else {
          __ incq(rdi);
        }
      }

      __ jmp(&load_char_start_regexp);
    } else {
      __ movl(rax, Immediate(SUCCESS));
    }
  }

  __ bind(&exit_label_);
  if (global()) {
    // Return the number of successful captures.
    __ movl(rax, Operand(rbp, kSuccessfulCaptures));
  }

  __ bind(&return_rax);
#ifdef _WIN64
  // Restore callee save registers.
  __ leal(rsp, Operand(rbp, kLastCalleeSaveRegister));
  __ pop(rbx);
  __ pop(rdi);
  __ pop(rsi);
  // Stack now at rbp.
#else
  // Restore callee save register.
  __ movl(rbx, Operand(rbp, kBackup_rbx));
  // Skip rsp to rbp.
  __ movl(rsp, rbp);
#endif
  // Exit function frame, restore previous one.
  __ pop(rbp);
  __ ret(0);

  // Backtrack code (branch target for conditional backtracks).
  if (backtrack_label_.is_linked()) {
    __ bind(&backtrack_label_);
    Backtrack();
  }

  Label exit_with_exception;

  // Preempt-code
  if (check_preempt_label_.is_linked()) {
    SafeCallTarget(&check_preempt_label_);

    __ push(backtrack_stackpointer());
    __ push(rdi);

    CallCheckStackGuardState();
    __ testl(rax, rax);
    // If returning non-zero, we should end execution with the given
    // result as return value.
    __ j(not_zero, &return_rax);

    // Restore registers.
    __ Move(code_object_pointer(), masm_.CodeObject());
    __ pop(rdi);
    __ pop(backtrack_stackpointer());
    // String might have moved: Reload esi from frame.
    __ movl(rsi, Operand(rbp, kInputEnd));
    SafeReturn();
  }

  // Backtrack stack overflow code.
  if (stack_overflow_label_.is_linked()) {
    SafeCallTarget(&stack_overflow_label_);
    // Reached if the backtrack-stack limit has been hit.

    Label grow_failed;
    // Save registers before calling C function
#ifndef _WIN64
    // Callee-save in Microsoft 64-bit ABI, but not in AMD64 ABI.
    __ push(rsi);
    __ push(rdi);
#endif

    // Call GrowStack(backtrack_stackpointer())
    static const int num_arguments = 3;
    __ PrepareCallCFunction(num_arguments);
#ifdef _WIN64
    // Microsoft passes parameters in rcx, rdx, r8.
    // First argument, backtrack stackpointer, is already in rcx.
    __ leal(rdx, Operand(rbp, kStackHighEnd));  // Second argument
    __ LoadAddress(r8, ExternalReference::isolate_address(isolate()));
#else
    // AMD64 ABI passes parameters in rdi, rsi, rdx.
    __ movl(rdi, backtrack_stackpointer());   // First argument.
    __ leal(rsi, Operand(rbp, kStackHighEnd));  // Second argument.
    __ LoadAddress(rdx, ExternalReference::isolate_address(isolate()));
#endif
    ExternalReference grow_stack =
        ExternalReference::re_grow_stack(isolate());
    __ CallCFunction(grow_stack, num_arguments);
    // If return NULL, we have failed to grow the stack, and
    // must exit with a stack-overflow exception.
    __ testl(rax, rax);
    __ j(equal, &exit_with_exception);
    // Otherwise use return value as new stack pointer.
    __ movl(backtrack_stackpointer(), rax);
    // Restore saved registers and continue.
    __ Move(code_object_pointer(), masm_.CodeObject());
#ifndef _WIN64
    __ pop(rdi);
    __ pop(rsi);
#endif
    SafeReturn();
  }

  if (exit_with_exception.is_linked()) {
    // If any of the code above needed to exit with an exception.
    __ bind(&exit_with_exception);
    // Exit with Result EXCEPTION(-1) to signal thrown exception.
    __ Set(rax, EXCEPTION);
    __ jmp(&return_rax);
  }

  FixupCodeRelativePositions();

  CodeDesc code_desc;
  masm_.GetCode(&code_desc);
  Isolate* isolate = ISOLATE;
  Handle<Code> code = isolate->factory()->NewCode(
      code_desc, Code::ComputeFlags(Code::REGEXP),
      masm_.CodeObject());
  PROFILE(isolate, RegExpCodeCreateEvent(*code, *source));
  return Handle<HeapObject>::cast(code);
}


void RegExpMacroAssemblerX32::GoTo(Label* to) {
  BranchOrBacktrack(no_condition, to);
}


void RegExpMacroAssemblerX32::IfRegisterGE(int reg,
                                           int comparand,
                                           Label* if_ge) {
  __ cmpl(register_location(reg), Immediate(comparand));
  BranchOrBacktrack(greater_equal, if_ge);
}


void RegExpMacroAssemblerX32::IfRegisterLT(int reg,
                                           int comparand,
                                           Label* if_lt) {
  __ cmpl(register_location(reg), Immediate(comparand));
  BranchOrBacktrack(less, if_lt);
}


void RegExpMacroAssemblerX32::IfRegisterEqPos(int reg,
                                              Label* if_eq) {
  __ cmpl(rdi, register_location(reg));
  BranchOrBacktrack(equal, if_eq);
}


RegExpMacroAssembler::IrregexpImplementation
    RegExpMacroAssemblerX32::Implementation() {
  return kX32Implementation;
}


void RegExpMacroAssemblerX32::LoadCurrentCharacter(int cp_offset,
                                                   Label* on_end_of_input,
                                                   bool check_bounds,
                                                   int characters) {
  ASSERT(cp_offset >= -1);      // ^ and \b can look behind one character.
  ASSERT(cp_offset < (1<<30));  // Be sane! (And ensure negation works)
  if (check_bounds) {
    CheckPosition(cp_offset + characters - 1, on_end_of_input);
  }
  LoadCurrentCharacterUnchecked(cp_offset, characters);
}


void RegExpMacroAssemblerX32::PopCurrentPosition() {
  Pop(rdi);
}


void RegExpMacroAssemblerX32::PopRegister(int register_index) {
  Pop(rax);
  __ movl(register_location(register_index), rax);
}


void RegExpMacroAssemblerX32::PushBacktrack(Label* label) {
  Push(label);
  CheckStackLimit();
}


void RegExpMacroAssemblerX32::PushCurrentPosition() {
  Push(rdi);
}


void RegExpMacroAssemblerX32::PushRegister(int register_index,
                                           StackCheckFlag check_stack_limit) {
  __ movl(rax, register_location(register_index));
  Push(rax);
  if (check_stack_limit) CheckStackLimit();
}


void RegExpMacroAssemblerX32::ReadCurrentPositionFromRegister(int reg) {
  __ movsxlq(rdi, register_location(reg));
}


void RegExpMacroAssemblerX32::ReadStackPointerFromRegister(int reg) {
  __ movsxlq(backtrack_stackpointer(), register_location(reg));
  __ addl(backtrack_stackpointer(), Operand(rbp, kStackHighEnd));
}


void RegExpMacroAssemblerX32::SetCurrentPositionFromEnd(int by) {
  Label after_position;
  __ cmpl(rdi, Immediate(-by * char_size()));
  __ j(greater_equal, &after_position, Label::kNear);
  __ movq(rdi, Immediate(-by * char_size()));
  // On RegExp code entry (where this operation is used), the character before
  // the current position is expected to be already loaded.
  // We have advanced the position, so it's safe to read backwards.
  LoadCurrentCharacterUnchecked(-1, 1);
  __ bind(&after_position);
}


void RegExpMacroAssemblerX32::SetRegister(int register_index, int to) {
  ASSERT(register_index >= num_saved_registers_);  // Reserved for positions!
  __ movl(register_location(register_index), Immediate(to));
}


bool RegExpMacroAssemblerX32::Succeed() {
  __ jmp(&success_label_);
  return global();
}


void RegExpMacroAssemblerX32::WriteCurrentPositionToRegister(int reg,
                                                             int cp_offset) {
  if (cp_offset == 0) {
    __ movl(register_location(reg), rdi);
  } else {
    __ leal(rax, Operand(rdi, cp_offset * char_size()));
    __ movl(register_location(reg), rax);
  }
}


void RegExpMacroAssemblerX32::ClearRegisters(int reg_from, int reg_to) {
  ASSERT(reg_from <= reg_to);
  __ movl(rax, Operand(rbp, kInputStartMinusOne));
  for (int reg = reg_from; reg <= reg_to; reg++) {
    __ movl(register_location(reg), rax);
  }
}


void RegExpMacroAssemblerX32::WriteStackPointerToRegister(int reg) {
  __ movl(rax, backtrack_stackpointer());
  __ subl(rax, Operand(rbp, kStackHighEnd));
  __ movl(register_location(reg), rax);
}


// Private methods:

void RegExpMacroAssemblerX32::CallCheckStackGuardState() {
  // This function call preserves no register values. Caller should
  // store anything volatile in a C call or overwritten by this function.
  static const int num_arguments = 3;
  __ PrepareCallCFunction(num_arguments);
#ifdef _WIN64
  // Second argument: Code* of self. (Do this before overwriting r8).
  __ movl(rdx, code_object_pointer());
  // Third argument: RegExp code frame pointer.
  __ movl(r8, rbp);
  // First argument: Next address on the stack (will be address of
  // return address).
  __ leal(rcx, Operand(rsp, -kPointerSize));
#else
  // Third argument: RegExp code frame pointer.
  __ movl(rdx, rbp);
  // Second argument: Code* of self.
  __ movl(rsi, code_object_pointer());
  // First argument: Next address on the stack (will be address of
  // return address).
  __ leal(rdi, Operand(rsp, -kRegisterSize));
#endif
  ExternalReference stack_check =
      ExternalReference::re_check_stack_guard_state(isolate());
  __ CallCFunction(stack_check, num_arguments);
}


// Helper function for reading a value out of a stack frame.
template <typename T>
static T& frame_entry(Address re_frame, int frame_offset) {
  return reinterpret_cast<T&>(Memory::int32_at(re_frame + frame_offset));
}


int RegExpMacroAssemblerX32::CheckStackGuardState(Address* return_address,
                                                  Code* re_code,
                                                  Address re_frame) {
  Isolate* isolate = frame_entry<Isolate*>(re_frame, kIsolate);
  ASSERT(isolate == Isolate::Current());
  if (isolate->stack_guard()->IsStackOverflow()) {
    isolate->StackOverflow();
    return EXCEPTION;
  }

  // If not real stack overflow the stack guard was used to interrupt
  // execution for another purpose.

  // If this is a direct call from JavaScript retry the RegExp forcing the call
  // through the runtime system. Currently the direct call cannot handle a GC.
  if (frame_entry<int>(re_frame, kDirectCall) == 1) {
    return RETRY;
  }

  // Prepare for possible GC.
  HandleScope handles(isolate);
  Handle<Code> code_handle(re_code);

  Handle<String> subject(frame_entry<String*>(re_frame, kInputString));

  // Current string.
  bool is_ascii = subject->IsOneByteRepresentationUnderneath();

  ASSERT(re_code->instruction_start() <= *return_address);
  ASSERT(*return_address <=
      re_code->instruction_start() + re_code->instruction_size());

  MaybeObject* result = Execution::HandleStackGuardInterrupt(isolate);

  if (*code_handle != re_code) {  // Return address no longer valid
    intptr_t delta = code_handle->address() - re_code->address();
    // Overwrite the return address on the stack.
    *return_address += delta;
  }

  if (result->IsException()) {
    return EXCEPTION;
  }

  Handle<String> subject_tmp = subject;
  int slice_offset = 0;

  // Extract the underlying string and the slice offset.
  if (StringShape(*subject_tmp).IsCons()) {
    subject_tmp = Handle<String>(ConsString::cast(*subject_tmp)->first());
  } else if (StringShape(*subject_tmp).IsSliced()) {
    SlicedString* slice = SlicedString::cast(*subject_tmp);
    subject_tmp = Handle<String>(slice->parent());
    slice_offset = slice->offset();
  }

  // String might have changed.
  if (subject_tmp->IsOneByteRepresentation() != is_ascii) {
    // If we changed between an ASCII and an UC16 string, the specialized
    // code cannot be used, and we need to restart regexp matching from
    // scratch (including, potentially, compiling a new version of the code).
    return RETRY;
  }

  // Otherwise, the content of the string might have moved. It must still
  // be a sequential or external string with the same content.
  // Update the start and end pointers in the stack frame to the current
  // location (whether it has actually moved or not).
  ASSERT(StringShape(*subject_tmp).IsSequential() ||
      StringShape(*subject_tmp).IsExternal());

  // The original start address of the characters to match.
  const byte* start_address = frame_entry<const byte*>(re_frame, kInputStart);

  // Find the current start address of the same character at the current string
  // position.
  int start_index = frame_entry<int>(re_frame, kStartIndex);
  const byte* new_address = StringCharacterPosition(*subject_tmp,
                                                    start_index + slice_offset);

  if (start_address != new_address) {
    // If there is a difference, update the object pointer and start and end
    // addresses in the RegExp stack frame to match the new value.
    const byte* end_address = frame_entry<const byte* >(re_frame, kInputEnd);
    int byte_length = static_cast<int>(end_address - start_address);
    frame_entry<const String*>(re_frame, kInputString) = *subject;
    frame_entry<const byte*>(re_frame, kInputStart) = new_address;
    frame_entry<const byte*>(re_frame, kInputEnd) = new_address + byte_length;
  } else if (frame_entry<const String*>(re_frame, kInputString) != *subject) {
    // Subject string might have been a ConsString that underwent
    // short-circuiting during GC. That will not change start_address but
    // will change pointer inside the subject handle.
    frame_entry<const String*>(re_frame, kInputString) = *subject;
  }

  return 0;
}


Operand RegExpMacroAssemblerX32::register_location(int register_index) {
  ASSERT(register_index < (1<<30));
  if (num_registers_ <= register_index) {
    num_registers_ = register_index + 1;
  }
  return Operand(rbp, kRegisterZero - register_index * kPointerSize);
}


void RegExpMacroAssemblerX32::CheckPosition(int cp_offset,
                                            Label* on_outside_input) {
  __ cmpl(rdi, Immediate(-cp_offset * char_size()));
  BranchOrBacktrack(greater_equal, on_outside_input);
}


void RegExpMacroAssemblerX32::BranchOrBacktrack(Condition condition,
                                                Label* to) {
  if (condition < 0) {  // No condition
    if (to == NULL) {
      Backtrack();
      return;
    }
    __ jmp(to);
    return;
  }
  if (to == NULL) {
    __ j(condition, &backtrack_label_);
    return;
  }
  __ j(condition, to);
}


void RegExpMacroAssemblerX32::SafeCall(Label* to) {
  __ call(to);
}


void RegExpMacroAssemblerX32::SafeCallTarget(Label* label) {
  __ bind(label);
  __ subl(Operand(rsp, 0), code_object_pointer());
}


void RegExpMacroAssemblerX32::SafeReturn() {
  __ addl(Operand(rsp, 0), code_object_pointer());
  __ ret(0);
}


void RegExpMacroAssemblerX32::Push(Register source) {
  ASSERT(!source.is(backtrack_stackpointer()));
  // Notice: This updates flags, unlike normal Push.
  __ subl(backtrack_stackpointer(), Immediate(kIntSize));
  __ movl(Operand(backtrack_stackpointer(), 0), source);
}


void RegExpMacroAssemblerX32::Push(Immediate value) {
  // Notice: This updates flags, unlike normal Push.
  __ subl(backtrack_stackpointer(), Immediate(kIntSize));
  __ movl(Operand(backtrack_stackpointer(), 0), value);
}


void RegExpMacroAssemblerX32::FixupCodeRelativePositions() {
  for (int i = 0, n = code_relative_fixup_positions_.length(); i < n; i++) {
    int position = code_relative_fixup_positions_[i];
    // The position succeeds a relative label offset from position.
    // Patch the relative offset to be relative to the Code object pointer
    // instead.
    int patch_position = position - kIntSize;
    int offset = masm_.long_at(patch_position);
    masm_.long_at_put(patch_position,
                       offset
                       + position
                       + Code::kHeaderSize
                       - kHeapObjectTag);
  }
  code_relative_fixup_positions_.Clear();
}


void RegExpMacroAssemblerX32::Push(Label* backtrack_target) {
  __ subl(backtrack_stackpointer(), Immediate(kIntSize));
  __ movl(Operand(backtrack_stackpointer(), 0), backtrack_target);
  MarkPositionForCodeRelativeFixup();
}


void RegExpMacroAssemblerX32::Pop(Register target) {
  ASSERT(!target.is(backtrack_stackpointer()));
  __ movsxlq(target, Operand(backtrack_stackpointer(), 0));
  // Notice: This updates flags, unlike normal Pop.
  __ addl(backtrack_stackpointer(), Immediate(kIntSize));
}


void RegExpMacroAssemblerX32::Drop() {
  __ addl(backtrack_stackpointer(), Immediate(kIntSize));
}


void RegExpMacroAssemblerX32::CheckPreemption() {
  // Check for preemption.
  Label no_preempt;
  ExternalReference stack_limit =
      ExternalReference::address_of_stack_limit(isolate());
  __ load_rax(stack_limit);
  __ cmpl(rsp, rax);
  __ j(above, &no_preempt);

  SafeCall(&check_preempt_label_);

  __ bind(&no_preempt);
}


void RegExpMacroAssemblerX32::CheckStackLimit() {
  Label no_stack_overflow;
  ExternalReference stack_limit =
      ExternalReference::address_of_regexp_stack_limit(isolate());
  __ load_rax(stack_limit);
  __ cmpl(backtrack_stackpointer(), rax);
  __ j(above, &no_stack_overflow);

  SafeCall(&stack_overflow_label_);

  __ bind(&no_stack_overflow);
}


void RegExpMacroAssemblerX32::LoadCurrentCharacterUnchecked(int cp_offset,
                                                            int characters) {
  if (mode_ == ASCII) {
    if (characters == 4) {
      __ movl(current_character(), Operand(rsi, rdi, times_1, cp_offset));
    } else if (characters == 2) {
      __ movzxwl(current_character(), Operand(rsi, rdi, times_1, cp_offset));
    } else {
      ASSERT(characters == 1);
      __ movzxbl(current_character(), Operand(rsi, rdi, times_1, cp_offset));
    }
  } else {
    ASSERT(mode_ == UC16);
    if (characters == 2) {
      __ movl(current_character(),
              Operand(rsi, rdi, times_1, cp_offset * sizeof(uc16)));
    } else {
      ASSERT(characters == 1);
      __ movzxwl(current_character(),
                 Operand(rsi, rdi, times_1, cp_offset * sizeof(uc16)));
    }
  }
}

#undef __

#endif  // V8_INTERPRETED_REGEXP

}}  // namespace v8::internal

#endif  // V8_TARGET_ARCH_X32
