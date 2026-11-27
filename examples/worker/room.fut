// fut worker build --durable Room --binding ROOMS examples/worker/room.fut build/room
// Serve build/room with Wrangler. Each URL path names a room with its own persisted message count.
import "std/cloudflare" as cf

fn route(request) {
  var start=request.url.find("://")+3
  var path=request.url.slice(start,request.url.len())
  var slash=path.find("/")
  if slash<0 { return "/" }
  path=path.slice(slash,path.len())
  var query=path.find("?")
  return query<0 ? path : path.slice(0,query)
}

fn durable(event,host) {
  if event.type=="fetch" {
    if event.headers.upgrade=="websocket" { return cf.upgrade(host,["chat"],{joined:true}) }
    return {body:{messages:cf.get(host,"messages")}}
  }
  if event.type=="message" && cf.attachment(event.socket).joined {
    var count=cf.get(host,"messages")
    cf.put(host,"messages",count==nil ? 1 : count+1)
    for peer in cf.sockets(host,"chat") { cf.send(peer,event.message) }
  }
}
