#!/usr/bin/env python3
"""Fake Anark scene for tools/menu/lua50vm.py: enough of the engine's API for the Options page script
(optHandler) to run offline. Every observable effect (node property writes, calls into the engine,
native get/set) is appended to env.trace so two scripts can be compared step by step."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lua50vm import VM, LuaTable, LuaFunc, LuaError, std_globals, tostr_  # noqa: E402

NATIVES_STOCK = ["get_sound_vol", "set_sound_vol", "get_music_vol", "set_music_vol", "get_voice_vol", "set_voice_vol",
                 "get_camera_speed", "set_camera_speed", "is_help_text_on", "set_help_text_on", "is_subtitles_on",
                 "set_subtitles_on", "is_quick_select_pause_on", "set_quick_select_pause_on", "is_look_x_inverted",
                 "set_look_x_inverted", "is_look_y_inverted", "set_look_y_inverted", "is_x_inverted", "set_x_inverted",
                 "is_y_inverted", "set_y_inverted", "get_six_axis_controls", "set_six_axis_controls",
                 "get_surround_on", "set_surround_on"]


class Env:
    def __init__(self, nslots=6, state=None):
        self.vm = VM()
        std_globals(self.vm)
        self.trace = []
        self.nslots = nslots
        self.state = {"get_sound_vol": 0.5, "get_music_vol": 0.7, "get_voice_vol": 1.0, "get_camera_speed": 0.5,
                      "is_help_text_on": True, "is_subtitles_on": False, "is_quick_select_pause_on": False,
                      "is_look_x_inverted": False, "is_look_y_inverted": False, "is_x_inverted": False,
                      "is_y_inverted": False, "get_six_axis_controls": False, "get_surround_on": True}
        if state:
            self.state.update(state)
        g = self.vm.g
        T = self.trace

        def logset(t, k, v):
            if isinstance(v, (LuaTable, LuaFunc)) or callable(v):
                v = "<obj>"
            T.append(("set", t.name, k, v))

        self.logset = logset

        def node(name):
            n = LuaTable(name, logset)
            return n

        self.node = node

        def mkvec(name, **kw):
            t = node(name)
            t.h.update({k: float(v) for k, v in kw.items()})
            t.h["set"] = lambda a, n=name: (T.append(("call", n + ":set", tuple(x for x in a[1:]))), [])[1]
            t.h["setColor"] = lambda a, n=name: (T.append(("call", n + ":setColor", "color")), [])[1]
            return t

        def mkrow(i):
            r = node("row%d" % i)
            r.h["optLabel"] = node("row%d.optLabel" % i)
            r.h["optLabel"].h["scale"] = mkvec("row%d.optLabel.scale" % i)
            r.h["optLabel"].h["textColor"] = mkvec("row%d.optLabel.textColor" % i)
            r.h["optLabel"].h["opacity"] = 0.0
            r.h["optValue"] = node("row%d.optValue" % i)
            r.h["optValue"].h["scale"] = mkvec("row%d.optValue.scale" % i)
            r.h["optValue"].h["textColor"] = mkvec("row%d.optValue.textColor" % i)
            r.h["optValue"].h["opacity"] = 0.0
            bar = node("row%d.bar" % i)
            bar.h["scale"] = mkvec("row%d.bar.scale" % i)
            val = node("row%d.bar.value" % i)
            val.h["scale"] = mkvec("row%d.bar.value.scale" % i, x=1)
            val.h["position"] = mkvec("row%d.bar.value.position" % i, x=0)
            bar.h["value"] = val
            r.h["bar"] = bar
            lt = node("row%d.lgt" % i)
            lt.h["_41"] = 100.0 + 0.0
            lt.h["_42"] = 50.0 * i
            r.h["lastGlobalTransform"] = lt
            return r

        self.rows = [mkrow(i + 1) for i in range(nslots)]
        scene_self = LuaTable("self", None)
        self.scene = scene_self
        g.set("self", scene_self)

        def getref(a):
            name = a[1]
            if name == "optGroup":
                grp = node("optGroup")
                grp.h["getNumChildren"] = lambda a2: [float(nslots)]
                grp.h["getChildByIndex"] = lambda a2: [self.rows[int(a2[1]) - 1]]
                return [grp]
            if name == "highlight":
                h = node("highlight")
                h.h["position"] = mkvec("highlight.position", x=0, y=0, z=7)
                return [h]
            if name == "desc":
                return [node("desc")]
            if name == "barControl":
                b = node("barControl")
                v = node("barControl.value")
                v.h["scale"] = mkvec("barControl.value.scale", x=2)
                v.h["position"] = mkvec("barControl.value.position", x=3)
                b.h["value"] = v
                return [b]
            return [node(name)]
        scene_self.h["getObjectReference"] = getref
        scene_self.h["_useOnActivateDeactivate"] = lambda a: []
        scene_self.h["fireEvent"] = lambda a: (T.append(("call", "fireEvent")), [])[1]
        scene = LuaTable("scene")
        scene.h["goToSlide"] = lambda a: (T.append(("call", "goToSlide", a[1])), [])[1]
        g.set("scene", scene)
        ev = LuaTable("Event")
        ev.h["new"] = lambda a: [a[1]]
        g.set("Event", ev)
        col = LuaTable("Color")
        col.h["new"] = lambda a: [("color",) + tuple(a[1:])] and [LuaTable("color%s" % (tuple(a[1:]),))]
        g.set("Color", col)

        def setloc(a):
            tgt = a[0]
            T.append(("text", tgt.name if isinstance(tgt, LuaTable) else str(tgt), a[1] if not isinstance(a[1], float) else tostr_(a[1])))
            return []
        g.set("SetLocalizedText", setloc)
        g.set("EffectEvent", lambda a: (T.append(("effect", a[0], a[1])), [])[1])
        for n in ("EVENT_SUCCESS", "EVENT_FAILED", "EVENT_GAINED_FOCUS", "CLASS_BUTTON", "CLASS_HIGHLIGHT_BOX"):
            g.set(n, n)
        for n in NATIVES_STOCK:
            self.install_native(n)

    def install_native(self, name):
        T = self.trace
        st = self.state
        if name.startswith("get_") or name.startswith("is_"):
            self.vm.g.set(name, lambda a, n=name: [st.get(n)])
        else:
            gk = "get_" + name[4:]
            gk = gk if gk in st else "is_" + name[4:]

            def setter(a, n=name, gk=gk):
                T.append(("native", n, a[0]))
                st[gk] = a[0] if not isinstance(a[0], float) else max(0.0, min(1.0, round(a[0], 4)))
                return []
            self.vm.g.set(name, setter)

    def load(self, proto):
        self.vm.run(proto, [])

    def method(self, name, *args):
        f = self.scene.get(name)
        return self.vm.call(f, [self.scene] + list(args))
