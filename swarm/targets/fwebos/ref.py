"""A reference implementation of swarm/targets/fwebos.md, read directly off the spec. gen.py uses it as the oracle for
the hidden tests. Change the spec and this file together."""
import copy, json, re, types

COMMANDS = sorted("[ cat cd clear cp cut echo env export false find grep head jobs kill ls mkdir mv ps pwd rm seq sh sleep "
                  "sort source tail test touch tr true uname uniq unset uptime wait wc whoami".split())
HOME = "/home/user"
NAME_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
INT_RE = re.compile(r"-?[0-9]+")
REDIR = {">", ">>", "<", "2>", "2>&1"}
RESERVED = {"then", "elif", "else", "fi", "do", "done"}
SW, SH = 1280, 800


class SyntaxErr(Exception):
    pass


class ShErr(Exception):
    def __init__(self, msg, status=1):
        self.msg, self.status = msg, status


# ---------- file system ----------
def newdir():
    return {"t": "d", "c": {}}


def newfile(s=""):
    return {"t": "f", "s": s}


def norm(cwd, p):
    parts = [] if p.startswith("/") else [x for x in cwd.split("/") if x]
    for x in p.split("/"):
        if x in ("", "."):
            continue
        if x == "..":
            if parts:
                parts.pop()
        else:
            parts.append(x)
    return "/" + "/".join(parts)


def lookup(fs, path):
    n = fs
    for x in [x for x in path.split("/") if x]:
        if n["t"] != "d" or x not in n["c"]:
            return None
        n = n["c"][x]
    return n


def parent_of(fs, path):
    if path == "/":
        return None, ""
    i = path.rfind("/")
    d = lookup(fs, path[:i] or "/")
    return (d if d is not None and d["t"] == "d" else None), path[i + 1:]


def basename(p):
    return p[p.rfind("/") + 1:] if p != "/" else "/"


def lines(t):
    if t == "":
        return []
    ps = t.split("\n")
    return ps[:-1] if t.endswith("\n") else ps


def unlines(ls):
    return "".join(l + "\n" for l in ls)


def to_int(v):
    return int(v) if INT_RE.fullmatch(v or "") else 0


def gmatch(pat, s):  # pat: list of (char, active)
    if not pat:
        return s == ""
    ch, act = pat[0]
    if act and ch == "*":
        return any(gmatch(pat[1:], s[k:]) for k in range(len(s) + 1))
    if not s:
        return False
    if act and ch == "?":
        return gmatch(pat[1:], s[1:])
    return s[0] == ch and gmatch(pat[1:], s[1:])


def gpat(p):
    return [(c, True) for c in p]


# ---------- lexer ----------
class Tok:
    def __init__(self, kind, val, start, end):
        self.kind, self.val, self.start, self.end = kind, val, start, end

    def desc(self):
        if self.kind == "eof":
            return "unexpected end of line"
        if self.kind == "nl":
            return "unexpected newline"
        return "unexpected '%s'" % (self.val if self.kind == "op" else "word")


def scan_dollar(s, i, quoted):
    n = len(s)
    if s.startswith("$((", i):
        j, depth = i + 3, 0
        while j < n:
            if s[j] == "(":
                depth += 1
            elif s[j] == ")":
                if depth == 0:
                    if j + 1 < n and s[j + 1] == ")":
                        return ("arith", s[i + 3:j], quoted), j + 2
                    raise SyntaxErr("bad arithmetic")
                depth -= 1
            j += 1
        raise SyntaxErr("unterminated $((")
    if s.startswith("$(", i):
        toks, j = tokenize(s, i + 2, True)
        return ("cmd", Parser(toks, s).program(), quoted), j + 1
    if s.startswith("${", i):
        j = s.find("}", i + 2)
        if j < 0 or not NAME_RE.fullmatch(s[i + 2:j]):
            raise SyntaxErr("bad substitution")
        return ("var", s[i + 2:j], quoted), j + 1
    if s.startswith("$?", i):
        return ("status", None, quoted), i + 2
    m = NAME_RE.match(s, i + 1)
    if m:
        return ("var", m.group(0), quoted), m.end()
    return None, i + 1


def scan_word(s, i, sub):
    n, parts, lit = len(s), [], []

    def flush():
        if lit:
            parts.append(("lit", "".join(lit), False))
            lit.clear()
    while i < n:
        c = s[i]
        if c in " \t\n|&;<>" or (sub and c == ")"):
            break
        if c == "\\":
            flush()
            parts.append(("lit", s[i + 1] if i + 1 < n else "\\", True))
            i += 2
        elif c == "'":
            j = s.find("'", i + 1)
            if j < 0:
                raise SyntaxErr("unterminated quote")
            flush()
            parts.append(("lit", s[i + 1:j], True))
            i = j + 1
        elif c == '"':
            flush()
            parts.append(("lit", "", True))
            buf, i = [], i + 1
            while True:
                if i >= n:
                    raise SyntaxErr("unterminated quote")
                c = s[i]
                if c == '"':
                    i += 1
                    break
                if c == "\\" and i + 1 < n and s[i + 1] in '"\\$':
                    buf.append(s[i + 1])
                    i += 2
                elif c == "$":
                    p, j = scan_dollar(s, i, True)
                    if p is None:
                        buf.append("$")
                        i += 1
                    else:
                        if buf:
                            parts.append(("lit", "".join(buf), True))
                            buf = []
                        parts.append(p)
                        i = j
                else:
                    buf.append(c)
                    i += 1
            if buf:
                parts.append(("lit", "".join(buf), True))
        elif c == "$":
            p, j = scan_dollar(s, i, False)
            if p is None:
                lit.append("$")
                i += 1
            else:
                flush()
                parts.append(p)
                i = j
        else:
            lit.append(c)
            i += 1
    flush()
    return parts, i


def tokenize(s, i, sub):
    toks, n = [], len(s)
    while True:
        while i < n and s[i] in " \t":
            i += 1
        if i < n and s[i] == "#":
            while i < n and s[i] != "\n":
                i += 1
            continue
        if i >= n:
            if sub:
                raise SyntaxErr("unterminated $(")
            toks.append(Tok("eof", None, i, i))
            return toks, i
        c = s[i]
        if sub and c == ")":
            toks.append(Tok("eof", None, i, i))
            return toks, i
        if c == "\n":
            toks.append(Tok("nl", "\n", i, i + 1))
            i += 1
            continue
        for op in ("2>&1", "2>", "&&", "||", ">>", "|", "&", ";", "<", ">"):
            if s.startswith(op, i):
                toks.append(Tok("op", op, i, i + len(op)))
                i += len(op)
                break
        else:
            parts, j = scan_word(s, i, sub)
            toks.append(Tok("word", parts, i, j))
            i = j


# ---------- parser ----------
class Parser:
    def __init__(self, toks, src):
        self.t, self.k, self.src = toks, 0, src

    def peek(self):
        return self.t[self.k]

    def err(self, t):
        raise SyntaxErr(t.desc())

    def is_kw(self, t, kws):
        return t.kind == "word" and len(t.val) == 1 and t.val[0][0] == "lit" and not t.val[0][2] and t.val[0][1] in kws

    def is_op(self, t, *ops):
        return t.kind == "op" and t.val in ops

    def skipnl(self):
        while self.peek().kind == "nl":
            self.k += 1

    def expect(self, kw):
        if not self.is_kw(self.peek(), {kw}):
            self.err(self.peek())
        self.k += 1

    def program(self):
        items = self.list_(set(), False)
        if self.peek().kind != "eof":
            self.err(self.peek())
        return items

    def list_(self, stops, need):
        items = []
        while True:
            self.skipnl()
            t = self.peek()
            if t.kind == "eof" or self.is_kw(t, stops):
                break
            ao = self.andor()
            t, bg = self.peek(), False
            if self.is_op(t, ";"):
                self.k += 1
            elif self.is_op(t, "&"):
                self.k += 1
                bg = True
            elif t.kind == "nl":
                self.k += 1
            elif not (t.kind == "eof" or self.is_kw(t, stops)):
                self.err(t)
            items.append((ao, bg))
        if need and not items:
            self.err(self.peek())
        return items

    def andor(self):
        start = self.peek().start
        first, rest = self.pipeline(), []
        while self.is_op(self.peek(), "&&", "||"):
            op = self.peek().val
            self.k += 1
            self.skipnl()
            rest.append((op, self.pipeline()))
        return {"first": first, "rest": rest, "src": self.src[start:self.t[self.k - 1].end]}

    def pipeline(self):
        cmds = [self.command()]
        while self.is_op(self.peek(), "|"):
            self.k += 1
            self.skipnl()
            cmds.append(self.command())
        return cmds

    def command(self):
        t = self.peek()
        for kw, f in (("if", self.if_), ("for", self.for_), ("while", self.while_)):
            if self.is_kw(t, {kw}):
                self.k += 1
                c = f()
                if self.peek().kind == "op" and self.peek().val in REDIR:
                    self.err(self.peek())
                return c
        if self.is_kw(t, RESERVED):
            self.err(t)
        words, redirs = [], []
        while True:
            t = self.peek()
            if t.kind == "word":
                words.append(t.val)
                self.k += 1
            elif t.kind == "op" and t.val in REDIR:
                self.k += 1
                if t.val == "2>&1":
                    redirs.append((t.val, None))
                else:
                    f = self.peek()
                    if f.kind != "word":
                        self.err(f)
                    self.k += 1
                    redirs.append((t.val, f.val))
            else:
                break
        if not words and not redirs:
            self.err(t)
        return ("simple", words, redirs)

    def if_(self):
        branches = []
        cond = self.list_({"then"}, True)
        self.expect("then")
        branches.append((cond, self.list_({"elif", "else", "fi"}, True)))
        while self.is_kw(self.peek(), {"elif"}):
            self.k += 1
            cond = self.list_({"then"}, True)
            self.expect("then")
            branches.append((cond, self.list_({"elif", "else", "fi"}, True)))
        els = None
        if self.is_kw(self.peek(), {"else"}):
            self.k += 1
            els = self.list_({"fi"}, True)
        self.expect("fi")
        return ("if", branches, els)

    def for_(self):
        t = self.peek()
        if not (t.kind == "word" and len(t.val) == 1 and t.val[0][0] == "lit" and not t.val[0][2] and NAME_RE.fullmatch(t.val[0][1])):
            self.err(t)
        name = t.val[0][1]
        self.k += 1
        if not self.is_kw(self.peek(), {"in"}):
            self.err(self.peek())
        self.k += 1
        words = []
        while self.peek().kind == "word":
            words.append(self.peek().val)
            self.k += 1
        t = self.peek()
        if self.is_op(t, ";") or t.kind == "nl":
            self.k += 1
        else:
            self.err(t)
        self.skipnl()
        self.expect("do")
        body = self.list_({"done"}, True)
        self.expect("done")
        return ("for", name, words, body)

    def while_(self):
        cond = self.list_({"do"}, True)
        self.expect("do")
        body = self.list_({"done"}, True)
        self.expect("done")
        return ("while", cond, body)


def parse(src):
    toks, _ = tokenize(src, 0, False)
    return Parser(toks, src).program()


# ---------- arithmetic ----------
def tdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def arith(c, text):
    toks, i, n = [], 0, len(text)
    while i < n:
        ch = text[i]
        if ch in " \t\n":
            i += 1
        elif ch.isdigit():
            j = i
            while j < n and text[j].isdigit():
                j += 1
            toks.append(int(text[i:j]))
            i = j
        elif ch == "$":
            if text.startswith("$?", i):
                toks.append(c.sess["status"])
                i += 2
                continue
            m = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}|\$([A-Za-z_][A-Za-z0-9_]*)").match(text, i)
            if not m:
                raise ShErr("sh: arithmetic syntax error")
            toks.append(to_int(c.sess["vars"].get(m.group(1) or m.group(2))))
            i = m.end()
        elif NAME_RE.match(text, i):
            m = NAME_RE.match(text, i)
            toks.append(to_int(c.sess["vars"].get(m.group(0))))
            i = m.end()
        elif ch in "+-*/%()":
            toks.append(ch)
            i += 1
        else:
            raise ShErr("sh: arithmetic syntax error")
    pos = [0]

    def peek():
        return toks[pos[0]] if pos[0] < len(toks) else None

    def take():
        pos[0] += 1
        return toks[pos[0] - 1]

    def primary():
        t = peek()
        if isinstance(t, int):
            return take()
        if t == "(":
            take()
            v = expr()
            if peek() != ")":
                raise ShErr("sh: arithmetic syntax error")
            take()
            return v
        if t == "-":
            take()
            return -primary()
        if t == "+":
            take()
            return primary()
        raise ShErr("sh: arithmetic syntax error")

    def term():
        v = primary()
        while peek() in ("*", "/", "%"):
            op, r = take(), primary()
            if op == "*":
                v = v * r
            else:
                if r == 0:
                    raise ShErr("sh: division by zero")
                v = tdiv(v, r) if op == "/" else v - r * tdiv(v, r)
        return v

    def expr():
        v = term()
        while peek() in ("+", "-"):
            op, r = take(), term()
            v = v + r if op == "+" else v - r
        return v
    v = expr()
    if pos[0] != len(toks):
        raise ShErr("sh: arithmetic syntax error")
    return v


# ---------- regular expressions ----------
def rparse(p):
    anchor_s = p.startswith("^")
    if anchor_s:
        p = p[1:]
    anchor_e = False
    if p.endswith("$"):
        k = len(p) - 1
        bs = 0
        while k - 1 - bs >= 0 and p[k - 1 - bs] == "\\":
            bs += 1
        if bs % 2 == 0:
            anchor_e, p = True, p[:-1]
    atoms, i = [], 0
    while i < len(p):
        c = p[i]
        if c == "\\":
            atom = ("ch", p[i + 1] if i + 1 < len(p) else "\\")
            i += 2
        elif c == ".":
            atom = ("any",)
            i += 1
        elif c == "[":
            j, neg, items = i + 1, False, []
            if p[j:j + 1] == "^":
                neg, j = True, j + 1
            if p[j:j + 1] == "]":
                items.append(("]", "]"))
                j += 1
            while j < len(p) and p[j] != "]":
                if j + 2 < len(p) and p[j + 1] == "-" and p[j + 2] != "]":
                    items.append((p[j], p[j + 2]))
                    j += 3
                else:
                    items.append((p[j], p[j]))
                    j += 1
            if j >= len(p):
                atom = ("ch", "[")
                i += 1
            else:
                atom = ("set", neg, items)
                i = j + 1
        else:
            atom = ("ch", c)
            i += 1
        q = None
        if i < len(p) and p[i] in "*+?":
            q = p[i]
            i += 1
        atoms.append((atom, q))
    return anchor_s, anchor_e, atoms


def amatch(atom, c, icase):
    if atom[0] == "any":
        return True
    if atom[0] == "ch":
        return c == atom[1] or (icase and c.lower() == atom[1].lower())

    def inset(x):
        return any(a <= x <= b for a, b in atom[2])
    r = inset(c) or bool(icase and (inset(c.lower()) or inset(c.upper())))
    return r != atom[1]


def rmatch(rx, s, icase):
    anchor_s, anchor_e, atoms = rx

    def m(k, i):
        if k == len(atoms):
            return not anchor_e or i == len(s)
        atom, q = atoms[k]
        if q is None:
            return i < len(s) and amatch(atom, s[i], icase) and m(k + 1, i + 1)
        if q == "?":
            return (i < len(s) and amatch(atom, s[i], icase) and m(k + 1, i + 1)) or m(k + 1, i)
        j = i
        while j < len(s) and amatch(atom, s[j], icase):
            j += 1
        lo = i + (1 if q == "+" else 0)
        return any(m(k + 1, e) for e in range(j, lo - 1, -1))
    return any(m(0, st) for st in ([0] if anchor_s else range(len(s) + 1)))


# ---------- the OS ----------
class Ctx:
    def __init__(self, os, sess, out, err, inp, job):
        self.os, self.sess, self.out, self.err, self.inp, self.job = os, sess, out, err, inp, job

    def but(self, **kw):
        c = copy.copy(self)
        c.__dict__.update(kw)
        return c


class Job:
    def __init__(self, num, text, gen, wake, sess):
        self.num, self.pid, self.text, self.gen, self.wake, self.sess = num, num + 1, text, gen, wake, sess


class OS:
    def __init__(self, fs=None):
        if fs is None:
            fs = newdir()
            fs["c"]["home"] = newdir()
            fs["c"]["home"]["c"]["user"] = newdir()
            fs["c"]["tmp"] = newdir()
        self.fs, self.clock, self.jobs, self.njobs = fs, 0, [], 0
        self.wins, self.nwin = [], 0
        self.terms, self.nterm = {}, 0
        self.out0, self.err0 = [], []
        self.s0 = self.session(self.out0.append, self.err0.append, None)
        self.s0["out"] = lambda t: self.out0.append(t)
        self.s0["err"] = lambda t: self.err0.append(t)

    def session(self, out, err, term):
        return {"cwd": HOME, "vars": {"HOME": HOME, "USER": "user", "PWD": HOME}, "status": 0, "term": term, "out": out, "err": err}

    # --- running lines ---
    def run_line(self, sess, line):
        try:
            items = parse(line)
        except SyntaxErr as e:
            sess["err"]("sh: syntax error: %s\n" % e)
            sess["status"] = 2
            self.run_due()
            return 2
        if items:
            self.drive_fg(exec_list(Ctx(self, sess, sess["out"], sess["err"], "", False), items))
        self.run_due()
        return sess["status"] if items else 0

    def sh(self, line):
        self.out0.clear()
        self.err0.clear()
        code = self.run_line(self.s0, line)
        return {"out": "".join(self.out0), "err": "".join(self.err0), "code": code}

    def tick(self, ms):
        self.out0.clear()
        self.err0.clear()
        self.move_clock(self.clock + ms)
        return {"out": "".join(self.out0), "err": "".join(self.err0)}

    def drive_fg(self, gen):
        try:
            v = gen.send(None)
            while True:
                if v[0] == "sleep":
                    self.move_clock(self.clock + v[1])
                else:
                    target = v[1]
                    while (target in self.jobs) if target is not None else self.jobs:
                        self.move_clock(max(self.clock, min(j.wake for j in self.jobs)))
                v = gen.send(None)
        except StopIteration as e:
            return e.value

    def run_job(self, j):
        try:
            v = j.gen.send(None)
            while v[0] != "sleep":
                v = j.gen.send(None)
            j.wake = self.clock + v[1]
        except StopIteration as e:
            self.jobs.remove(j)
            st = e.value
            j.sess["out"](("[%d] Done\t%s\n" % (j.num, j.text)) if st == 0 else ("[%d] Exit %d\t%s\n" % (j.num, st, j.text)))

    def run_due(self):
        while True:
            due = [j for j in self.jobs if j.wake <= self.clock]
            if not due:
                return
            self.run_job(min(due, key=lambda j: (j.wake, j.num)))

    def move_clock(self, t):
        while True:
            due = [j for j in self.jobs if j.wake <= t]
            if not due:
                break
            self.clock = max(self.clock, min(j.wake for j in due))
            self.run_due()
        self.clock = max(self.clock, t)

    def start_job(self, c, ao):
        self.njobs += 1
        sess = dict(c.sess)
        sess["vars"] = dict(c.sess["vars"])
        jc = Ctx(self, sess, sess["out"], sess["err"], "", True)
        j = Job(self.njobs, ao["src"], exec_andor(jc, ao), self.clock, sess)
        self.jobs.append(j)
        c.out("[%d] %d\n" % (j.num, j.pid))

    # --- save/load ---
    def save(self):
        return json.dumps(self.fs, sort_keys=True)

    # --- windows ---
    def win(self, id):
        for w in self.wins:
            if w["id"] == id:
                return w
        return None

    def focused(self):
        for w in reversed(self.wins):
            if not w["min"]:
                return w
        return None

    def wm_open(self, app, title):
        k = self.nwin % 8
        self.nwin += 1
        self.wins.append({"id": self.nwin, "app": app, "title": title, "x": 40 * k, "y": 30 * k, "w": 640, "h": 400, "min": False, "max": None})
        return self.nwin

    def wm_close(self, id):
        w = self.win(id)
        if w:
            self.wins.remove(w)

    def wm_focus(self, id):
        w = self.win(id)
        if w:
            w["min"] = False
            self.wins.remove(w)
            self.wins.append(w)

    def wm_move(self, id, x, y):
        w = self.win(id)
        if w:
            w["max"] = None
            w["x"] = max(40 - w["w"], min(SW - 40, x))
            w["y"] = max(0, min(SH - 24, y))

    def wm_resize(self, id, ww, hh):
        w = self.win(id)
        if w:
            w["max"] = None
            w["w"], w["h"] = max(160, ww), max(100, hh)

    def wm_minimize(self, id):
        w = self.win(id)
        if w:
            w["min"] = True

    def wm_maximize(self, id):
        w = self.win(id)
        if not w:
            return
        if w["max"]:
            w["x"], w["y"], w["w"], w["h"] = w["max"]
            w["max"] = None
        else:
            w["max"] = (w["x"], w["y"], w["w"], w["h"])
            w["x"], w["y"], w["w"], w["h"] = 0, 0, SW, SH

    def wm_tile(self):
        vis = sorted([w for w in self.wins if not w["min"]], key=lambda w: w["id"])
        n = len(vis)
        if not n:
            return
        c = 1
        while c * c < n:
            c += 1
        r = (n + c - 1) // c
        cw, ch = SW // c, SH // r
        for i, w in enumerate(vis):
            w["x"], w["y"], w["w"], w["h"], w["max"] = (i % c) * cw, (i // c) * ch, cw, ch, None

    def wm_cycle(self):
        vis = [w for w in self.wins if not w["min"]]
        if len(vis) < 2:
            return
        top = vis[-1]
        self.wins.remove(top)
        self.wins.insert(0, top)

    def wm_at(self, x, y):
        for w in reversed(self.wins):
            if not w["min"] and w["x"] <= x < w["x"] + w["w"] and w["y"] <= y < w["y"] + w["h"]:
                return w["id"]
        return None

    def wm_list(self):
        f = self.focused()
        return [{"id": w["id"], "app": w["app"], "title": w["title"], "x": w["x"], "y": w["y"], "w": w["w"], "h": w["h"],
                 "focused": w is f, "minimized": w["min"], "maximized": w["max"] is not None} for w in self.wins]

    # --- terminals ---
    def term_new(self, cols, rows):
        self.nterm += 1
        t = {"id": self.nterm, "cols": cols, "rows": rows, "scroll": "", "inp": "", "cur": 0, "hist": [], "hidx": None, "saved": ""}

        def w(s):
            t["scroll"] += s
        t["sess"] = self.session(w, w, t)
        self.terms[self.nterm] = t
        return self.nterm

    def prompt(self, t):
        cwd = t["sess"]["cwd"]
        if cwd == HOME or cwd.startswith(HOME + "/"):
            cwd = "~" + cwd[len(HOME):]
        return "user@fwebos:%s$ " % cwd

    def term_keys(self, id, keys):
        t = self.terms.get(id)
        if t:
            for k in keys:
                self.term_key(t, k)

    def term_type(self, id, text):
        self.term_keys(id, list(text))

    def term_key(self, t, k):
        k = {"\n": "Enter", "\t": "Tab"}.get(k, k)
        inp, cur = t["inp"], t["cur"]
        if len(k) == 1:
            t["inp"], t["cur"], t["hidx"] = inp[:cur] + k + inp[cur:], cur + 1, None
        elif k == "Backspace":
            if cur > 0:
                t["inp"], t["cur"] = inp[:cur - 1] + inp[cur:], cur - 1
            t["hidx"] = None
        elif k == "Delete":
            t["inp"] = inp[:cur] + inp[cur + 1:]
            t["hidx"] = None
        elif k == "Left":
            t["cur"] = max(0, cur - 1)
        elif k == "Right":
            t["cur"] = min(len(inp), cur + 1)
        elif k == "Home":
            t["cur"] = 0
        elif k == "End":
            t["cur"] = len(inp)
        elif k == "Up":
            h = t["hist"]
            if not h:
                return
            if t["hidx"] is None:
                t["saved"], t["hidx"] = inp, len(h) - 1
            elif t["hidx"] > 0:
                t["hidx"] -= 1
            t["inp"] = h[t["hidx"]]
            t["cur"] = len(t["inp"])
        elif k == "Down":
            if t["hidx"] is None:
                return
            if t["hidx"] < len(t["hist"]) - 1:
                t["hidx"] += 1
                t["inp"] = t["hist"][t["hidx"]]
            else:
                t["hidx"], t["inp"] = None, t["saved"]
            t["cur"] = len(t["inp"])
        elif k == "Enter":
            t["scroll"] += self.prompt(t) + inp + "\n"
            t["inp"], t["cur"], t["hidx"] = "", 0, None
            if inp.strip(" ") and (not t["hist"] or t["hist"][-1] != inp):
                t["hist"].append(inp)
            self.run_line(t["sess"], inp)
        elif k == "Ctrl-C":
            t["scroll"] += self.prompt(t) + inp + "^C\n"
            t["inp"], t["cur"], t["hidx"] = "", 0, None
        elif k == "Ctrl-L":
            t["scroll"] = ""
        elif k == "Tab":
            self.complete(t)

    def complete(self, t):
        inp, cur = t["inp"], t["cur"]
        before = inp[:cur]
        sp = before.rfind(" ")
        word = before[sp + 1:]
        if sp < 0:
            dirpart, rest = "", word
            cands = [(n, False) for n in COMMANDS if n.startswith(word)]
        else:
            sl = word.rfind("/")
            dirpart, rest = word[:sl + 1], word[sl + 1:]
            d = lookup(self.fs, norm(t["sess"]["cwd"], dirpart))
            cands = []
            if d is not None and d["t"] == "d":
                cands = [(n, d["c"][n]["t"] == "d") for n in sorted(d["c"])
                         if n.startswith(rest) and (rest.startswith(".") or not n.startswith("."))]
        if not cands:
            return
        if len(cands) == 1:
            rep = dirpart + cands[0][0] + ("/" if cands[0][1] else " ")
        else:
            names = [n for n, _ in cands]
            lcp = names[0]
            for n in names[1:]:
                while not n.startswith(lcp):
                    lcp = lcp[:-1]
            if len(lcp) <= len(rest):
                t["scroll"] += self.prompt(t) + inp + "\n" + "  ".join(n + ("/" if d else "") for n, d in cands) + "\n"
                return
            rep = dirpart + lcp
        new = before[:sp + 1] + rep
        t["inp"], t["cur"], t["hidx"] = new + inp[cur:], len(new), None

    def term_layout(self, id):
        t = self.terms[id]
        cols, rows = t["cols"], t["rows"]
        out = []
        for p in lines(t["scroll"]):
            p = p.replace("\t", " ")
            if p == "":
                out.append("")
            for k in range(0, len(p), cols):
                out.append(p[k:k + cols])
        pl = self.prompt(t) + t["inp"]
        pos = len(self.prompt(t)) + t["cur"]
        n = max((len(pl) + cols - 1) // cols, pos // cols + 1)
        for k in range(n):
            out.append(pl[k * cols:(k + 1) * cols])
        cline = len(out) - n + pos // cols
        vis = out[-rows:]
        off = len(out) - len(vis)
        screen = [l.ljust(cols) for l in vis] + [" " * cols] * (rows - len(vis))
        return screen, {"row": cline - off, "col": pos % cols}


def load(text):
    return OS(json.loads(text))


# ---------- the executor ----------
def call(r):
    if isinstance(r, types.GeneratorType):
        r = yield from r
    return r


def exec_list(c, items):
    for ao, bg in items:
        if bg:
            c.os.start_job(c, ao)
            c.sess["status"] = 0
        else:
            yield from exec_andor(c, ao)
    return c.sess["status"]


def exec_andor(c, ao):
    st = yield from exec_pipeline(c, ao["first"])
    for op, p in ao["rest"]:
        if (op == "&&") == (st == 0):
            st = yield from exec_pipeline(c, p)
    return st


def exec_pipeline(c, cmds):
    inp = c.inp
    st = 0
    for i, cmd in enumerate(cmds):
        if i == len(cmds) - 1:
            st = yield from exec_command(c.but(inp=inp), cmd)
        else:
            buf = []
            st = yield from exec_command(c.but(inp=inp, out=buf.append), cmd)
            inp = "".join(buf)
    c.sess["status"] = st
    return st


def exec_command(c, cmd):
    if cmd[0] == "simple":
        return (yield from exec_simple(c, cmd[1], cmd[2]))
    if cmd[0] == "if":
        for cond, body in cmd[1]:
            if (yield from exec_list(c, cond)) == 0:
                return (yield from exec_list(c, body))
        if cmd[2] is not None:
            return (yield from exec_list(c, cmd[2]))
        return 0
    if cmd[0] == "for":
        try:
            ws = []
            for w in cmd[2]:
                ws += yield from expand(c, w)
        except ShErr as e:
            c.err(e.msg + "\n")
            return e.status
        st = 0
        for w in ws:
            c.sess["vars"][cmd[1]] = w
            st = yield from exec_list(c, cmd[3])
        return st
    st = 0
    while (yield from exec_list(c, cmd[1])) == 0:
        st = yield from exec_list(c, cmd[2])
    return st


def assign_split(w):
    p = w[0]
    if p[0] != "lit" or p[2] or "=" not in p[1]:
        return None
    name, rest = p[1].split("=", 1)
    if not NAME_RE.fullmatch(name):
        return None
    return name, ([("lit", rest, False)] if rest else []) + w[1:]


def exec_simple(c, words, redirs):
    try:
        if words and all(assign_split(w) for w in words):
            for w in words:
                name, val = assign_split(w)
                s = ""
                for p in val:
                    s += p[1] if p[0] == "lit" else (yield from pval(c, p))
                c.sess["vars"][name] = s
            fields = []
        else:
            fields = []
            for w in words:
                fields += yield from expand(c, w)
        out, err, inp = c.out, c.err, c.inp
        fs = c.os.fs
        for op, w in redirs:
            if op == "2>&1":
                err = out
                continue
            f = yield from expand(c, w)
            x = " ".join(f)
            path = norm(c.sess["cwd"], x)
            n = lookup(fs, path)
            if op == "<":
                if n is None:
                    raise ShErr("sh: %s: No such file or directory" % x)
                if n["t"] == "d":
                    raise ShErr("sh: %s: Is a directory" % x)
                inp = n["s"]
                continue
            if n is not None and n["t"] == "d":
                raise ShErr("sh: %s: Is a directory" % x)
            if n is None:
                par, nm = parent_of(fs, path)
                if par is None:
                    raise ShErr("sh: %s: No such file or directory" % x)
                n = par["c"][nm] = newfile()
            if op in (">", "2>"):
                n["s"] = ""

            def sink(t, n=n):
                n["s"] += t
            if op == "2>":
                err = sink
            else:
                out = sink
    except ShErr as e:
        c.err(e.msg + "\n")
        return e.status
    if not fields:
        return 0
    name, args = fields[0], fields[1:]
    if name not in COMMANDS:
        err("sh: %s: command not found\n" % name)
        return 127
    return (yield from call(BUILTINS[name](c.but(out=out, err=err, inp=inp), args, name)))


def pval(c, p):
    if p[0] == "var":
        return c.sess["vars"].get(p[1], "")
    if p[0] == "status":
        return str(c.sess["status"])
    if p[0] == "arith":
        return str(arith(c, p[1]))
    sess = dict(c.sess)
    sess["vars"] = dict(c.sess["vars"])
    buf = []
    yield from exec_list(c.but(sess=sess, out=buf.append, inp=""), p[1])
    return "".join(buf).rstrip("\n")


def expand(c, parts):
    fields, cur = [], [None]

    def add(t, g):
        if cur[0] is None:
            cur[0] = []
        cur[0].append((t, g))

    def push():
        if cur[0] is not None:
            fields.append(cur[0])
            cur[0] = None
    p0 = parts[0] if parts else None
    if p0 and p0[0] == "lit" and not p0[2] and p0[1].startswith("~") and (p0[1][1:2] == "/" or (p0[1] == "~" and len(parts) == 1)):
        parts = [("lit", c.sess["vars"].get("HOME", ""), True)] + ([("lit", p0[1][1:], False)] if p0[1][1:] else []) + parts[1:]
    for p in parts:
        if p[0] == "lit":
            add(p[1], not p[2])
            continue
        v = yield from call(pval(c, p))
        if p[2]:
            add(v, False)
            continue
        if v == "":
            continue
        pieces = v.split()
        if v[0] in " \t\n":
            push()
        for k, piece in enumerate(pieces):
            if k > 0:
                push()
            add(piece, False)
        if v[-1] in " \t\n":
            push()
    push()
    res = []
    for f in fields:
        text = "".join(t for t, _ in f)
        chars = [(ch, g) for t, g in f for ch in t]
        if not any(g and ch in "*?" for ch, g in chars):
            res.append(text)
            continue
        sl = text.rfind("/")
        if any(g and ch in "*?" for ch, g in chars[:sl + 1]):
            res.append(text)
            continue
        dirpart, pat = text[:sl + 1], chars[sl + 1:]
        d = lookup(c.os.fs, norm(c.sess["cwd"], dirpart))
        ms = []
        if d is not None and d["t"] == "d":
            dot = bool(pat) and pat[0][0] == "."
            ms = [dirpart + n for n in sorted(d["c"]) if (dot or not n.startswith(".")) and gmatch(pat, n)]
        res += ms or [text]
    return res


# ---------- commands ----------
def opts(args, withval=""):
    o, i = {}, 0
    while i < len(args) and args[i].startswith("-") and len(args[i]) > 1:
        a = args[i]
        i += 1
        j = 1
        while j < len(a):
            if a[j] in withval:
                if j + 1 < len(a):
                    o[a[j]] = a[j + 1:]
                else:
                    o[a[j]] = args[i] if i < len(args) else ""
                    i += 1
                break
            o[a[j]] = True
            j += 1
    return o, args[i:]


def node(c, x):
    return lookup(c.os.fs, norm(c.sess["cwd"], x))


def read_input(c, name, f):
    if f is None:
        return c.inp
    n = node(c, f)
    if n is None:
        c.err("%s: %s: No such file or directory\n" % (name, f))
        return None
    if n["t"] == "d":
        c.err("%s: %s: Is a directory\n" % (name, f))
        return None
    return n["s"]


def b_pwd(c, a, name):
    c.out(c.sess["cwd"] + "\n")
    return 0


def b_cd(c, a, name):
    x = a[0] if a else c.sess["vars"].get("HOME", "/")
    p = norm(c.sess["cwd"], x)
    n = lookup(c.os.fs, p)
    if n is None:
        c.err("cd: %s: No such file or directory\n" % x)
        return 1
    if n["t"] != "d":
        c.err("cd: %s: Not a directory\n" % x)
        return 1
    c.sess["cwd"] = p
    c.sess["vars"]["PWD"] = p
    return 0


def b_ls(c, a, name):
    o, a = opts(a)
    show = lambda n: o.get("a") or not n.startswith(".")
    if not a:
        c.out(unlines([n for n in sorted(node(c, ".")["c"]) if show(n)]))
        return 0
    st, files, dirs = 0, [], []
    for x in a:
        n = node(c, x)
        if n is None:
            c.err("ls: %s: No such file or directory\n" % x)
            st = 1
        elif n["t"] == "f":
            files.append(x)
        else:
            dirs.append((x, n))
    out = unlines(files)
    for x, n in dirs:
        ents = unlines([e for e in sorted(n["c"]) if show(e)])
        if len(a) == 1:
            out += ents
        else:
            out += ("\n" if out else "") + x + ":\n" + ents
    c.out(out)
    return st


def b_cat(c, a, name):
    if not a:
        c.out(c.inp)
        return 0
    st = 0
    for x in a:
        t = read_input(c, "cat", x)
        if t is None:
            st = 1
        else:
            c.out(t)
    return st


def b_touch(c, a, name):
    st = 0
    for x in a:
        p = norm(c.sess["cwd"], x)
        if lookup(c.os.fs, p) is not None:
            continue
        par, nm = parent_of(c.os.fs, p)
        if par is None:
            c.err("touch: %s: No such file or directory\n" % x)
            st = 1
        else:
            par["c"][nm] = newfile()
    return st


def b_mkdir(c, a, name):
    o, a = opts(a)
    st = 0
    for x in a:
        p = norm(c.sess["cwd"], x)
        if o.get("p"):
            n = c.os.fs
            for part in [q for q in p.split("/") if q]:
                if part not in n["c"]:
                    n["c"][part] = newdir()
                n = n["c"][part]
                if n["t"] != "d":
                    c.err("mkdir: %s: File exists\n" % x)
                    st = 1
                    break
            continue
        if lookup(c.os.fs, p) is not None:
            c.err("mkdir: %s: File exists\n" % x)
            st = 1
            continue
        par, nm = parent_of(c.os.fs, p)
        if par is None:
            c.err("mkdir: %s: No such file or directory\n" % x)
            st = 1
        else:
            par["c"][nm] = newdir()
    return st


def b_rm(c, a, name):
    o, a = opts(a)
    st = 0
    for x in a:
        p = norm(c.sess["cwd"], x)
        n = lookup(c.os.fs, p)
        if n is None:
            if not o.get("f"):
                c.err("rm: %s: No such file or directory\n" % x)
                st = 1
            continue
        if n["t"] == "d" and not o.get("r"):
            c.err("rm: %s: Is a directory\n" % x)
            st = 1
            continue
        par, nm = parent_of(c.os.fs, p)
        if par is not None:
            del par["c"][nm]
    return st


def b_cpmv(c, a, name):
    o, a = opts(a)
    if len(a) < 2:
        c.err("%s: missing operand\n" % name)
        return 1
    src, dst = a[0], a[1]
    sp = norm(c.sess["cwd"], src)
    sn = lookup(c.os.fs, sp)
    if sn is None:
        c.err("%s: %s: No such file or directory\n" % (name, src))
        return 1
    if name == "cp" and sn["t"] == "d" and not o.get("r"):
        c.err("cp: %s: Is a directory\n" % src)
        return 1
    dp = norm(c.sess["cwd"], dst)
    dn = lookup(c.os.fs, dp)
    target = dp if dn is None or dn["t"] != "d" else norm(dp, basename(sp))
    par, nm = parent_of(c.os.fs, target)
    if par is None:
        c.err("%s: %s: No such file or directory\n" % (name, dst))
        return 1
    if name == "mv":
        spar, snm = parent_of(c.os.fs, sp)
        del spar["c"][snm]
        par["c"][nm] = sn
    else:
        par["c"][nm] = copy.deepcopy(sn)
    return 0


def b_echo(c, a, name):
    if a and a[0] == "-n":
        c.out(" ".join(a[1:]))
    else:
        c.out(" ".join(a) + "\n")
    return 0


def b_headtail(c, a, name):
    o, a = opts(a, "n")
    n = int(o.get("n", 10))
    t = read_input(c, name, a[0] if a else None)
    if t is None:
        return 1
    ls = lines(t)
    c.out(unlines(ls[:n] if name == "head" else (ls[-n:] if n > 0 else [])))
    return 0


def b_wc(c, a, name):
    o, a = opts(a)
    f = a[0] if a else None
    t = read_input(c, "wc", f)
    if t is None:
        return 1
    counts = {"l": t.count("\n"), "w": len(t.split()), "c": len(t)}
    which = [k for k in "lwc" if o.get(k)] or ["l", "w", "c"]
    c.out(" ".join(str(counts[k]) for k in which) + (" " + f if f is not None else "") + "\n")
    return 0


NUM_RE = re.compile(r"-?[0-9]+(\.[0-9]+)?")


def b_sort(c, a, name):
    o, a = opts(a)
    t = read_input(c, "sort", a[0] if a else None)
    if t is None:
        return 1
    ls = lines(t)
    if o.get("n"):
        def key(l):
            m = NUM_RE.match(l)
            return (float(m.group(0)) if m else 0.0, l)
        ls = sorted(ls, key=key)
    else:
        ls = sorted(ls)
    if o.get("r"):
        ls.reverse()
    if o.get("u"):
        ls = [l for i, l in enumerate(ls) if i == 0 or ls[i - 1] != l]
    c.out(unlines(ls))
    return 0


def b_uniq(c, a, name):
    o, a = opts(a)
    t = read_input(c, "uniq", a[0] if a else None)
    if t is None:
        return 1
    runs = []
    for l in lines(t):
        if runs and runs[-1][0] == l:
            runs[-1][1] += 1
        else:
            runs.append([l, 1])
    c.out(unlines([("%d %s" % (k, l)) if o.get("c") else l for l, k in runs]))
    return 0


def b_cut(c, a, name):
    o, a = opts(a, "df")
    d = o.get("d", "\t")
    f = int(o.get("f", "1"))
    t = read_input(c, "cut", a[0] if a else None)
    if t is None:
        return 1
    res = []
    for l in lines(t):
        if d not in l:
            res.append(l)
        else:
            ps = l.split(d)
            res.append(ps[f - 1] if f - 1 < len(ps) else "")
    c.out(unlines(res))
    return 0


def trset(s):
    s = s.replace("\\\\", "\0").replace("\\n", "\n").replace("\\t", "\t").replace("\0", "\\")
    r, i = [], 0
    while i < len(s):
        if i + 2 < len(s) and s[i + 1] == "-":
            r += [chr(x) for x in range(ord(s[i]), ord(s[i + 2]) + 1)]
            i += 3
        else:
            r.append(s[i])
            i += 1
    return r


def b_tr(c, a, name):
    o, a = opts(a)
    s1 = trset(a[0]) if a else []
    if o.get("d"):
        c.out("".join(ch for ch in c.inp if ch not in s1))
        return 0
    s2 = trset(a[1]) if len(a) > 1 else []
    res = []
    for ch in c.inp:
        if ch in s1 and s2:
            res.append(s2[min(s1.index(ch), len(s2) - 1)])
        else:
            res.append(ch)
    c.out("".join(res))
    return 0


def b_grep(c, a, name):
    o, a = opts(a)
    if not a:
        c.err("grep: missing pattern\n")
        return 2
    rx = rparse(a[0])
    files = a[1:]
    many = len(files) > 1
    sel, missing = 0, False
    srcs = [(None, c.inp)] if not files else [(f, read_input(c, "grep", f)) for f in files]
    for f, t in srcs:
        if t is None:
            missing = True
            continue
        pre = f + ":" if many else ""
        cnt = 0
        for i, l in enumerate(lines(t)):
            if rmatch(rx, l, o.get("i")) != bool(o.get("v")):
                cnt += 1
                if not o.get("c"):
                    c.out(pre + ("%d:" % (i + 1) if o.get("n") else "") + l + "\n")
        if o.get("c"):
            c.out(pre + str(cnt) + "\n")
        sel += cnt
    return 0 if sel else (2 if missing else 1)


def b_seq(c, a, name):
    first, last = (int(a[0]), int(a[1])) if len(a) > 1 else (1, int(a[0]))
    c.out(unlines([str(i) for i in range(first, last + 1)]))
    return 0


def b_find(c, a, name):
    path = "."
    if a and not a[0].startswith("-"):
        path, a = a[0], a[1:]
    pat, typ = None, None
    i = 0
    while i + 1 < len(a):
        if a[i] == "-name":
            pat = a[i + 1]
        elif a[i] == "-type":
            typ = a[i + 1]
        i += 2
    root = node(c, path)
    if root is None:
        c.err("find: %s: No such file or directory\n" % path)
        return 1
    out = []
    rootname = path.rstrip("/") or "/"
    rootname = rootname[rootname.rfind("/") + 1:] if rootname != "/" else "/"

    def walk(n, shown, nm):
        if (pat is None or gmatch(gpat(pat), nm)) and (typ is None or n["t"] == typ):
            out.append(shown)
        if n["t"] == "d":
            for k in sorted(n["c"]):
                walk(n["c"][k], shown + k if shown.endswith("/") else shown + "/" + k, k)
    walk(root, path, rootname)
    c.out(unlines(out))
    return 0


def b_true(c, a, name):
    return 0


def b_false(c, a, name):
    return 1


def b_whoami(c, a, name):
    c.out("user\n")
    return 0


def b_uname(c, a, name):
    c.out("fwebos\n")
    return 0


def b_env(c, a, name):
    v = c.sess["vars"]
    c.out(unlines(["%s=%s" % (k, v[k]) for k in sorted(v)]))
    return 0


def b_export(c, a, name):
    for x in a:
        if "=" in x:
            k, v = x.split("=", 1)
            if NAME_RE.fullmatch(k):
                c.sess["vars"][k] = v
    return 0


def b_unset(c, a, name):
    for x in a:
        c.sess["vars"].pop(x, None)
    return 0


def b_test(c, a, name):
    if name == "[":
        if not a or a[-1] != "]":
            c.err("[: missing ]\n")
            return 2
        a = a[:-1]
    neg = False
    if a and a[0] == "!":
        neg, a = True, a[1:]

    def ints(x, y):
        for v in (x, y):
            if not INT_RE.fullmatch(v):
                raise ShErr("%s: %s: integer expression expected" % (name, v), 2)
        return int(x), int(y)
    try:
        if len(a) == 0:
            r = False
        elif len(a) == 1:
            r = a[0] != ""
        elif len(a) == 2 and a[0] in ("-e", "-f", "-d", "-z", "-n"):
            op, x = a
            if op in ("-z", "-n"):
                r = (x == "") == (op == "-z")
            else:
                n = node(c, x)
                r = n is not None and (op == "-e" or n["t"] == op[1])
        elif len(a) == 3 and a[1] in ("=", "!=", "-eq", "-ne", "-lt", "-le", "-gt", "-ge"):
            x, op, y = a
            if op == "=":
                r = x == y
            elif op == "!=":
                r = x != y
            else:
                i, j = ints(x, y)
                r = {"-eq": i == j, "-ne": i != j, "-lt": i < j, "-le": i <= j, "-gt": i > j, "-ge": i >= j}[op]
        else:
            raise ShErr("%s: bad expression" % name, 2)
    except ShErr as e:
        c.err(e.msg + "\n")
        return e.status
    return 0 if r != neg else 1


def b_source(c, a, name):
    if not a:
        return 0
    t = read_input(c, "sh", a[0])
    if t is None:
        return 1
    try:
        items = parse(t)
    except SyntaxErr as e:
        c.err("sh: syntax error: %s\n" % e)
        return 2
    return (yield from exec_list(c, items)) if items else 0


def b_clear(c, a, name):
    if c.sess["term"] is not None:
        c.sess["term"]["scroll"] = ""
    return 0


def b_sleep(c, a, name):
    if not a:
        return 0
    yield ("sleep", round(float(a[0]) * 1000))
    return 0


def find_job(os, x):
    for j in os.jobs:
        if (x.startswith("%") and x[1:] == str(j.num)) or x == str(j.pid):
            return j
    return None


def b_wait(c, a, name):
    if c.job:
        return 0
    target = None
    if a:
        target = find_job(c.os, a[0])
        if target is None:
            return 0
    yield ("wait", target)
    return 0


def b_jobs(c, a, name):
    c.out(unlines(["[%d] Running\t%s" % (j.num, j.text) for j in sorted(c.os.jobs, key=lambda j: j.num)]))
    return 0


def b_ps(c, a, name):
    ls = ["PID\tSTAT\tCMD", "1\tR\tsh"]
    for j in sorted(c.os.jobs, key=lambda j: j.pid):
        ls.append("%d\t%s\t%s" % (j.pid, "R" if j.wake <= c.os.clock else "S", j.text))
    c.out(unlines(ls))
    return 0


def b_kill(c, a, name):
    st = 0
    for x in a:
        j = find_job(c.os, x)
        if j is None:
            c.err("kill: %s: No such process\n" % x)
            st = 1
            continue
        c.os.jobs.remove(j)
        c.out("[%d] Terminated\t%s\n" % (j.num, j.text))
    return st


def b_uptime(c, a, name):
    c.out("up %d.%03ds\n" % (c.os.clock // 1000, c.os.clock % 1000))
    return 0


BUILTINS = {"pwd": b_pwd, "cd": b_cd, "ls": b_ls, "cat": b_cat, "touch": b_touch, "mkdir": b_mkdir, "rm": b_rm, "cp": b_cpmv,
            "mv": b_cpmv, "echo": b_echo, "head": b_headtail, "tail": b_headtail, "wc": b_wc, "sort": b_sort, "uniq": b_uniq,
            "cut": b_cut, "tr": b_tr, "grep": b_grep, "seq": b_seq, "find": b_find, "true": b_true, "false": b_false,
            "whoami": b_whoami, "uname": b_uname, "env": b_env, "export": b_export, "unset": b_unset, "test": b_test,
            "[": b_test, "sh": b_source, "source": b_source, "clear": b_clear, "sleep": b_sleep, "wait": b_wait,
            "jobs": b_jobs, "ps": b_ps, "kill": b_kill, "uptime": b_uptime}
assert sorted(BUILTINS) == COMMANDS
