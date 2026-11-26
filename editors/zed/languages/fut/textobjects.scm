[
  (function_declaration body: (block "{" (_)* @function.inside "}"))
  (local_function body: (block "{" (_)* @function.inside "}"))
  (method_declaration body: (block "{" (_)* @function.inside "}"))
  (test_declaration body: (block "{" (_)* @function.inside "}"))
  (view_declaration body: (view_block "{" (_)* @function.inside "}"))
  (extern_declaration)
  (effect_operation)
] @function.around

[
  (class_declaration body: (class_body "{" (_)* @class.inside "}"))
  (enum_declaration body: (enum_body "{" (_)* @class.inside "}"))
  (effect_declaration body: (effect_body "{" (_)* @class.inside "}"))
] @class.around

(comment)+ @comment.around
