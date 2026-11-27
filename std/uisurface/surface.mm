// A layout owns the Yoga tree and its measured text. The same Core Text lines used
// to compute a leaf's height supply the positioned glyphs for its WebGPU batch.
#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#include <yoga/Yoga.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <memory>
#include <string>
#include <vector>

namespace {
NSDictionary* decode(const char* s) {
    return [NSJSONSerialization JSONObjectWithData:[@(s) dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
}
unsigned rgb(NSString* color) {
    return color.length ? (unsigned)strtoul([color substringFromIndex:1].UTF8String, nullptr, 16) : 0;
}
#include "glyphs.h"
struct Node {
    YGNodeRef box = YGNodeNew();
    NSDictionary *props, *style;
    NSString *text, *color, *family, *imagePath;
    NSArray *lines;
    CGFloat size = 16, lineHeight = 20, measuredWidth = -1;
    NSString *framePath;
    bool button = false, scroll = false;
    std::vector<std::unique_ptr<Node>> kids;
    ~Node() { YGNodeFree(box); }

    void wrap(CGFloat width) {
        if (measuredWidth == width) return;
        measuredWidth = width;
        CTFontRef font = CTFontCreateWithName((__bridge CFStringRef)family, size, nullptr);
        NSAttributedString* attr = [[NSAttributedString alloc] initWithString:text attributes:@{
            (__bridge NSString*)kCTFontAttributeName:(__bridge id)font,
            (__bridge NSString*)kCTForegroundColorFromContextAttributeName:@YES}];
        CTTypesetterRef ts = CTTypesetterCreateWithAttributedString((__bridge CFAttributedStringRef)attr);
        NSMutableArray* result = [NSMutableArray array];
        for (CFIndex i=0; i<(CFIndex)text.length;) {
            CFIndex len = std::max<CFIndex>(1, CTTypesetterSuggestLineBreak(ts, i, std::max<CGFloat>(1, width)));
            [result addObject:CFBridgingRelease(CTTypesetterCreateLine(ts, CFRangeMake(i, len)))];
            i += len;
        }
        lines = result;
        CFRelease(ts);
        CFRelease(font);
    }
};
YGSize measure(YGNodeConstRef node, float w, YGMeasureMode mode, float, YGMeasureMode) {
    Node& n = *(Node*)YGNodeGetContext(node);
    n.wrap(mode == YGMeasureModeUndefined ? 100000 : w);
    CGFloat maxWidth = 0;
    for (id line in n.lines) maxWidth = std::max(maxWidth, CTLineGetTypographicBounds((__bridge CTLineRef)line, nullptr, nullptr, nullptr));
    return {(float)ceil(maxWidth), (float)ceil(n.lineHeight * n.lines.count)};
}
using Setter = void (*)(YGNodeRef,float);
struct Property { NSString* key; Setter set; };
NSString* localImagePath(NSString* url) {
    if (![url hasPrefix:@"url("] || ![url hasSuffix:@")"]) return nil;
    return [[url substringWithRange:NSMakeRange(4,url.length-5)]
        stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"\"' "]];
}
std::unique_ptr<Node> build(NSDictionary* tree, NSDictionary* rules, Node* parent) {
    auto n = std::make_unique<Node>();
    n->props = tree[@"props"];
    NSMutableDictionary* s = [NSMutableDictionary dictionary];
    for (NSString* cls in [n->props[@"class"] componentsSeparatedByString:@" "]) {
        if (rules[cls]) [s addEntriesFromDictionary:rules[cls]];
    }
    if (n->props[@"style"]) {
        for (NSString* decl in [n->props[@"style"] componentsSeparatedByString:@";"]) {
            NSArray* pair = [decl componentsSeparatedByString:@":"];
            if (pair.count==2) s[[pair[0] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]] =
                [pair[1] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        }
    }
    n->style = s;
    n->framePath=localImagePath(s[@"border-image-source"]);
    if ([tree[@"tag"] isEqual:@"img"]) n->imagePath=n->props[@"src"];
    n->family = s[@"font-family"] ?: (parent ? parent->family : @"Helvetica");
    n->color = s[@"color"] ?: (parent ? parent->color : @"#ffffff");
    n->size = s[@"font-size"] ? [s[@"font-size"] doubleValue] : parent ? parent->size : 16;
    n->lineHeight = s[@"line-height"] ? [s[@"line-height"] doubleValue] : ceil(n->size*1.3);
    n->button = [tree[@"tag"] isEqual:@"button"];
    n->scroll = [s[@"overflow-y"] isEqual:@"auto"];
    auto b = n->box;
    YGNodeSetContext(b,n.get());
    YGNodeStyleSetFlexDirection(b,[s[@"flex-direction"] isEqual:@"row"] ? YGFlexDirectionRow : YGFlexDirectionColumn);
    YGNodeStyleSetFlexShrink(b,0);
    const Property properties[] = {{@"width",YGNodeStyleSetWidth},{@"height",YGNodeStyleSetHeight},
        {@"min-height",YGNodeStyleSetMinHeight},{@"min-width",YGNodeStyleSetMinWidth},
        {@"flex-grow",YGNodeStyleSetFlexGrow},{@"flex-shrink",YGNodeStyleSetFlexShrink}};
    for (auto& p : properties) if (s[p.key]) p.set(b,[s[p.key] floatValue]);
    if ([s[@"width"] hasSuffix:@"%"]) YGNodeStyleSetWidthPercent(b,[s[@"width"] floatValue]);
    if (s[@"padding"]) YGNodeStyleSetPadding(b,YGEdgeAll,[s[@"padding"] floatValue]);
    if (s[@"padding-left"]) YGNodeStyleSetPadding(b,YGEdgeLeft,[s[@"padding-left"] floatValue]);
    if (s[@"border-width"]) YGNodeStyleSetBorder(b,YGEdgeAll,[s[@"border-width"] floatValue]);
    if (s[@"gap"]) YGNodeStyleSetGap(b,YGGutterAll,[s[@"gap"] floatValue]);
    if ([s[@"align-items"] isEqual:@"center"]) YGNodeStyleSetAlignItems(b,YGAlignCenter);
    if ([s[@"align-items"] isEqual:@"flex-end"]) YGNodeStyleSetAlignItems(b,YGAlignFlexEnd);
    if ([s[@"justify-content"] isEqual:@"center"]) YGNodeStyleSetJustifyContent(b,YGJustifyCenter);
    if ([s[@"justify-content"] isEqual:@"space-between"]) YGNodeStyleSetJustifyContent(b,YGJustifySpaceBetween);
    if (n->scroll) YGNodeStyleSetOverflow(b,YGOverflowScroll);
    NSMutableString* text = [NSMutableString string];
    for (id child in tree[@"kids"]) {
        if ([child isKindOfClass:NSString.class]) [text appendString:child];
        else {
            auto item = build(child,rules,n.get());
            YGNodeInsertChild(b,item->box,n->kids.size());
            n->kids.push_back(std::move(item));
        }
    }
    if (text.length && n->kids.empty()) { n->text = text; YGNodeSetMeasureFunc(b,measure); }
    return n;
}

// AppKit delivers scroll events while Fut pumps its window. Keep the monitor
// scoped to the open surface and consume the accumulated delta in the game loop.
struct Wheel {
    id monitor;
    CGFloat delta=0;
    ~Wheel() { if (monitor) [NSEvent removeMonitor:monitor]; }
};
struct Layout {
    std::unique_ptr<Node> root;
    NSMutableArray* buttons = [NSMutableArray array];
    NSMutableDictionary* views = [NSMutableDictionary dictionary];
    std::string info;
    std::vector<Quad> quads;
    CGFloat scroll = 0, maxScroll = 0;
    int width, height;
    void visit(Node& n, CGPoint origin, CGRect clip, Batch* batch, NSString* focus) {
        auto b = n.box;
        CGRect rect = CGRectMake(origin.x+YGNodeLayoutGetLeft(b),origin.y+YGNodeLayoutGetTop(b),
                                 YGNodeLayoutGetWidth(b),YGNodeLayoutGetHeight(b));
        if (!CGRectIntersectsRect(rect,clip)) return;
        if (batch) {
            if (NSString* bg=n.style[@"background-color"]) batch->add(rect,clip,bg);
            if (n.imagePath) {
                Glyph img=batch->atlas.image(n.imagePath);
                if (img.width && img.height) {
                    CGRect dest=rect;
                    if ([n.style[@"object-fit"] isEqual:@"contain"]) {
                        CGFloat ratio=std::min(rect.size.width/img.width,rect.size.height/img.height);
                        dest=CGRectMake(CGRectGetMidX(rect)-img.width*ratio/2,CGRectGetMidY(rect)-img.height*ratio/2,img.width*ratio,img.height*ratio);
                    }
                    if (!CGRectIsEmpty(dest) && CGRectIntersectsRect(dest,clip)) {
                        batch->add(dest,clip,@"#ffffff",&img);
                        batch->quads.back().mask[3]=1;
                        if (n.style[@"opacity"]) batch->quads.back().ink[3]=[n.style[@"opacity"] floatValue];
                    }
                }
            }
            CGFloat border=[n.style[@"border-width"] doubleValue];
            bool framed=false;
            if (n.framePath && border>0 && !CGRectIsEmpty(rect)) {
                Glyph img=batch->atlas.image(n.framePath);
                if (img.width && img.height) {
                    batch->add(rect,clip,@"#ffffff",&img);
                    Quad& q=batch->quads.back();
                    q.mask[3]=2;
                    q.ink[0]=std::min<CGFloat>(0.49,border/rect.size.width);
                    q.ink[1]=std::min<CGFloat>(0.49,border/rect.size.height);
                    q.ink[2]=[n.style[@"border-image-slice"] floatValue];
                    framed=true;
                }
            }
            if (!framed && border>0 && n.style[@"border-color"]) {
                NSString* ink=n.style[@"border-color"];
                batch->add(CGRectMake(rect.origin.x,rect.origin.y,rect.size.width,border),clip,ink);
                batch->add(CGRectMake(rect.origin.x,CGRectGetMaxY(rect)-border,rect.size.width,border),clip,ink);
                batch->add(CGRectMake(rect.origin.x,rect.origin.y,border,rect.size.height),clip,ink);
                batch->add(CGRectMake(CGRectGetMaxX(rect)-border,rect.origin.y,border,rect.size.height),clip,ink);
            }
            if (n.button && [n.props[@"id"] isEqual:focus]) {
                CGRect inner=CGRectInset(rect,2,2);
                batch->add(CGRectMake(inner.origin.x,CGRectGetMaxY(inner)-3,inner.size.width,3),clip,@"#e6bb73");
            }
            if (n.text) {
                CGFloat pad = [n.style[@"padding"] doubleValue];
                n.wrap(std::max<CGFloat>(1,rect.size.width-2*pad));
                CGFloat y = rect.origin.y + (n.button ? (rect.size.height-n.lineHeight*n.lines.count)/2 : pad);
                for (id obj in n.lines) {
                    CTLineRef line=(__bridge CTLineRef)obj;
                    CGFloat ascent,descent;
                    CGFloat len=CTLineGetTypographicBounds(line,&ascent,&descent,nullptr);
                    CGFloat x=rect.origin.x + ([n.style[@"text-align"] isEqual:@"center"] ? (rect.size.width-len)/2 : pad);
                    batch->line(line,x,y+(n.lineHeight+ascent-descent)/2,CGRectIntersection(rect,clip),n.color);
                    y+=n.lineHeight;
                }
            }
        } else if ((n.props[@"data-gpu-view"] || (!n.button && n.props[@"data-inspect"])) && n.props[@"id"]) {
            CGRect hit=CGRectIntersection(rect,clip);
            views[n.props[@"id"]]=@{@"x":@(rect.origin.x),@"y":@(rect.origin.y),@"w":@(rect.size.width),@"h":@(rect.size.height),
                @"left":@(CGRectGetMinX(hit)),@"top":@(CGRectGetMinY(hit)),@"right":@(CGRectGetMaxX(hit)),@"bottom":@(CGRectGetMaxY(hit))};
        } else if (n.button && n.props[@"id"]) {
            CGRect hit=CGRectIntersection(rect,clip);
            [buttons addObject:@{@"id":n.props[@"id"],@"enabled":@(![n.props[@"disabled"] boolValue]),
                @"x":@(hit.origin.x),@"y":@(hit.origin.y),@"w":@(hit.size.width),@"h":@(hit.size.height)}];
        }
        CGPoint inner=rect.origin;
        if (n.scroll) {
            CGFloat bottom=0;
            for (auto& k:n.kids) bottom=std::max<CGFloat>(bottom,YGNodeLayoutGetTop(k->box)+YGNodeLayoutGetHeight(k->box));
            maxScroll=std::max<CGFloat>(0,bottom+[n.style[@"padding"] doubleValue]-rect.size.height);
            inner.y-=std::clamp(scroll,(CGFloat)0,maxScroll);
            clip=CGRectIntersection(rect,clip);
        }
        for (auto& child:n.kids) visit(*child,inner,clip,batch,focus);
    }
    void updateHits() {
        [buttons removeAllObjects];
        [views removeAllObjects];
        visit(*root,CGPointZero,bounds(),nullptr,nil);
        NSData* data=[NSJSONSerialization dataWithJSONObject:@{@"buttons":buttons,@"views":views,@"maxScroll":@(maxScroll)} options:0 error:nil];
        info.assign((const char*)data.bytes,data.length);
    }
    CGRect bounds() const { return CGRectMake(0,0,width,height); }
};
}

extern "C" {
int us_dom() { return 0; }
double us_scale() { return NSApp.keyWindow.backingScaleFactor ?: 1; }
Wheel* us_watch_wheel() {
    NSWindow* window=NSApp.keyWindow;
    if (!window) return nullptr;
    auto wheel=std::make_unique<Wheel>();
    Wheel* target=wheel.get();
    wheel->monitor=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel handler:^NSEvent*(NSEvent* event) {
        if (event.window!=window) return event;
        target->delta-=event.scrollingDeltaY*(event.hasPreciseScrollingDeltas ? 1 : 16);
        return nil;
    }];
    return wheel.release();
}
double us_take_wheel(Wheel* w) { double result=w->delta; w->delta=0; return result; }
void us_unwatch_wheel(Wheel* w) { delete w; }
int us_font(const char* path) { @autoreleasepool {
    NSURL* url=[NSURL fileURLWithPath:@(path)];
    CFErrorRef err=nullptr;
    bool ok=CTFontManagerRegisterFontsForURL((__bridge CFURLRef)url,kCTFontManagerScopeProcess,&err);
    if (err) { ok |= CFErrorGetCode(err)==kCTFontManagerErrorAlreadyRegistered; CFRelease(err); }
    return ok;
} }
Layout* us_layout(const char* json, int width, int height, double scroll) { @autoreleasepool {
    NSDictionary* data=decode(json);
    if (!data || width<=0 || height<=0) return nullptr;
    auto l=std::make_unique<Layout>();
    l->root=build(data[@"tree"],data[@"rules"],nullptr);
    l->width=width; l->height=height; l->scroll=scroll;
    YGNodeCalculateLayout(l->root->box,width,height,YGDirectionLTR);
    l->updateHits();
    return l.release();
} }
const char* us_info(Layout* l) { return l->info.c_str(); }
void us_scroll(Layout* l,double offset) { @autoreleasepool { l->scroll=offset; l->updateHits(); } }
void us_free(Layout* l) { delete l; }
Atlas* us_atlas() { return new Atlas(); }
void us_drop_atlas(Atlas* a) { delete a; }
int us_emit(Layout* l, Atlas* a, double scale, const char* focus) { @autoreleasepool {
    if (a->scale!=scale) { a->scale=scale; a->glyphs.clear(); a->images.clear(); a->coverage.assign(4,0); }
    Batch batch{*a,(CGFloat)l->width,(CGFloat)l->height,{}};
    l->visit(*l->root,CGPointZero,l->bounds(),&batch,@(focus));
    l->quads.swap(batch.quads);
    return (int)l->quads.size();
} }
void us_quads(Layout* l,float* output) { memcpy(output,l->quads.data(),l->quads.size()*sizeof(Quad)); }
int us_bytes(Atlas* a) { return (int)a->coverage.size(); }
void us_coverage(Atlas* a,void* output) { memcpy(output,a->coverage.data(),a->coverage.size()); }
}
