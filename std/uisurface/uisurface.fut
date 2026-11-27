// Modal views shared by the web DOM and the native Yoga/Core Text renderer.
import "std/ui"
import "std/math" as math
extern platform fn us_dom(): int
extern dialog fn us_open(str,str,str,str,str): int
extern dialog fn us_target(int): str
extern dialog fn us_width(int): int
extern dialog fn us_height(int): int
extern dialog fn us_inspect(int): str
extern dialog fn us_viewport(int,str): str
extern dialog fn us_active(int): str
extern dialog fn us_focus(int,str)
extern dialog fn us_close(int)
extern paint fn us_font(str): int
extern paint fn us_scale(): num
extern paint fn us_watch_wheel(): ptr
extern paint fn us_take_wheel(ptr): num
extern paint fn us_unwatch_wheel(ptr)
extern paint fn us_layout(str,int,int,num): ptr
extern paint fn us_info(ptr): str
extern paint fn us_scroll(ptr,num)
extern paint fn us_free(ptr)
extern paint fn us_atlas(): ptr
extern paint fn us_drop_atlas(ptr)
extern paint fn us_emit(ptr,ptr,num,str): int
extern paint fn us_quads(ptr,ptr)
extern paint fn us_bytes(ptr): int
extern paint fn us_coverage(ptr,ptr)

var registered={}
var SHADER="
struct Quad \{ rect:vec4f, clip:vec4f, mask:vec4f, ink:vec4f }
@group(0) @binding(0) var<storage,read> quads:array<Quad>;
@group(0) @binding(1) var<storage,read> atlas:array<u32>;
struct V \{ @builtin(position) pos:vec4f, @location(0) uv:vec2f, @location(1) xy:vec2f, @location(2) @interpolate(flat) id:u32 }
@vertex fn vs(@builtin(vertex_index) i:u32)->V \{
  var corners=array<vec2f,6>(vec2f(0,0),vec2f(1,0),vec2f(1,1),vec2f(0,0),vec2f(1,1),vec2f(0,1));
  let id=i/6u; let uv=corners[i%6u]; let rect=quads[id].rect; let xy=rect.xy+uv*rect.zw;
  return V(vec4f(xy*vec2f(2,-2)+vec2f(-1,1),0,1),uv,xy,id);
}
fn sampleMask(mask:vec4f,p:vec2i)->f32 \{
  let xy=vec2u(clamp(p,vec2i(0),vec2i(mask.yz)-1));
  let addr=u32(mask.x)+xy.y*u32(mask.y)+xy.x;
  return f32((atlas[addr/4u]>>((addr%4u)*8u))&255u)/255.;
}
fn sampleImage(mask:vec4f,p:vec2i)->vec4f \{
  let xy=vec2u(clamp(p,vec2i(0),vec2i(mask.yz)-1));
  return unpack4x8unorm(atlas[u32(mask.x)/4u+xy.y*u32(mask.y)+xy.x]);
}
@fragment fn fs(v:V)->@location(0) vec4f \{
  let q=quads[v.id];
  if any(v.xy<q.clip.xy) || any(v.xy>=q.clip.zw) \{ discard; }
  if q.mask.x<0 \{ return q.ink; }
  var uv=v.uv;
  if q.mask.w==2 \{
    let dest=q.ink.xy; let src=vec2f(q.ink.z)/q.mask.yz;
    let center=src+(uv-dest)*(vec2f(1)-2*src)/(vec2f(1)-2*dest);
    uv=select(select(center,uv*src/dest,uv<dest),vec2f(1)-(vec2f(1)-uv)*src/dest,uv>vec2f(1)-dest);
  }
  let p=uv*q.mask.yz-.5; let cell=vec2i(floor(p)); let f=fract(p);
  if q.mask.w>0 \{
    let top=mix(sampleImage(q.mask,cell),sampleImage(q.mask,cell+vec2i(1,0)),f.x);
    let bottom=mix(sampleImage(q.mask,cell+vec2i(0,1)),sampleImage(q.mask,cell+vec2i(1,1)),f.x);
    let c=mix(top,bottom,f.y);
    return vec4f(c.rgb/max(c.a,0.0001),c.a*q.ink.a);
  }
  let a=mix(sampleMask(q.mask,cell),sampleMask(q.mask,cell+vec2i(1,0)),f.x);
  let b=mix(sampleMask(q.mask,cell+vec2i(0,1)),sampleMask(q.mask,cell+vec2i(1,1)),f.x);
  return vec4f(q.ink.rgb,q.ink.a*mix(a,b,f.y));
}"

pub class Surface {
  var dom=false
  var width=0
  var height=0
  var views={}
  var buttons=[]
  var focus=""
  var scroll=0
  var maxScroll=0
  var rules
  var handle=0
  var layout=0
  var dispose
  var handlers={}
  var revision
  var wheel=0
  var atlas=0
  var coverage
  var quads
  var count=0
  var usedBytes=0
  var scale=0
  var pipeline
  var device
  var dirty=true

  fn init(title,rules,fonts,dismiss,shortcut) {
    self.rules=rules
    self.dom=us_dom()!=0
    self.focus=dismiss
    if self.dom { self.handle=us_open(title,json(rules),json(fonts),dismiss,shortcut) }
    else {
      for f in fonts {
        if !registered.has(f.path) {
          if us_font(f.path)==0 { throw "surface font could not load: {f.path}" }
          registered[f.path]=true
        }
      }
      self.wheel=us_watch_wheel()
    }
  }

  // Rebuild a view only when its model or logical viewport changes.
  fn update(w,h,revision,view) {
    if self.dom { w=us_width(self.handle) h=us_height(self.handle) }
    w=math.max(1,w) h=math.max(1,h)
    if self.width==w && self.height==h && self.revision==revision { return }
    self.width=w self.height=h
    if self.dom {
      var focus=us_active(self.handle)
      if self.dispose!=nil { var stop=self.dispose stop() }
      self.dispose=ui.mount(us_target(self.handle),view)
      us_focus(self.handle,focus)
    } else {
      var snap=ui.snapshot(view)
      var data=json({tree:snap.tree,rules:self.rules})
      var next=us_layout(data,w,h,self.scroll)
      if next==0 { throw "surface layout failed" }
      if self.layout!=0 { us_free(self.layout) }
      self.layout=next
      var info=parseJson(us_info(next))
      self.views=info.views self.buttons=info.buttons self.maxScroll=info.maxScroll
      self.scroll=math.min(self.maxScroll,self.scroll)
      self.handlers=snap.handlers
      self.dirty=true
      var found=false
      for b in self.buttons { if b.enabled && b.id==self.focus { found=true } }
      if !found { for b in self.buttons { if b.enabled { self.focus=b.id break } } }
    }
    self.revision=revision
  }
  fn scrollBy(delta) {
    var offset=math.clamp(self.scroll+delta,0,self.maxScroll)
    if self.dom || offset==self.scroll { return }
    self.scroll=offset
    us_scroll(self.layout,offset)
    var info=parseJson(us_info(self.layout))
    self.buttons=info.buttons self.views=info.views
    self.dirty=true
  }
  fn inspectTarget() { return self.dom ? us_inspect(self.handle) : self.focus }
  fn viewport(id) {
    return self.dom ? parseJson(us_viewport(self.handle,id)) : self.views[id]
  }
  fn activate(id) {
    for b in self.buttons {
      if b.id==id && b.enabled {
        var h=self.handlers[id]
        if h!=nil && h.onclick!=nil {
          self.focus=id self.dirty=true
          h.onclick({type:"click"})
        }
        return
      }
    }
  }
  fn click(x,y) {
    for b in self.buttons {
      if x>=b.x && x<b.x+b.w && y>=b.y && y<b.y+b.h { self.activate(b.id) return }
    }
  }
  fn advance() {
    var ids=[]
    for b in self.buttons { if b.enabled { ids.push(b.id) } }
    if ids.len()==0 { return }
    var index=-1
    for j in 0..ids.len() { if ids[j]==self.focus { index=j } }
    self.focus=ids[(index+1)%ids.len()] self.dirty=true
  }
  fn releaseGpu() {
    if self.pipeline!=nil { self.pipeline.free() self.pipeline=nil }
    if self.quads!=nil { self.quads.free() self.quads=nil }
    if self.coverage!=nil { self.coverage.free() self.coverage=nil }
    self.usedBytes=0
  }
  // Append ordered panel and glyph quads to the caller's WebGPU frame.
  fn draw(gpu,frame) {
    if self.dom || self.layout==0 { return }
    if self.wheel!=0 { self.scrollBy(us_take_wheel(self.wheel)) }
    var scale=gpu.headless ? 1 : math.max(1,us_scale())
    if self.device!=gpu { self.releaseGpu() self.device=gpu self.dirty=true }
    if self.scale!=scale { self.scale=scale self.usedBytes=0 self.dirty=true }
    if self.atlas==0 { self.atlas=us_atlas() }
    if self.dirty {
      self.count=us_emit(self.layout,self.atlas,scale,self.focus)
      var bytes=us_bytes(self.atlas)
      var growth=self.quads==nil || self.quads.bytes<self.count*64 || self.coverage==nil || self.coverage.bytes<bytes
      if growth {
        self.releaseGpu()
        self.quads=gpu.storage(f32s(math.max(64,self.count*24)))
        self.coverage=gpu.storage(u32s(math.ceil(bytes/4)*2))
        self.pipeline=gpu.render({shader:SHADER,blend:true,bind:[self.quads,self.coverage]})
      }
      if self.count>0 {
        var data=f32s(self.count*16)
        us_quads(self.layout,data)
        self.quads.write(data)
      }
      if self.usedBytes!=bytes {
        var pixels=u32s(math.ceil(bytes/4))
        us_coverage(self.atlas,pixels)
        self.coverage.write(pixels)
        self.usedBytes=bytes
      }
      self.dirty=false
    }
    if self.count>0 { frame.draw(self.pipeline,nil,self.count*6) }
  }
  fn close() {
    if self.dispose!=nil { var stop=self.dispose stop() self.dispose=nil }
    if self.dom && self.handle!=0 { us_close(self.handle) self.handle=0 }
    if self.layout!=0 { us_free(self.layout) self.layout=0 }
    self.releaseGpu()
    if self.wheel!=0 { us_unwatch_wheel(self.wheel) self.wheel=0 }
    if self.atlas!=0 { us_drop_atlas(self.atlas) self.atlas=0 }
    self.handlers={}
  }
}
