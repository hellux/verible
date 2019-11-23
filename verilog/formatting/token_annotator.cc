// Copyright 2017-2019 The Verible Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "verilog/formatting/token_annotator.h"

#include <iterator>
#include <vector>

#include "absl/strings/string_view.h"
#include "common/formatting/format_token.h"
#include "common/formatting/tree_annotator.h"
#include "common/strings/range.h"
#include "common/text/syntax_tree_context.h"
#include "common/text/text_structure.h"
#include "common/text/token_info.h"
#include "common/util/iterator_range.h"
#include "common/util/logging.h"
#include "verilog/CST/verilog_nonterminals.h"
#include "verilog/formatting/format_style.h"
#include "verilog/formatting/verilog_token.h"
#include "verilog/parser/verilog_parser.h"
#include "verilog/parser/verilog_token_enum.h"

namespace verilog {
namespace formatter {

using ::verible::PreFormatToken;
using ::verible::SpacingOptions;
using ::verible::SyntaxTreeContext;
using FTT = FormatTokenType;

// In functions with priority-ordered return statements, this helps with
// identifying the statement that took effect.
template <typename T>
struct WithReason {
  T value;
  const char* reason;  // A simple string literal shall suffice.
};

// Signal that spacing was not explicitly handled in case logic.
// This value must be negative.
static constexpr int kUnhandledSpacesRequired = -1;

static bool IsUnaryPrefixExpressionOperand(const PreFormatToken& left,
                                           const SyntaxTreeContext& context) {
  return IsUnaryOperator(yytokentype(left.TokenEnum())) &&
         context.IsInsideFirst({NodeEnum::kUnaryPrefixExpression},
                               {NodeEnum::kExpression});
}

// Returns minimum number of spaces required between left and right token.
// Returning kUnhandledSpacesRequired means the case was not explicitly
// handled, and it is up to the caller to decide what to do when this happens.
static WithReason<int> SpacesRequiredBetween(const PreFormatToken& left,
                                             const PreFormatToken& right,
                                             const SyntaxTreeContext& context) {
  VLOG(3) << "Spacing between " << verilog_symbol_name(left.TokenEnum())
          << " and " << verilog_symbol_name(right.TokenEnum());
  // Higher precedence rules should be handled earlier in this function.

  // Preserve space after escaped identifiers.
  if (left.TokenEnum() == EscapedIdentifier) {
    return {1, "Escaped identifiers must end with whitespace."};
  }

  if (left.format_token_enum == FormatTokenType::open_group ||
      right.format_token_enum == FormatTokenType::close_group) {
    return {0,
            "Prefer \"(foo)\" over \"( foo )\", \"[x]\" over \"[ x ]\", "
            "and \"{y}\" over \"{ y }\"."};
  }

  // For now, leave everything inside [dimensions] alone.
  if (context.IsInsideFirst(
          {NodeEnum::kDimensionRange, NodeEnum::kDimensionScalar}, {})) {
    // ... except for the spacing before '[', which is covered elsewhere.
    if (right.TokenEnum() != '[') {
      return {kUnhandledSpacesRequired,
              "Leave [expressions] inside scalar and range dimensions alone "
              "(for now)."};
    }
  }

  // Unary operators (context-sensitive)
  if (IsUnaryPrefixExpressionOperand(left, context)) {
    // TODO(b/143739545): Don't accidentally join into valid tokens, e.g.:
    //   '-' and '-' could form '--' (decrement).
    //   '-' and '=' could form '-=' (subtract-assign).
    return {0, "Bind unary prefix operator close to its operand."};
  }

  if (IsComment(FormatTokenType(right.format_token_enum))) {
    return {2, "Style: require 2+ spaces before comments"};
    // TODO(fangism): Take this from FormatStyle.
  }

  if (left.TokenEnum() == TK_SCOPE_RES) {
    return {0, "Prefer \"::id\" over \":: id\", \"::*\" over \":: *\""};
  }

  // Delimiters, list separators
  if (right.TokenEnum() == ',') return {0, "No space before comma"};
  if (left.TokenEnum() == ',') return {1, "Require space after comma"};

  if (right.TokenEnum() == ';') return {0, "No space before semicolon"};
  if (left.TokenEnum() == ';') return {1, "Require space after semicolon"};

  // Add missing space around either side of all types of assignment operator.
  // "assign foo = bar;"  instead of "assign foo =bar;"
  // Consider assignment operators in the same class as binary operators.
  if (left.format_token_enum == FormatTokenType::binary_operator ||
      right.format_token_enum == FormatTokenType::binary_operator) {
    return {1, "Space around binary and assignment operators"};
  }

  // If the token on either side is an empty string, do not inject any
  // additional spaces.  This can occur with some lexical tokens like
  // yytokentype::PP_define_body.
  if (left.token->text.empty() || right.token->text.empty()) {
    return {0, "No additional space around empty-string tokens."};
  }

  // Remove any extra spaces between numeric literals' width, base and digits.
  // "16'h123, 'h123" instead of "16 'h123", "16'h 123, 'h 123"
  if ((left.format_token_enum == FormatTokenType::numeric_literal &&
       right.format_token_enum == FormatTokenType::numeric_base) ||
      left.format_token_enum == FormatTokenType::numeric_base)
    return {0, "No space inside based numeric literals"};

  // TODO(fangism): Never insert trailing spaces before a newline.

  // Hierarchy examples: "a.b", "a::b"
  if (left.format_token_enum == FormatTokenType::hierarchy ||
      right.format_token_enum == FormatTokenType::hierarchy)
    return {0,
            "No space separating hierarchy components "
            "(separated by . or ::)"};
  // TODO(fangism): space between numeric literals and '.'
  // Don't want to accidentally form m.d floating-point values.

  // cast operator, e.g. "void'(...)"
  if (right.TokenEnum() == '\'' || left.TokenEnum() == '\'') {
    return {0, "No space around cast operator '\\''"};
  }

  if (right.TokenEnum() == '(') {
    // "#(" vs. "# (" for parameter formals and arguments
    if (left.TokenEnum() == '#') return {0, "Fuse \"#(\""};

    // "@(" vs. "@ (" for event control
    if (left.TokenEnum() == '@') return {0, "Fuse \"@(\""};

    // ") (" vs. ")(" for between parameter and port formals
    if (left.TokenEnum() == ')') {
      return {1, "Separate \") (\" between parameters and ports"};
    }

    // This case intended to cover function/task/macro calls:
    if (left.format_token_enum == FormatTokenType::identifier ||
        left.TokenEnum() == TK_new) {
      // TODO(fangism): other keywords that behave like built-in functions
      // should not have a space before the (.
      return {0, "Function/constructor calls: no space before ("};
    }
  }

  // Handle padding around packed array dimensions like "type [N] id;"
  if ((left.format_token_enum == FormatTokenType::keyword ||
       left.format_token_enum == FormatTokenType::identifier) &&
      right.TokenEnum() == '[') {
    if (context.IsInsideFirst({NodeEnum::kPackedDimensions},
                              {NodeEnum::kExpression})) {
      // "type [packed...]" (space between type and packed dimensions)
      // avoid touching any expressions inside the packed dimensions
      return {1, "spacing before [packed dimensions] of declarations"};
    }
    // All other contexts, such as "a[i]" indices, no space.
    return {0, "All other cases of \".*[\", no space"};
  }
  if (left.TokenEnum() == ']' &&
      right.format_token_enum == FormatTokenType::identifier) {
    if (context.DirectParentsAre(
            {NodeEnum::kUnqualifiedId,
             NodeEnum::kDataTypeImplicitBasicIdDimensions})) {
      // "[packed...] id" (space between packed dimensions and id)
      return {1, "spacing after [packed dimensions] of declarations"};
    }
    // Not sure if "] id" appears in any other context, so leave it unhandled.
  }

  // Keywords:

  if (right.TokenEnum() == ':') {
    if (left.TokenEnum() == TK_default) {
      return {0, "No space inside \"default:\""};
    }
  }

  // "if (...)", "for (...) instead of "if(...)", "for(...)",
  // "case ...", "return ..."
  if (left.format_token_enum == FormatTokenType::keyword) {
    // TODO(b/144605476): function-like keywords, however, do not get a space.
    return {1, "Space between flow control keywords and ("};
  }

  if (left.format_token_enum == FormatTokenType::unary_operator)
    return {0, "++i over ++ i"};  // "++i" instead of "++ i"
  if (right.format_token_enum == FormatTokenType::unary_operator)
    return {0, "i++ over i ++"};  // "i++" instead of "i ++"

  // TODO(fangism): handle ranges [ ... : ... ]

  if (left.TokenEnum() == TK_DecNumber &&
      right.TokenEnum() == TK_UnBasedNumber) {
    // e.g. 1'b1, 16'hbabe
    return {0, "No space between numeric width and un-based number"};
  }

  // Brackets in multi-dimensional arrays/indices.
  if (left.TokenEnum() == ']' && right.TokenEnum() == '[') {
    return {0, "No spaces separating multidimensional arrays/indices"};
  }

  if (left.TokenEnum() == '#') {
    return {0, "No spaces after # (delay expressions, parameters)."};
  }

  // Case was not explicitly handled.
  return {kUnhandledSpacesRequired, "Default: spacing not explicitly handled"};
}

struct SpacePolicy {
  int spaces_required;
  bool force_preserve_spaces;
};

static SpacePolicy SpacesRequiredBetween(const FormatStyle& style,
                                         const PreFormatToken& left,
                                         const PreFormatToken& right,
                                         const SyntaxTreeContext& context) {
  // Default for unhandled cases, 1 space to be conservative.
  // TODO(fangism): return a strange value like 3 once we have covered the vast
  // majority of cases above -- then omissions will stand out clearly.
  // Having this return a default value of 1 may result in mutation testing
  // reporting false positives about vacuous "return 1" statements above,
  // but this is intentional; we do not actually want to reach this this final
  // return statement most of the time (in the long run).
  constexpr int kUnhandledSpacesDefault = 1;
  const auto spaces = SpacesRequiredBetween(left, right, context);
  VLOG(1) << "spaces: " << spaces.value << ", reason: " << spaces.reason;

  // We switch on style.preserve_horizontal_spaces even though there may be
  // newlines and vertical spacing between tokens.
  if (spaces.value == kUnhandledSpacesRequired) {
    VLOG(1) << "Unhandled inter-token spacing between "
            << verilog_symbol_name(left.TokenEnum()) << " and "
            << verilog_symbol_name(right.TokenEnum()) << ", defaulting to "
            << kUnhandledSpacesDefault;
    switch (style.preserve_horizontal_spaces) {
      case PreserveSpaces::None:
        return SpacePolicy{kUnhandledSpacesDefault, false};
      case PreserveSpaces::All:
      case PreserveSpaces::UnhandledCasesOnly:
        return SpacePolicy{kUnhandledSpacesDefault, true};
    }
  } else {  // spacing was explicitly handled in a case
    switch (style.preserve_horizontal_spaces) {
      case PreserveSpaces::All:
        return SpacePolicy{spaces.value, true};
      case PreserveSpaces::None:
      case PreserveSpaces::UnhandledCasesOnly:
        return SpacePolicy{spaces.value, false};
    }
  }
  return {};  // not reached.
}

// Returns the split penalty for line-breaking before the right token.
// TODO(b/143789697): accompany return value WithReason.
static int BreakPenaltyBetween(const verible::PreFormatToken& left,
                               const verible::PreFormatToken& right) {
  // TODO(fangism): populate this.

  // Hierarchy examples: "a.b", "a::b"
  // TODO(fangism): '.' is not always hierarchy, differentiate by context.
  if (left.format_token_enum == FormatTokenType::hierarchy) return 50;
  if (right.format_token_enum == FormatTokenType::hierarchy)
    return 45;  // slightly prefer to break on the left

  // Prefer to split after commas than before them.
  if (right.TokenEnum() == ',') return 10;

  // Prefer to keep '(' with whatever is on the left.
  // TODO(fangism): ... except when () is used as precedence.
  if (right.format_token_enum == FormatTokenType::open_group) return 5;

  if (left.TokenEnum() == TK_DecNumber &&
      right.TokenEnum() == TK_UnBasedNumber) {
    // e.g. 1'b1, 16'hbabe
    return 90;  // doesn't really matter, because we never break here
  }

  // Default penalty should be non-zero to favor fitting more tokens on
  // the same line.
  // TODO(fangism): This value should come from the BasicFormatStyle.
  constexpr int kUnhandledWrapPenalty = 1;
  VLOG(1) << "Unhandled line-wrap penalty between "
          << verilog_symbol_name(left.TokenEnum()) << " and "
          << verilog_symbol_name(right.TokenEnum()) << ", defaulting to "
          << kUnhandledWrapPenalty;
  return kUnhandledWrapPenalty;
}

// Returns decision whether to break, not break, or evaluate both choices.
static WithReason<SpacingOptions> BreakDecisionBetween(
    const FormatStyle& style, const PreFormatToken& left,
    const PreFormatToken& right, const SyntaxTreeContext& context) {
  // For now, leave everything inside [dimensions] alone.
  if (context.IsInsideFirst(
          {NodeEnum::kDimensionRange, NodeEnum::kDimensionScalar}, {})) {
    // ... except for the spacing immediately around '[' and ']',
    // which is covered by other rules.
    if (left.TokenEnum() != '[' && left.TokenEnum() != ']' &&
        right.TokenEnum() != '[' && right.TokenEnum() != ']') {
      return {SpacingOptions::Preserve,
              "For now, leave spaces inside [] untouched."};
    }
  }

  // Check for mandatory line breaks.
  if (left.format_token_enum == FTT::eol_comment ||
      left.TokenEnum() == PP_define_body  // definition excludes trailing '\n'
  ) {
    return {SpacingOptions::MustWrap, "Token must be newline-terminated"};
  }
  // TODO(fangism): check for all token types in verilog.lex that
  // scan to an end-of-line, even if it returns the newline to scanning with
  // yyless().

  // Unary operators (context-sensitive)
  // For now, never separate unary prefix operators from their operands.
  if (IsUnaryPrefixExpressionOperand(left, context)) {
    return {SpacingOptions::MustAppend,
            "Never separate unary prefix operator from its operand"};
  }

  if (left.TokenEnum() == TK_DecNumber &&
      right.TokenEnum() == TK_UnBasedNumber) {
    // e.g. 1'b1, 16'hbabe
    return {SpacingOptions::MustAppend,
            "Never separate numeric width from rest of numeric literal"};
  }

  // Preprocessor macro definitions with args: no space between ID and '('.
  if (left.TokenEnum() == PP_Identifier && right.TokenEnum() == '(') {
    return {SpacingOptions::MustAppend, "No space between macro call id and ("};
  }

  // TODO(fangism): No break between `define and PP_Identifier.

  // By default, leave undecided for penalty minimization.
  return {SpacingOptions::Undecided,
          "Default: leave wrap decision to algorithm"};
}

// Sets pointers that establish substring ranges of (whitespace) text *between*
// non-whitespace tokens.
static void AnnotateOriginalSpacing(
    const char* buffer_start,
    std::vector<verible::PreFormatToken>::iterator tokens_begin,
    std::vector<verible::PreFormatToken>::iterator tokens_end) {
  VLOG(4) << __FUNCTION__;
  CHECK(buffer_start != nullptr);
  for (auto& ftoken : verible::make_range(tokens_begin, tokens_end)) {
    ftoken.before.preserved_space_start = buffer_start;
    VLOG(4) << "original spacing: \""
            << verible::make_string_view_range(buffer_start,
                                               ftoken.Text().begin())
            << "\"";
    buffer_start = ftoken.Text().end();
  }
  // This does not cover the spacing between the last token and EOF.
}

// Extern linkage for sake of direct testing, though not exposed in public
// headers.
// TODO(fangism): could move this to a -internal.h header.
void AnnotateFormatToken(const FormatStyle& style,
                         const PreFormatToken& prev_token,
                         PreFormatToken* curr_token,
                         const SyntaxTreeContext& context) {
  const auto p = SpacesRequiredBetween(style, prev_token, *curr_token, context);
  curr_token->before.spaces_required = p.spaces_required;
  if (p.force_preserve_spaces) {
    // forego all inter-token calculations
    curr_token->before.break_decision = SpacingOptions::Preserve;
  } else {
    // Update the break penalty and if the curr_token is allowed to
    // break before it.
    curr_token->before.break_penalty =
        BreakPenaltyBetween(prev_token, *curr_token);
    const auto breaker =
        BreakDecisionBetween(style, prev_token, *curr_token, context);
    curr_token->before.break_decision = breaker.value;
    VLOG(3) << "line break constraint: " << breaker.reason;
  }
}

void AnnotateFormattingInformation(
    const FormatStyle& style, const verible::TextStructureView& text_structure,
    std::vector<verible::PreFormatToken>::iterator tokens_begin,
    std::vector<verible::PreFormatToken>::iterator tokens_end) {
  // This interface just forwards the relevant information from text_structure.
  AnnotateFormattingInformation(style, text_structure.Contents().begin(),
                                text_structure.SyntaxTree().get(),
                                text_structure.EOFToken(), tokens_begin,
                                tokens_end);
}

void AnnotateFormattingInformation(
    const FormatStyle& style, const char* buffer_start,
    const verible::Symbol* syntax_tree_root,
    const verible::TokenInfo& eof_token,
    std::vector<verible::PreFormatToken>::iterator tokens_begin,
    std::vector<verible::PreFormatToken>::iterator tokens_end) {
  if (tokens_begin == tokens_end) {  // empty range
    return;
  }

  if (buffer_start != nullptr) {
    // For unit testing, tokens' text snippets don't necessarily originate
    // from the same contiguous string buffer, so skip this step.
    AnnotateOriginalSpacing(buffer_start, tokens_begin, tokens_end);
  }

  // Annotate inter-token information using the syntax tree for context.
  AnnotateFormatTokensUsingSyntaxContext(
      syntax_tree_root, eof_token, tokens_begin, tokens_end,
      // lambda: bind the FormatStyle, forwarding all other arguments
      [&style](const PreFormatToken& prev_token, PreFormatToken* curr_token,
               const SyntaxTreeContext& context) {
        AnnotateFormatToken(style, prev_token, curr_token, context);
      });
}

}  // namespace formatter
}  // namespace verilog
