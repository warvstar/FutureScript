(comment) @comment

["(" ")" "[" "]" "{" "}"] @punctuation.bracket
(generic_type ["<" ">"] @punctuation.bracket)
["," ";" ":" "."] @punctuation.delimiter

(number) @number
(signed_number "-" @operator)
(boolean) @boolean
(nil) @constant.builtin
[(self) (super)] @variable.special

(string_content) @string
(string "\"" @string)
(test_name) @string
(escape_sequence) @string.escape
(interpolation "{" @punctuation.special "}" @punctuation.special)
(interpolation ["=" ":"] @punctuation.special)
(format_spec) @string.special

; Contextual words are highlighted only in the syntax that uses them.
(variable_declaration ["pub" "var" "state"] @keyword)
(field_declaration ["var" "state"] @keyword)
(shared_declaration ["shared" "var"] @keyword)
(function_declaration ["pub" "gpu" "fn"] @keyword)
(method_declaration ["override" "fn"] @keyword)
(local_function "fn" @keyword)
(lambda "fn" @keyword)
(class_declaration ["pub" "class"] @keyword)
(enum_declaration ["pub" "enum"] @keyword)
(type_declaration ["pub" "type"] @keyword)
(effect_declaration ["pub" "effect"] @keyword)
(effect_operation "fn" @keyword)
(effect_annotation "yield" @keyword)
(extern_declaration ["pub" "extern" "fn"] @keyword)
(import_declaration ["pub" "import" "as"] @keyword)
(test_declaration "test" @keyword)
(expect_statement ["expect" "like"] @keyword)
(expect_capture ["throws" "prints" "allocs"] @keyword)
(expect_growth ["grows" "allocs"] @keyword)
(contract_clause ["requires" "ensures"] @keyword)
(invariant_clause "invariant" @keyword)
(atomic_statement "atomic" @keyword)
(handle_statement "handle" @keyword)
(handler_clause ["with" "yield"] @keyword)
(match_statement "match" @keyword)
(match_else "else" @keyword)
(if_statement ["if" "else"] @keyword)
(while_statement "while" @keyword)
(for_statement ["for" "in"] @keyword)
(parallel_for_statement ["parallel" "for" "in"] @keyword)
(try_statement ["try" "catch"] @keyword)
(return_statement "return" @keyword)
(throw_statement "throw" @keyword)
(break_statement) @keyword
(continue_statement) @keyword
(yield_expression "yield" @keyword)
(spawn_expression "spawn" @keyword)
(view_declaration "view" @keyword)
(view_if ["if" "else"] @keyword)
(view_for ["for" "in" "key"] @keyword)
(view_event "on" @keyword)

(attribute) @attribute

(member_expression property: (property_identifier) @property)
(shape_field name: (property_identifier) @property)
(field_declaration name: (identifier) @property)
(map_key) @property

[
  (function_declaration name: (identifier) @function)
  (method_declaration name: (identifier) @function)
  (local_function name: (identifier) @function)
  (extern_declaration name: (identifier) @function)
  (effect_operation name: (identifier) @function)
  (view_declaration name: (identifier) @function)
  (call_expression function: (identifier) @function)
  (call_expression function: (member_expression property: (property_identifier) @function))
  (spawn_expression function: (qualified_name (identifier) @function .))
  (handler_clause operation: (qualified_name (identifier) @function .))
]
(parameter name: (identifier) @variable.parameter)
(match_bindings (identifier) @variable)
(expect_growth size: (identifier) @variable.parameter)
(try_statement error: (identifier) @variable)

(named_type) @type
(generic_type name: (identifier) @type)
((named_type) @type.builtin
  (#any-of? @type.builtin "any" "bool" "fn" "int" "list" "long" "map" "num" "ptr" "str" "void"
    "f64s" "f32s" "u32s" "i32s" "u8s" "vec2f" "vec3f" "vec4f"))
((generic_type name: (identifier) @type.builtin)
  (#any-of? @type.builtin "list" "map"))
(class_declaration name: (identifier) @type superclass: (qualified_name)? @type)
(enum_declaration name: (identifier) @enum)
(enum_variant name: (identifier) @variant)
(match_arm variant: (qualified_name (identifier) @variant .))
(type_declaration name: (identifier) @type)
(effect_declaration name: (identifier) @type)
(effect_annotation (qualified_name) @type)

(view_tag) @tag
(view_class_name) @attribute
(view_attribute name: (property_identifier) @attribute)
(view_event event: (identifier) @attribute)

(unary_expression operator: _ @operator)
(binary_expression operator: _ @operator)
(assignment_statement operator: _ @operator)
(expect_statement operator: _ @operator)
(effect_annotation "!" @operator)
(member_expression "?." @operator)
(optional_type "?" @operator)
(union_type "|" @operator)
(ternary_expression ["?" ":"] @operator)
(range ".." @operator)
(expect_growth "->" @operator)
