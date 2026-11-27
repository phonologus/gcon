; Highlights for tree-sitter-gcon (Neovim capture conventions: later
; patterns take precedence over earlier ones). From tree-sitter-icon's.

(identifier) @variable

(comment) @comment @spell
(preprocessor_directive) @keyword.directive

(integer) @number
(real) @number.float
(string) @string
(cset) @string.special
(escape_sequence) @string.escape
(range_operator) @operator

; &keyword
(keyword) @constant.builtin
(keyword (identifier) @constant.builtin)

[
  "procedure"
  "end"
] @keyword.function

[
  "return"
  "suspend"
  "fail"
] @keyword.return

[
  "if"
  "then"
  "else"
  "case"
  "of"
  "default"
] @keyword.conditional

[
  "while"
  "until"
  "every"
  "repeat"
  "do"
  "break"
] @keyword.repeat

(next_expression) @keyword.repeat

[
  "global"
  "local"
  "static"
  "initial"
  "record"
  "invocable"
] @keyword.storage

"link" @keyword.import

[
  "create"
  "not"
  "to"
  "by"
] @keyword.operator

; Declarations
(procedure_declaration name: (identifier) @function)
(parameter_list (identifier) @variable.parameter)
(record_declaration name: (identifier) @type)
(record_declaration field: (identifier) @variable.member)
(field_expression field: (identifier) @variable.member)
(link_declaration (identifier) @module)

; Calls: built-in functions, then any other call
; gen.sh: built-in functions
((call_expression function: (identifier) @function.builtin)
  (#any-of? @function.builtin "Active" "Alert" "Bg" "Clip" "Clone" "Color" "ColorValue" "CopyArea" "Couple" "DrawArc" "DrawCircle" "DrawCurve" "DrawImage" "DrawLine" "DrawPoint" "DrawPolygon" "DrawRectangle" "DrawSegment" "DrawString" "EraseArea" "Event" "Fg" "FillArc" "FillCircle" "FillPolygon" "FillRectangle" "Font" "FreeColor" "GotoRC" "GotoXY" "Lower" "NewColor" "PaletteChars" "PaletteColor" "PaletteKey" "Pattern" "Pending" "Pixel" "QueryPointer" "Raise" "ReadImage" "TextWidth" "Uncouple" "WAttrib" "WDefault" "WFlush" "WSync" "WinButton" "WinColorDialog" "WinEditRegion" "WinFontDialog" "WinMenuBar" "WinOpenDialog" "WinPlayMedia" "WinSaveDialog" "WinScrollBar" "WinSelectDialog" "WriteImage" "abs" "acos" "any" "args" "asin" "atan" "bal" "bytes" "center" "char" "chdir" "close" "collect" "copy" "cos" "cset" "delay" "delete" "detab" "display" "dtor" "entab" "errorclear" "exit" "exp" "fattrib" "find" "flush" "function" "get" "getch" "getche" "getenv" "iand" "icom" "image" "insert" "integer" "ior" "ishift" "ixor" "kbhit" "key" "left" "len" "list" "loadfunc" "log" "many" "map" "match" "member" "move" "name" "numeric" "open" "ord" "pop" "pos" "proc" "pull" "push" "put" "read" "readrunes" "reads" "real" "remove" "rename" "repl" "reverse" "right" "rtod" "runerr" "runes" "seek" "seq" "serial" "set" "sin" "sort" "sortf" "sqrt" "stop" "string" "system" "tab" "table" "tan" "trim" "type" "uchar" "upto" "utf8valid" "variable" "where" "write" "writes"))
; gen.sh: end
(call_expression function: (identifier) @function.call)
(pdco_expression function: (identifier) @function.call)

(binary_expression operator: _ @operator)
(assignment_expression operator: _ @operator)
(unary_expression operator: _ @operator)
(section_expression operator: _ @operator)

["(" ")" "[" "]" "{" "}" "$(" "$)" "$<" "$>"] @punctuation.bracket
["," ";" ":" "."] @punctuation.delimiter

