// Generates syntaxes/icon.tmLanguage.json, taking the names of the
// built-in functions from gcon's catalogue (src/builtin.inc), as
// editors/gen.sh does for the tree-sitter queries.
"use strict";
const fs = require("fs");
const path = require("path");

const catalog = fs.readFileSync(
  path.join(__dirname, "..", "..", "..", "src", "builtin.inc"), "utf8");
const table = catalog.split("static const BuiltinFunc functions[] = {")[1].split("\n};")[0];
const builtins = [...table.matchAll(/^\t\{"([A-Za-z0-9]+)", /gm)].map((m) => m[1]);
if (builtins.length < 100) throw new Error("built-in functions not found in catalogue");

// Icon 9.5's escapes, and gcon's \u with four hex digits and \U with up
// to six.
const escapes = {
  name: "constant.character.escape.icon",
  match: "\\\\(\\^.|[0-7]{1,3}|x[0-9A-Fa-f]{1,2}|u[0-9A-Fa-f]{4}|U[0-9A-Fa-f]{1,6}|.)",
};

// gcon's cset ranges, a..z (an escape comes first, so "\." is not one).
const range = { name: "keyword.operator.range.icon", match: "\\.\\." };

// A literal continues past a line end only after "_" or "\" (approximate:
// Icon also allows blanks after the "_"). The engine matches each line with
// a newline appended, so "$" can match before and after that newline; the
// lookbehind excludes both an escape and the newline itself.
const literal = (quote, scope, extra = []) => ({
  name: scope,
  begin: quote,
  end: `${quote}|(?<![_\\\\\\n])$`,
  patterns: [escapes, ...extra],
});

const grammar = {
  $schema: "https://raw.githubusercontent.com/martinring/tmlanguage/master/tmlanguage.json",
  name: "Icon",
  scopeName: "source.icon",
  fileTypes: ["icn"],
  patterns: [
    { include: "#comment" },
    { include: "#preprocessor" },
    { include: "#declaration" },
    { include: "#expression" },
  ],
  repository: {
    comment: { name: "comment.line.number-sign.icon", match: "#.*$" },
    preprocessor: {
      name: "meta.preprocessor.icon",
      match: "^\\s*(\\$\\s*[A-Za-z_]\\w*)(.*)$",
      captures: { 1: { name: "keyword.control.directive.icon" } },
    },
    declaration: {
      patterns: [
        {
          match: "\\b(procedure)\\s+([A-Za-z_]\\w*)",
          captures: {
            1: { name: "storage.type.function.icon" },
            2: { name: "entity.name.function.icon" },
          },
        },
        {
          match: "\\b(record)\\s+([A-Za-z_]\\w*)",
          captures: {
            1: { name: "storage.type.record.icon" },
            2: { name: "entity.name.type.icon" },
          },
        },
        { name: "keyword.other.icon", match: "\\b(end|link|invocable)\\b" },
        { name: "storage.modifier.icon", match: "\\b(global|local|static|initial)\\b" },
      ],
    },
    expression: {
      patterns: [
        { include: "#comment" },
        literal('"', "string.quoted.double.icon"),
        literal("'", "string.quoted.single.cset.icon", [range]),
        {
          name: "keyword.control.icon",
          match: "\\b(if|then|else|case|of|default|while|until|every|repeat|do|break|next|return|suspend|fail|create)\\b",
        },
        { name: "keyword.operator.word.icon", match: "\\b(not|to|by)\\b" },
        { name: "variable.language.keyword.icon", match: "&[A-Za-z_]\\w*" },
        {
          name: "constant.numeric.icon",
          match: "\\b[0-9]+[rR][0-9A-Za-z]+\\b|\\b[0-9]+\\.[0-9]*([eE][+-]?[0-9]+)?|\\.[0-9]+([eE][+-]?[0-9]+)?\\b|\\b[0-9]+([eE][+-]?[0-9]+)?\\b",
        },
        {
          match: `\\b(${builtins.join("|")})\\s*(?=\\()`,
          captures: { 1: { name: "support.function.builtin.icon" } },
        },
        {
          match: "\\b([A-Za-z_]\\w*)\\s*(?=[({])",
          captures: { 1: { name: "entity.name.function.call.icon" } },
        },
        {
          match: "(\\.)\\s*([A-Za-z_]\\w*)",
          captures: {
            1: { name: "punctuation.accessor.icon" },
            2: { name: "variable.other.member.icon" },
          },
        },
        {
          name: "keyword.operator.assignment.icon",
          match: ":=:|<->|<-|(\\|\\|\\||\\|\\||--|\\+\\+|\\*\\*|~===|~==|~=|===|==|>>=|>>|<<=|<<|>=|<=|[-+*/%^=<>?&@|])?:=",
        },
        {
          name: "keyword.operator.icon",
          match: "\\|\\|\\||\\|\\||~===|~==|~=|===|==|>>=|>>|<<=|<<|>=|<=|\\*\\*|\\+\\+|--|\\+:|-:|[-+*/%^=<>?&@!|\\\\~.]",
        },
        { name: "punctuation.separator.icon", match: "[,;:]" },
        { name: "variable.other.icon", match: "[A-Za-z_]\\w*" },
      ],
    },
  },
};

fs.writeFileSync(
  path.join(__dirname, "..", "syntaxes", "icon.tmLanguage.json"),
  JSON.stringify(grammar, null, 2) + "\n",
);
