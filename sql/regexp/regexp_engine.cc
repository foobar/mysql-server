/* Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/regexp/regexp_engine.h"

#include <stdint.h>

#include <algorithm>  // copy
#include <string>  // strlen

#include "my_dbug.h"
#include "my_pointer_arithmetic.h"  // is_aligned, is_aligned_to
#include "nullable.h"
#include "sql/regexp/errors.h"
#include "sql/regexp/regexp_facade.h"
#include "sql/sql_class.h"
#include "sql_string.h"

namespace regexp {

UBool QueryNotKilled(const void *thd, int32_t) {
  return !static_cast<const THD*>(thd)->is_killed();
}

const char *icu_version_string() { return U_ICU_VERSION; }

bool Regexp_engine::Reset(String *subject) {
  auto usubject = pointer_cast<const UChar *>(subject->ptr());
  int length = subject->length() / sizeof(UChar);

  DBUG_ASSERT(is_aligned(usubject));
  DBUG_ASSERT(subject->charset() == regexp_lib_charset);
  uregex_setText(m_re, usubject, length, &m_error_code);
  m_current_subject = subject;
  return false;
}

bool Regexp_engine::Matches(int start, int occurrence) {
  bool found = uregex_find(m_re, start, &m_error_code);

  for (int i = 1; i < occurrence && found; ++i)
    found = uregex_findNext(m_re, &m_error_code);

  check_icu_status(m_error_code);
  return found;
}

String *Regexp_engine::Replace(const char *replacement, int length,
                               int start, int occurrence, String *result) {
  // Find the first match, starting at the chosen position, ...
  bool found = uregex_find(m_re, start, &m_error_code);

  int end_of_previous_match = -1;
  // ... fast-forward to the chosen occurrence, ...
  for (int i = 1; i < occurrence && found; ++i) {
    end_of_previous_match = uregex_end(m_re, 0, &m_error_code);
    found = uregex_findNext(m_re, &m_error_code);
  }

  /*
    If no match is found, the return value is the same as the subject
    string. Since `found` is false, this is what would eventually be produced
    by falling through to the call to uregex_appendTail() below anyway. This
    is more than just premature optimization, however; according to the ICU
    documentation, calls to uregex_appendReplacement() and uregex_appendTail()
    are meant to be chained, and a call to uregex_appendTail() without a prior
    call to uregex_appendReplacement() leads to ICU trying to free the buffer
    that we own, thus causing a double-delete.
  */
  if (!found && m_error_code == U_ZERO_ERROR) return m_current_subject;

  auto ureplacement = pointer_cast<const UChar *>(replacement);

  // ... replacing all occurrences if 'occurrence' is 0, and finally ...
  AppendHead(end_of_previous_match, start);
  if (found) {
    do {
      AppendReplacement(ureplacement, length / sizeof(UChar));
    }
    while (occurrence == 0 && uregex_findNext(m_re, &m_error_code));
  }

  // ... put the part after the matches back.
  AppendTail();

  check_icu_status(m_error_code);

  result->set(pointer_cast<const char *>(&m_replace_buffer[0].the_char),
              m_replace_buffer.size() * sizeof(UChar), regexp_lib_charset);
  result->copy();
  return result;
}

String *Regexp_engine::MatchedSubstring(String *result) {
  int start = uregex_start(m_re, 0, &m_error_code);
  int end = uregex_end(m_re, 0, &m_error_code);
  auto text =
      pointer_cast<const char *>(uregex_getText(m_re, nullptr, &m_error_code));
  int start_in_bytes = start * sizeof(UChar);
  int length_in_bytes = (end - start) * sizeof(UChar);

  if (U_FAILURE(m_error_code)) return nullptr;
  /*
    The ownership of the text was with us all along, we can safely pass it to
    `result`.
  */
  result->set(text + start_in_bytes, length_in_bytes, regexp_lib_charset);

  return result;
}

void Regexp_engine::AppendHead(int end_of_previous_match, int start_of_search) {
  DBUG_ENTER("Regexp_engine::AppendHead");
  /*
    We're not supposed to find matches before the place where we started the
    search. If we do, something went awry.
  */
  DBUG_ASSERT(end_of_previous_match == -1 ||
              start_of_search <= end_of_previous_match);

  // This won't be written to in case of errors.
  int text_length = 0;
  auto text = uregex_getText(m_re, &text_length, &m_error_code);

  // We make sure we are not in an error state before we start copying.
  if (m_error_code != U_ZERO_ERROR) DBUG_VOID_RETURN;

  m_replace_buffer.reserve(std::min(text_length + 1, HardLimit()));

  /*
    The part we have to worry about here, the part that ICU doesn't add for
    us is, is if the search didn't start on the first character or first
    match for the regular expression. It's the longest such prefix that we
    have to copy ourselves.
  */
  int head_size = std::max(start_of_search, end_of_previous_match);

  DBUG_ASSERT(head_size <= text_length);
  std::copy(text, text + head_size, &m_replace_buffer[0].the_char);
  if (head_size > 0) m_replace_buffer.resize(head_size);

  DBUG_VOID_RETURN;
}

void Regexp_engine::AppendReplacement(const UChar *replacement, size_t length) {
  DBUG_ENTER("Regexp_engine::AppendReplacement");

  int replacement_size = TryToAppendReplacement(replacement, length);

  if (m_error_code == U_BUFFER_OVERFLOW_ERROR) {
    int required_buffer_size = m_replace_buffer.size() + replacement_size;
    if (required_buffer_size >= HardLimit()) DBUG_VOID_RETURN;
    /*
      The buffer size was inadequate to write the replacement, but there is
      still room to try and grow the buffer before we hit the hard limit. ICU
      will now have set capacity to zero, m_replace_buffer to the newly
      allocated buffer, and m_error_code to U_BUFFER_OVERFLOW_ERROR. So we try
      once again, by resetting these values after reserving the extra space in
      the buffer.
    */
    m_replace_buffer.reserve(required_buffer_size);
    m_error_code = U_ZERO_ERROR;
    TryToAppendReplacement(replacement, length);
    /*
      The string gets terminated in AppendTail(), which should always be
      called. No need to worry about that here.
    */
    DBUG_ASSERT(m_error_code == U_ZERO_ERROR ||
                m_error_code == U_STRING_NOT_TERMINATED_WARNING);
  }

  m_replace_buffer.resize(m_replace_buffer.size() + replacement_size);

  DBUG_VOID_RETURN;
}

void Regexp_engine::AppendTail() {
  DBUG_ENTER("Regexp_engine::AppendTail");
  int tail_size = TryToAppendTail() + 1;

  if (m_error_code == U_BUFFER_OVERFLOW_ERROR ||
      m_error_code == U_STRING_NOT_TERMINATED_WARNING) {
    /*
      We add a zero terminator for the string, even though strictly speaking,
      we shouldn't have to.
    */
    int required_buffer_size = m_replace_buffer.size() + tail_size;
    if (required_buffer_size >= HardLimit()) DBUG_VOID_RETURN;

    /*
      The buffer size was inadequate to write the tail, but there is still
      room to try and grow the buffer before we hit the hard limit. ICU will
      now have worked in preflight mode, i.e. it has set capacity to zero and
      m_error_code to U_BUFFER_OVERFLOW_ERROR or
      U_STRING_NOT_TERMINATED_WARNING. So we try once again, by resetting
      these values after reserving the extra space in the buffer.
    */
    m_replace_buffer.reserve(required_buffer_size);
    m_error_code = U_ZERO_ERROR;
    TryToAppendTail();
    DBUG_ASSERT(m_error_code == U_ZERO_ERROR);
  }
  m_replace_buffer.resize(m_replace_buffer.size() + tail_size - 1);

  DBUG_VOID_RETURN;
}

}  // namespace regexp
