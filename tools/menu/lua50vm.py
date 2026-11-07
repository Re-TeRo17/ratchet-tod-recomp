#!/usr/bin/env python3
"""A tiny Lua 5.0 virtual machine (stdlib only) to RUN the Anark UI scripts offline.

Used by tools/menu/test_opt_handler.py to check, without starting the game, that a script compiled by
lua50c.py behaves like the stock one (same calls, same values) and that the scrolling Options page works.
Natives are plain Python callables: f(args:list) -> list of results. Numbers are Python floats.
Only the instructions the page scripts use are implemented (all of Lua 5.0's, minus upvalue closures
that capture live registers: CLOSURE builds closures without upvalues).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lua50_patch as L  # noqa: E402

OPN = L.OPS


class LuaTable:
    __slots__ = ("h", "name", "on_set")

    def __init__(self, name=None, on_set=None):
        self.h = {}
        self.name = name
        self.on_set = on_set

    def get(self, k):
        return self.h.get(nk(k))

    def set(self, k, v):
        k = nk(k)
        if self.on_set:
            self.on_set(self, k, v)
        if v is None:
            self.h.pop(k, None)
        else:
            self.h[k] = v

    def getn(self):
        n = 0
        while (n + 1.0) in self.h:
            n += 1
        return n

    def __repr__(self):
        return "<table %s>" % (self.name or id(self))


def nk(k):
    if isinstance(k, bool):
        return ("b", k)
    if isinstance(k, int):
        return float(k)
    return k


class LuaFunc:
    def __init__(self, proto):
        self.proto = proto


class LuaError(Exception):
    pass


def truthy(v):
    return v is not None and v is not False


def leq(a, b):
    if a is None or b is None:
        return a is None and b is None
    if isinstance(a, bool) or isinstance(b, bool):
        return isinstance(a, bool) and isinstance(b, bool) and a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return a == b
    if isinstance(a, str) and isinstance(b, str):
        return a == b
    return a is b


def tonum(v):
    if isinstance(v, (int, float)) and not isinstance(v, bool):
        return float(v)
    if isinstance(v, str):
        try:
            return float(v)
        except ValueError:
            pass
    raise LuaError("attempt to perform arithmetic on %r" % (v,))


def tostr_(v):
    if v is None: return "nil"
    if v is True: return "true"
    if v is False: return "false"
    if isinstance(v, float):
        return "%d" % v if v == int(v) else "%.14g" % v
    return str(v)


class VM:
    def __init__(self):
        self.g = LuaTable("_G")
        self.steps = 0
        self.max_steps = 2_000_000

    def kval(self, p, i):
        t, v = p.k[i]
        if t == 3:
            import struct
            return struct.unpack("<f", v)[0]
        if t == 4:
            return v[:-1].decode("latin1")
        return None

    def call(self, f, args):
        if isinstance(f, LuaFunc):
            return self.run(f.proto, args)
        if callable(f):
            r = f(list(args))
            return list(r) if r else []
        raise LuaError("attempt to call %r" % (f,))

    def run(self, p, args):
        R = [None] * (p.maxstack + 8)
        for i in range(p.nparams):
            R[i] = args[i] if i < len(args) else None
        code = p.code
        pc = 0
        top = 0
        K = [self.kval(p, i) for i in range(len(p.k))]

        def RK(x):
            return K[x - 250] if x >= 250 else R[x]

        while True:
            self.steps += 1
            if self.steps > self.max_steps:
                raise LuaError("step limit (infinite loop?)")
            ins = code[pc]
            pc += 1
            op, A, B, C = L.dec(ins)
            Bx = L.decbx(ins)
            n = OPN[op]
            if n == "MOVE": R[A] = R[B]
            elif n == "LOADK": R[A] = K[Bx]
            elif n == "LOADBOOL":
                R[A] = bool(B)
                if C: pc += 1
            elif n == "LOADNIL":
                for i in range(A, B + 1): R[i] = None
            elif n == "GETGLOBAL": R[A] = self.g.get(K[Bx])
            elif n == "SETGLOBAL": self.g.set(K[Bx], R[A])
            elif n == "GETTABLE":
                t = R[B]
                if not isinstance(t, LuaTable):
                    raise LuaError("index a %r value (%s)" % (t, K[C - 250] if C >= 250 else ""))
                R[A] = t.get(RK(C))
            elif n == "SETTABLE":
                t = R[A]
                if not isinstance(t, LuaTable):
                    raise LuaError("index a %r value" % (t,))
                t.set(RK(B), RK(C))
            elif n == "NEWTABLE": R[A] = LuaTable()
            elif n == "SELF":
                t = R[B]
                if not isinstance(t, LuaTable):
                    raise LuaError("SELF on %r (%s)" % (t, K[C - 250] if C >= 250 else ""))
                R[A + 1] = t
                R[A] = t.get(RK(C))
            elif n in ("ADD", "SUB", "MUL", "DIV", "POW"):
                x, y = tonum(RK(B)), tonum(RK(C))
                R[A] = x + y if n == "ADD" else x - y if n == "SUB" else x * y if n == "MUL" else x / y if n == "DIV" else x ** y
            elif n == "UNM": R[A] = -tonum(R[B])
            elif n == "NOT": R[A] = not truthy(R[B])
            elif n == "CONCAT": R[A] = "".join(tostr_(R[i]) for i in range(B, C + 1))
            elif n == "JMP": pc += Bx - 131071
            elif n == "EQ":
                if leq(RK(B), RK(C)) != bool(A): pc += 1
                else: pc += L.decbx(code[pc]) - 131071 + 1
            elif n in ("LT", "LE"):
                x, y = RK(B), RK(C)
                if isinstance(x, (int, float)) and isinstance(y, (int, float)) or (isinstance(x, str) and isinstance(y, str)):
                    r = x < y if n == "LT" else x <= y
                else:
                    raise LuaError("attempt to compare %r with %r" % (x, y))
                if r != bool(A): pc += 1
                else: pc += L.decbx(code[pc]) - 131071 + 1
            elif n == "TEST":
                if (not truthy(R[B])) != bool(C):
                    R[A] = R[B]
                    pc += L.decbx(code[pc]) - 131071 + 1
                else:
                    pc += 1
            elif n in ("CALL", "TAILCALL"):
                nargs = (top - A - 1) if B == 0 else B - 1
                f = R[A]
                res = self.call(f, R[A + 1:A + 1 + nargs])
                if n == "TAILCALL":
                    return res
                if C == 0:
                    for i, v in enumerate(res):
                        if A + i >= len(R): R.extend([None] * 16)
                        R[A + i] = v
                    top = A + len(res)
                else:
                    for i in range(C - 1):
                        R[A + i] = res[i] if i < len(res) else None
            elif n == "RETURN":
                cnt = (top - A) if B == 0 else B - 1
                return R[A:A + cnt]
            elif n == "FORLOOP":
                step = R[A + 2]
                R[A] = R[A] + step
                if (R[A] <= R[A + 1]) if step > 0 else (R[A] >= R[A + 1]):
                    pc += Bx - 131071
            elif n == "CLOSURE":
                R[A] = LuaFunc(p.p[Bx])
            else:
                raise LuaError("opcode %s not implemented" % n)


def std_globals(vm, log=None):
    """table.getn/insert, tostring, type -- the parts of the Lua 5.0 library the pages use."""
    def getn(a): return [float(a[0].getn())]

    def insert(a):
        t = a[0]
        t.set(float(t.getn() + 1), a[1])
        return []

    tbl = LuaTable("table")
    tbl.set("getn", getn)
    tbl.set("insert", insert)
    vm.g.set("table", tbl)
    vm.g.set("tostring", lambda a: [tostr_(a[0])])

    def type_(a):
        v = a[0]
        return ["nil" if v is None else "boolean" if isinstance(v, bool) else "number" if isinstance(v, (int, float))
                else "string" if isinstance(v, str) else "table" if isinstance(v, LuaTable) else "function"]
    vm.g.set("type", type_)

    def pcall(a):
        try:
            return [True] + list(vm.call(a[0], a[1:]))
        except LuaError as ex:
            return [False, str(ex)]
    vm.g.set("pcall", pcall)
    return vm
