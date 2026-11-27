/**
 * @file Icon grammar for tree-sitter, as gcon reads Icon
 * @license MIT
 *
 * Follows the yacc grammar of the Icon translator (icon/src/h/grammar.h)
 * and its lexical rules (icon/src/common/yylex.h, lextab.h). The external
 * scanner (src/scanner.c) implements Icon's semicolon insertion.
 *
 * Forked from tree-sitter-icon (github.com/phonologus/tree-sitter-icon,
 * at 6070fd3). gcon's syntax is Icon 9.5's, with two lexical additions
 * (gcon's docs/design.md, "Strings and Unicode"): the escapes \uXXXX and
 * \UXXXXXX, and ranges a..z in cset literals. So that editors can show
 * them, a string or cset is not one token here, as it is in
 * tree-sitter-icon: its escapes are escape_sequence nodes, and a cset's
 * ".." is a token of its own.
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

// Precedence levels, loosest first, as in grammar.h (expr .. expr11).
const PREC = {
  control: 0, // if, while, every, return, create, ...: extend rightwards
  and: 1,     // &
  scan: 2,    // ?
  assign: 3,  // := :=: <- <-> op:=   (right associative)
  to: 4,      // to ... by
  alt: 5,     // |                   (right associative)
  compare: 6, // == ~== << = ~= < === ...
  concat: 7,  // || |||
  add: 8,     // + - ++ --
  mul: 9,     // * / % **
  power: 10,  // ^                   (right associative)
  limit: 11,  // \ @ !  (binary)
  unary: 12,
  postfix: 13, // calls, subscripts, fields
};

const AUGMENTED = ['||', '|||', '--', '++', '+', '-', '*', '**', '/', '%', '^',
  '=', '===', '>=', '>', '<=', '<', '~=', '~===', '==', '>>=', '>>', '<<=',
  '<<', '~==', '?', '&', '@'].map(op => op + ':=');

const UNARY = ['@', 'not', '|', '||', '|||', '.', '!', '--', '+', '*', '/',
  '^', '**', '~', '-', '=', '~=', '==', '~==', '===', '++', '?', '~===', '\\'];

module.exports = grammar({
  name: 'gcon',

  externals: $ => [$._automatic_semicolon],

  extras: $ => [/\s/, $.comment, $.preprocessor_directive],

  word: $ => $.identifier,

  conflicts: $ => [],

  rules: {
    source_file: $ => repeat($._declaration),

    _declaration: $ => choice(
      $.link_declaration,
      $.global_declaration,
      $.record_declaration,
      $.invocable_declaration,
      $.procedure_declaration,
    ),

    link_declaration: $ => seq('link', commaSep1(choice($.identifier, $.string))),

    global_declaration: $ => seq('global', commaSep1($.identifier)),

    record_declaration: $ => seq(
      'record',
      field('name', $.identifier),
      '(',
      optional(commaSep1(field('field', $.identifier))),
      ')',
    ),

    invocable_declaration: $ => seq(
      'invocable',
      commaSep1(choice(
        $.identifier,
        seq($.string, optional(seq(':', $.integer))),
      )),
    ),

    procedure_declaration: $ => seq(
      'procedure',
      field('name', $.identifier),
      field('parameters', $.parameter_list),
      $._terminator,
      repeat($.local_declaration),
      optional($.initial_clause),
      repeat($._statement),
      'end',
    ),

    parameter_list: $ => seq(
      '(',
      optional(seq(
        commaSep1($.identifier),
        optional(seq($._open_bracket, $._close_bracket)),
      )),
      ')',
    ),

    local_declaration: $ => seq(
      choice('local', 'static'),
      commaSep1($.identifier),
      $._terminator,
    ),

    initial_clause: $ => seq('initial', $._expression, $._terminator),

    // A statement in a procedure body: an expression and its terminator,
    // or an empty statement (a lone ";").
    _statement: $ => choice(seq($._expression, $._terminator), ';'),

    _terminator: $ => choice(';', $._automatic_semicolon),

    // ---------------------------------------------------------------
    // Expressions

    _expression: $ => choice(
      $.binary_expression,
      $.assignment_expression,
      $.to_expression,
      $.unary_expression,
      $._primary,
    ),

    binary_expression: $ => {
      const table = [
        [PREC.and, '&'],
        [PREC.scan, '?'],
        [PREC.compare, choice('==', '>>=', '>>', '<<=', '<<', '~==', '=', '>=', '>', '<=', '<', '~=', '===', '~===')],
        [PREC.concat, choice('||', '|||')],
        [PREC.add, choice('+', '-', '++', '--')],
        [PREC.mul, choice('*', '/', '%', '**')],
        [PREC.limit, choice('\\', '@', '!')],
      ];
      return choice(
        ...table.map(([p, op]) => prec.left(p, seq(
          field('left', $._expression),
          field('operator', op),
          field('right', $._expression),
        ))),
        prec.right(PREC.alt, seq(
          field('left', $._expression),
          field('operator', '|'),
          field('right', $._expression),
        )),
        prec.right(PREC.power, seq(
          field('left', $._expression),
          field('operator', '^'),
          field('right', $._expression),
        )),
      );
    },

    assignment_expression: $ => prec.right(PREC.assign, seq(
      field('left', $._expression),
      field('operator', choice(':=', ':=:', '<-', '<->', ...AUGMENTED)),
      field('right', $._expression),
    )),

    to_expression: $ => prec.left(PREC.to, seq(
      field('from', $._expression),
      'to',
      field('to', $._expression),
      optional(seq('by', field('by', $._expression))),
    )),

    unary_expression: $ => prec(PREC.unary, seq(
      field('operator', choice(...UNARY)),
      field('operand', $._expression),
    )),

    _primary: $ => choice(
      $.identifier,
      $.keyword,
      $.integer,
      $.real,
      $.string,
      $.cset,
      $.uset,
      $.parenthesized_expression,
      $.block,
      $.list,
      $.call_expression,
      $.pdco_expression,
      $.subscript_expression,
      $.section_expression,
      $.field_expression,
      $.create_expression,
      $.if_expression,
      $.case_expression,
      $.loop_expression,
      $.repeat_expression,
      $.return_expression,
      $.break_expression,
      $.next_expression,
    ),

    keyword: $ => seq('&', choice($.identifier, 'fail')),

    // (e1, e2, ...) is mutual evaluation; (e) is grouping.
    parenthesized_expression: $ => seq('(', optional($._expression_list), ')'),

    block: $ => seq($._open_brace, optional($._compound), $._close_brace),

    // Statements separated by semicolons; the last needs no terminator.
    _compound: $ => choice(
      seq(repeat1($._statement), optional($._expression)),
      $._expression,
    ),

    list: $ => seq($._open_bracket, optional($._expression_list), $._close_bracket),

    call_expression: $ => prec(PREC.postfix, seq(
      field('function', $._expression),
      field('arguments', $.argument_list),
    )),

    argument_list: $ => seq('(', optional($._expression_list), ')'),

    pdco_expression: $ => prec(PREC.postfix, seq(
      field('function', $._expression),
      field('arguments', $.pdco_argument_list),
    )),

    pdco_argument_list: $ => seq($._open_brace, optional($._expression_list), $._close_brace),

    subscript_expression: $ => prec(PREC.postfix, seq(
      field('object', $._expression),
      $._open_bracket,
      optional($._expression_list),
      $._close_bracket,
    )),

    section_expression: $ => prec(PREC.postfix, seq(
      field('object', $._expression),
      $._open_bracket,
      field('start', $._expression),
      field('operator', choice(':', '+:', '-:')),
      field('end', $._expression),
      $._close_bracket,
    )),

    field_expression: $ => prec(PREC.postfix, seq(
      field('object', $._expression),
      '.',
      field('field', $.identifier),
    )),

    // e1, e2, ... where any element may be empty, as in f(a,,b). (Used
    // optionally: tree-sitter rules may not match the empty string.)
    _expression_list: $ => choice(
      seq($._expression, repeat($._list_tail)),
      repeat1($._list_tail),
    ),

    _list_tail: $ => seq(',', optional($._expression)),

    create_expression: $ => prec.right(PREC.control, seq('create', $._expression)),

    if_expression: $ => prec.right(PREC.control, seq(
      'if', field('condition', $._expression),
      'then', field('consequence', $._expression),
      optional(seq('else', field('alternative', $._expression))),
    )),

    case_expression: $ => seq(
      'case', field('value', $._expression), 'of',
      $._open_brace,
      sepBy1($._terminator, $.case_clause),
      $._close_brace,
    ),

    case_clause: $ => seq(
      field('label', choice('default', $._expression)),
      ':',
      field('body', $._expression),
    ),

    loop_expression: $ => prec.right(PREC.control, seq(
      choice('while', 'until', 'every'),
      field('condition', $._expression),
      optional(seq('do', field('body', $._expression))),
    )),

    repeat_expression: $ => prec.right(PREC.control, seq('repeat', field('body', $._expression))),

    return_expression: $ => prec.right(PREC.control, choice(
      'fail',
      seq('return', optional($._expression)),
      seq('suspend', optional($._expression), optional(seq('do', field('body', $._expression)))),
    )),

    break_expression: $ => prec.right(PREC.control, seq('break', optional($._expression))),

    next_expression: $ => 'next',

    _open_brace: $ => choice('{', '$('),
    _close_brace: $ => choice('}', '$)'),
    _open_bracket: $ => choice('[', '$<'),
    _close_bracket: $ => choice(']', '$>'),

    // ---------------------------------------------------------------
    // Tokens

    identifier: $ => /[A-Za-z_][A-Za-z0-9_]*/,

    integer: $ => token(choice(
      /[0-9]+/,
      /[0-9]+[rR][0-9A-Za-z]+/,
    )),

    real: $ => token(choice(
      /[0-9]+\.[0-9]*([eE][+-]?[0-9]+)?/,
      /\.[0-9]+([eE][+-]?[0-9]+)?/,
      /[0-9]+[eE][+-]?[0-9]+/,
    )),

    // A literal's parts are immediate tokens, so that no extras (blanks,
    // comments) are skipped inside it. "_" as the last non-blank on a line
    // continues the literal at the first non-blank of the next line.
    string: $ => seq(
      '"',
      repeat(choice($._string_text, $.escape_sequence)),
      token.immediate('"'),
    ),

    _string_text: $ => token.immediate(prec(1,
      /([^"\\\n_]|_[ \t\r\f\v]*\n[ \t\r\f\v\n]*|_)+/)),

    // In a cset, a lone "." is text and ".." a range (a..z); "\." is the
    // escape for a "." at either end of a range.
    cset: $ => seq(
      "'",
      repeat(choice($._cset_text, $.escape_sequence, $.range_operator)),
      token.immediate("'"),
    ),

    // gcon's uset literal, u'...': a cset literal's parts after "u'" (one
    // token, so that u is not an identifier there).
    uset: $ => seq(
      token(prec(2, "u'")),
      repeat(choice($._cset_text, $.escape_sequence, $.range_operator)),
      token.immediate("'"),
    ),

    _cset_text: $ => token.immediate(prec(1, choice(
      /([^'\\\n_.]|_[ \t\r\f\v]*\n[ \t\r\f\v\n]*|_)+/,
      '.',
    ))),

    range_operator: $ => token.immediate(prec(2, '..')),

    // A backslash escapes any character (including a newline); "\^c"
    // takes one more. The longest form is taken: \x with up to two hex
    // digits, up to three octal digits, gcon's \u with four hex digits
    // and \U with up to six (in Icon 9.5, \u and \U are the letters).
    escape_sequence: $ => token.immediate(prec(2, choice(
      /\\x[0-9A-Fa-f]{1,2}/,
      /\\[0-7]{1,3}/,
      /\\u[0-9A-Fa-f]{4}/,
      /\\U[0-9A-Fa-f]{1,6}/,
      /\\\^[\s\S]/,
      /\\[\s\S]/,
    ))),

    comment: $ => token(seq('#', /.*/)),

    // $define, $include, ... (a "$" followed by a word; "$(" etc. are
    // digraphs for brackets).
    preprocessor_directive: $ => token(seq('$', /[ \t]*[A-Za-z_][^\n]*/)),
  },
});

function commaSep1(rule) {
  return seq(rule, repeat(seq(',', rule)));
}

function sepBy1(sep, rule) {
  return seq(rule, repeat(seq(sep, rule)));
}
