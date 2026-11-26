// A Cloudflare Worker in fut: requests come to serve(req), req = {method, url, headers, body}.
//   node web/worker.mjs build examples/worker/hello.fut build/worker && cd build/worker && npx wrangler dev
//   GET /            JSON: a greeting and how many requests this instance has served
//   GET /echo        the request, as JSON
//   GET /fetch?url=  fetches url and reports its status and size
var hits = 0

fn pathOf(url) {  // "https://host/a/b?q" -> "/a/b"
  var rest = url.slice(url.find("//") + 2, len(url))
  var at = rest.find("/")
  if at < 0 { return "/" }
  var p = rest.slice(at, len(rest))
  var q = p.find("?")
  if q < 0 { return p }
  return p.slice(0, q)
}

fn query(url, name) {  // the value of ?name=..., or nil
  var q = url.find("?")
  if q < 0 { return nil }
  for kv in url.slice(q + 1, len(url)).split("&") {
    var eq = kv.find("=")
    if eq > 0 && kv.slice(0, eq) == name { return kv.slice(eq + 1, len(kv)) }
  }
  return nil
}

fn serve(req) {
  hits += 1
  var path = pathOf(req.url)
  if path == "/" { return {body: {hello: "from fut", hits: hits}} }
  if path == "/echo" { return {body: req} }
  if path == "/fetch" {
    var url = query(req.url, "url")
    if url == nil { return {status: 400, body: "give ?url=\n"} }
    var r = fetch(url)
    if r.status == 0 { return {status: 502, body: {error: r.error}} }
    return {body: {url: url, status: r.status, bytes: len(r.body), type: r.headers["content-type"]}}
  }
  return {status: 404, body: "no route for {path}\n"}
}
