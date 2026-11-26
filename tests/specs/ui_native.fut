// std/ui natively (native only: build.py's NATIVE_SPECS): its DOM module is JavaScript, which the native build
// can't load, so its externs compile to stubs. Importing it works, its pure functions run, and a DOM call raises
// [host] naming where it runs.
import "std/ui" as ui

test "importing std/ui natively compiles; its pure functions run" {
  expect ui.size(2048) == "2 KB"
  expect ui.classes(["a", true, "b", false, "c", true]) == "a c"
}

test "a DOM call raises, saying it needs the wasm build" {
  var got = nil
  try { ui.el("div", nil, nil) } catch e { got = e }
  expect got == "dom.create() is JavaScript (std/ui's dom.js), which only the wasm build loads: run this in a browser or with node web/run.mjs"
}

test "top-level DOM code is a runtime error with code [host]" {
  var r = run({source: "import \"std/ui\" as ui\nprint(ui.size(1))\nui.mount(\"body\", fn() \{ return \"x\" })"})
  expect r.out == "1 B\n"
  expect r.err.has("dom.query() is JavaScript (std/ui's dom.js), which only the wasm build loads") && r.err.has(" [host]\n")
}
