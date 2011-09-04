// Copyright (c) 2011 Cloudera, Inc. All rights reserved.

#include "exprs/like-predicate.h"

#include <sstream>
#include <boost/regex.hpp>
#include <glog/logging.h>

#include "runtime/string-value.h"

using namespace boost;
using namespace std;

namespace impala {

LikePredicate::LikePredicate(const TExprNode& node)
  : Predicate(node),
    op_(node.op),
    escape_char_(node.like_pred.escape_char[0]) {
  DCHECK_EQ(node.like_pred.escape_char.size(), 1);
}

void* LikePredicate::ConstantSubstringFn(Expr* e, TupleRow* row) {
  LikePredicate* p = static_cast<LikePredicate*>(e);
  DCHECK_EQ(p->GetNumChildren(), 2);
  StringValue* val = static_cast<StringValue*>(e->GetChild(0)->GetValue(row));
  string string_val(val->ptr, val->len);
  p->result_.bool_val = string_val.find(p->substring_) != string::npos;
  return &p->result_.bool_val;
}

void* LikePredicate::ConstantRegexFn(Expr* e, TupleRow* row) {
  LikePredicate* p = static_cast<LikePredicate*>(e);
  DCHECK_EQ(p->GetNumChildren(), 2);
  StringValue* operand_val = static_cast<StringValue*>(e->GetChild(0)->GetValue(row));
  string operand_str(operand_val->ptr, operand_val->len);
  p->result_.bool_val = regex_match(operand_str, *p->regex_);
  return &p->result_.bool_val;
}

void* LikePredicate::RegexMatch(Expr* e, TupleRow* row, bool is_like_pattern) {
  LikePredicate* p = static_cast<LikePredicate*>(e);
  StringValue* operand_value = static_cast<StringValue*>(e->GetChild(0)->GetValue(row));
  if (operand_value == NULL) return NULL;
  StringValue* pattern_value = static_cast<StringValue*>(e->GetChild(1)->GetValue(row));
  if (pattern_value == NULL) return NULL;
  string re_pattern;
  if (is_like_pattern) {
    p->ConvertLikePattern(pattern_value, &re_pattern);
  } else {
    re_pattern = string(pattern_value->ptr, pattern_value->len);
  }
  try {
    regex re(re_pattern, regex_constants::extended);
    string operand_str(operand_value->ptr, operand_value->len);
    // TODO: change this to use the BidirectionIterator form of regex_match(), so we
    // don't need to construct strings
    p->result_.bool_val = regex_match(operand_str, re);
    return &p->result_.bool_val;
  } catch (bad_expression e) {
    // TODO: log error in runtime state
    return NULL;
  }
}

void* LikePredicate::LikeFn(Expr* e, TupleRow* row) {
  return RegexMatch(e, row, true);
}

void* LikePredicate::RegexFn(Expr* e, TupleRow* row) {
  return RegexMatch(e, row, false);
}

Status LikePredicate::Prepare(RuntimeState* state) {
  Expr::Prepare(state);
  DCHECK_EQ(children_.size(), 2);
  if (GetChild(1)->IsConstant()) {
    // determine pattern and decide on eval fn
    StringValue* pattern = static_cast<StringValue*>(GetChild(1)->GetValue(NULL));
    string pattern_str(pattern->ptr, pattern->len);
    regex substring_re("(%*)([^%_]*)(%*)", regex::extended);
    smatch match_res;
    if (op_ == TExprOperator::LIKE
        && regex_match(pattern_str, match_res, substring_re)) {
      // match_res.str(0) is the whole string, match_res.str(1) the first group, etc.
      substring_ = match_res.str(2);
      compute_function_ = ConstantSubstringFn;
    } else {
      string re_pattern;
      if (op_ == TExprOperator::LIKE) {
        ConvertLikePattern(pattern, &re_pattern);
      } else {
        re_pattern = pattern_str;
      }
      try {
        regex_.reset(new regex(re_pattern, regex_constants::extended));
      } catch (bad_expression e) {
        return Status("Invalid regular expression: " + pattern_str);
      }
      compute_function_ = ConstantRegexFn;
    }
  } else {
    switch (op_) {
      case TExprOperator::LIKE:
        compute_function_ = LikeFn;
        break;
      case TExprOperator::REGEXP:
        compute_function_ = RegexFn;
        break;
      default:
        stringstream error;
        error << "Invalid LIKE operator: " << op_;
        return Status(error.str());
    }
  }
  return Status::OK;
}

void LikePredicate::ConvertLikePattern(
    const StringValue* pattern, string* re_pattern) const {
  re_pattern->clear();
  int i = 0;
  bool is_escaped = false;
  while (i < pattern->len) {
    if (!is_escaped && pattern->ptr[i] == '%') {
      re_pattern->append(".*");
    } else if (!is_escaped && pattern->ptr[i] == '_') {
      re_pattern->append(".");
    // check for escape char before checking for regex special chars, they might overlap
    } else if (!is_escaped && pattern->ptr[i] == escape_char_) {
      is_escaped = true;
    } else if (
        pattern->ptr[i] == '.'
        || pattern->ptr[i] == '['
        || pattern->ptr[i] == ']'
        || pattern->ptr[i] == '{'
        || pattern->ptr[i] == '}'
        || pattern->ptr[i] == '('
        || pattern->ptr[i] == ')'
        || pattern->ptr[i] == '\\'
        || pattern->ptr[i] == '*'
        || pattern->ptr[i] == '+'
        || pattern->ptr[i] == '?'
        || pattern->ptr[i] == '|'
        || pattern->ptr[i] == '^'
        || pattern->ptr[i] == '$'
        ) {
      // escape all regex special characters; see list at
      // http://www.boost.org/doc/libs/1_47_0/libs/regex/doc/html/boost_regex/syntax/basic_extended.html
      re_pattern->append("\\");
      re_pattern->append(1, pattern->ptr[i]);
      is_escaped = false;
    } else {
      // regular character or escaped special character
      re_pattern->append(1, pattern->ptr[i]);
      is_escaped = false;
    }
    ++i;
  }
}

}
