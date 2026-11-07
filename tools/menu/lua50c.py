#!/usr/bin/env python3
"""A small Lua 5.0 compiler (stdlib only) for the Anark UI scripts of Ratchet & Clank: Tools of Destruction.

Why it exists: the game's scripts are Lua 5.0 bytecode (little endian, number = float32, instructions
op:6 C:9 B:9 A:8) and the stock luac 5.0.3 is not on this PC (and we may not download it, see
log/menu104/REPORT.md). The page scripts only need a small subset of the language, so this compiles
exactly that subset with the code generator conventions of Lua 5.0.3 (register allocation, RK operands,
jump lists, numeric `for` with the SUB/JMP/FORLOOP prologue, TEST fused with the following JMP):

  statements   local, assignment (one target), call, if/elseif/else, while, numeric for, return, break,
               do..end, function name / function a.b:c(...) (closures without upvalues)
  expressions  nil true false number string, names, a.b, a[e], f(...), a:m(...), {} / {k=v,[e]=v}
               (no positional items), unary - not, binary + - * / ^ .. == ~= < <= > >= and or, (e)
  NOT supported (raises): upvalues, varargs, generic for, repeat, positional table items, multiple assignment.

Output: a `Proto` of tools/menu/lua50_patch.py, dumped with lua50_patch.dump(): header identical to the
game's (1b 4c 75 61 50 01 04 04 04 06 08 09 09 04 + float32 test number).

Usage:
  python -I lua50c.py SRC.lua OUT.luac [chunkname]
"""
import re
import struct
import sys

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
import lua50_patch as L  # noqa: E402

OP = L.OP
enc, encbx = L.enc, L.encbx
NO_JUMP = -1
MAXRK_K = 261             # largest constant index that fits an RK operand (RK(x): x >= 250 -> K[x-250], 9 bits)
BIAS = 131071             # sBx bias (18 bit Bx)


class LuaSyntaxError(Exception):
    pass


# ----------------------------------------------------------------------------- lexer
KEYWORDS = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in", "local", "nil",
            "not", "or", "repeat", "return", "then", "true", "until", "while"}
TOKEN_RE = re.compile(r"""
    (?P<ws>[ \t\r]+)
  | (?P<nl>\n)
  | (?P<lcomment>--\[\[.*?\]\])
  | (?P<comment>--[^\n]*)
  | (?P<num>(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?)
  | (?P<name>[A-Za-z_][A-Za-z_0-9]*)
  | (?P<str>"(\\.|[^"\\\n])*"|'(\\.|[^'\\\n])*')
  | (?P<op>\.\.\.|\.\.|==|~=|<=|>=|[-+*/^<>=(){}\[\];:,.])
""", re.X | re.S)


def lex(src):
    toks, pos, line = [], 0, 1
    while pos < len(src):
        m = TOKEN_RE.match(src, pos)
        if not m:
            raise LuaSyntaxError("line %d: bad character %r" % (line, src[pos]))
        k, text = m.lastgroup, m.group()
        pos = m.end()
        if k == "nl":
            line += 1
        elif k == "lcomment":
            line += text.count("\n")
        elif k in ("ws", "comment"):
            pass
        elif k == "num":
            toks.append(("num", float(text), line))
        elif k == "name":
            toks.append((text if text in KEYWORDS else "name", text, line))
        elif k == "str":
            toks.append(("str", unescape(text[1:-1]), line))
            line += text.count("\n")
        else:
            toks.append((text, text, line))
    toks.append(("<eof>", "<eof>", line))
    return toks


def unescape(s):
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == "\\":
            i += 1
            c = s[i]
            if c == "n": out.append("\n")
            elif c == "t": out.append("\t")
            elif c == "r": out.append("\r")
            elif c.isdigit():
                j = i
                while j < len(s) and j < i + 3 and s[j].isdigit():
                    j += 1
                out.append(chr(int(s[i:j])))
                i = j - 1
            else: out.append(c)
        else:
            out.append(c)
        i += 1
    return "".join(out)


# ----------------------------------------------------------------------------- parser (AST = tuples)
class Parser:
    def __init__(self, src):
        self.t = lex(src)
        self.i = 0

    @property
    def tok(self):
        return self.t[self.i]

    def next(self):
        t = self.t[self.i]
        self.i += 1
        return t

    def check(self, k):
        if self.tok[0] != k:
            raise LuaSyntaxError("line %d: %r expected near %r" % (self.tok[2], k, self.tok[1]))
        return self.next()

    def test(self, k):
        if self.tok[0] == k:
            self.next()
            return True
        return False

    def block(self):
        out = []
        while self.tok[0] not in ("end", "else", "elseif", "<eof>", "until"):
            if self.tok[0] == "return":
                line = self.next()[2]
                exps = []
                if self.tok[0] not in ("end", "else", "elseif", "<eof>", ";"):
                    exps = self.exprlist()
                self.test(";")
                out.append(("return", line, exps))
                break
            if self.tok[0] == "break":
                out.append(("break", self.next()[2]))
                self.test(";")
                break
            out.append(self.statement())
            self.test(";")
        return out

    def statement(self):
        k, v, line = self.tok
        if k == "if":
            self.next()
            arms = []
            cond = self.expr()
            self.check("then")
            arms.append((cond, self.block()))
            els = None
            while True:
                if self.test("elseif"):
                    c = self.expr()
                    self.check("then")
                    arms.append((c, self.block()))
                elif self.test("else"):
                    els = self.block()
                    self.check("end")
                    break
                else:
                    self.check("end")
                    break
            return ("if", line, arms, els)
        if k == "while":
            self.next()
            c = self.expr()
            self.check("do")
            b = self.block()
            self.check("end")
            return ("while", line, c, b)
        if k == "do":
            self.next()
            b = self.block()
            self.check("end")
            return ("do", line, b)
        if k == "for":
            self.next()
            name = self.check("name")[1]
            if self.tok[0] != "=":
                raise LuaSyntaxError("line %d: generic for is not supported" % line)
            self.next()
            a = self.expr()
            self.check(",")
            b = self.expr()
            c = self.expr() if self.test(",") else None
            self.check("do")
            body = self.block()
            self.check("end")
            return ("for", line, name, a, b, c, body)
        if k == "function":
            self.next()
            target = ("name", self.check("name")[1], line)
            method = False
            while self.tok[0] in (".", ":"):
                sep = self.next()[0]
                key = self.check("name")[1]
                target = ("index", target, ("str", key, line), line)
                if sep == ":":
                    method = True
                    break
            return ("assign", line, target, self.funcbody(line, method))
        if k == "local":
            self.next()
            if self.test("function"):
                name = self.check("name")[1]
                raise LuaSyntaxError("line %d: local function (upvalue risk) is not supported" % line)
            names = [self.check("name")[1]]
            while self.test(","):
                names.append(self.check("name")[1])
            exps = self.exprlist() if self.test("=") else []
            return ("local", line, names, exps)
        if k == "repeat":
            raise LuaSyntaxError("line %d: repeat is not supported" % line)
        e = self.suffixed()
        if self.tok[0] in ("=", ","):
            if self.tok[0] == ",":
                raise LuaSyntaxError("line %d: multiple assignment is not supported" % line)
            self.next()
            if e[0] not in ("name", "index"):
                raise LuaSyntaxError("line %d: cannot assign" % line)
            return ("assign", line, e, self.expr())
        if e[0] not in ("call", "method"):
            raise LuaSyntaxError("line %d: syntax error near %r" % (line, self.tok[1]))
        return ("callstat", line, e)

    def funcbody(self, line, method=False):
        self.check("(")
        params = ["self"] if method else []
        if self.tok[0] != ")":
            params.append(self.check("name")[1])
            while self.test(","):
                params.append(self.check("name")[1])
        self.check(")")
        body = self.block()
        endline = self.check("end")[2]
        return ("function", params, body, line, endline)

    def exprlist(self):
        es = [self.expr()]
        while self.test(","):
            es.append(self.expr())
        return es

    PRI = {"or": (1, 1), "and": (2, 2), "<": (3, 3), ">": (3, 3), "<=": (3, 3), ">=": (3, 3), "~=": (3, 3),
           "==": (3, 3), "..": (5, 4), "+": (6, 6), "-": (6, 6), "*": (7, 7), "/": (7, 7), "^": (10, 9)}
    UNARY_PRI = 8

    def expr(self, limit=0):
        k, v, line = self.tok
        if k in ("not", "-"):
            self.next()
            operand = self.expr(self.UNARY_PRI)
            if k == "-" and operand[0] == "num":
                left = ("num", -operand[1], line)
            else:
                left = ("unop", k, operand, line)
        else:
            left = self.simple()
        while self.tok[0] in self.PRI and self.PRI[self.tok[0]][0] > limit:
            op, _, line = self.next()
            right = self.expr(self.PRI[op][1])
            left = ("binop", op, left, right, line)
        return left

    def simple(self):
        k, v, line = self.tok
        if k == "num": self.next(); return ("num", v, line)
        if k == "str": self.next(); return ("str", v, line)
        if k == "nil": self.next(); return ("nil", line)
        if k == "true": self.next(); return ("true", line)
        if k == "false": self.next(); return ("false", line)
        if k == "...": raise LuaSyntaxError("line %d: varargs are not supported" % line)
        if k == "{": return self.table()
        if k == "function":
            self.next()
            return self.funcbody(line)
        return self.suffixed()

    def primary(self):
        k, v, line = self.tok
        if k == "name":
            self.next()
            return ("name", v, line)
        if k == "(":
            self.next()
            e = self.expr()
            self.check(")")
            return ("paren", e, line)
        raise LuaSyntaxError("line %d: unexpected symbol near %r" % (line, v))

    def suffixed(self):
        e = self.primary()
        while True:
            k, v, line = self.tok
            if k == ".":
                self.next()
                e = ("index", e, ("str", self.check("name")[1], line), line)
            elif k == "[":
                self.next()
                idx = self.expr()
                self.check("]")
                e = ("index", e, idx, line)
            elif k == ":":
                self.next()
                name = self.check("name")[1]
                e = ("method", e, name, self.args(), line)
            elif k in ("(", "str", "{"):
                e = ("call", e, self.args(), line)
            else:
                return e

    def args(self):
        k, v, line = self.tok
        if k == "str":
            self.next()
            return [("str", v, line)]
        if k == "{":
            return [self.table()]
        self.check("(")
        a = []
        if self.tok[0] != ")":
            a = self.exprlist()
        self.check(")")
        return a

    def table(self):
        line = self.check("{")[2]
        items = []
        while self.tok[0] != "}":
            if self.tok[0] == "name" and self.t[self.i + 1][0] == "=":
                key = ("str", self.next()[1], line)
                self.next()
                items.append((key, self.expr()))
            elif self.tok[0] == "[":
                self.next()
                key = self.expr()
                self.check("]")
                self.check("=")
                items.append((key, self.expr()))
            else:
                raise LuaSyntaxError("line %d: positional table items are not supported" % self.tok[2])
            if not (self.test(",") or self.test(";")):
                break
        self.check("}")
        return ("table", items, line)


# ----------------------------------------------------------------------------- code generator
class FS:
    def __init__(self, parent=None):
        self.parent = parent
        self.code, self.lines, self.k, self.protos = [], [], [], []
        self.actvars = []            # [(name, reg)]
        self.freereg = 0
        self.maxstack = 2
        self.locs = []               # [name, startpc, endpc] (debug)
        self.line = 1
        self.breaks = []


def kidx(fs, t, v):
    for i, (tt, vv) in enumerate(fs.k):
        if tt == t and vv == v:
            return i
    fs.k.append((t, v))
    return len(fs.k) - 1


def kstr(fs, s):
    return kidx(fs, 4, s.encode("latin1") + b"\0")


def knum(fs, x):
    return kidx(fs, 3, struct.pack("<f", x))


def emit(fs, ins):
    fs.code.append(ins & 0xFFFFFFFF)
    fs.lines.append(fs.line)
    return len(fs.code) - 1


def reserve(fs, n=1):
    fs.freereg += n
    if fs.freereg > fs.maxstack:
        fs.maxstack = fs.freereg
        if fs.maxstack > 250:
            raise LuaSyntaxError("function needs too many registers")
    return fs.freereg - n


def emit_jmp(fs):
    return emit(fs, encbx(OP["JMP"], 0, BIAS))        # offset 0 for now, patched later


def patch(fs, pc, target):
    fs.code[pc] = encbx(OP["JMP"], 0, target - (pc + 1) + BIAS)


def patch_list(fs, lst, target):
    for pc in lst:
        patch(fs, pc, target)


def here(fs):
    return len(fs.code)


def find_local(fs, name):
    for n, r in reversed(fs.actvars):
        if n == name:
            return r
    return None


def is_const(e):
    return e[0] in ("num", "str", "nil")


def const_rk(fs, e):
    if e[0] == "num": i = knum(fs, e[1])
    elif e[0] == "str": i = kstr(fs, e[1])
    else: i = kidx(fs, 0, None)
    return 250 + i if i <= MAXRK_K else None


def ex_rk(fs, e):
    """Operand as RK: constant -> K, local -> its register, else evaluated into a fresh temp."""
    if is_const(e):
        r = const_rk(fs, e)
        if r is not None:
            return r
    if e[0] == "paren":
        return ex_rk(fs, e[1])
    if e[0] == "name":
        r = find_local(fs, e[1])
        if r is not None:
            return r
    return ex(fs, e)


def ex_reg(fs, e):
    """Value in a REGISTER (locals stay where they are, everything else goes to a fresh temp)."""
    if e[0] == "paren":
        return ex_reg(fs, e[1])
    if e[0] == "name":
        r = find_local(fs, e[1])
        if r is not None:
            return r
    return ex(fs, e)


def ex(fs, e, dst=None, tmp=False):
    """Evaluate e into register dst (a fresh temp when dst is None); returns the register.
    tmp=True: dst is the top-most temp and ours to clobber (it was reserved by an enclosing ex)."""
    fresh = dst is None or tmp
    if dst is None:
        dst = reserve(fs)
    ek = e[0]
    if ek == "paren":
        return ex(fs, e[1], dst)
    if ek == "nil":
        emit(fs, enc(OP["LOADNIL"], dst, dst))
    elif ek in ("true", "false"):
        emit(fs, enc(OP["LOADBOOL"], dst, 1 if ek == "true" else 0, 0))
    elif ek == "num":
        emit(fs, encbx(OP["LOADK"], dst, knum(fs, e[1])))
    elif ek == "str":
        emit(fs, encbx(OP["LOADK"], dst, kstr(fs, e[1])))
    elif ek == "name":
        r = find_local(fs, e[1])
        if r is not None:
            if r != dst:
                emit(fs, enc(OP["MOVE"], dst, r))
        else:
            enclosing = fs.parent
            while enclosing is not None:
                if find_local(enclosing, e[1]) is not None:
                    raise LuaSyntaxError("line %d: upvalue %r is not supported" % (e[2], e[1]))
                enclosing = enclosing.parent
            emit(fs, encbx(OP["GETGLOBAL"], dst, kstr(fs, e[1])))
    elif ek == "index":
        save = fs.freereg
        base_local = e[1][0] == "name" and find_local(fs, e[1][1]) is not None
        if fresh and not base_local and e[1][0] != "paren":
            t = ex(fs, e[1], dst, tmp=True)     # a fresh temp may hold the table itself (as luac does)
        else:
            t = ex_reg(fs, e[1])
        key = ex_rk(fs, e[2])
        emit(fs, enc(OP["GETTABLE"], dst, t, key))
        fs.freereg = max(save, dst + 1)
    elif ek in ("call", "method"):
        if fresh:                              # the new temp is the top register: call right there
            fs.freereg = dst
            call(fs, e, 2)
        else:
            save = fs.freereg
            base = call(fs, e, 2)
            emit(fs, enc(OP["MOVE"], dst, base))
            fs.freereg = save
    elif ek == "table":
        tmp = dst
        nh = len(e[1])
        emit(fs, enc(OP["NEWTABLE"], tmp, 0, (nh.bit_length() if nh > 0 else 0)))   # floor(log2 nh)+1 as luac
        save = fs.freereg
        for key, val in e[1]:
            k = ex_rk(fs, key)
            v = ex_rk(fs, val)
            emit(fs, enc(OP["SETTABLE"], tmp, k, v))
            fs.freereg = save
    elif ek == "function":
        emit(fs, encbx(OP["CLOSURE"], dst, new_proto(fs, e)))
    elif ek == "unop":
        save = fs.freereg
        if e[1] == "-":
            r = ex_reg(fs, e[2])
            emit(fs, enc(OP["UNM"], dst, r))
        else:
            r = ex_reg(fs, e[2])
            emit(fs, enc(OP["NOT"], dst, r))
        fs.freereg = max(save, dst + 1)
    elif ek == "binop":
        op = e[1]
        if op in ("+", "-", "*", "/", "^"):
            save = fs.freereg
            b = ex_rk(fs, e[2])
            c = ex_rk(fs, e[3])
            emit(fs, enc(OP[{"+": "ADD", "-": "SUB", "*": "MUL", "/": "DIV", "^": "POW"}[op]], dst, b, c))
            fs.freereg = max(save, dst + 1)
        elif op == "..":
            # collect the right-associative chain into consecutive registers
            parts = []
            x = e
            while x[0] == "binop" and x[1] == "..":
                parts.append(x[2])
                x = x[3]
            parts.append(x)
            save = fs.freereg
            base = fs.freereg
            for p in parts:
                ex(fs, p)
            emit(fs, enc(OP["CONCAT"], dst, base, base + len(parts) - 1))
            fs.freereg = max(save, dst + 1)
        elif op in ("and", "or"):
            # value context: R(dst) = a; TEST dst dst (jump if short circuit); R(dst) = b
            ex(fs, e[2], dst)
            emit(fs, enc(OP["TEST"], dst, dst, 0 if op == "and" else 1))
            j = emit_jmp(fs)
            ex(fs, e[3], dst)
            patch(fs, j, here(fs))
        else:
            jf = cond_false_jump(fs, e)
            emit(fs, enc(OP["LOADBOOL"], dst, 1, 1))
            patch_list(fs, jf, here(fs))
            emit(fs, enc(OP["LOADBOOL"], dst, 0, 0))
    else:
        raise LuaSyntaxError("cannot compile %r" % (ek,))
    return dst


def call(fs, e, nresults_plus1, multi_args=True):
    """Compile a call / method call at the top of the stack; returns base register. C = nresults+1 (0: open)."""
    base = fs.freereg
    if e[0] == "method":
        obj = e[1]
        r = find_local(fs, obj[1]) if obj[0] == "name" else None
        if r is None:
            ex(fs, obj)                  # lands in base
            r = base
            reserve(fs, 1)               # base+1 = self
        else:
            reserve(fs, 2)
        emit(fs, enc(OP["SELF"], base, r, ex_name_rk(fs, e[2])))
        args, line = e[3], e[4]
        nfixed = 1
    else:
        ex(fs, e[1])                     # the function lands in base
        args, line = e[2], e[3]
        nfixed = 0
    open_args = False
    for i, a in enumerate(args):
        last = i == len(args) - 1
        if last and a[0] in ("call", "method") and multi_args:
            call(fs, a, 0)           # open results: C = 0
            open_args = True
        else:
            ex(fs, a)
    fs.line = line
    b = 0 if open_args else (fs.freereg - base)
    emit(fs, enc(OP["CALL"], base, b, nresults_plus1))
    fs.freereg = base + (1 if nresults_plus1 == 2 else 0)
    return base


def ex_name_rk(fs, name):
    i = kstr(fs, name)
    if i > MAXRK_K:
        raise LuaSyntaxError("too many constants for a method name RK")
    return 250 + i


CMP = {"==": ("EQ", False), "~=": ("EQ", True), "<": ("LT", False), ">": ("LT", True), "<=": ("LE", False), ">=": ("LE", True)}


def cmp_jump(fs, e, jump_when):
    """Emit a comparison + JMP that jumps when (e == jump_when). Returns [pc of the JMP]."""
    op = e[1]
    name, swap = CMP[op]
    a, b = e[2], e[3]
    if op in (">", ">="):
        a, b = b, a
    save = fs.freereg
    rb = ex_rk(fs, a)
    rc = ex_rk(fs, b)
    fs.freereg = save
    # the JMP after EQ/LT/LE executes when (cmp result) == A
    truth = jump_when if op != "~=" else (not jump_when)
    emit(fs, enc(OP[name], 1 if truth else 0, rb, rc))
    return [emit_jmp(fs)]


def cond_false_jump(fs, e):
    """Code that falls through when e is true; returns the JMPs taken when it is false."""
    ek = e[0]
    if ek == "paren":
        return cond_false_jump(fs, e[1])
    if ek in ("true", "num", "str"):
        return []
    if ek in ("false", "nil"):
        return [emit_jmp(fs)]
    if ek == "binop" and e[1] in CMP:
        return cmp_jump(fs, e, False)
    if ek == "binop" and e[1] == "and":
        return cond_false_jump(fs, e[2]) + cond_false_jump(fs, e[3])
    if ek == "binop" and e[1] == "or":
        jt = cond_true_jump(fs, e[2])
        jf = cond_false_jump(fs, e[3])
        patch_list(fs, jt, here(fs))
        return jf
    if ek == "unop" and e[1] == "not":
        return cond_true_jump(fs, e[2])
    save = fs.freereg
    r = ex_reg(fs, e)
    fs.freereg = save
    emit(fs, enc(OP["TEST"], r, r, 0))
    return [emit_jmp(fs)]


def cond_true_jump(fs, e):
    """Code that falls through when e is false; returns the JMPs taken when it is true."""
    ek = e[0]
    if ek == "paren":
        return cond_true_jump(fs, e[1])
    if ek in ("true", "num", "str"):
        return [emit_jmp(fs)]
    if ek in ("false", "nil"):
        return []
    if ek == "binop" and e[1] in CMP:
        return cmp_jump(fs, e, True)
    if ek == "binop" and e[1] == "or":
        return cond_true_jump(fs, e[2]) + cond_true_jump(fs, e[3])
    if ek == "binop" and e[1] == "and":
        jf = cond_false_jump(fs, e[2])
        jt = cond_true_jump(fs, e[3])
        patch_list(fs, jf, here(fs))
        return jt
    if ek == "unop" and e[1] == "not":
        return cond_false_jump(fs, e[2])
    save = fs.freereg
    r = ex_reg(fs, e)
    fs.freereg = save
    emit(fs, enc(OP["TEST"], r, r, 1))
    return [emit_jmp(fs)]


def declare(fs, name, reg):
    fs.actvars.append((name, reg))
    fs.locs.append([name, here(fs), None])


def close_scope(fs, nvars_before, locs_before=None):
    """Leave a block: end of the locals declared since, free their registers."""
    while len(fs.actvars) > nvars_before:
        name, reg = fs.actvars.pop()
        for rec in reversed(fs.locs):
            if rec[0] == name and rec[2] is None:
                rec[2] = here(fs) - 1
                break
    fs.freereg = fs.actvars[-1][1] + 1 if fs.actvars else 0


def block(fs, stmts):
    n = len(fs.actvars)
    for s in stmts:
        stat(fs, s)
        if s[0] == "return":
            break
    close_scope(fs, n)


def stat(fs, s):
    k = s[0]
    fs.line = s[1]
    if k == "local":
        names, exps = s[2], s[3]
        base = fs.freereg
        for i, nm in enumerate(names):
            if i < len(exps):
                if i == len(exps) - 1 and exps[i][0] in ("call", "method") and len(names) > len(exps):
                    # local a, b, c = f(): the last call supplies all the missing values (CALL with C = n + 1)
                    nwant = len(names) - i
                    call(fs, exps[i], nwant + 1)
                    reserve(fs, nwant)
                    break
                ex(fs, exps[i])
            else:
                r = reserve(fs)
                emit(fs, enc(OP["LOADNIL"], r, r))
        for i, nm in enumerate(names):
            declare(fs, nm, base + i)
        fs.freereg = base + len(names)
    elif k == "assign":
        target, e = s[2], s[3]
        save = fs.freereg
        if target[0] == "name":
            r = find_local(fs, target[1])
            if r is not None:
                if e[0] in ("table", "call", "method", "binop", "unop") and e[0] != "binop":
                    t = ex(fs, e)
                    emit(fs, enc(OP["MOVE"], r, t))
                elif e[0] == "binop" and e[1] in ("and", "or", "..") or e[0] == "binop" and e[1] in CMP:
                    t = ex(fs, e)
                    emit(fs, enc(OP["MOVE"], r, t))
                else:
                    ex(fs, e, r)
            else:
                v = ex_reg(fs, e)
                emit(fs, encbx(OP["SETGLOBAL"], v, kstr(fs, target[1])))
        else:  # index
            t = ex_reg(fs, target[1])
            key = ex_rk(fs, target[2])
            v = ex_rk(fs, e)
            emit(fs, enc(OP["SETTABLE"], t, key, v))
        fs.freereg = save
    elif k == "callstat":
        save = fs.freereg
        call(fs, s[2], 1)
        fs.freereg = save
    elif k == "if":
        arms, els = s[2], s[3]
        ends = []
        for i, (cond, body) in enumerate(arms):
            jf = cond_false_jump(fs, cond)
            block(fs, body)
            if i < len(arms) - 1 or els is not None:
                ends.append(emit_jmp(fs))
            patch_list(fs, jf, here(fs))
        if els is not None:
            block(fs, els)
        patch_list(fs, ends, here(fs))
    elif k == "while":
        top = here(fs)
        jf = cond_false_jump(fs, s[2])
        saveb = fs.breaks
        fs.breaks = []
        block(fs, s[3])
        j = emit_jmp(fs)
        patch(fs, j, top)
        patch_list(fs, jf, here(fs))
        patch_list(fs, fs.breaks, here(fs))
        fs.breaks = saveb
    elif k == "do":
        block(fs, s[2])
    elif k == "break":
        fs.breaks.append(emit_jmp(fs))
    elif k == "for":
        name, a, b, c, body = s[2], s[3], s[4], s[5], s[6]
        n = len(fs.actvars)
        base = fs.freereg
        ex(fs, a)
        ex(fs, b)
        if c is not None:
            ex(fs, c)
        else:
            r = reserve(fs)
            emit(fs, encbx(OP["LOADK"], r, knum(fs, 1.0)))
        emit(fs, enc(OP["SUB"], base, base, base + 2))
        prep = emit_jmp(fs)
        declare(fs, name, base)
        declare(fs, "(for limit)", base + 1)
        declare(fs, "(for step)", base + 2)
        fs.freereg = base + 3
        saveb = fs.breaks
        fs.breaks = []
        start = here(fs)
        block(fs, body)
        patch(fs, prep, here(fs))
        fs.line = s[1]
        emit(fs, encbx(OP["FORLOOP"], base, start - (here(fs) + 1) + BIAS))
        patch_list(fs, fs.breaks, here(fs))
        fs.breaks = saveb
        close_scope(fs, n)
    elif k == "return":
        exps = s[2]
        base = fs.freereg
        if not exps:
            emit(fs, enc(OP["RETURN"], 0, 1))
        else:
            open_ret = False
            for i, e in enumerate(exps):
                if i == len(exps) - 1 and e[0] in ("call", "method"):
                    call(fs, e, 0)
                    open_ret = True
                else:
                    ex(fs, e)
            emit(fs, enc(OP["RETURN"], base, 0 if open_ret else len(exps) + 1))
        fs.freereg = base
    else:
        raise LuaSyntaxError("cannot compile statement %r" % (k,))


def new_proto(parent, fnode):
    _, params, body, line, endline = fnode
    fs = FS(parent)
    fs.line = line
    for i, p in enumerate(params):
        fs.actvars.append((p, i))
        fs.locs.append([p, 0, None])
    fs.freereg = len(params)
    fs.maxstack = max(2, fs.freereg)
    n = len(fs.actvars)
    for s in body:
        stat(fs, s)
        if s[0] == "return":
            break
    fs.line = endline
    emit(fs, enc(OP["RETURN"], 0, 1))
    for rec in fs.locs:
        if rec[2] is None:
            rec[2] = here(fs) - 1
    p = L.Proto()
    p.source, p.linedef = None, line
    p.nups, p.nparams, p.vararg, p.maxstack = 0, len(params), 0, fs.maxstack
    p.lines = fs.lines
    p.locs = [(r[0].encode() + b"\0", r[1], r[2]) for r in fs.locs]
    p.upvals = []
    p.k = fs.k
    p.p = fs.protos
    p.code = fs.code
    parent.protos.append(p)
    return len(parent.protos) - 1


def compile_chunk(src, name="chunk"):
    """-> Proto of the main function (the chunk), source = name."""
    ast = Parser(src).block()
    fs = FS(None)
    fs.maxstack = 2
    n = 0
    for s in ast:
        stat(fs, s)
        if s[0] == "return":
            break
    emit(fs, enc(OP["RETURN"], 0, 1))
    p = L.Proto()
    p.source, p.linedef = name.encode() + b"\0", 0
    p.nups, p.nparams, p.vararg, p.maxstack = 0, 0, 0, fs.maxstack
    p.lines, p.locs, p.upvals = fs.lines, [(r[0].encode() + b"\0", r[1], r[2]) for r in fs.locs], []
    p.k, p.p, p.code = fs.k, fs.protos, fs.code
    return p


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    src = open(argv[0], "r", encoding="latin1").read()
    name = argv[2] if len(argv) > 2 else "chunk"
    p = compile_chunk(src, name)
    data = L.dump(p)
    open(argv[1], "wb").write(data)
    print("%s -> %s (%d bytes)" % (argv[0], argv[1], len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
