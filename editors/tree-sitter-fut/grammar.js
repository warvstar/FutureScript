// Keep lexical and expression rules in step with src/compile.h and src/compile.cpp.
// The view and state syntax is rewritten before compilation by src/views.cpp.

const KEYWORDS = [
  'var', 'fn', 'class', 'if', 'else', 'while', 'for', 'in', 'return', 'break',
  'continue', 'true', 'false', 'nil', 'self', 'super', 'import', 'extern',
  'yield', 'try', 'catch', 'throw',
];

const CONTEXTUAL = [
  'as', 'test', 'expect', 'throws', 'prints', 'pub', 'override', 'atomic',
  'effect', 'handle', 'with', 'requires', 'ensures', 'invariant', 'allocs',
  'grows', 'like', 'spawn', 'type', 'enum', 'match', 'parallel', 'shared',
  'gpu', 'state', 'view', 'on', 'key',
];

const PREC = {
  ternary: 1,
  unary: 12,
  power: 13,
  postfix: 14,
};

function commaSeparated(rule) {
  return seq(rule, repeat(seq(',', rule)));
}

function functionBody($) {
  return seq(
    field('parameters', $.parameters),
    optional(seq(':', field('return_type', $._type))),
    optional($.effect_annotation),
    repeat($.contract_clause),
    field('body', $.block),
  );
}

module.exports = grammar({
  name: 'fut',

  word: $ => $._word,

  extras: $ => [/[ \t\r\n]/, $.comment],

  externals: $ => [
    $._inline_paren,
    $._inline_bracket,
    $._inline_minus,
    $._inline_brace,
    $._inline_value,
    $._public_declaration,
    $._gpu_declaration,
    $._view_end,
    $.view_class_name,
    $.view_tag,
  ],

  conflicts: $ => [
    [$.effect_declaration, $.identifier],
    [$.type_declaration, $.identifier],
    [$.enum_declaration, $.identifier],
    [$.map, $.view_block],
    [$.view_event, $.identifier],
    [$.extern_declaration],
    [$.handle_statement],
    [$.effect_annotation],
    [$.expect_growth, $.identifier],
    [$.expect_capture, $.identifier],
    [$.import_declaration],
    [$._primary, $.map_key],
    [$.map_key, $.super],
    [$.yield_expression, $.map_key],
    [$.block, $.map],
    [$.view_declaration, $.identifier],
    [$.spawn_expression, $.identifier],
    [$.expect_statement, $.identifier],
    [$.match_statement, $.identifier],
    [$.parallel_for_statement, $.identifier],
    [$.test_declaration, $.identifier],
    [$.shared_declaration, $.identifier],
    [$._variable_declaration, $.identifier],
  ],

  rules: {
    source_file: $ => repeat(choice(
      $.function_declaration,
      $.class_declaration,
      $.enum_declaration,
      $.type_declaration,
      $.effect_declaration,
      $.extern_declaration,
      $.import_declaration,
      $.test_declaration,
      $.view_declaration,
      $.shared_declaration,
      alias($.global_variable, $.variable_declaration),
      $._top_statement,
    )),

    _top_statement: $ => choice(
      $.block, $.if_statement, $.while_statement, $.for_statement, $.parallel_for_statement,
      $.try_statement, $.throw_statement, $.break_statement, $.continue_statement,
      $.atomic_statement, $.handle_statement, $.match_statement, $.expect_statement,
      $.expression_statement, $.assignment_statement, ';',
    ),

    _statement: $ => choice(
      $._top_statement,
      $.local_function,
      $.variable_declaration,
      $.return_statement,
    ),

    block: $ => seq('{', repeat($._statement), '}'),

    attribute: $ => seq('@', field('name', $.identifier)),

    global_variable: $ => seq(optional($._visibility), $._variable_declaration),

    _visibility: $ => seq('pub', $._public_declaration),

    _gpu_modifier: $ => seq('gpu', $._gpu_declaration),

    variable_declaration: $ => $._variable_declaration,

    _variable_declaration: $ => seq(
      choice('var', 'state'),
      field('name', $.identifier),
      optional(seq(':', field('type', $._type))),
      optional(seq('=', field('value', $._expression))),
    ),

    shared_declaration: $ => seq(
      'shared', 'var',
      field('name', $.identifier),
      optional(seq(':', field('type', $._type))),
      '=', field('value', $._expression),
    ),

    function_declaration: $ => seq(
      repeat($.attribute), optional($._visibility), optional($._gpu_modifier),
      'fn', field('name', $.identifier), functionBody($),
    ),

    local_function: $ => seq(
      'fn', field('name', $.identifier), functionBody($),
    ),

    lambda: $ => seq('fn', functionBody($)),

    parameters: $ => seq('(', optional(commaSeparated($.parameter)), ')'),

    parameter: $ => seq(
      field('name', $.identifier),
      optional(seq(':', field('type', $._type))),
    ),

    contract_clause: $ => seq(
      choice('requires', 'ensures'), field('condition', $._expression),
    ),

    effect_annotation: $ => seq('!', optional(commaSeparated($._effect_name))),

    _effect_name: $ => choice($.qualified_name, 'yield'),

    qualified_name: $ => seq($.identifier, repeat(seq('.', $.identifier))),

    class_declaration: $ => seq(
      optional($._visibility), 'class', field('name', $.identifier),
      optional(seq(':', field('superclass', $.qualified_name))),
      field('body', $.class_body),
    ),

    class_body: $ => seq('{', repeat(choice(
      $.method_declaration, alias($._variable_declaration, $.field_declaration), $.invariant_clause,
    )), '}'),

    method_declaration: $ => seq(
      repeat($.attribute), optional('override'), 'fn', field('name', $.identifier), functionBody($),
    ),

    invariant_clause: $ => seq('invariant', field('condition', $._expression)),

    enum_declaration: $ => seq(
      optional($._visibility), 'enum', field('name', $.identifier), field('body', $.enum_body),
    ),

    enum_body: $ => seq('{', repeat(choice($.enum_variant, ',')), '}'),

    enum_variant: $ => seq(
      field('name', $.identifier),
      optional(field('fields', alias($._variant_parameters, $.parameters))),
    ),

    _variant_parameters: $ => seq(
      alias($._inline_paren, '('), optional(seq(commaSeparated($.parameter), optional(','))), ')',
    ),

    type_declaration: $ => seq(
      optional($._visibility), 'type', field('name', $.identifier), '=', field('type', $._type),
    ),

    _type: $ => choice(
      $._type_atom, $.optional_type, $.union_type,
    ),

    _type_atom: $ => choice(
      $.named_type, $.generic_type, $.shape_type, $.parenthesized_type,
      $.string, $.number, $.signed_number, $.boolean, $.nil,
    ),

    named_type: $ => choice($.qualified_name, 'fn'),

    generic_type: $ => seq(
      field('name', $.identifier), '<', commaSeparated($._type), '>',
    ),

    parenthesized_type: $ => seq('(', $._type, ')'),

    optional_type: $ => seq($._type_atom, '?'),

    union_type: $ => prec.left(seq(
      field('left', $._type), '|', field('right', $._type),
    )),

    signed_number: $ => seq('-', $.number),

    shape_type: $ => seq('{', repeat(seq($.shape_field, optional(','))), '}'),

    shape_field: $ => seq(
      field('name', choice($.property_identifier, $.string)),
      ':', field('type', $._type),
    ),

    effect_declaration: $ => seq(
      optional($._visibility), 'effect', field('name', $.identifier), field('body', $.effect_body),
    ),

    effect_body: $ => seq('{', repeat($.effect_operation), '}'),

    effect_operation: $ => seq(
      'fn', field('name', $.identifier), field('parameters', $.parameters),
      optional(seq(':', field('return_type', $._type))),
    ),

    extern_declaration: $ => seq(
      optional($._visibility), 'extern', optional(field('library', choice($.string, $.identifier))),
      'fn', field('name', $.identifier), field('parameters', $.extern_parameters),
      optional(seq(':', field('return_type', $.named_type))), optional('!'),
    ),

    extern_parameters: $ => seq('(', optional(commaSeparated($.named_type)), ')'),

    import_declaration: $ => seq(
      optional($._visibility), 'import', field('path', $.string),
      optional(seq('as', field('alias', $.identifier))),
    ),

    test_declaration: $ => seq(
      'test', field('name', $.test_name), field('body', $.block),
    ),

    if_statement: $ => seq(
      'if', field('condition', $._expression), field('consequence', $.block),
      optional(seq('else', field('alternative', choice($.block, $.if_statement)))),
    ),

    while_statement: $ => seq(
      'while', field('condition', $._expression), field('body', $.block),
    ),

    for_statement: $ => seq(
      'for', field('name', $.identifier), 'in',
      field('iterable', choice($._expression, $.range)), field('body', $.block),
    ),

    parallel_for_statement: $ => seq(
      'parallel', 'for', field('name', $.identifier), 'in',
      field('iterable', $.range), field('body', $.block),
    ),

    range: $ => seq(field('start', $._expression), '..', field('end', $._expression)),

    try_statement: $ => seq(
      'try', field('body', $.block),
      'catch', optional(field('error', $.identifier)), field('handler', $.block),
    ),

    throw_statement: $ => seq('throw', field('value', $._expression)),

    return_statement: $ => prec.right(seq(
      'return', optional(seq($._inline_value, field('value', $._expression))),
    )),

    break_statement: _ => 'break',
    continue_statement: _ => 'continue',

    atomic_statement: $ => seq('atomic', field('body', alias($._inline_block, $.block))),

    handle_statement: $ => seq(
      'handle', field('body', alias($._inline_block, $.block)), repeat1($.handler_clause),
    ),

    _inline_block: $ => seq(alias($._inline_brace, '{'), repeat($._statement), '}'),

    handler_clause: $ => seq(
      'with', field('operation', $._effect_name), functionBody($),
    ),

    match_statement: $ => seq(
      'match', field('value', $._expression),
      '{', repeat1($.match_arm), optional($.match_else), '}',
    ),

    match_arm: $ => seq(
      field('variant', $.qualified_name),
      optional(field('bindings', $.match_bindings)),
      field('body', $.block),
    ),

    match_bindings: $ => seq(
      alias($._inline_paren, '('), optional(seq(commaSeparated($.identifier), optional(','))), ')',
    ),

    match_else: $ => seq('else', field('body', $.block)),

    expect_statement: $ => prec.dynamic(1, seq('expect', choice(
      field('condition', $._expression),
      seq(field('capture', $.expect_capture), optional(seq(
        field('operator', choice('==', '!=', '<', '<=', '>', '>=')),
        field('expected', $._expression),
      ))),
      seq(field('growth', $.expect_growth), 'like', field('expected', $._expression)),
    ))),

    expect_capture: $ => seq(choice('throws', 'prints', 'allocs'), field('body', $.block)),

    expect_growth: $ => seq(
      'grows', optional('allocs'), '{', field('size', $.identifier), '->',
      repeat($._statement), '}',
    ),

    expression_statement: $ => $._expression,

    assignment_statement: $ => seq(
      field('left', choice($.identifier, $.member_expression, $.index_expression)),
      field('operator', choice('=', '+=', '-=', '*=', '/=')),
      field('right', $._expression),
    ),

    _expression: $ => choice(
      $._primary, $.unary_expression, $.binary_expression, $.ternary_expression,
      $.member_expression, $.index_expression, $.slice_expression, $.call_expression,
      $.yield_expression, $.spawn_expression,
    ),

    _primary: $ => choice(
      $.identifier, $.number, $.string, $.boolean, $.nil, $.self, $.super,
      $.list, $.map, $.parenthesized_expression, $.lambda,
    ),

    yield_expression: $ => prec.right(seq(
      'yield', optional(seq($._inline_value, field('value', $._expression))),
    )),

    spawn_expression: $ => seq(
      'spawn', field('function', choice($.qualified_name, $.lambda)),
      field('arguments', $.arguments),
    ),

    unary_expression: $ => prec(PREC.unary, seq(
      field('operator', choice('-', '!', '~')), field('argument', $._expression),
    )),

    binary_expression: $ => choice(
      ...[
        [2, '||'], [3, '&&'], [4, '==', '!='], [5, '<', '<=', '>', '>='],
        [6, '|'], [7, '^'], [8, '&'], [9, '<<', '>>'],
        [10, '+', alias($._inline_minus, '-')], [11, '*', '/', '%'],
      ].map(([precedence, ...operators]) => prec.left(precedence, seq(
        field('left', $._expression),
        field('operator', operators.length == 1 ? operators[0] : choice(...operators)),
        field('right', $._expression),
      ))),
      // A signed exponent is also valid: 2 ** -2 ** 2.
      prec.right(PREC.power, seq(
        field('left', $._expression), field('operator', '**'), field('right', $._expression),
      )),
    ),

    ternary_expression: $ => prec.right(PREC.ternary, seq(
      field('condition', $._expression), '?',
      field('consequence', $._expression), ':', field('alternative', $._expression),
    )),

    member_expression: $ => prec.left(PREC.postfix, seq(
      field('object', $._expression), choice('.', '?.'), field('property', $.property_identifier),
    )),

    index_expression: $ => prec.left(PREC.postfix, seq(
      field('object', $._expression),
      alias($._inline_bracket, '['), field('index', $._expression), ']',
    )),

    slice_expression: $ => prec.left(PREC.postfix, seq(
      field('object', $._expression),
      alias($._inline_bracket, '['), optional(field('start', $._expression)),
      ':', optional(field('end', $._expression)), ']',
    )),

    call_expression: $ => prec.left(PREC.postfix, seq(
      field('function', $._expression), field('arguments', $.arguments),
    )),

    arguments: $ => seq(
      alias($._inline_paren, '('), optional(commaSeparated($._expression)), ')',
    ),

    parenthesized_expression: $ => seq(choice('(', alias($._inline_paren, '(')), $._expression, ')'),

    list: $ => seq(choice('[', alias($._inline_bracket, '[')), optional(commaSeparated($._expression)), ']'),

    map: $ => seq('{', optional(commaSeparated($.pair)), '}'),

    pair: $ => seq(
      field('key', choice($.map_key, $._expression)), ':', field('value', $._expression),
    ),

    map_key: $ => prec.dynamic(1, choice(
      $.identifier, ...KEYWORDS.filter(keyword => !['true', 'false', 'nil', 'self'].includes(keyword)),
    )),

    string: $ => seq('"', repeat(choice($.string_content, $.escape_sequence, $.interpolation)), token.immediate('"')),

    string_content: _ => token.immediate(prec(1, /[^"\\{]+/)),

    escape_sequence: _ => token.immediate(/\\(u\{[0-9a-fA-F]+\}|[^\n]|\n)/),

    // Test labels are printed literally, with backslashes removed from escape pairs.
    test_name: _ => seq('"', optional(token.immediate(prec(1, /([^"\\]|\\[\s\S])+/))), token.immediate('"')),

    interpolation: $ => seq(
      token.immediate('{'), field('expression', $._expression),
      optional('='), optional(seq(':', optional($.format_spec))), '}',
    ),

    format_spec: _ => token.immediate(/[^{}"\r\n]+/),

    view_declaration: $ => seq(
      'view', field('name', $.identifier), optional(field('parameters', $.parameters)),
      field('body', $.view_block),
    ),

    view_block: $ => seq('{', repeat(choice(
      $.view_if, $.view_for, $.view_element, $.variable_declaration,
      $.assignment_statement, $.expression_statement,
    )), '}'),

    view_if: $ => seq(
      'if', field('condition', $._expression), field('consequence', $.view_block),
      optional(seq('else', field('alternative', choice($.view_if, $.view_block)))),
    ),

    view_for: $ => seq(
      'for', field('name', $.identifier), 'in', field('iterable', $._expression),
      optional(seq('key', field('key', $._expression))), field('body', $.view_block),
    ),

    view_element: $ => seq(
      field('tag', $.view_tag),
      repeat(choice($.view_class, $.view_attribute, $.view_event, $._expression)),
      choice(field('body', $.view_block), $._view_end),
    ),

    view_class: $ => seq(
      field('name', $.view_class_name), optional(seq(token.immediate(':'), field('condition', $._expression))),
    ),

    view_attribute: $ => seq(
      field('name', $.property_identifier), token.immediate(':'), field('value', $._expression),
    ),

    view_event: $ => seq('on', field('event', $.identifier), field('body', $.block)),

    boolean: _ => choice('true', 'false'),
    nil: _ => 'nil',
    self: _ => 'self',
    super: _ => 'super',

    number: _ => token(choice(/0[xX][0-9a-fA-F]+/, /[0-9]+(\.[0-9]+)?([eE][+-]?[0-9]+)?/)),

    identifier: $ => choice($._word, ...CONTEXTUAL),

    property_identifier: $ => choice($.identifier, ...KEYWORDS),

    _word: _ => /[A-Za-z_][A-Za-z_0-9]*/,

    comment: _ => token(seq('//', /[^\n]*/)),
  },
});
