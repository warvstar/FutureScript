// Packed 8-bit coverage masks. A glyph is rasterized once per face, size and
// display scale while the surface is open. Geometry can change independently.
#pragma once

struct Glyph {
    int offset, width, height, left, top;
};
struct Atlas {
    std::map<std::tuple<std::string,CGFloat,CGGlyph>,Glyph> glyphs;
    std::vector<uint8_t> coverage{0,0,0,0};
    std::map<std::string,Glyph> images;
    CGFloat scale = 1;

    Glyph image(NSString* path) {
        std::string key(path.UTF8String);
        if (auto i=images.find(key); i!=images.end()) return i->second;
        Glyph g{};
        NSImage* src=[[NSImage alloc] initWithContentsOfFile:path];
        CGImageRef bitmap=[src CGImageForProposedRect:nullptr context:nil hints:nil];
        if (bitmap) {
            size_t w=CGImageGetWidth(bitmap), h=CGImageGetHeight(bitmap);
            if (w && h && w<=4096 && h<=4096 && coverage.size()+w*h*4 < (1u<<24)) {
                size_t offset=(coverage.size()+3)&~size_t(3);
                coverage.resize(offset+w*h*4);
                CGColorSpaceRef space=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
                CGContextRef ctx=CGBitmapContextCreate(coverage.data()+offset,w,h,8,4*w,space,
                    kCGImageAlphaPremultipliedLast | (uint32_t)kCGBitmapByteOrder32Big);
                CGColorSpaceRelease(space);
                if (ctx) {
                    CGContextDrawImage(ctx,CGRectMake(0,0,w,h),bitmap);
                    CGContextRelease(ctx);
                    g={(int)offset,(int)w,(int)h,0,0};
                }
            }
        }
        images.emplace(key,g);
        return g;
    }

    Glyph get(CTFontRef font, CGGlyph id) {
        NSString* name=CFBridgingRelease(CTFontCopyPostScriptName(font));
        auto key=std::make_tuple(std::string(name.UTF8String),CTFontGetSize(font),id);
        if (auto i=glyphs.find(key); i!=glyphs.end()) return i->second;
        CGRect box=CTFontGetBoundingRectsForGlyphs(font,kCTFontOrientationHorizontal,&id,nullptr,1);
        Glyph g{};
        if (!CGRectIsEmpty(box)) {
            g.left=floor(CGRectGetMinX(box)*scale)-1;
            g.top=ceil(CGRectGetMaxY(box)*scale)+1;
            g.width=ceil(CGRectGetMaxX(box)*scale)+1-g.left;
            int bottom=floor(CGRectGetMinY(box)*scale)-1;
            g.height=g.top-bottom;
            g.offset=(int)coverage.size();
            coverage.resize(g.offset+g.width*g.height);
            CGContextRef ctx=CGBitmapContextCreate(coverage.data()+g.offset,g.width,g.height,8,
                                                    g.width,nullptr,kCGImageAlphaOnly);
            if (ctx) {
                CGContextScaleCTM(ctx,scale,scale);
                CGPoint pos=CGPointMake(-g.left/scale,-bottom/scale);
                CTFontDrawGlyphs(font,&id,&pos,1,ctx);
                CGContextRelease(ctx);
            }
        }
        glyphs.emplace(key,g);
        return g;
    }
};

// Four vec4s per instance: normalized rectangle and clip, atlas address and ink.
struct Quad { float rect[4],clip[4],mask[4],ink[4]; };
struct Batch {
    Atlas& atlas;
    CGFloat width,height;
    std::vector<Quad> quads;

    void add(CGRect r, CGRect clip, NSString* color, const Glyph* glyph=nullptr) {
        if (!CGRectIntersectsRect(r,clip) || CGRectIsEmpty(r)) return;
        unsigned value=rgb(color);
        unsigned alpha=255;
        if (color.length==9) { alpha=value&255; value>>=8; }
        Quad q={{(float)(r.origin.x/width),(float)(r.origin.y/height),(float)(r.size.width/width),(float)(r.size.height/height)},
                {(float)(clip.origin.x/width),(float)(clip.origin.y/height),(float)(CGRectGetMaxX(clip)/width),(float)(CGRectGetMaxY(clip)/height)},
                {-1,0,0,0},{((value>>16)&255)/255.f,((value>>8)&255)/255.f,(value&255)/255.f,alpha/255.f}};
        if (glyph) { q.mask[0]=glyph->offset; q.mask[1]=glyph->width; q.mask[2]=glyph->height; }
        quads.push_back(q);
    }
    void line(CTLineRef line, CGFloat x, CGFloat baseline, CGRect clip, NSString* color) {
        for (id item in (__bridge NSArray*)CTLineGetGlyphRuns(line)) {
            CTRunRef run=(__bridge CTRunRef)item;
            CTFontRef font=(CTFontRef)CFDictionaryGetValue(CTRunGetAttributes(run),kCTFontAttributeName);
            CFIndex count=CTRunGetGlyphCount(run);
            std::vector<CGPoint> positions(count);
            std::vector<CGGlyph> ids(count);
            CTRunGetPositions(run,CFRangeMake(0,0),positions.data());
            CTRunGetGlyphs(run,CFRangeMake(0,0),ids.data());
            for (CFIndex i=0;i<count;i++) {
                Glyph g=atlas.get(font,ids[i]);
                if (!g.width) continue;
                CGRect r=CGRectMake((round((x+positions[i].x)*atlas.scale)+g.left)/atlas.scale,
                    (round((baseline-positions[i].y)*atlas.scale)-g.top)/atlas.scale,g.width/atlas.scale,g.height/atlas.scale);
                add(r,clip,color,&g);
            }
        }
    }
};
