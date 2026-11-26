#!/usr/bin/env python3
"""Writes accept/*_test.fut for swarm/targets/fwebbrowser.md.

The expected values come from the reference below, a direct reading of the spec (no browser agrees with every
simplification the spec makes, so none can be the oracle). Change the spec and this file together; the cases were
also checked by hand. Run: python3 swarm/targets/fwebbrowser/gen.py
"""
import os, re

WS = " \t\n\r\f"
WSRE = r"[ \t\n\r\f]"

# ---------- HTML ----------
VOID = set("area base br col embed hr img input link meta source track wbr".split())
RAW = {"script", "style"}
P_CLOSERS = set("address article aside blockquote div dl fieldset footer form h1 h2 h3 h4 h5 h6 header hr main nav ol p pre "
                "section table ul".split())
ENT = {"amp": "&", "lt": "<", "gt": ">", "quot": '"', "#39": "'"}


def decode(s):
    return re.sub(r"&(amp|lt|gt|quot|#39);", lambda m: ENT[m.group(1)], s)


def is_letter(c):
    return ("a" <= c <= "z") or ("A" <= c <= "Z")


def parse_start(h, i):
    n = len(h)
    m = re.compile(r"[^ \t\n\r\f/>]+").match(h, i + 1)
    name, j = m.group(0).lower(), m.end()
    attrs = {}
    while True:
        while j < n and h[j] in WS:
            j += 1
        if j >= n:
            return None
        if h[j] == ">":
            return name, attrs, False, j + 1
        if h[j] == "/":
            j += 1
            if j < n and h[j] == ">":
                return name, attrs, True, j + 1
            continue
        m = re.compile(r"[^ \t\n\r\f/>=]+").match(h, j)
        if not m:
            j += 1
            continue
        an, j = m.group(0).lower(), m.end()
        k = j
        while k < n and h[k] in WS:
            k += 1
        val = ""
        if k < n and h[k] == "=":
            k += 1
            while k < n and h[k] in WS:
                k += 1
            if k < n and h[k] in "\"'":
                e = h.find(h[k], k + 1)
                if e < 0:
                    return None
                val, j = h[k + 1:e], e + 1
            else:
                m2 = re.compile(r"[^ \t\n\r\f>]*").match(h, k)
                val, j = m2.group(0), m2.end()
        if an not in attrs:
            attrs[an] = decode(val)


def parse(html):
    doc = {"type": "document", "children": []}
    stack = [doc]
    buf = []
    i, n = 0, len(html)

    def flush():
        s = "".join(buf)
        buf.clear()
        if s == "":
            return
        stack[-1]["children"].append({"type": "text", "text": decode(s)})

    def close_nearest(targets, stoppers):
        for k in range(len(stack) - 1, 0, -1):
            t = stack[k]["tag"]
            if t in targets:
                del stack[k:]
                return
            if t in stoppers:
                return

    while i < n:
        c = html[i]
        if c != "<":
            buf.append(c)
            i += 1
            continue
        if html.startswith("<!--", i):
            j = html.find("-->", i + 4)
            i = n if j < 0 else j + 3
            continue
        if html.startswith("<!", i) or html.startswith("<?", i):
            j = html.find(">", i)
            i = n if j < 0 else j + 1
            continue
        if html.startswith("</", i) and i + 2 < n and is_letter(html[i + 2]):
            j = html.find(">", i)
            if j < 0:
                break
            name = re.compile(r"[^ \t\n\r\f/>]+").match(html, i + 2).group(0).lower()
            i = j + 1
            for k in range(len(stack) - 1, 0, -1):
                if stack[k]["tag"] == name:
                    flush()
                    del stack[k:]
                    break
            continue
        if i + 1 < n and is_letter(html[i + 1]):
            r = parse_start(html, i)
            if r is None:
                break
            name, attrs, selfclose, i = r
            flush()
            if name in P_CLOSERS and stack[-1].get("tag") == "p":
                stack.pop()
            if name == "li":
                close_nearest({"li"}, {"ul", "ol"})
            if name == "tr":
                close_nearest({"tr"}, {"table", "tbody", "thead", "tfoot"})
            if name in ("td", "th"):
                close_nearest({"td", "th"}, {"tr", "table"})
            e = {"type": "element", "tag": name, "attrs": attrs, "children": []}
            stack[-1]["children"].append(e)
            if name in VOID or selfclose:
                continue
            if name in RAW:
                m = re.compile(r"</" + name + r"[ \t\n\r\f/>]", re.I).search(html, i)
                end = m.start() if m else n
                raw = html[i:end]
                if raw != "":
                    e["children"].append({"type": "text", "text": raw})
                if m:
                    j = html.find(">", m.start())
                    i = n if j < 0 else j + 1
                else:
                    i = n
                continue
            stack.append(e)
            if name == "pre" and html.startswith("\n", i):
                i += 1
            continue
        buf.append(c)
        i += 1
    flush()
    return doc


def esc_text(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def serialize(node, parent=None):
    if node["type"] == "text":
        return node["text"] if parent in RAW else esc_text(node["text"])
    if node["type"] == "document":
        return "".join(serialize(c) for c in node["children"])
    t = node["tag"]
    a = "".join(' %s="%s"' % (k, esc_text(v).replace('"', "&quot;")) for k, v in node["attrs"].items())
    if t in VOID:
        return "<%s%s>" % (t, a)
    return "<%s%s>%s</%s>" % (t, a, "".join(serialize(c, t) for c in node["children"]), t)


def text_content(node):
    if node["type"] == "text":
        return node["text"]
    return "".join(text_content(c) for c in node["children"])


# ---------- tree helpers ----------
class Tree:
    def __init__(self, doc):
        self.doc, self.parent = doc, {}
        self._index(doc)

    def _index(self, node):
        for c in node.get("children", []):
            self.parent[id(c)] = node
            self._index(c)

    def parent_el(self, e):
        p = self.parent.get(id(e))
        return p if p is not None and p["type"] == "element" else None

    def siblings(self, e):
        return [c for c in self.parent[id(e)]["children"] if c["type"] == "element"]

    def preorder(self, node):
        for c in node.get("children", []):
            if c["type"] == "element":
                yield c
                yield from self.preorder(c)


# ---------- selectors ----------
class SelErr(Exception):
    pass


IDENT = re.compile(r"[A-Za-z0-9_-]+")


class SelParser:
    def __init__(self, s):
        self.s, self.i = s, 0

    def ws(self):
        st = self.i
        while self.i < len(self.s) and self.s[self.i] in WS:
            self.i += 1
        return self.i > st

    def ident(self):
        m = IDENT.match(self.s, self.i)
        if not m:
            raise SelErr()
        self.i = m.end()
        return m.group(0)

    def compound(self):
        s = self.s
        c = {"tag": None, "id": [], "cls": [], "attr": [], "pseudo": [], "not": []}
        st = self.i
        if self.i < len(s) and s[self.i] == "*":
            self.i += 1
            c["tag"] = "*"
        elif IDENT.match(s, self.i):
            c["tag"] = self.ident().lower()
        while self.i < len(s):
            ch = s[self.i]
            if ch == "#":
                self.i += 1
                c["id"].append(self.ident())
            elif ch == ".":
                self.i += 1
                c["cls"].append(self.ident())
            elif ch == "[":
                self.i += 1
                self.ws()
                name = self.ident().lower()
                self.ws()
                op = val = None
                for o in ("~=", "^=", "$=", "*=", "="):
                    if s.startswith(o, self.i):
                        op = o
                        self.i += len(o)
                        break
                if op:
                    self.ws()
                    if self.i < len(s) and s[self.i] in "\"'":
                        e = s.find(s[self.i], self.i + 1)
                        if e < 0:
                            raise SelErr()
                        val = s[self.i + 1:e]
                        self.i = e + 1
                    else:
                        val = self.ident()
                    self.ws()
                if self.i >= len(s) or s[self.i] != "]":
                    raise SelErr()
                self.i += 1
                c["attr"].append((name, op, val))
            elif ch == ":":
                if s.startswith(":first-child", self.i):
                    self.i += 12
                    c["pseudo"].append("first")
                elif s.startswith(":last-child", self.i):
                    self.i += 11
                    c["pseudo"].append("last")
                elif s.startswith(":not(", self.i):
                    self.i += 5
                    self.ws()
                    inner = self.compound()
                    self.ws()
                    if self.i >= len(s) or s[self.i] != ")":
                        raise SelErr()
                    self.i += 1
                    c["not"].append(inner)
                else:
                    raise SelErr()
            else:
                break
        if self.i == st:
            raise SelErr()
        return c

    def complex(self):
        parts = [(None, self.compound())]
        while True:
            had = self.ws()
            if self.i >= len(self.s) or self.s[self.i] == ",":
                return parts
            if self.s[self.i] in ">+~":
                comb = self.s[self.i]
                self.i += 1
                self.ws()
                parts.append((comb, self.compound()))
            elif had:
                parts.append((" ", self.compound()))
            else:
                raise SelErr()

    def groups(self):
        self.ws()
        gs = [self.complex()]
        while self.i < len(self.s):
            self.i += 1
            self.ws()
            gs.append(self.complex())
        return gs


def parse_selector(s):
    return SelParser(s).groups()


def match_compound(t, e, c):
    if c["tag"] and c["tag"] != "*" and e["tag"] != c["tag"]:
        return False
    a = e["attrs"]
    for x in c["id"]:
        if a.get("id") != x:
            return False
    for x in c["cls"]:
        if x not in re.split(WSRE + "+", a.get("class", "")):
            return False
    for name, op, val in c["attr"]:
        if name not in a:
            return False
        v = a[name]
        if op is None:
            continue
        ok = {"=": lambda: v == val, "~=": lambda: val in re.split(WSRE + "+", v), "^=": lambda: val != "" and v.startswith(val),
              "$=": lambda: val != "" and v.endswith(val), "*=": lambda: val != "" and val in v}[op]()
        if not ok:
            return False
    for p in c["pseudo"]:
        sib = t.siblings(e)
        if sib[0 if p == "first" else -1] is not e:
            return False
    for inner in c["not"]:
        if match_compound(t, e, inner):
            return False
    return True


def match_complex(t, e, parts):
    def m(e, k):
        if not match_compound(t, e, parts[k][1]):
            return False
        if k == 0:
            return True
        comb = parts[k][0]
        if comb == " ":
            a = t.parent_el(e)
            while a is not None:
                if m(a, k - 1):
                    return True
                a = t.parent_el(a)
            return False
        if comb == ">":
            a = t.parent_el(e)
            return a is not None and m(a, k - 1)
        sib = t.siblings(e)
        before = sib[:[x is e for x in sib].index(True)]
        if comb == "+":
            return bool(before) and m(before[-1], k - 1)
        return any(m(p, k - 1) for p in reversed(before))
    return m(e, len(parts) - 1)


def spec_compound(c):
    a, b, d = len(c["id"]), len(c["cls"]) + len(c["attr"]) + len(c["pseudo"]), 1 if c["tag"] not in (None, "*") else 0
    for inner in c["not"]:
        x = spec_compound(inner)
        a, b, d = a + x[0], b + x[1], d + x[2]
    return (a, b, d)


def specificity(parts):
    s = [0, 0, 0]
    for _, c in parts:
        x = spec_compound(c)
        s = [s[0] + x[0], s[1] + x[1], s[2] + x[2]]
    return tuple(s)


def qsa(t, root, sel):
    gs = parse_selector(sel)
    return [e for e in t.preorder(root) if any(match_complex(t, e, g) for g in gs)]


# ---------- CSS ----------
UA = """
html, body, div, p, h1, h2, h3, h4, h5, h6, ul, ol, pre, blockquote, section, article, header, footer, nav, main, form, hr { display: block }
li { display: list-item }
head, script, style, title, meta, link { display: none }
h1 { font-size: 2em; font-weight: bold }
h2 { font-size: 1.5em; font-weight: bold }
h3, h4, h5, h6, b, strong, th { font-weight: bold }
i, em { font-style: italic }
"""
INITIAL = {"display": "inline", "color": "black", "font-size": "16px", "font-weight": "normal", "font-style": "normal",
           "text-align": "left", "text-transform": "none", "visibility": "visible", "line-height": "normal", "width": "auto",
           "height": "auto", "background-color": "transparent"}
SIDES = ["top", "right", "bottom", "left"]
for s in SIDES:
    INITIAL["margin-" + s] = INITIAL["padding-" + s] = INITIAL["border-%s-width" % s] = "0"
INHERITED = {"color", "font-size", "font-weight", "font-style", "text-align", "text-transform", "visibility", "line-height"}
LENGTHS = {"width", "height"} | {p + s for s in SIDES for p in ("margin-", "padding-")} | {"border-%s-width" % s for s in SIDES}
NUM = r"(-?(?:\d+(?:\.\d+)?|\.\d+))"


def four(vals):
    if len(vals) == 1:
        return vals * 4
    if len(vals) == 2:
        return [vals[0], vals[1], vals[0], vals[1]]
    if len(vals) == 3:
        return [vals[0], vals[1], vals[2], vals[1]]
    if len(vals) == 4:
        return vals
    return None


def expand(p, v):
    if p in ("margin", "padding", "border-width"):
        f = four(v.split(" "))
        if f is None:
            return []
        name = (lambda s: "border-%s-width" % s) if p == "border-width" else (lambda s: p + "-" + s)
        return [(name(s), f[k]) for k, s in enumerate(SIDES)]
    if p == "border":
        for tok in v.split(" "):
            if tok == "0" or re.fullmatch(NUM + "(px|em)", tok, re.I):
                return [("border-%s-width" % s, tok) for s in SIDES]
        return []
    return [(p, v)]


def parse_decls(body):
    out = []
    for part in body.split(";"):
        if ":" not in part:
            continue
        p, v = part.split(":", 1)
        p, v = p.strip(WS).lower(), v.strip(WS)
        imp = False
        m = re.search(r"!" + WSRE + r"*important" + WSRE + r"*$", v, re.I)
        if m:
            imp, v = True, v[:m.start()].strip(WS)
        v = re.sub(WSRE + "+", " ", v)
        if p and v:
            out += [(lp, lv, imp) for lp, lv in expand(p, v)]
    return out


def parse_css(t):
    t = re.sub(r"/\*.*?\*/", "", t, flags=re.S)
    if "/*" in t:
        t = t[:t.find("/*")]
    rules, i, n = [], 0, len(t)
    while True:
        while i < n and t[i] in WS:
            i += 1
        if i >= n:
            break
        if t[i] == "@":
            j = i
            while j < n and t[j] not in ";{":
                j += 1
            if j >= n:
                break
            if t[j] == ";":
                i = j + 1
                continue
            depth = 0
            while j < n:
                if t[j] == "{":
                    depth += 1
                elif t[j] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            i = j + 1
            continue
        j = t.find("{", i)
        if j < 0:
            break
        k = t.find("}", j)
        if k < 0:
            k = n
        prelude, body, i = t[i:j].strip(WS), t[j + 1:k], k + 1
        try:
            gs = parse_selector(prelude)
        except SelErr:
            continue
        rules.append((gs, parse_decls(body)))
    return rules


def fmt(x):
    assert abs(x - round(x)) < 1e-9, x  # the tests keep to whole numbers
    return str(int(round(x)))


class Styler:
    def __init__(self, doc):
        self.t = Tree(doc)
        sheets = [(0, r) for r in parse_css(UA)]
        for e in self.t.preorder(doc):
            if e["tag"] == "style":
                sheets += [(1, r) for r in parse_css(text_content(e))]
        self.sheets, self.memo, self.decl_memo = sheets, {}, {}

    def declared(self, e):
        if id(e) in self.decl_memo:
            return self.decl_memo[id(e)]
        best, order = {}, 0
        def offer(p, v, key):
            if p not in best or key > best[p][0]:
                best[p] = (key, v)
        for origin, (gs, ds) in self.sheets:
            specs = [specificity(g) for g in gs if match_complex(self.t, e, g)]
            for p, v, imp in ds:
                order += 1
                if specs:
                    offer(p, v, (imp, origin, 0, max(specs), order))
        if "style" in e["attrs"]:
            for p, v, imp in parse_decls(e["attrs"]["style"]):
                order += 1
                offer(p, v, (imp, 1, 1, (0, 0, 0), order))
        d = {p: kv[1] for p, kv in best.items()}
        self.decl_memo[id(e)] = d
        return d

    def fs(self, e):
        return float(self.get(e, "font-size")[:-2]) if e is not None else 16.0

    def get(self, e, prop):
        key = (id(e), prop)
        if key not in self.memo:
            self.memo[key] = self._get(e, prop)
        return self.memo[key]

    def _get(self, e, prop):
        d = self.declared(e).get(prop)
        parent = self.t.parent_el(e)
        if d is not None and prop == "font-size" and d.lower() not in ("inherit", "initial") and \
                not re.fullmatch(NUM + "(px|em|%)", d.lower()):
            d = None
        if d is None:
            if prop in INHERITED and parent is not None:
                return self.get(parent, prop)
            d = "initial"
        if d == "inherit":
            if parent is not None:
                return self.get(parent, prop)
            d = "initial"
        if d == "initial":
            if prop not in INITIAL:
                return None
            d = INITIAL[prop]
        v = d.lower()
        if prop == "font-size":
            m = re.fullmatch(NUM + "(px|em|%)", v)
            num, unit = float(m.group(1)), m.group(2)
            pfs = self.fs(parent)
            return fmt(num if unit == "px" else num * pfs if unit == "em" else num * pfs / 100) + "px"
        if prop in LENGTHS or prop == "line-height":
            if v == "0" and prop != "line-height":
                return "0px"
            m = re.fullmatch(NUM + "(px|em)", v)
            if m:
                num = float(m.group(1))
                return fmt(num if m.group(2) == "px" else num * self.fs(e)) + "px"
        return v


def computed_style(html, sel, prop):
    doc = parse(html)
    st = Styler(doc)
    l = qsa(st.t, doc, sel)
    return st.get(l[0], prop) if l else None


# ---------- inline content, layout, text ----------
BR = object()


def display(st, e):
    return st.get(e, "display")


def flatten(st, nodes, out):
    for nd in nodes:
        if nd["type"] == "text":
            p = st.t.parent.get(id(nd))
            tt = st.get(p, "text-transform") if p is not None and p["type"] == "element" else "none"
            s = nd["text"]
            out.append(s.upper() if tt == "uppercase" else s.lower() if tt == "lowercase" else s)
        elif display(st, nd) == "none":
            continue
        elif nd["tag"] == "br":
            out.append(BR)
        elif nd["tag"] == "img":
            if nd["attrs"].get("alt", ""):
                out.append("[" + nd["attrs"]["alt"] + "]")
        else:
            flatten(st, nd["children"], out)
    return out


def segments(items):
    segs, cur = [], []
    for x in items:
        if x is BR:
            segs.append("".join(cur))
            cur = []
        else:
            cur.append(x)
    segs.append("".join(cur))
    return segs


def wrap(items, fits):
    lines = []
    for seg in segments(items):
        cur = []
        for w in [w for w in re.split(WSRE + "+", seg) if w]:
            if cur and not fits(len(" ".join(cur + [w]))):
                lines.append(" ".join(cur))
                cur = [w]
            else:
                cur.append(w)
        if cur:
            lines.append(" ".join(cur))
    return lines


def pre_lines(items):
    text = "".join("\n" if x is BR else x for x in items)
    pieces = text.split("\n")
    if pieces and pieces[-1] == "":
        pieces.pop()
    return pieces


def in_pre(st, e):
    while e is not None:
        if e["tag"] == "pre":
            return True
        e = st.t.parent_el(e)
    return False


def block_level(st, e):
    return display(st, e) in ("block", "list-item")


def layout(html, width):
    doc = parse(html)
    st = Styler(doc)
    boxes = []

    def lenpx(v, cbw):
        if v.endswith("px"):
            return float(v[:-2])
        if v.endswith("%"):
            return float(v[:-1]) * cbw / 100
        return 0.0

    def line_metrics(e):
        if e is None:
            return 8.0, 20.0
        fs = st.fs(e)
        lh = st.get(e, "line-height")
        if lh.endswith("px"):
            h = float(lh[:-2])
        elif re.fullmatch(NUM, lh):
            h = float(lh) * fs
        else:
            h = 1.25 * fs
        return fs / 2, h

    def children(parent, e, x, cw, top):
        y, run = top, []
        cw_char, lh = line_metrics(e)
        pre = e is not None and in_pre(st, e)

        def flush():
            nonlocal y
            items = flatten(st, run, [])
            n = len(pre_lines(items)) if pre else len(wrap(items, lambda k: k * cw_char <= cw))
            y += n * lh
            run.clear()
        for ch in parent["children"]:
            if ch["type"] == "element" and display(st, ch) == "none":
                continue
            if ch["type"] == "element" and block_level(st, ch):
                flush()
                y = block(ch, x, cw, y)
            else:
                run.append(ch)
        flush()
        return y - top

    def block(e, cbx, cbw, y):
        g = lambda p: lenpx(st.get(e, p), cbw)
        ml, mr, mt, mb = g("margin-left"), g("margin-right"), g("margin-top"), g("margin-bottom")
        pl, pr, pt, pb = g("padding-left"), g("padding-right"), g("padding-top"), g("padding-bottom")
        bl, br, bt, bb = (g("border-%s-width" % s) for s in ("left", "right", "top", "bottom"))
        w = st.get(e, "width")
        cw = lenpx(w, cbw) if w != "auto" else cbw - ml - mr - pl - pr - bl - br
        box = {"tag": e["tag"], "x": cbx + ml, "y": y + mt, "width": cw + pl + pr + bl + br}
        boxes.append(box)
        h = children(e, e, box["x"] + bl + pl, cw, box["y"] + bt + pt)
        hv = st.get(e, "height")
        box["height"] = (lenpx(hv, 0) if hv.endswith("px") else h) + pt + pb + bt + bb
        return box["y"] + box["height"] + mb

    children(doc, None, 0.0, float(width), 0.0)
    return [[b["tag"], int(b["x"]), int(b["y"]), int(b["width"]), int(b["height"])] for b in boxes
            if all(abs(b[k] - round(b[k])) < 1e-9 for k in ("x", "y", "width", "height"))] if True else None


def render_text(html, cols):
    doc = parse(html)
    st = Styler(doc)
    lines, pending = [], []

    def out(text, ind, align):
        avail = cols - ind
        if align == "center":
            text = " " * max(0, (avail - len(text)) // 2) + text
        elif align == "right":
            text = " " * max(0, avail - len(text)) + text
        prefix = [" "] * ind
        for p in pending:
            prefix[p] = "*"
        pending.clear()
        lines.append("".join(prefix) + text)

    def kids(parent, ind, align, pre):
        run = []

        def flush():
            items = flatten(st, run, [])
            if pre:
                for piece in pre_lines(items):
                    out(piece, ind, "left")
            else:
                for l in wrap(items, lambda k: k <= cols - ind):
                    out(l, ind, align)
            run.clear()
        for ch in parent["children"]:
            if ch["type"] == "element" and display(st, ch) == "none":
                continue
            if ch["type"] == "element" and block_level(st, ch):
                flush()
                blk(ch, ind)
            else:
                run.append(ch)
        flush()

    def blk(e, ind):
        if e["tag"] == "hr":
            out("-" * (cols - ind), ind, "left")
            return
        mine = None
        if display(st, e) == "list-item":
            mine = ind
            pending.append(ind)
            ind += 2
        kids(e, ind, st.get(e, "text-align"), in_pre(st, e))
        if mine is not None and mine in pending:
            pending.remove(mine)

    kids(doc, 0, "left", False)
    return lines


# ---------- the tests ----------
def lit(v):
    if v is None:
        return "nil"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return fmt(v)
    if isinstance(v, str):
        return '"' + v.replace("\\", "\\\\").replace('"', '\\"').replace("{", "\\{").replace("\n", "\\n").replace("\t", "\\t") + '"'
    if isinstance(v, list):
        return "[" + ", ".join(lit(x) for x in v) + "]"
    if isinstance(v, dict):
        return "{" + ", ".join(lit(k) + ": " + lit(x) for k, x in v.items()) + "}"
    raise TypeError(v)


HEADER = "// generated by gen.py from its reference implementation of the spec; do not edit\nimport \"../src/browser.fut\"\n"


def test(name, lines):
    return "\ntest " + lit(name) + " {\n" + "".join("  " + l + "\n" for l in lines) + "}\n"


def parse_tests():
    out = []
    def ser(name, html, extra=()):
        out.append(test(name, ["var d = parseHtml(%s)" % lit(html), "expect serialize(d) == %s" % lit(serialize(parse(html)))] + list(extra)))
    ser("elements and text", "<div><p>Hello <b>world</b></p></div>")
    ser("tag and attribute names are lowercased", '<DIV ID="a" Class="b">x</DIV>')
    h = "<a href='x y' title=\"q\" data-n=5 hidden>z</a>"
    d = parse(h)
    ser("attribute quoting styles", h, ["expect d.children[0].tag == \"a\"",
        "expect d.children[0].attrs == %s" % lit(d["children"][0]["attrs"]),
        "expect d.children[0].children[0].type == \"text\""])
    h = '<p>a<br>b<img src="i.png">c</p>'
    ser("void elements have no children or end tag", h, ["expect d.children[0].children.len() == %d" % len(parse(h)["children"][0]["children"]),
        "expect d.children[0].children[1].children == []"])
    ser("self-closing slash", "<div><br/><span/>x</div>")
    h = '<p title="&quot;hi&quot;">a &amp; b &lt;c&gt; it&#39;s &copy;</p>'
    ser("entities are decoded", h, ["expect textContent(d) == %s" % lit(text_content(parse(h))),
        "expect d.children[0].attrs.title == %s" % lit(parse(h)["children"][0]["attrs"]["title"])])
    h = "<!DOCTYPE html><p>a<!-- x -->b</p><!-- y -->"
    ser("comments and doctype are dropped", h, ["expect d.children.len() == 1", "expect d.children[0].children.len() == 1"])
    h = "<ul>\n  <li>a</li>\n  <li>b c</li>\n</ul>"
    ser("whitespace between elements is kept", h, ["expect d.children[0].children.len() == 5", "expect d.children[0].children[0].text == \"\\n  \""])
    h = "<p>  a   b  </p>"
    ser("other text is kept as written", h, ["expect textContent(d) == %s" % lit(text_content(parse(h)))])
    ser("p closes an open p", "<p>one<p>two")
    ser("a block closes an open p", "<p>one<div>two</div><p>three<hr>")
    ser("li closes an open li", "<ul><li>a<li>b<ul><li>c<li>d</ul><li>e</ul>")
    ser("table cells and rows close", "<table><tr><td>1<td>2<tr><th>3</table>")
    ser("stray end tags are ignored, open elements closed at the end", "<div>a</span>b<em>c</div>d")
    ser("an end tag closes the elements inside it", "<div><b><i>x</div>y")
    h = '<script>if (a < b && c) { x("</p>") }</script><style>p > a { color: red }</style>'
    ser("script and style are raw text", h, ["expect d.children[0].children[0].text == %s" % lit(parse(h)["children"][0]["children"][0]["text"])])
    h = "<pre>\n  a\n\n  b\n</pre>"
    ser("pre keeps its whitespace, minus a leading newline", h, ["expect textContent(d) == %s" % lit(text_content(parse(h)))])
    ser("a space between inline elements is a text node", "<p><b>x</b> <i>y</i></p>", ["expect d.children[0].children[1].text == \" \""])
    ser("unquoted values may contain slashes", "<a href=/x/y>z</a>")
    ser("the first of a repeated attribute wins", "<p id=a id=b>x</p>")
    ser("a lone < is text", "a < b <p>c</p>")
    h = "<div>a<p>b<b>c</b></p>d</div>"
    out.append(test("node shapes", ["var d = parseHtml(%s)" % lit(h), "expect d.type == \"document\"", "var div = d.children[0]",
        "expect div.type == \"element\" && div.tag == \"div\" && div.attrs == {}", "expect div.children.len() == 3",
        "expect div.children[0].type == \"text\" && div.children[0].text == \"a\"", "expect div.children[1].tag == \"p\"",
        "expect textContent(div) == %s" % lit(text_content(parse(h)))]))
    return out


SEL_DOC = """<div id="root" class="page">
  <header id="h"><h1 id="t" class="title big">Title</h1><nav id="n"><a id="a1" href="/x">x</a><a id="a2" href="https://e.com" class="ext">e</a></nav></header>
  <ul id="list"><li id="l1" class="item">one</li><li id="l2" class="item odd">two</li><li id="l3" class="item">three</li></ul>
  <p id="p1">para <span id="s1" lang="en-us">s</span></p>
  <p id="p2" data-k="a b c"><span id="s2">t</span><em id="e1">u</em></p>
  <form id="f"><input id="i1" type="text" name="q"><input id="i2" type="checkbox" checked></form>
</div>"""


def select_tests():
    doc = parse(SEL_DOC)
    t = Tree(doc)
    ids = lambda l: [e["attrs"]["id"] for e in l]
    out = [("fn acceptIds(l) {\n  var r = []\n  for e in l { r.push(e.attrs.id) }\n  return r\n}\n" +
            "fn acceptDoc() { return parseHtml(%s) }\n" % lit(SEL_DOC))]
    def sel(name, sels, root=None):
        ls = ["var d = acceptDoc()"]
        if root:
            ls.append("var r = querySelector(d, %s)" % lit(root))
        r = qsa(t, doc, root)[0] if root else doc
        for s in sels:
            ls.append("expect acceptIds(querySelectorAll(%s, %s)) == %s" % ("r" if root else "d", lit(s), lit(ids(qsa(t, r, s)))))
        out.append(test(name, ls))
    sel("type selectors", ["li", "LI", "table"])
    sel("universal selector", ["nav *", "#n > *"])
    sel("id selectors", ["#p2", "#nope"])
    sel("class selectors", [".item", ".odd.item", ".title.big", ".title.small"])
    sel("compound selectors", ["li.item#l3", "p#l3", "a.ext[href]"])
    sel("attribute presence", ["[checked]", "[href]", "[HREF]"])
    sel("attribute equals", ["input[type=text]", "[type=\"checkbox\"]", "[type='radio']"])
    sel("attribute operators", ["[href^=\"https\"]", "[href$=x]", "[data-k~=b]", "[lang*=us]", "[data-k~=\"a b\"]"])
    sel("descendant combinator", ["div span", "header a", "ul span"])
    sel("child combinator", ["ul > li", "div > span", "#root > p > span"])
    sel("adjacent sibling combinator", ["#l1 + li", "span + em", "h1 + a", "header + ul"])
    sel("general sibling combinator", ["#l1 ~ li", "#p1 ~ *", "#l3 ~ li"])
    sel("first-child and last-child", ["li:first-child", "li:last-child", "#root > :first-child", "p :last-child", "a:first-child"])
    sel(":not", ["li:not(.odd)", "input:not([type=text])", "#list > :not(li)"])
    sel("a selector list is in document order, without duplicates", ["#e1, .item, li#l1", "em,h1"])
    sel("whitespace around combinators", ["ul>li", "  ul  >  li  ", "#l1+li", "#l1 ~li"])
    sel("only the root's descendants, matched in the whole document", ["span", "p span", "p", "div > p > *"], root="#p2")
    out.append(test("querySelector gives the first match or nil", ["var d = acceptDoc()", "expect querySelector(d, \"li\").attrs.id == \"l1\"",
        "expect querySelector(d, \"p > *\").attrs.id == \"s1\"", "expect querySelector(d, \"table\") == nil"]))
    bad = ["", "div[", "a >", ":hover", "a,,b", "p..x", "[x=]", "#"]
    for b in bad:
        try:
            parse_selector(b)
            raise AssertionError("valid: " + b)
        except SelErr:
            pass
    out.append(test("invalid selectors throw", ["var d = acceptDoc()"] + ["expect throws { querySelectorAll(d, %s) }" % lit(b) for b in bad]))
    return out


def cascade_tests():
    out = []
    def cs(name, html, checks):
        ls = ["var h = %s" % lit(html)]
        for sel, prop in checks:
            ls.append("expect computedStyle(h, %s, %s) == %s" % (lit(sel), lit(prop), lit(computed_style(html, sel, prop))))
        out.append(test(name, ls))
    cs("initial values", "<p>x</p>", [("p", "color"), ("p", "font-size"), ("p", "margin-top"), ("p", "width"), ("p", "background-color"),
                                       ("p", "text-align"), ("p", "border-left-width")])
    cs("the default stylesheet", "<head><title>x</title></head><h1>a <span>b</span></h1><ul><li>c</li></ul><p><em>d</em></p>",
       [("title", "display"), ("h1", "display"), ("span", "display"), ("li", "display"), ("h1", "font-weight"), ("span", "font-weight"),
        ("em", "font-style"), ("h1", "font-size")])
    cs("a type selector", "<style>p { color: red }</style><p>x</p>", [("p", "color")])
    cs("specificity", "<style>#a { color: green } .c { color: blue } p { color: red }</style><p id=\"a\" class=\"c\">x</p><p class=\"c\">y</p><p>z</p>",
       [("#a", "color"), ("p.c:not(#a)", "color"), ("p:not(.c)", "color")])
    cs("specificity adds up over the compounds", "<style>div p { color: red } p.c { color: blue } div > p.c { color: green }</style><div><p class=\"c\">x</p></div>",
       [("p", "color")])
    cs("the later rule wins a tie", "<style>p { color: red } p { color: blue } p { margin-top: 1px; margin-top: 2px }</style><p>x</p>",
       [("p", "color"), ("p", "margin-top")])
    cs("!important beats specificity", "<style>p { color: red !important } #a { color: blue } p { font-weight: bold ! important } #a { font-weight: normal }</style><p id=\"a\">x</p>",
       [("p", "color"), ("p", "font-weight")])
    cs("inline styles", "<style>#a { color: blue } #b { color: blue !important } #c { color: blue !important }</style>" +
       "<p id=\"a\" style=\"color: purple\">x</p><p id=\"b\" style=\"color: purple\">y</p><p id=\"c\" style=\"color: purple !important\">z</p>",
       [("#a", "color"), ("#b", "color"), ("#c", "color")])
    cs("a selector list counts its most specific matching selector", "<style>#a, p { color: red } .c { color: blue }</style><p id=\"a\" class=\"c\">x</p><p class=\"c\">y</p>",
       [("#a", "color"), ("p:last-child", "color")])
    cs("inherited and not inherited", "<style>div { color: red; font-weight: bold; margin-top: 5px; text-align: center }</style><div><p>x</p></div>",
       [("p", "color"), ("p", "font-weight"), ("p", "margin-top"), ("p", "text-align"), ("div", "margin-top")])
    cs("inherit and initial", "<style>div { margin-left: 7px; color: red } p { margin-left: inherit; color: initial }</style><div><p>x</p></div>",
       [("p", "margin-left"), ("p", "color")])
    cs("font-size in em and percent", "<style>div { font-size: 20px } p { font-size: 1.5em } span { font-size: 50% }</style><div><p>a<span>b</span></p></div>" +
       "<section style=\"font-size: 10px\"><h1>x</h1><h2>y</h2></section>",
       [("div", "font-size"), ("p", "font-size"), ("span", "font-size"), ("h1", "font-size"), ("h2", "font-size")])
    cs("em lengths use the element's own font size", "<style>p { font-size: 20px; margin-top: 2em; padding-left: 0; width: 50% }</style><p>x</p>",
       [("p", "margin-top"), ("p", "padding-left"), ("p", "width")])
    cs("shorthands", "<style>p { margin: 1px 2px 3px; padding: 4px 5px; border: solid 3px red } div { margin: 9px; border-width: 1px 2px 3px 4px }</style><p>x</p><div>y</div>",
       [("p", "margin-top"), ("p", "margin-right"), ("p", "margin-bottom"), ("p", "margin-left"), ("p", "padding-top"), ("p", "padding-left"),
        ("p", "border-left-width"), ("p", "margin"), ("div", "margin-left"), ("div", "border-top-width"), ("div", "border-left-width")])
    cs("comments and at-rules are skipped", "<style>/* p { color: red } */ @import \"x.css\"; @media screen { p { color: blue } } p { color: green }</style><p>x</p>",
       [("p", "color")])
    cs("an invalid selector drops its whole rule", "<style>p:hover { color: red } p:hover, p { color: red } p { font-weight: bold }</style><p>x</p>",
       [("p", "color"), ("p", "font-weight")])
    cs("style elements apply in document order, wherever they are", "<style>p { color: red }</style><p>x</p><div><style>p { color: blue }</style></div>",
       [("p", "color")])
    cs("names and values are lowercased, spaces collapsed", "<style>p { COLOR: RED; Font-Family:  Times   New Roman ; margin-top: 3PX }</style><p>x</p>",
       [("p", "color"), ("p", "font-family"), ("p", "margin-top")])
    cs("combinators in the cascade", "<style>div p { color: red } div > p { color: blue } p + p { font-weight: bold }</style><div><section><p id=x>a</p></section><p id=y>b</p><p id=z>c</p></div>",
       [("#x", "color"), ("#y", "color"), ("#y", "font-weight"), ("#z", "font-weight")])
    cs("no element, or a property nobody set", "<style>p { foo: bar }</style><p>x</p>", [("div", "color"), ("p", "foo"), ("p", "baz")])
    cs("line-height", "<style>p { font-size: 20px; line-height: 1.5 } span { font-size: 10px } div { line-height: 2em; font-size: 10px }</style><p>a<span>b</span></p><div>c</div>",
       [("p", "line-height"), ("span", "line-height"), ("div", "line-height"), ("body", "line-height")])
    cs("an unsupported font-size is ignored", "<style>div { font-size: 20px } p { font-size: large }</style><div><p>x</p></div>", [("p", "font-size")])
    cs("display: none doesn't change the children's display", "<div style=\"display:none\"><p>x</p></div>", [("div", "display"), ("p", "display")])
    return out


def layout_tests():
    out = ["fn acceptBoxes(l) {\n  var r = []\n  for b in l { r.push([b.tag, b.x, b.y, b.width, b.height]) }\n  return r\n}\n"]
    def lay(name, cases):
        out.append(test(name, ["expect acceptBoxes(layout(%s, %s)) == %s" % (lit(h), fmt(w), lit(layout(h, w))) for h, w in cases]))
    lay("empty blocks", [("<div></div><p></p>", 800)])
    lay("a line of text", [("<p>hello world</p>", 800), ("hello", 800)])
    lay("blocks stack", [("<div>a</div><div>b</div>", 800)])
    lay("text wraps at the width", [("<p>aaaa bbbb cccc</p>", 80), ("<p>abcdefghijklmnop q</p>", 80), ("<p>aaaa bbbbb</p>", 80)])
    lay("margins, borders and padding", [("<style>div { margin: 10px 20px; padding: 5px; border: 2px solid }</style><div>x</div>", 800)])
    lay("margins don't collapse", [("<style>p { margin: 10px 0 }</style><p>a</p><p>b</p>", 800)])
    lay("nested blocks", [("<div style=\"padding: 10px\"><p style=\"margin-left: 30px\">a</p></div>", 400)])
    lay("width and height", [("<div style=\"width: 200px; height: 50px; padding: 5px\">a b c</div>", 800), ("<div style=\"width: 50%\">x</div>", 600),
                             ("<div style=\"width: 40px\">aaa bbb</div>", 600)])
    lay("font size changes the lines", [("<h1>Big title here</h1>", 160), ("<p style=\"font-size: 8px\">aaaa bbbb cccc</p>", 40)])
    lay("line-height", [("<p style=\"line-height: 30px\">a<br>b</p>", 800), ("<p style=\"line-height: 2\">a</p>", 800)])
    lay("inline elements flow with the text", [("<p>aaa <b>bbb</b> <span>ccc</span></p>", 64)])
    lay("display: none takes no space", [("<div>a</div><div style=\"display:none\">b</div><div>c</div>", 800),
                                         ("<p>a<span style=\"display: none\">hidden words here</span></p>", 16)])
    lay("inline text between blocks", [("<div>a<p>b</p>c</div>", 800)])
    lay("head is not laid out", [("<html><head><title>T</title><style>p { margin-top: 4px }</style></head><body><p>x</p></body></html>", 800)])
    lay("list items are blocks", [("<ul><li>a</li><li>b</li></ul>", 800)])
    lay("whitespace collapses", [("<p>   a  \n  b   </p>", 24)])
    lay("img alt is text", [("<p>a <img alt=\"pic\"> b</p>", 40)])
    lay("pre lines", [("<pre>a\n\nb c d e f g h</pre>", 16)])
    return out


def render_tests():
    out = []
    def ren(name, cases):
        out.append(test(name, ["expect renderText(%s, %s) == %s" % (lit(h), fmt(c), lit(render_text(h, c))) for h, c in cases]))
    ren("paragraphs", [("<h1>Title</h1><p>Hello <b>world</b>.</p>", 40)])
    ren("wrapping", [("<p>the quick brown fox jumps</p>", 10), ("<p>unbelievable day</p>", 9)])
    ren("br, and no empty lines", [("<p>a<br>b<br><br>c</p><p></p><div> </div>", 20)])
    ren("lists", [("<ul><li>one</li><li>two words here</li></ul>", 10)])
    ren("nested lists", [("<ul><li>a<ul><li>b</li></ul></li><li>c</li></ul>", 20), ("<ol><li><p>x</p><p>y</p></li></ol>", 20)])
    ren("hidden elements", [("<head><title>T</title></head><p>a<span style=\"display:none\">b</span>c</p><script>x()</script>", 20)])
    ren("display from CSS", [("<style>.blk { display: block } p { display: inline }</style><p>a</p> <p>b</p><span class=\"blk\">c</span>", 20)])
    ren("text-transform", [("<p style=\"text-transform: uppercase\">Hi <span style=\"text-transform: none\">there</span> <b>you</b></p>", 20)])
    ren("text-align", [("<p style=\"text-align: center\">abc</p><p style=\"text-align: right\">ab</p><div style=\"text-align: center\"><p>abcd</p></div>", 9)])
    ren("hr", [("<p>a</p><hr><p>b</p>", 5)])
    ren("pre", [("<pre>\nline 1\n  line  2\n</pre><p>x  y</p>", 20), ("<pre>a\n\nb</pre>", 20)])
    ren("img alt", [("<p>see <img src=\"x.png\" alt=\"logo\"> here <img src=\"y.png\"></p>", 30)])
    ren("inline text between blocks", [("<div>a<div>b</div>c</div>", 20)])
    ren("entities come out decoded", [("<p>a &lt; b &amp;&amp; c</p>", 20)])
    ren("words join across inline elements", [("<p>un<b>believ</b>able day</p>", 9)])
    ren("a whole page", [("<html><head><title>T</title><style>h1 { text-align: center } .note { text-transform: uppercase }</style></head><body>" +
                          "<h1>Fut</h1><p>A <a href=\"/x\">small</a> page.</p><ul><li>one</li><li class=\"note\">two</li></ul><hr></body></html>", 12)])
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    acc = os.path.join(here, "accept")
    os.makedirs(acc, exist_ok=True)
    files = {"parse": parse_tests(), "select": select_tests(), "cascade": cascade_tests(), "layout": layout_tests(), "render": render_tests()}
    total = 0
    for name, parts in files.items():
        with open(os.path.join(acc, name + "_test.fut"), "w") as f:
            f.write(HEADER + "".join(("\n" + p if not p.startswith("\ntest") else p) for p in parts))
        total += sum(1 for p in parts if p.startswith("\ntest"))
    print("wrote", total, "tests")


if __name__ == "__main__":
    main()
