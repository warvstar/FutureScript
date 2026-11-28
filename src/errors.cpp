// Error codes: every compile and runtime error the compiler or runtime raises names the rule it broke with a stable
// code, shown after the message ("undefined name 'x' [undefined-name]") and as "code" in --json records. A code is
// never renamed or reused; `fut explain code` prints its rule and fix. The format string of an error states its
// code with a leading "@code " (fail("@syntax expected '%c'", c), ERR("@index index %s out of range")), which
// takeCode strips. tests/specs/codes.fut raises each code at least once.
#include "fut.h"
#include <cstring>

namespace {
struct ErrCode { const char *code, *rule, *fix; };
const ErrCode codes[] = {
    // compile time
    {"syntax", "The source doesn't parse.", "Read the caret: it points where the parser expected something else."},
    {"undefined-name", "A name is used that nothing defines where it is used.", "Define it, import the file or package that does, or fix the spelling."},
    {"private", "A name private to a file is used from another file (or in a pub contract).", "Mark it pub where it is declared, or don't use it here."},
    {"not-in-interface", "A name is pub inside a package but not part of the package's interface.", "Make it pub in the root file, or re-export its file there with pub import."},
    {"not-a-value", "A module or an effect is used as a value.", "Use one of its members: m.name, or Effect.op(...)."},
    {"arity", "A call passes a different number of arguments than the function, operation or handler takes.", "Pass exactly the declared parameters."},
    {"unknown-type", "A type annotation names no type.", "Use num, str, bool, list, map, fn, any, a class, a type declared with type, A | B, a literal (\"GET\", 3, true, nil), a {shape}, list<T>, map<K, V>, or T?."},
    {"unknown-effect", "An effect name or a handled operation isn't one.", "Use io, clock, random, yield, a declared effect, or one of its operations."},
    {"misplaced", "A construct is used where it isn't allowed (self outside a method, break outside a loop, a nested named fn, ...).", "Move it where the message says it belongs."},
    {"duplicate", "A name is declared twice in one scope.", "Rename or remove one of them."},
    {"class", "A class declaration breaks a rule (its superclass, field defaults, init).", "Follow the message: use scalar literals, [] or {} for fields; set other values in init."},
    {"override", "A method replaces an inherited one without override, or override replaces nothing, or changes its parameters, types or effects.", "Write override exactly when replacing, and keep the replaced method's signature."},
    {"import", "An import can't be resolved, crosses a package boundary, or forms a cycle.", "Import files by path inside a package and packages by name (with a dep line in fut.pkg)."},
    {"manifest", "A fut.pkg line is malformed.", "Fix the line: name, version, root, dep, js, wasm or c."},
    {"extern", "An extern declaration can't bind: an unknown module, function, library or parameter type.", "List the module in fut.pkg for this build, and use the types the message names."},
    {"gpu", "A gpu fn uses something the GPU doesn't have (strings, lists, maps, globals, recursion, an untyped parameter, ...).", "Use num, bool and vec2f/3f/4f values, locals and parameters, and call only gpu fns, sqrt, floor, int and std/math."},
    {"limit", "A fixed limit is exceeded (parameters, fields, effects).", "Pass a map or list instead of more parameters; split the class or effect."},
    // compile time or run time
    {"type", "A value doesn't have the declared type of a parameter, result, field or var.", "Pass or store a value of that type, or change the declaration (T? allows nil)."},
    {"atomic", "Something that can't be undone (io, yield) happens inside atomic { }.", "Do it after the block, or handle the io inside the block."},
    {"effect", "A function performs or calls something its declared effects don't allow (a compile error where the operation or the declaring fn is called directly; otherwise when it runs).", "Add the effect to the declaration (and its callers'), or handle it."},
    {"unhandled", "An operation of a declared effect is performed with no handler for it.", "Wrap the call in handle { } with Effect.op(...) { }."},
    {"match", "A match leaves out variants of its enum with no else arm, or its value is not one of the enum's variants.", "Add an arm for each variant the message lists (or else { }), and match only values of that enum."},
    {"no-field", "A field is read or written that the object doesn't have (or, in a closed class, didn't declare). A compile error where the type is known: x: C, or a local var x = C(...).", "Declare it in the class (var name) or the shape, or fix the spelling."},
    // run time
    {"operand", "An operator got operand types it doesn't take.", "Convert first: str(x) for text, int(x) for whole numbers; check for nil."},
    {"no-method", "A method is called that the value doesn't have.", "Check the value's type; list, map, str and fiber have a fixed set of methods."},
    {"index", "An index is out of range or not a whole number, or the value can't be indexed.", "Check 0 <= i < len first; pop() needs a non-empty list."},
    {"not-iterable", "for-in over a value that can't be iterated.", "Iterate a list, map, str, range (a..b) or fiber."},
    {"not-callable", "A call of something that isn't a function.", "Check the value; a map key must hold a function to be called."},
    {"budget", "Code running under budget(n, f) took more than n steps (loop iterations and calls).", "Give it a bigger budget, or make it do less: the count is the same on every platform."},
    {"stack-overflow", "Calls nested too deep, usually unbounded recursion.", "Add the base case, or turn the recursion into a loop."},
    {"stale-call", "Old code still running called a function whose parameters a hot reload changed.", "Nothing to fix in the source: let the old frame finish or restart."},
    {"fiber", "A fiber was used wrongly: resumed when finished or running, yield outside one.", "Check f.done() before resuming; yield only inside a fiber's function."},
    {"argument", "A builtin got an argument it doesn't take.", "Pass what the message lists."},
    {"requires", "A caller broke a function's requires clause: the caller's fault.", "Make the call satisfy the clause, or check it before calling."},
    {"ensures", "A function broke its own ensures clause: the implementation's fault.", "Fix the function's body so the clause holds on every return."},
    {"invariant", "A class invariant failed after a call or a field store.", "Fix the method or store that left the object inconsistent."},
    {"expect", "An expect failed.", "The message shows both sides; fix the code or the expectation."},
    {"send", "A value can't be sent to another isolate (a fiber, a js value, a native function, or nested too deep).", "Send plain data: numbers, strings, lists, maps, instances of compiled classes."},
    {"channel", "A channel was used after it was closed.", "Stop receiving once recv() reports the end; close a channel once."},
    {"parallel", "A parallel for's body did something that would make its result depend on which isolate ran which iteration: assigned a variable from outside it, changed a value from outside it, reached a buffer it writes at an index other than its own, or performed an effect.", "Write each iteration's result to a buffer at [i], and combine the results after the loop."},
    {"frozen", "A frozen value (freeze() made it) was changed, or freeze() was given something it can't copy: a channel, task, fiber or js value, a value that contains itself, or one nested too deep.", "Change a copy: thaw(v) gives one you may change. Freeze plain data without cycles."},
    {"spawn", "spawn was given something it can't run, or the build can't run isolates.", "spawn a fut function (not a builtin), in the native build."},
    {"isolate", "This isolate was stopped because the isolate that started it ended with an error.", "Fix the error the other isolate reported."},
    {"replay", "A run under fut --replay asked for something other than what the recorded run did at that point, or printed something else.", "The program changed since the recording, or did something the log can't repeat (isolates' timing): record it again."},
    {"js", "A value can't cross to or from JavaScript, or JavaScript threw.", "Pass plain data (numbers, strings, lists, maps with string keys); catch the JS error with try."},
    {"host", "The host couldn't do it: no display, a file that can't be written, a process that can't start.", "Check the environment the message names."},
    {"memory", "Out of memory.", "Allocate less, or free references to what's no longer needed."},
};
const ErrCode* find(std::string_view c) {
  for (auto& k : codes) if (c == k.code) return &k;
  return nullptr;
}
// The messages of the last runtime errors raised, with their codes: an error is a string value like any other, so its
// code is found again by that value (and its text, in case the string was collected and its address reused).
struct Raised { V v = NIL; std::string text; const char* code = nullptr; };
TLS Raised raised[16];
TLS unsigned nraised;
}  // namespace

const char* takeCode(const char*& fmt) {
  if (!fmt || fmt[0] != '@') return nullptr;
  const char* e = strchr(fmt, ' ');
  if (!e) return nullptr;
  const ErrCode* k = find(std::string_view(fmt + 1, size_t(e - fmt - 1)));
  if (!k) return nullptr;
  fmt = e + 1;
  return k->code;
}
void noteRaised(V v, const std::string& text, const char* code) { raised[nraised++ % 16] = {v, text, code}; }
const char* raisedCode(V v) {
  for (auto& r : raised)
    if (r.code && r.v == v && is(v, K::Str) && as<Str>(v)->sv() == r.text) return r.code;
  return nullptr;
}
std::string errorLine(V v) {
  std::string s = toStr(v);
  if (const char* c = raisedCode(v)) s += " [" + std::string(c) + "]";
  return s;
}
std::string splitCode(std::string& line) {
  size_t e = line.size();
  if (e < 4 || line[e - 1] != ']') return "";
  size_t b = line.rfind(" [");
  if (b == std::string::npos || !find(std::string_view(line).substr(b + 2, e - b - 3))) return "";
  std::string c = line.substr(b + 2, e - b - 3);
  return line.resize(b), c;
}
std::string errorExplanation(std::string_view code) {
  const ErrCode* k = find(code);
  return k ? std::string(k->rule) + "\n\nFix: " + k->fix : "";
}
int explain(const char* code) {
  if (!code) {
    for (auto& k : codes) printf("%-17s %s\n", k.code, k.rule);
    return 0;
  }
  const ErrCode* k = find(code);
  if (!k) return fprintf(stderr, "no error code '%s' (fut explain lists them)\n", code), 1;
  printf("%s: %s\nfix: %s\n", k->code, k->rule, k->fix);
  return 0;
}
