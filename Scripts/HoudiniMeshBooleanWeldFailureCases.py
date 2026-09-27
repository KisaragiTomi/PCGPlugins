"""Build 7 demo objects in /obj showing how the MeshBoolean weld flow can fail.

Companion to Docs/meshboolean-weld-failure-cases.md. Run inside Houdini (tested on 20.0.724):
    exec(open(r"<path>/HoudiniMeshBooleanWeldFailureCases.py", encoding="utf-8").read())

Only touches nodes it creates itself: geo objects named weld_case*, sticky notes named weld_note*.
Rerunning deletes and rebuilds those; every other node is left where it is.
"""
import hou
import json

OBJ = hou.node("/obj")
FONT = "C:/Windows/Fonts/msyh.ttc"

# ---------------------------------------------------------------------------
# Python SOP bodies
# ---------------------------------------------------------------------------

WELD_CODE = r'''
# 逐位复刻插件 GPU 焊接（CSGpuTriangleUtilities.ush OutputWeldHashCS / OutputWeldResolveCS）
# + CPU 后处理 Stage 13（删退化、去重）与 Stage 14（按源法线纠正绕序）。
import math, json
node = hou.pwd()
geo = node.geometry()
d = node.evalParm("weld_distance")
bucket_mult = node.evalParm("bucket_mult")
stage13 = node.evalParm("remove_degenerate_duplicate")
stage14 = node.evalParm("orientation_fix")
mode = node.evalParm("mode")                 # 0 = 插件现状（哈希表，可多张盐），1 = 按真实格子分组，2 = 传递合并（并查集）
salt_passes = max(1, node.evalParm("salt_passes"))
M32 = 0xffffffff

def whash(cx, cy, cz):
    h = ((cx & M32) * 0x8da6b343) & M32
    h ^= ((cy & M32) * 0xd8163841) & M32
    h ^= ((cz & M32) * 0xcb1ab31f) & M32
    h ^= h >> 16
    h = (h * 0x7feb352d) & M32
    h ^= h >> 15
    return h

def whash_salted(cx, cy, cz, salt):
    h = whash(cx, cy, cz)
    if salt == 0:
        return h                            # 第 0 张表与插件完全相同
    h ^= (salt * 0x9e3779b9) & M32
    h = (h * 0x85ebca6b) & M32
    h ^= h >> 13
    return h

tris = [p for p in geo.prims() if isinstance(p, hou.Polygon) and len(p.vertices()) == 3]
pos = []
src_houdini_n = []
for pr in tris:
    for v in pr.vertices():
        pos.append(hou.Vector3(v.point().position()))
    src_houdini_n.append(hou.Vector3(pr.normal()))
N = len(pos)
T = len(tris)
raw_n = [(pos[3*t+1] - pos[3*t]).cross(pos[3*t+2] - pos[3*t]) for t in range(T)]

org = geo.boundingBox().minvec()
inv = 1.0 / d
desired = max(1024, min(int(T * bucket_mult), 1 << 24))   # 插件：clamp(源三角 × 6, 1024, 2^24)
bc = 1
while bc < desired:
    bc <<= 1
mask = bc - 1
limit = d * d + max(1e-12, d * d * 1e-5)
cells = []
for i in range(N):
    p = pos[i]
    cells.append((int(math.floor((p[0] - org[0]) * inv)), int(math.floor((p[1] - org[1]) * inv)), int(math.floor((p[2] - org[2]) * inv))))
NEIGHBORS = [(dx, dy, dz) for dz in (-1, 0, 1) for dy in (-1, 0, 1) for dx in (-1, 0, 1)]
rep = list(range(N))
shadowed = 0
if mode == 0:
    for salt in range(salt_passes):
        buckets = [M32] * bc
        for i in range(N):
            b = whash_salted(cells[i][0], cells[i][1], cells[i][2], salt) & mask
            if i < buckets[b]:
                buckets[b] = i              # 每个桶只记最小角点序号
        for i in range(N):
            best = rep[i]                   # 多张表：在上一张表的结果上继续取最小
            c = cells[i]
            p = pos[i]
            own = buckets[whash_salted(c[0], c[1], c[2], salt) & mask]
            if salt == 0 and own < N and cells[own] != c:
                shadowed += 1               # 自己格子的桶被别的格子占了
            for dx, dy, dz in NEIGHBORS:
                cand = buckets[whash_salted(c[0] + dx, c[1] + dy, c[2] + dz, salt) & mask]
                if cand >= N or cand >= best:
                    continue
                if (pos[cand] - p).lengthSquared() <= limit:
                    best = cand
            rep[i] = best                   # 单趟，不做传递合并
elif mode == 1:
    cell_min = {}
    for i in range(N):
        if cells[i] not in cell_min:
            cell_min[cells[i]] = i          # 按真实格子记最小序号：没有哈希冲突
    for i in range(N):
        best = i
        c = cells[i]
        p = pos[i]
        for dx, dy, dz in NEIGHBORS:
            cand = cell_min.get((c[0] + dx, c[1] + dy, c[2] + dz), N)
            if cand < best and (pos[cand] - p).lengthSquared() <= limit:
                best = cand
        rep[i] = best
else:
    members = {}
    for i in range(N):
        members.setdefault(cells[i], []).append(i)
    parent = list(range(N))
    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for i in range(N):
        c = cells[i]
        for dx, dy, dz in NEIGHBORS:
            for j in members.get((c[0] + dx, c[1] + dy, c[2] + dz), ()):
                if j > i and (pos[j] - pos[i]).lengthSquared() <= limit:
                    ra, rb = find(i), find(j)
                    if ra != rb:
                        parent[max(ra, rb)] = min(ra, rb)
    rep = [find(i) for i in range(N)]       # 连通分量里序号最小的点当代表

geo.clear()
for _t, _n, _d in ((hou.attribType.Prim, "srcN", (0.0, 0.0, 0.0)), (hou.attribType.Prim, "flipped", 0), (hou.attribType.Prim, "orient_fixed", 0), (hou.attribType.Point, "rep", -1)):
    if geo.findPrimAttrib(_n) is None and geo.findPointAttrib(_n) is None:
        geo.addAttrib(_t, _n, _d)
pt_of = {}

def pt_for(r):
    pt = pt_of.get(r)
    if pt is None:
        pt = geo.createPoint()
        pt.setPosition(pos[r])
        pt.setAttribValue("rep", r)
        pt_of[r] = pt
    return pt

seen = set()
removed_deg = removed_dup = n_flip = n_fix = 0
for t in range(T):
    r = [rep[3*t], rep[3*t+1], rep[3*t+2]]
    P0, P1, P2 = pos[r[0]], pos[r[1]], pos[r[2]]
    if stage13:
        if r[0] == r[1] or r[1] == r[2] or r[0] == r[2]:
            removed_deg += 1
            continue
        if (P1 - P0).cross(P2 - P0).lengthSquared() <= 1e-24:
            removed_deg += 1
            continue
        key = tuple(sorted(r))              # 插件去重不看朝向
        if key in seen:
            removed_dup += 1
            continue
        seen.add(key)
    flipped = (P1 - P0).cross(P2 - P0).dot(raw_n[t]) < 0.0
    order = [r[0], r[2], r[1]] if (stage14 and flipped) else r
    poly = geo.createPolygon()
    for x in order:
        poly.addVertex(pt_for(x))
    poly.setAttribValue("srcN", tuple(src_houdini_n[t]))
    poly.setAttribValue("flipped", 1 if flipped else 0)
    poly.setAttribValue("orient_fixed", 1 if (stage14 and flipped) else 0)
    n_flip += 1 if flipped else 0
    n_fix += 1 if (stage14 and flipped) else 0

stats = {"corners": N, "points": len(pt_of), "tris_in": T, "degenerate_removed": removed_deg,
         "duplicate_removed": removed_dup, "flipped": n_flip, "orientation_fixed": n_fix, "buckets": bc,
         "shadowed": shadowed}
if geo.findGlobalAttrib("weld_stats") is None:
    geo.addAttrib(hou.attribType.Global, "weld_stats", "")
geo.setGlobalAttribValue("weld_stats", json.dumps(stats))
'''

REPORT_CODE = r'''
# 拓扑体检：黄=开放边，红=一条边挂 3 片以上，紫=两片面同向经过同一条边，红球=蝴蝶点。
import json
from collections import defaultdict
node = hou.pwd()
geo = node.geometry()
radius = node.evalParm("radius")
color_mode = node.evalParm("color_mode")
crack_dist = node.evalParm("crack_dist")
mark_samedir = node.evalParm("mark_samedir")
polys = [p for p in geo.prims() if isinstance(p, hou.Polygon) and p.isClosed() and len(p.vertices()) >= 3]
if geo.findPrimAttrib("Cd") is None:
    geo.addAttrib(hou.attribType.Prim, "Cd", (0.75, 0.75, 0.75))
has_srcn = geo.findPrimAttrib("srcN") is not None
GREEN = (0.35, 0.78, 0.35)
RED = (0.9, 0.28, 0.28)
for pr in polys:
    n = pr.normal()
    if color_mode == 1 and has_srcn:
        pr.setAttribValue("Cd", GREEN if n.dot(hou.Vector3(pr.attribValue("srcN"))) >= 0 else RED)
    elif color_mode == 2:
        pr.setAttribValue("Cd", GREEN if n[1] >= 0 else RED)
    elif color_mode == 3:
        pr.setAttribValue("Cd", GREEN if n[2] >= 0 else RED)

edges = defaultdict(list)
for pr in polys:
    vs = [v.point().number() for v in pr.vertices()]
    for k in range(len(vs)):
        a, b = vs[k], vs[(k + 1) % len(vs)]
        if a == b:
            continue
        edges[(a, b) if a < b else (b, a)].append((pr.number(), a))
positions = {p.number(): hou.Vector3(p.position()) for p in geo.points()}
bb = geo.boundingBox()
size = bb.sizevec()
eps = 1e-5 * max(size.length(), 1e-9)

def is_border(key):
    pa, pb = positions[key[0]], positions[key[1]]
    for ax in range(3):
        if size[ax] <= eps:
            continue
        for v in (bb.minvec()[ax], bb.maxvec()[ax]):
            if abs(pa[ax] - v) < eps and abs(pb[ax] - v) < eps:
                return True
    return False

YELLOW = (1.0, 0.85, 0.1)
RED2 = (1.0, 0.08, 0.08)
MAGENTA = (0.85, 0.2, 1.0)
counts = {"boundary": 0, "nonmanifold": 0, "samedir": 0, "bowtie": 0}
marks = []
def seg_dist(pt, a, b):
    ab = b - a
    t = max(0.0, min(1.0, (pt - a).dot(ab) / max(ab.dot(ab), 1e-30)))
    return (a + ab * t - pt).length()
boundary = [key for key, uses in edges.items() if len(uses) == 1]
for key in boundary:
    if crack_dist > 0.0:
        mid = (positions[key[0]] + positions[key[1]]) * 0.5
        if not any(k2 != key and seg_dist(mid, positions[k2[0]], positions[k2[1]]) <= crack_dist for k2 in boundary):
            continue      # 附近没有另一条开放边：是网格外轮廓，不是裂缝
    marks.append((key, YELLOW)); counts["boundary"] += 1
for key, uses in edges.items():
    if len(uses) > 2:
        marks.append((key, RED2)); counts["nonmanifold"] += 1
    elif len(uses) == 2 and uses[0][1] == uses[1][1]:
        counts["samedir"] += 1
        if mark_samedir:
            marks.append((key, MAGENTA))

pt_polys = defaultdict(set)
for pr in polys:
    for v in pr.vertices():
        pt_polys[v.point().number()].add(pr.number())
pt_edges = defaultdict(list)
for key in edges:
    pt_edges[key[0]].append(key)
    pt_edges[key[1]].append(key)
bowties = []
for pt, ps in pt_polys.items():
    parent = {p: p for p in ps}
    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for key in pt_edges[pt]:
        users = [u[0] for u in edges[key]]
        for i in range(1, len(users)):
            ra, rb = find(users[0]), find(users[i])
            if ra != rb:
                parent[ra] = rb
    if len({find(p) for p in ps}) > 1:
        bowties.append(pt)
counts["bowtie"] = len(bowties)

grp = geo.findPrimGroup("report_marks") or geo.createPrimGroup("report_marks")

def add_prism(a, b, col):
    pa, pb = positions[a], positions[b]
    dv = pb - pa
    L = dv.length()
    if L < 1e-12:
        return
    dn = dv * (1.0 / L)
    up = hou.Vector3(0, 0, 1) if abs(dn[2]) < 0.9 else hou.Vector3(1, 0, 0)
    u = dn.cross(up).normalized() * radius
    w = dn.cross(u).normalized() * radius
    offs = [u + w, u - w, u * -1.0 - w, u * -1.0 + w]
    ra = [geo.createPoint() for _ in range(4)]
    rb = [geo.createPoint() for _ in range(4)]
    for i in range(4):
        ra[i].setPosition(pa + offs[i])
        rb[i].setPosition(pb + offs[i])
    for i in range(4):
        j = (i + 1) % 4
        poly = geo.createPolygon()
        for pt in (ra[i], ra[j], rb[j], rb[i]):
            poly.addVertex(pt)
        poly.setAttribValue("Cd", col)
        grp.add(poly)

def add_ball(p, col, r):
    c = positions[p]
    dirs = [(1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)]
    pts = []
    for dv in dirs:
        pt = geo.createPoint()
        pt.setPosition(c + hou.Vector3(dv) * r)
        pts.append(pt)
    for f in ((0, 2, 4), (2, 1, 4), (1, 3, 4), (3, 0, 4), (2, 0, 5), (1, 2, 5), (3, 1, 5), (0, 3, 5)):
        poly = geo.createPolygon()
        for i in f:
            poly.addVertex(pts[i])
        poly.setAttribValue("Cd", col)
        grp.add(poly)

for key, col in marks:
    add_prism(key[0], key[1], col)
for p in bowties:
    add_ball(p, RED2, radius * 6.0)

if geo.findPrimAttrib("N") is None:
    geo.addAttrib(hou.attribType.Prim, "N", (0.0, 0.0, 0.0))
for pr in geo.prims():
    if isinstance(pr, hou.Polygon):
        pr.setAttribValue("N", tuple(pr.normal()))
summary = (u"裂缝边 %d" if crack_dist > 0.0 else u"开放边 %d") % counts["boundary"] + u"  非流形边 %d  同向边 %d  蝴蝶点 %d" % (counts["nonmanifold"], counts["samedir"], counts["bowtie"])
ws = geo.findGlobalAttrib("weld_stats")
if ws is not None:
    s = json.loads(geo.attribValue("weld_stats"))
    summary += u"\n焊接：%d 角点 → %d 点，删退化 %d，去重 %d，翻面 %d，纠正 %d" % (
        s["corners"], s["points"], s["degenerate_removed"], s["duplicate_removed"], s["flipped"], s["orientation_fixed"])
for k, v in counts.items():
    if geo.findGlobalAttrib("n_" + k) is None:
        geo.addAttrib(hou.attribType.Global, "n_" + k, 0)
    geo.setGlobalAttribValue("n_" + k, v)
if geo.findGlobalAttrib("summary") is None:
    geo.addAttrib(hou.attribType.Global, "summary", "")
geo.setGlobalAttribValue("summary", summary)
'''

BACKING_CODE = r"""
# 在文字后面垫一块浅色底板，任何视口背景下都读得清
node = hou.pwd(); geo = node.geometry()
pad = %f
bb = geo.boundingBox()
lo, hi = bb.minvec(), bb.maxvec()
pts = []
for x, y in ((lo[0] - pad, lo[1] - pad), (hi[0] + pad, lo[1] - pad), (hi[0] + pad, hi[1] + pad), (lo[0] - pad, hi[1] + pad)):
    pt = geo.createPoint(); pt.setPosition(hou.Vector3(x, y, lo[2] - 0.03)); pts.append(pt)
poly = geo.createPolygon()
for pt in reversed(pts): poly.addVertex(pt)
poly.setAttribValue("Cd", (0.97, 0.96, 0.88))
"""

WIRE_CODE = r"""
# 把每片三角的边画成细棱柱，贴在面前面一点，让共用顶点和各片三角看得出来
node = hou.pwd(); geo = node.geometry()
r = %f
polys = [p for p in geo.prims() if isinstance(p, hou.Polygon) and p.isClosed()]
edges = set()
for pr in polys:
    vs = [v.point().number() for v in pr.vertices()]
    for k in range(len(vs)):
        a, b = vs[k], vs[(k + 1) %% len(vs)]
        if a != b:
            edges.add((min(a, b), max(a, b)))
pos = {pt.number(): hou.Vector3(pt.position()) for pt in geo.points()}
geo.clear()
geo.addAttrib(hou.attribType.Prim, "Cd", (0.12, 0.12, 0.15))
lift = hou.Vector3(0, 0, 0.03)
for a, b in edges:
    pa, pb = pos[a] + lift, pos[b] + lift
    dv = pb - pa
    L = dv.length()
    if L < 1e-9:
        continue
    dn = dv * (1.0 / L)
    up = hou.Vector3(0, 0, 1) if abs(dn[2]) < 0.9 else hou.Vector3(1, 0, 0)
    u = dn.cross(up).normalized() * r
    w = dn.cross(u).normalized() * r
    offs = [u + w, u - w, u * -1.0 - w, u * -1.0 + w]
    ra = [geo.createPoint() for _ in range(4)]
    rb = [geo.createPoint() for _ in range(4)]
    for i in range(4):
        ra[i].setPosition(pa + offs[i]); rb[i].setPosition(pb + offs[i])
    for i in range(4):
        j = (i + 1) %% 4
        poly = geo.createPolygon()
        for pt in (ra[i], ra[j], rb[j], rb[i]):
            poly.addVertex(pt)
"""

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def py_sop(parent, name, code, spare=()):
    n = parent.createNode("python", name)
    ptg = n.parmTemplateGroup()
    for kind, pname, label, default in spare:
        if kind == "float":
            ptg.append(hou.FloatParmTemplate(pname, label, 1, default_value=(default,)))
        elif kind == "int":
            ptg.append(hou.IntParmTemplate(pname, label, 1, default_value=(default,)))
        elif kind == "toggle":
            ptg.append(hou.ToggleParmTemplate(pname, label, default_value=bool(default)))
    n.setParmTemplateGroup(ptg)
    n.parm("python").set(code)
    return n


def weld_sop(parent, name, d, stage14=False, bucket_mult=6, mode=0, salt_passes=1):
    return py_sop(parent, name, WELD_CODE, [
        ("float", "weld_distance", "Weld Distance", d),
        ("float", "bucket_mult", "Buckets per Source Triangle", bucket_mult),
        ("toggle", "remove_degenerate_duplicate", "Stage 13: Remove Degenerate/Duplicate", 1),
        ("toggle", "orientation_fix", "Stage 14: Orientation Fix", 1 if stage14 else 0),
        ("int", "mode", "Mode (0 plugin hash, 1 exact cells, 2 transitive)", mode),
        ("int", "salt_passes", "Hash Tables With Different Salts (mode 0)", salt_passes),
    ])


def report_sop(parent, name, radius, color_mode=0, crack_dist=0.0, mark_samedir=True):
    return py_sop(parent, name, REPORT_CODE, [
        ("float", "radius", "Mark Radius", radius),
        ("int", "color_mode", "Color (0 keep,1 srcN,2 +Y,3 +Z)", color_mode),
        ("float", "crack_dist", "Only Mark Open Edges With a Partner Within (0 = all)", crack_dist),
        ("toggle", "mark_samedir", "Mark Same-Direction Edges", 1 if mark_samedir else 0),
    ])


def xform(parent, name, inp, t=(0, 0, 0), scale=1.0):
    n = parent.createNode("xform", name)
    n.setInput(0, inp)
    n.parmTuple("t").set(t)
    n.parm("scale").set(scale)
    return n


def wrangle(parent, name, inp, code, run_over="point"):
    n = parent.createNode("attribwrangle", name)
    if inp is not None:
        n.setInput(0, inp)
    n.parm("class").set(run_over)
    n.parm("snippet").set(code)
    return n


def label(parent, name, text, pos, size=1.0):
    f = parent.createNode("font", name)
    f.parm("file").set(FONT)
    f.parm("text").set(text)
    f.parm("fontsize").set(size)
    f.parm("halign").set("left")
    col = wrangle(parent, name + "_ink", f, "@Cd = {0.06, 0.06, 0.08};", "primitive")
    back = py_sop(parent, name + "_backing", BACKING_CODE % (0.3 * size,))
    back.setInput(0, col)
    pk = parent.createNode("pack", name + "_packed")   # 打包：开着点显示时一条标签只有一个点
    pk.setInput(0, back)
    return xform(parent, name + "_at", pk, pos)


def make_obj(name, t, net_pos):
    o = OBJ.createNode("geo", name)
    for c in o.children():
        c.destroy()
    o.parmTuple("t").set(t)
    o.setPosition(net_pos)
    return o


def finish(o, inputs):
    m = o.createNode("merge", "merge_all")
    for i, n in enumerate(inputs):
        m.setInput(i, n)
    out = o.createNode("null", "OUT")
    out.setInput(0, m)
    out.setDisplayFlag(True)
    out.setRenderFlag(True)
    o.layoutChildren()
    return out


def obj_note(name, text, pos, size=(4.6, 3.2)):
    n = OBJ.createStickyNote(name)
    n.setText(text)
    n.setPosition(pos)
    n.setSize(hou.Vector2(*size))
    return n


def inner_note(o, text, size=(7.0, 3.0)):
    n = o.createStickyNote("weld_note_inner")
    n.setText(text)
    bb_min = min((c.position()[1] for c in o.children()), default=0.0)
    n.setPosition(hou.Vector2(-6.0, bb_min - size[1] - 1.0))
    n.setSize(hou.Vector2(*size))
    return n


# ---------------------------------------------------------------------------
# clean up only what this script created before
# ---------------------------------------------------------------------------
for c in list(OBJ.children()):
    if c.name().startswith("weld_case"):
        c.destroy()
for s in list(OBJ.stickyNotes()):
    if s.name().startswith("weld_note"):
        s.destroy()

g1 = hou.node("/obj/geo1")
base = g1.position() if g1 else hou.Vector2(0, 0)
row_y = base[1] - 3.0
def net(i):
    return hou.Vector2(base[0] + i * 5.2, row_y)

report = {}

# ---------------------------------------------------------------------------
# Case 1: weld moves a vertex across the opposite edge -> flip -> orientation fix -> same-direction edges
# ---------------------------------------------------------------------------
o = make_obj("weld_case1_flip", (40, 0, 0), net(0))
src = py_sop(o, "src_patch", r'''
node = hou.pwd(); geo = node.geometry()
geo.addAttrib(hou.attribType.Prim, "srcN", (0.0, 0.0, 0.0))
def tri(a, b, c):
    pts = []
    for p in (a, b, c):
        pt = geo.createPoint(); pt.setPosition(hou.Vector3(p[0], p[1], 0.0)); pts.append(pt)
    poly = geo.createPolygon()
    for pt in pts: poly.addVertex(pt)
    poly.setAttribValue("srcN", tuple(poly.normal()))
W, W3, W2 = (3.3, -0.9), (1.6, -2.8), (5.2, -2.6)
A, B, U, V = (0.0, 0.0), (6.0, 0.0), (3.0, 6.0), (3.0, 0.9)
tri(W, W3, W2)   # 另一块面上的点 W：序号最小，会被选成代表点
tri(A, B, V)     # 贴着边的窄三角，V 离 W 1.82（焊接距离 2）
tri(B, U, V)
tri(U, A, V)
''')
arrow = py_sop(o, "arrow_V_to_W", r'''
# 蓝色箭头：焊接会把 V 从这里挪到 W（箭头尖 = W）
node = hou.pwd(); geo = node.geometry()
geo.addAttrib(hou.attribType.Prim, "Cd", (0.1, 0.55, 0.95))
a = hou.Vector3(3.0, 0.9, 0.06); b = hou.Vector3(3.3, -0.9, 0.06)
d = (b - a).normalized(); n = hou.Vector3(-d[1], d[0], 0)
base = b - d * 0.5
for pts_pos in ([a + n * 0.06, a - n * 0.06, base - n * 0.06, base + n * 0.06], [base + n * 0.24, base - n * 0.24, b]):
    pts = [geo.createPoint() for _ in pts_pos]
    for pt, pp in zip(pts, pts_pos): pt.setPosition(pp)
    poly = geo.createPolygon()
    for pt in pts: poly.addVertex(pt)
''')
src_fused = o.createNode("fuse", "src_fused_for_display"); src_fused.setInput(0, src); src_fused.parm("usetol3d").set(1); src_fused.parm("tol3d").set(1e-4)
r0 = report_sop(o, "report_src", 0.07, color_mode=1, crack_dist=0.2); r0.setInput(0, src_fused)
w1 = weld_sop(o, "plugin_weld_no_fix", 2.0, stage14=False); w1.setInput(0, src)
r1 = report_sop(o, "report_weld", 0.07, color_mode=1, crack_dist=0.2); r1.setInput(0, w1)
w2 = weld_sop(o, "plugin_weld_with_fix", 2.0, stage14=True); w2.setInput(0, src)
r2 = report_sop(o, "report_fix", 0.07, color_mode=1, crack_dist=0.2); r2.setInput(0, w2)

def overlay(name, inp, cond, color):
    wr = wrangle(o, name, inp, "if (%s) removeprim(0, @primnum, 1); else @Cd = %s;" % (cond, color), "primitive")
    un = o.createNode("facet", name + "_own_points"); un.setInput(0, wr); un.parm("unique").set(1)
    return xform(o, name + "_lift", un, (0, 0, 0.04))

ov1 = overlay("overlay_flipped", w1, "i@flipped == 0", "{1, 0.12, 0.12}")
ov2 = overlay("overlay_orient_fixed", w2, "i@orient_fixed == 0", "{1, 0.55, 0.05}")
wire0 = py_sop(o, "edges_src", WIRE_CODE % 0.035); wire0.setInput(0, src_fused)
wire1 = py_sop(o, "edges_weld", WIRE_CODE % 0.035); wire1.setInput(0, w1)
wire2 = py_sop(o, "edges_fix", WIRE_CODE % 0.035); wire2.setInput(0, w2)
m0 = o.createNode("merge", "stage1"); m0.setInput(0, r0); m0.setInput(1, arrow); m0.setInput(2, wire0)
m1 = o.createNode("merge", "stage2"); m1.setInput(0, r1); m1.setInput(1, ov1); m1.setInput(2, wire1)
m2 = o.createNode("merge", "stage3"); m2.setInput(0, r2); m2.setInput(1, ov2); m2.setInput(2, wire2)
parts = [
    m0,
    xform(o, "at_weld", m1, (22, 0, 0)),
    xform(o, "at_fix", m2, (44, 0, 0)),
    label(o, "lbl0", u"① 焊接前：V 离另一块面上的 W 只有 1.82", (-2, 7.4, 0), 0.5),
    label(o, "lbl0b", u"绿色大三角其实是围着 V 的三片三角；蓝箭头 = 焊接把 V 挪到 W（焊接距离 2）", (-2, 6.7, 0), 0.4),
    label(o, "pt_A", "A", (-0.7, -0.6, 0), 0.45),
    label(o, "pt_B", "B", (6.25, -0.6, 0), 0.45),
    label(o, "pt_U", "U", (3.25, 6.0, 0), 0.45),
    label(o, "pt_V", u"V（要被挪走的点）", (3.3, 1.1, 0), 0.4),
    label(o, "pt_W", u"W（另一块面上的点）", (3.6, -1.1, 0), 0.4),
    label(o, "pt_W2", u"V 被挪到这里（= W）", (25.6, -1.1, 0), 0.4),
    label(o, "pt_thin", u"(A, B, V) 这片窄三角", (0.4, 0.25, 0), 0.3),
    label(o, "lbl1", u"② 焊接后：V 被挪到 W，贴边的窄三角翻了（红）", (20, 7.4, 0), 0.5),
    label(o, "lbl1b", u"W 同时连着两块面，成了蝴蝶点（红球）", (20, 6.7, 0), 0.4),
    label(o, "lbl2", u"③ 按源法线纠正绕序：这片被扳回来了（橙）", (42, 7.4, 0), 0.5),
    label(o, "lbl2b", u"但它两条边和邻面同向（紫），合并边会被拒绝", (42, 6.7, 0), 0.4),
]
out = finish(o, parts)
inner_note(o, u"情况 1：焊接压翻三角，绕序修正又把它扳成和邻居同向。\n"
              u"plugin_weld_* 是插件 GPU 焊接的逐位复刻（同一哈希、每格最小序号、27 格、单趟）"
              u"加 CPU Stage 13/14。红 / 橙那片单独抬高了一点显示，实际与邻面重叠（折叠）。"
              u"你的 7 次运行里每次有 746–1912 片三角走到了 ③ 这一步。")
report["case1"] = out

# ---------------------------------------------------------------------------
# Case 2: buried faces kept (expansion) -> non-manifold seams; edge / point contact
# ---------------------------------------------------------------------------
o = make_obj("weld_case2_nonmanifold", (100, 0, 0), net(1))
def box(name, size, center):
    b = o.createNode("box", name)
    b.parmTuple("size").set(size)
    b.parmTuple("t").set(center)
    return b
A = box("box_A", (10, 10, 10), (0, -5, 0))
Bb = box("plank_B_sunk_0p5", (4, 2, 6), (0, 0.5, 0))
un = o.createNode("boolean", "boolean_union"); un.setInput(0, A); un.setInput(1, Bb); un.parm("booleanop").set("union")
rs = o.createNode("boolean", "boolean_resolve_keep_buried"); rs.setInput(0, A); rs.setInput(1, Bb); rs.parm("booleanop").set("resolve")
rs_f = o.createNode("fuse", "fuse_resolve"); rs_f.setInput(0, rs); rs_f.parm("usetol3d").set(1); rs_f.parm("tol3d").set(1e-4)
ru = report_sop(o, "report_union", 0.15, crack_dist=0.2, mark_samedir=False); ru.setInput(0, un)
rr = report_sop(o, "report_resolve", 0.15, crack_dist=0.2, mark_samedir=False); rr.setInput(0, rs_f)
c1 = box("cube_1", (4, 4, 4), (0, 2, 0)); c2 = box("cube_2_edge_touch", (4, 4, 4), (4, 6, 0))
me = o.createNode("merge", "edge_contact"); me.setInput(0, c1); me.setInput(1, c2)
fe = o.createNode("fuse", "fuse_edge"); fe.setInput(0, me); fe.parm("usetol3d").set(1); fe.parm("tol3d").set(1e-4)
re_ = report_sop(o, "report_edge", 0.15, crack_dist=0.2, mark_samedir=False); re_.setInput(0, fe)
c3 = box("cube_3", (4, 4, 4), (0, 2, 0)); c4 = box("cube_4_corner_touch", (4, 4, 4), (4, 6, 4))
mc = o.createNode("merge", "corner_contact"); mc.setInput(0, c3); mc.setInput(1, c4)
fc = o.createNode("fuse", "fuse_corner"); fc.setInput(0, mc); fc.parm("usetol3d").set(1); fc.parm("tol3d").set(1e-4)
rc = report_sop(o, "report_corner", 0.15, crack_dist=0.2, mark_samedir=False); rc.setInput(0, fc)
parts = [
    ru,
    xform(o, "at_resolve", rr, (16, 0, 0)),
    xform(o, "at_edge", re_, (32, -5, 0)),
    xform(o, "at_corner", rc, (46, -5, 0)),
    label(o, "lbl0", u"并集（正确）：每条边正好两片面", (-5, 3.5, 0), 0.6),
    label(o, "lbl1", u"埋面也留着：交线上一条边挂四片面（红）", (11, 3.5, 0), 0.6),
    label(o, "lbl1b", u"扩距 10 在盖板薄于 ~10 cm 处就是这样", (11, 2.6, 0), 0.45),
    label(o, "lbl2", u"只沿一条棱接触：这条棱挂四片面", (28, 7.5, 0), 0.6),
    label(o, "lbl3", u"只在一个点接触：蝴蝶点（红球）", (42, 7.5, 0), 0.6),
]
out = finish(o, parts)
inner_note(o, u"情况 2：非流形。Boolean 的 resolve 把两个输入沿交线切开、所有面都保留，"
              u"等于扩距把埋面全留下的极端情况。一条边挂四片面时，边配对没有唯一解。")
report["case2"] = out

# ---------------------------------------------------------------------------
# Case 3: weld pairing is unreliable (hash shadowing, one representative per cell, no transitivity)
# ---------------------------------------------------------------------------
o = make_obj("weld_case3_pairing", (175, 0, 0), net(2))
grid = o.createNode("grid", "grid_30x30")
grid.parm("type").set("poly"); grid.parm("surftype").set("triangles"); grid.parm("orient").set("xy")
grid.parm("sizex").set(30); grid.parm("sizey").set(30); grid.parm("rows").set(31); grid.parm("cols").set(31)
soup = o.createNode("facet", "soup_unique_points"); soup.setInput(0, grid); soup.parm("unique").set(1)
jit = wrangle(o, "float_mismatch_jitter", soup,
              "@P += (set(rand(@ptnum * 3 + 1), rand(@ptnum * 3 + 2), 0) - set(0.5, 0.5, 0)) * chf('jitter');", "point")
ptg = jit.parmTemplateGroup(); ptg.append(hou.FloatParmTemplate("jitter", "Jitter", 1, default_value=(0.01,))); jit.setParmTemplateGroup(ptg)
w = weld_sop(o, "plugin_weld", 0.1)
w.setInput(0, jit)
rw = report_sop(o, "report_plugin_weld", 0.08, crack_dist=0.05); rw.setInput(0, w)
fz = o.createNode("fuse", "houdini_fuse"); fz.setInput(0, jit); fz.parm("usetol3d").set(1); fz.parm("tol3d").set(0.1)
rf = report_sop(o, "report_houdini_fuse", 0.08, crack_dist=0.05); rf.setInput(0, fz)
chain = py_sop(o, "chain_fan", r'''
node = hou.pwd(); geo = node.geometry()
# 三片三角围着同一个中心点，中心点的三个副本沿 x 排开：间距 0.08（焊接距离 0.1）
o0, o1, o2 = (-0.8, 0.6), (0.8, 0.6), (0.0, -1.0)
cs = [(0.013, 0.013), (0.093, 0.013), (0.173, 0.013)]
for (c, a, b) in ((cs[0], o0, o1), (cs[1], o1, o2), (cs[2], o2, o0)):
    pts = []
    for p in (c, a, b):
        pt = geo.createPoint(); pt.setPosition(hou.Vector3(p[0], p[1], 0.0)); pts.append(pt)
    poly = geo.createPolygon()
    for pt in pts: poly.addVertex(pt)
''')
wc = weld_sop(o, "plugin_weld_chain", 0.1); wc.setInput(0, chain)
rcn = report_sop(o, "report_chain", 0.01, crack_dist=0.15); rcn.setInput(0, wc)
parts = [
    rw,
    xform(o, "at_fuse", rf, (36, 0, 0)),
    xform(o, "at_chain", rcn, (80, 0, 0), 8.0),
    label(o, "lbl0", u"插件焊接复刻：距离内的点没并上 → 黄色星形", (-15, 17, 0), 0.9),
    label(o, "lbl0b", "`details('../report_plugin_weld', 'summary')`", (-15, 15.6, 0), 0.6),
    label(o, "lbl1", u"同一份输入用 Houdini Fuse：没有漏焊", (21, 17, 0), 0.9),
    label(o, "lbl1b", "`details('../report_houdini_fuse', 'summary')`", (21, 15.6, 0), 0.6),
    label(o, "lbl2", u"不传递：中心点三个副本间距 0.08", (70, 9.5, 0), 0.8),
    label(o, "lbl2b", u"第二个并到第一个，第三个找不到第二个 → 裂开", (70, 8.3, 0), 0.6),
]
out = finish(o, parts)
inner_note(o, u"情况 3：焊接配对不可靠。30×30 网格拆成三角汤，每个角点加 0.01 的抖动模拟 float 误差，"
              u"焊接距离 0.1、桶数按插件规则（源三角 ×6）。每个哈希桶只记最小序号的点，冲突时整簇点都并不上；"
              u"右边的小扇形演示每格只有一个代表点、而且不做传递合并。")
report["case3"] = out

# ---------------------------------------------------------------------------
# Case 3b: fixes for case 3 side by side
# ---------------------------------------------------------------------------
CHAIN_CODE = r"""
node = hou.pwd(); geo = node.geometry()
o0, o1, o2 = (-0.8, 0.6), (0.8, 0.6), (0.0, -1.0)
cs = [(0.013, 0.013), (0.093, 0.013), (0.173, 0.013)]
for (c, a, b) in ((cs[0], o0, o1), (cs[1], o1, o2), (cs[2], o2, o0)):
    pts = []
    for p in (c, a, b):
        pt = geo.createPoint(); pt.setPosition(hou.Vector3(p[0], p[1], 0.0)); pts.append(pt)
    poly = geo.createPolygon()
    for pt in pts: poly.addVertex(pt)
"""
o = make_obj("weld_case3b_fixes", (175, -50, 0), hou.Vector2(net(2)[0], row_y - 4.6))
grid = o.createNode("grid", "grid_30x30")
grid.parm("type").set("poly"); grid.parm("surftype").set("triangles"); grid.parm("orient").set("xy")
grid.parm("sizex").set(30); grid.parm("sizey").set(30); grid.parm("rows").set(31); grid.parm("cols").set(31)
soup = o.createNode("facet", "soup_unique_points"); soup.setInput(0, grid); soup.parm("unique").set(1)
jit = wrangle(o, "float_mismatch_jitter", soup,
              "@P += (set(rand(@ptnum * 3 + 1), rand(@ptnum * 3 + 2), 0) - set(0.5, 0.5, 0)) * 0.01;", "point")
variants = [
    ("plugin_weld_1_table", dict(), u"插件现状：1 张哈希表"),
    ("plugin_weld_2_tables", dict(salt_passes=2), u"2 张不同盐的哈希表，取较小的代表点"),
    ("weld_exact_cells", dict(mode=1), u"按真实格子分组：没有哈希冲突"),
]
parts = []
for i, (name, kw, text) in enumerate(variants):
    wv = weld_sop(o, name, 0.1, **kw); wv.setInput(0, jit)
    rv = report_sop(o, "report_" + name, 0.08, crack_dist=0.05); rv.setInput(0, wv)
    parts.append(xform(o, "at_" + name, rv, (36 * i, 0, 0)))
    parts.append(label(o, "lbl_" + name, text, (36 * i - 15, 17, 0), 0.9))
    parts.append(label(o, "lbl_" + name + "_s", "`details('../report_%s', 'summary')`" % name, (36 * i - 15, 15.6, 0), 0.6))
chain = py_sop(o, "chain_fan", CHAIN_CODE)
chain_variants = [
    ("chain_exact_cells", dict(mode=1), u"按真实格子分组：扇形仍裂开（不传递）"),
    ("chain_transitive", dict(mode=2), u"传递合并（并查集）：扇形合上了"),
]
for i, (name, kw, text) in enumerate(chain_variants):
    wv = weld_sop(o, name, 0.1, **kw); wv.setInput(0, chain)
    rv = report_sop(o, "report_" + name, 0.01, crack_dist=0.15); rv.setInput(0, wv)
    parts.append(xform(o, "at_" + name, rv, (118 + 26 * i, 0, 0), 8.0))
    parts.append(label(o, "lbl_" + name, text, (106 + 26 * i, 9.5, 0), 0.7))
    parts.append(label(o, "lbl_" + name + "_s", "`details('../report_%s', 'summary')`" % name, (106 + 26 * i, 8.4, 0), 0.5))
out = finish(o, parts)
inner_note(o, u"情况 3 的修法对比。输入与情况 3 完全相同。多张不同盐的哈希表：一张表里被挡住的格子，在另一张表里大概率不冲突，"
              u"取所有表里最小的代表点即可。按真实格子分组：根本没有冲突。两者都修不了扇形那种一串点，只有传递合并（并查集）能合上，"
              u"但传递合并会把一串本来就不同、却挨得很近的顶点一路并成一团。")
report["case3b"] = out

# ---------------------------------------------------------------------------
# Case 4: weld distance larger than a thin part -> front/back become the same 3 points -> dedupe keeps one side
# ---------------------------------------------------------------------------
o = make_obj("weld_case4_thin_sheet", (280, 0, 0), net(3))
plate = py_sop(o, "closed_thin_slab_0p05", r'''
# 封闭薄板：正面、背面（同样的三角划分、反向）和四周侧壁，厚 0.05
node = hou.pwd(); geo = node.geometry()
n, size, half = 6, 12.0, 0.025
F, B = {}, {}
for j in range(n + 1):
    for i in range(n + 1):
        x, y = -size / 2 + size * i / n, -size / 2 + size * j / n
        F[i, j] = geo.createPoint(); F[i, j].setPosition(hou.Vector3(x, y, half))
        B[i, j] = geo.createPoint(); B[i, j].setPosition(hou.Vector3(x, y, -half))
def tri(a, b, c):
    poly = geo.createPolygon()
    for pt in (a, b, c): poly.addVertex(pt)
for j in range(n):
    for i in range(n):
        a, b, c, d = (i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)
        tri(F[a], F[c], F[b]); tri(F[a], F[d], F[c])
        tri(B[a], B[b], B[c]); tri(B[a], B[c], B[d])
rim = [(i, 0) for i in range(n)] + [(n, j) for j in range(n)] + [(i, n) for i in range(n, 0, -1)] + [(0, j) for j in range(n, 0, -1)]
for k in range(len(rim)):
    p, q = rim[k], rim[(k + 1) % len(rim)]
    tri(F[p], B[q], B[p]); tri(F[p], F[q], B[q])
''')
psoup = o.createNode("facet", "plate_soup"); psoup.setInput(0, plate); psoup.parm("unique").set(1)
pw = weld_sop(o, "plugin_weld_0p1", 0.1, bucket_mult=1000); pw.setInput(0, psoup)
rb = report_sop(o, "report_before", 0.03, color_mode=3, crack_dist=0.2); rb.setInput(0, plate)
ra = report_sop(o, "report_after", 0.03, color_mode=3, crack_dist=0.2); ra.setInput(0, pw)
keep_back = "vector n = prim_normal(0, @primnum, {0.5, 0.5, 0}); if (n.z >= 0) removeprim(0, @primnum, 1);"
vb = wrangle(o, "seen_from_back_before", rb, keep_back, "primitive")
va = wrangle(o, "seen_from_back_after", ra, keep_back, "primitive")
parts = [
    rb,
    xform(o, "at_after", ra, (16, 0, 0)),
    xform(o, "at_back_before", vb, (32, 0, 0)),
    xform(o, "at_back_after", va, (48, 0, 0)),
    label(o, "lbl0", u"封闭薄板：正面(绿)、背面(红)相距 0.05", (-6, 7.5, 0), 0.6),
    label(o, "lbl1", u"焊接 0.1 后：两面成了同样三个点，去重只留一面", (10, 7.5, 0), 0.6),
    label(o, "lbl1b", "`details('../report_after', 'summary')`", (10, 6.6, 0), 0.45),
    label(o, "lbl2", u"从背面看，焊接前：看得到红色背面", (26, 7.5, 0), 0.6),
    label(o, "lbl3", u"从背面看，焊接后：什么都没有 = 洞", (42, 7.5, 0), 0.6),
]
out = finish(o, parts)
inner_note(o, u"情况 4：焊接距离 0.1 大于薄件厚度。正反两面被焊到同样三个点上，插件的去重按「三个点相同」判、"
              u"不看朝向，于是只剩一面。右边两块只保留朝向背面的三角，模拟从背面、开背面剔除时看到的东西。"
              u"这里把桶数调到每源三角 1000 个，排除情况 3 的哈希冲突漏焊，只看去重本身。")
report["case4"] = out

# ---------------------------------------------------------------------------
# Case 5: far from origin -> float spacing ~0.002
# ---------------------------------------------------------------------------
o = make_obj("weld_case5_precision", (360, 0, 0), net(4))
sp = o.createNode("sphere", "sphere_r0p02")
sp.parm("type").set("poly"); sp.parm("freq").set(12)
sp.parm("radx").set(0.02); sp.parm("rady").set(0.02); sp.parm("radz").set(0.02)
near = xform(o, "shown_x500_at_origin", sp, (0, 0, 0), 500.0)
far1 = xform(o, "move_to_x29503", sp, (29503, 0, 0))
far2 = xform(o, "move_back", far1, (-29503, 0, 0))
far3 = xform(o, "shown_x500_after_trip", far2, (0, 0, 0), 500.0)
parts = [
    near,
    xform(o, "at_far", far3, (26, 0, 0)),
    label(o, "lbl0", u"原点处：半径 0.02 的球（放大 500 倍显示）", (-10, 12.5, 0), 0.8),
    label(o, "lbl1", u"在 x=29503 存过一次再搬回来：", (16, 12.5, 0), 0.8),
    label(o, "lbl1b", u"float 最小间隔 0.00195，球被吸成方块", (16, 11.3, 0), 0.8),
]
out = finish(o, parts)
inner_note(o, u"情况 5 不是焊接失败，只是精度说明：图里把 float 网格画出来看（x=29503 处最小间隔 2^-9≈0.00195）。"
              u"焊接距离 0.1 是它的 50 倍，精度不影响焊接；按 0.1 这么小的球本来就会被焊成一个点。"
              u"精度只限制远小于焊接距离的容差：给零面积补面撑出的高度、T 点落在边上的判定等，都要取约 0.01 cm 以上。")
report["case5"] = out

# ---------------------------------------------------------------------------
# Case 6: same input, different order -> different weld result
# ---------------------------------------------------------------------------
o = make_obj("weld_case6_order", (420, 0, 0), net(5))
grid = o.createNode("grid", "grid_30x30")
grid.parm("type").set("poly"); grid.parm("surftype").set("triangles"); grid.parm("orient").set("xy")
grid.parm("sizex").set(30); grid.parm("sizey").set(30); grid.parm("rows").set(31); grid.parm("cols").set(31)
soup = o.createNode("facet", "soup_unique_points"); soup.setInput(0, grid); soup.parm("unique").set(1)
jit = wrangle(o, "float_mismatch_jitter", soup,
              "@P += (set(rand(@ptnum * 3 + 1), rand(@ptnum * 3 + 2), 0) - set(0.5, 0.5, 0)) * 0.01;", "point")
outs = []
for i, seed in enumerate((1, 2)):
    s = o.createNode("sort", "gpu_order_seed%d" % seed); s.setInput(0, jit)
    s.parm("primsort").set("seed"); s.parm("primseed").set(seed)
    wv = weld_sop(o, "plugin_weld_seed%d" % seed, 0.1); wv.setInput(0, s)
    rv = report_sop(o, "report_seed%d" % seed, 0.08, crack_dist=0.05); rv.setInput(0, wv)
    outs.append(xform(o, "at_seed%d" % seed, rv, (36 * i, 0, 0)))
    outs.append(label(o, "lbl%d" % seed, u"同一份输入，三角顺序 %d：" % seed, (36 * i - 15, 17, 0), 0.9))
    outs.append(label(o, "lbl%db" % seed, "`details('../report_seed%d', 'summary')`" % seed, (36 * i - 15, 15.6, 0), 0.6))
out = finish(o, outs)
inner_note(o, u"情况 6：同一个输入，每次结果都不一样。GPU 管线每次写出的三角顺序不同，焊接按「最小序号」选代表点，"
              u"所以漏焊出现在不同地方、数量也不同。你 09-21 最后三次运行：437029 / 437039 / 437017 片。")
report["case6"] = out

# ---------------------------------------------------------------------------
# Case 7: downstream — soup expansion loses connectivity; area filter deletes the zero-area fill
# ---------------------------------------------------------------------------
o = make_obj("weld_case7_downstream", (510, 0, 0), net(6))
g = o.createNode("grid", "grid_10x10")
g.parm("type").set("poly"); g.parm("surftype").set("triangles"); g.parm("orient").set("xy")
g.parm("sizex").set(12); g.parm("sizey").set(12); g.parm("rows").set(11); g.parm("cols").set(11)
welded = o.createNode("fuse", "welded"); welded.setInput(0, g); welded.parm("usetol3d").set(1); welded.parm("tol3d").set(1e-4)
conn_code = "int c = i@class; @Cd = set(rand(c * 3 + 1), rand(c * 3 + 2), rand(c * 3 + 3));"
cw = o.createNode("connectivity", "pieces_welded"); cw.setInput(0, welded); cw.parm("connecttype").set("prim")
colw = wrangle(o, "color_by_piece_welded", cw, conn_code, "primitive")
soup = o.createNode("facet", "into_ucsmesh_soup"); soup.setInput(0, welded); soup.parm("unique").set(1)
cs_ = o.createNode("connectivity", "pieces_soup"); cs_.setInput(0, soup); cs_.parm("connecttype").set("prim")
cols = wrangle(o, "color_by_piece_soup", cs_, conn_code, "primitive")
tpatch = py_sop(o, "t_junction_with_zero_area_fill", r'''
node = hou.pwd(); geo = node.geometry()
P = {"A": (0, 0), "B": (0, 10), "C": (-10, 5), "X": (10, 5), "V": (0, 5)}
pts = {}
for k, p in P.items():
    pt = geo.createPoint(); pt.setPosition(hou.Vector3(p[0] * 0.6, p[1] * 0.6 - 3, 0)); pts[k] = pt
for f in (("A", "B", "C"), ("V", "A", "X"), ("B", "V", "X"), ("A", "V", "B")):   # 最后一片是 T 缝补面，零面积
    poly = geo.createPolygon()
    for k in f: poly.addVertex(pts[k])
''')
rt0 = report_sop(o, "report_with_fill", 0.06, crack_dist=0.1); rt0.setInput(0, tpatch)
area_filter = wrangle(o, "plugin_area_filter_5e-5", tpatch,
                      "if (primintrinsic(0, 'measuredarea', @primnum) <= 5e-5) removeprim(0, @primnum, 1);", "primitive")
rt1 = report_sop(o, "report_after_filter", 0.06, crack_dist=0.1); rt1.setInput(0, area_filter)
parts = [
    colw,
    xform(o, "at_soup", cols, (16, 0, 0)),
    xform(o, "at_t0", rt0, (36, 0, 0)),
    xform(o, "at_t1", rt1, (52, 0, 0)),
    label(o, "lbl0", u"焊好的网格：一整块", (-6, 7.5, 0), 0.6),
    label(o, "lbl1", u"进 UCSMesh 展开成三角汤：每片一个孤岛", (10, 7.5, 0), 0.6),
    label(o, "lbl2", u"T 缝 + 零面积补面：内部没有开放边", (30, 7.5, 0), 0.6),
    label(o, "lbl3", u"插件面积判据（≤5e-5）删掉补面：T 缝又开了（黄）", (46, 7.5, 0), 0.6),
]
out = finish(o, parts)
inner_note(o, u"情况 7：下游会把连通关系拆掉。UCSMesh 的常驻流逐顶点存储，焊好的快照会被展开成三角汤；"
              u"BuildGpuMeshDescription 还会删掉面积 ≤ 5e-5 cm² 的面，零面积补面在这里被删，T 缝重新变成开放边。")
report["case7"] = out

# ---------------------------------------------------------------------------
# /obj level notes
# ---------------------------------------------------------------------------
titles = [
    u"1 焊接压翻 + 绕序修正 → 同向边",
    u"2 埋面保留 / 棱接触 / 点接触 → 非流形",
    u"3 焊接配对不可靠（插件算法复刻）",
    u"4 焊接距离 > 薄件厚度 → 去重只留一面",
    u"5 精度说明（不是失败）：容差下限约 0.01 cm",
    u"6 同一输入每次结果不同",
    u"7 下游：展开成三角汤 / 面积判据删补面",
]
for i, t in enumerate(titles):
    obj_note("weld_note_%d" % (i + 1), t, hou.Vector2(net(i)[0] - 1.2, row_y - 1.6), (4.8, 1.2))
obj_note("weld_note_legend",
         u"颜色：黄 = 开放边（配不上的边）；红 = 一条边挂 3 片以上 / 红球 = 蝴蝶点；紫 = 两片面同向经过同一条边；"
         u"面上绿 / 红 = 朝向正常 / 翻了。每个对象里的 plugin_weld* 是插件 GPU 焊接的逐位复刻，参数可以直接改。",
         hou.Vector2(base[0] + 3.0, base[1] + 0.8), (18.0, 1.4))

result = {}
for k, n in report.items():
    err = list(n.errors()) + [c.name() + ": " + e for c in n.parent().children() for e in c.errors()]
    geo = n.geometry()
    result[k] = {"prims": len(geo.prims()), "points": len(geo.points()), "errors": err[:4]}
    for c in n.parent().children():
        if c.name().startswith("report"):
            g_ = c.geometry()
            a = g_.findGlobalAttrib("summary")
            result[k][c.name()] = g_.attribValue("summary") if a else None
RESULT = json.dumps(result, ensure_ascii=True)
