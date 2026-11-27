// A synchronous snapshot of a view for a non-DOM renderer. Event closures stay in Fut.
// Template slots use the same preorder as the DOM backend and the view compiler.
pub var current = nil

fn children(out, v) {
  var t=typeOf(v)
  if t=="function" { children(out,v()) }
  else if t=="list" { for c in v { children(out,c) } }
  else if t=="map" { out.push(v) }
  else if v!=nil && t!="bool" { out.push(str(v)) }
}

pub fn props(n, values) {
  for k in values {
    var v=values[k]
    if k.slice(0,2)=="on" { n.events[k]=v }
    else { n.props[k]=typeOf(v)=="function" ? v() : v }
  }
}

pub fn element(tag, values, kids) {
  var n={tag:tag,props:{},kids:[],events:{}}
  if values!=nil { props(n,values) }
  children(n.kids,kids)
  return n
}

fn instantiate(d,wired,root) {
  if d==nil {
    var slot=element("#slot",nil,nil)
    wired.push(slot)
    return slot
  }
  if typeOf(d)!="list" { return str(d) }
  var n=element(d[0],d[1],nil)
  if root || d[2]%2==1 { wired.push(n) }
  if d[2]>=2 {
    var slot=element("#slot",nil,nil)
    n.kids.push(slot)
    wired.push(slot)
  } else { for j in 3..d.len() { n.kids.push(instantiate(d[j],wired,false)) } }
  return n
}
pub fn template(desc,wire) {
  var wired=[]
  var root=instantiate(desc(),wired,true)
  wire(wired)
  return root
}
pub fn fill(slot,content) { children(slot.kids,content) }

fn finish(n,handlers) {
  var kids=[]
  for c in n.kids {
    if typeOf(c)=="map" {
      finish(c,handlers)
      if c.tag=="#slot" { for child in c.kids { kids.push(child) } }
      else { kids.push(c) }
    } else { kids.push(c) }
  }
  n.kids=kids
  if n.events.keys().len()>0 {
    if n.props.id==nil { throw "surface view: an interactive element needs an id" }
    handlers[n.props.id]=n.events
  }
  n.remove("events")
}
pub fn capture(view) {
  var previous=current
  current=true
  try {
    var nodes=[]
    children(nodes,view())
    if nodes.len()!=1 || typeOf(nodes[0])!="map" { throw "surface view: expected one root element" }
    var handlers={}
    finish(nodes[0],handlers)
    current=previous
    return {tree:nodes[0],handlers:handlers}
  } catch e { current=previous throw e }
}
