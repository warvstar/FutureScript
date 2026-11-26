// The host builtins: exec, files, env, JSON, and a fetch that fails without throwing (no network needed).
// native only
var e = exec({cmd: ["sh", "-c", "cat; echo oops >&2; exit 3"], input: "hi", cwd: "/tmp"})
print([e.code, e.out, e.err.len(), e.timedOut])
var es = execAll([["echo", "a"], ["sh", "-c", "sleep 0.2; echo b"]])
print(es[0].out + es[1].out)
print(exec({cmd: ["sleep", "5"], timeout: 0.2}).timedOut)
print(exec(["no-such-command-x"]).code)
writeFile("/tmp/fut-sys-test/a/b.txt", "x")
print([readFile("/tmp/fut-sys-test/a/b.txt"), readFile("/tmp/fut-sys-test/none")])
print([env("HOME") != nil, env("FUT_NO_SUCH_VAR"), time() > 1700000000])
var j = parseJson("\{\"a\": [1, 2, \{\"b\": null}], \"s\": \"\\u00e9\"}")
print([j.a[2], j.s, json({k: [1, "x", true, nil]})])
var f = fetch({url: "http://127.0.0.1:9/", timeout: 2})
print([f.status, f.error.len() > 0])
// expect: [3, "hi", 5, false]
// expect: a
// expect: b
// expect:
// expect: true
// expect: 127
// expect: ["x", nil]
// expect: [true, nil, true]
// expect: [{b: nil}, "é", "\{\"k\":[1,\"x\",true,null]}"]
// expect: [0, true]
