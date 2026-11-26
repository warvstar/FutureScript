(function_declaration
  (attribute)* @annotation
  "fn" @context
  name: (identifier) @name) @item

(local_function "fn" @context name: (identifier) @name) @item

(method_declaration
  (attribute)* @annotation
  "fn" @context
  name: (identifier) @name) @item

(extern_declaration "fn" @context name: (identifier) @name) @item
(class_declaration "class" @context name: (identifier) @name) @item
(enum_declaration "enum" @context name: (identifier) @name) @item
(enum_variant name: (identifier) @name) @item
(type_declaration "type" @context name: (identifier) @name) @item
(effect_declaration "effect" @context name: (identifier) @name) @item
(effect_operation "fn" @context name: (identifier) @name) @item
(test_declaration "test" @context name: (test_name) @name) @item
(view_declaration "view" @context name: (identifier) @name) @item

(source_file (variable_declaration ["var" "state"] @context name: (identifier) @name) @item)
(source_file (shared_declaration "shared" @context "var" @context name: (identifier) @name) @item)
