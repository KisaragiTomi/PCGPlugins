# -*- coding: utf-8 -*-
"""
inspect_checkpoint.py — 无 torch 环境下解析 NKSR ks.pth (PyTorch zip checkpoint)。

torch 保存格式 = zip:
  <prefix>/data.pkl        : pickle 流。张量以 persistent_load 元组
                             ('storage', <StorageType>, key, location, numel) 引用
  <prefix>/data/<key>      : 该 storage 的原始 little-endian 字节
  张量经 torch._utils._rebuild_tensor_v2(storage, storage_offset, size, stride,
                             requires_grad, backward_hooks) 重建。

输出:
  Docs/PortSpecs/F_checkpoint_inventory.json  — {key: {dtype, shape, numel}} 全量清单 + 顶层结构
  Docs/PortSpecs/F_hparams.json               — hyper_parameters 完整 dump（stub 转可序列化）
  Intermediate/PortWork/ks_weights.npz        — 全部权重数组（key 原样）

禁止 import torch。仅依赖标准库 + numpy。
"""
import io
import json
import os
import pickle
import sys
import zipfile
from collections import OrderedDict, Counter

import numpy as np

CKPT_PATH = r"C:/Users/KLW/.cache/torch/hub/checkpoints/ks.pth"
SPEC_DIR = r"D:/MyWork/UnrealProject/UETest574_2/Plugins/AITool/Docs/PortSpecs"
NPZ_DIR = r"D:/MyWork/UnrealProject/UETest574_2/Plugins/AITool/Intermediate/PortWork"
INVENTORY_JSON = os.path.join(SPEC_DIR, "F_checkpoint_inventory.json")
HPARAMS_JSON = os.path.join(SPEC_DIR, "F_hparams.json")
NPZ_PATH = os.path.join(NPZ_DIR, "ks_weights.npz")

# ---------------------------------------------------------------------------
# storage 类型名 -> (逻辑 dtype 名, numpy dtype)。bfloat16 numpy 不支持，特殊处理。
# ---------------------------------------------------------------------------
STORAGE_DTYPES = {
    "FloatStorage": ("float32", np.float32),
    "DoubleStorage": ("float64", np.float64),
    "HalfStorage": ("float16", np.float16),
    "BFloat16Storage": ("bfloat16", None),
    "LongStorage": ("int64", np.int64),
    "IntStorage": ("int32", np.int32),
    "ShortStorage": ("int16", np.int16),
    "CharStorage": ("int8", np.int8),
    "ByteStorage": ("uint8", np.uint8),
    "BoolStorage": ("bool", np.bool_),
    "ComplexFloatStorage": ("complex64", np.complex64),
    "ComplexDoubleStorage": ("complex128", np.complex128),
}

# 全局 zip 状态（main 中初始化）
_ZF = None          # zipfile.ZipFile
_PREFIX = None      # 'archive/' 之类
_STORAGE_CACHE = {}


class NPTensor(np.ndarray):
    """numpy 数组视图 + 记录原始 torch dtype（bfloat16 会被转成 float32 存储）。"""
    torch_dtype = None


def load_storage(dtype_name, key):
    cache_key = (dtype_name, key)
    if cache_key in _STORAGE_CACHE:
        return _STORAGE_CACHE[cache_key]
    raw = _ZF.read(_PREFIX + "data/" + key)
    if dtype_name == "bfloat16":
        u16 = np.frombuffer(raw, dtype=np.uint16)
        flat = (u16.astype(np.uint32) << 16).view(np.float32)
    else:
        np_dtype = None
        for stg, (nm, dt) in STORAGE_DTYPES.items():
            if nm == dtype_name:
                np_dtype = dt
                break
        if np_dtype is None:
            np_dtype = np.uint8  # 未知类型按字节读
        flat = np.frombuffer(raw, dtype=np_dtype)
    _STORAGE_CACHE[cache_key] = flat
    return flat


def rebuild_tensor_v2(storage, storage_offset, size, stride,
                      requires_grad=False, backward_hooks=None, metadata=None):
    """torch._utils._rebuild_tensor_v2 的 numpy 版本。storage = (dtype_name, key, numel)。"""
    dtype_name, key, _numel = storage
    flat = load_storage(dtype_name, key)
    itemsize = flat.dtype.itemsize
    size = tuple(int(s) for s in size)
    if len(size) == 0:
        out = np.array(flat[int(storage_offset)], dtype=flat.dtype)
    else:
        strides_bytes = tuple(int(s) * itemsize for s in stride)
        view = np.lib.stride_tricks.as_strided(
            flat[int(storage_offset):], shape=size, strides=strides_bytes)
        out = np.ascontiguousarray(view)
    t = out.view(NPTensor)
    t.torch_dtype = dtype_name
    return t


def rebuild_parameter(data, requires_grad=False, backward_hooks=None):
    return data


# ---------------------------------------------------------------------------
# 未知 pickle 类的宽容 stub
# ---------------------------------------------------------------------------
class StubBase(object):
    _pickle_module = "?"
    _pickle_name = "?"

    def __new__(cls, *args, **kwargs):
        obj = object.__new__(cls)
        obj._args = args
        obj._kwargs = kwargs
        obj._state = None
        obj._items = []
        obj._dict = {}
        return obj

    def __init__(self, *args, **kwargs):
        pass

    def __setstate__(self, state):
        self._state = state

    # 支持 pickle 的 listitems / dictitems 回填
    def append(self, item):
        self._items.append(item)

    def extend(self, items):
        self._items.extend(items)

    def __setitem__(self, k, v):
        self._dict[k] = v

    def __repr__(self):
        return "<Stub {}.{}>".format(self._pickle_module, self._pickle_name)


_STUB_CACHE = {}


def make_stub(module, name):
    full = module + "." + name
    if full not in _STUB_CACHE:
        _STUB_CACHE[full] = type(
            "Stub_" + name.replace(".", "_"), (StubBase,),
            {"_pickle_module": module, "_pickle_name": name})
    return _STUB_CACHE[full]


class CkptUnpickler(pickle.Unpickler):
    def persistent_load(self, pid):
        # pid = ('storage', StorageType占位, key(str), location(str), numel(int))
        try:
            if isinstance(pid, tuple) and len(pid) >= 5 and pid[0] == "storage":
                storage_type, key, _location, numel = pid[1], pid[2], pid[3], pid[4]
                type_name = getattr(storage_type, "_pickle_name", None) \
                    or getattr(storage_type, "__name__", str(storage_type))
                dtype_name = STORAGE_DTYPES.get(type_name, ("unknown:" + type_name, None))[0]
                return (dtype_name, str(key), int(numel))
        except Exception as e:
            sys.stderr.write("persistent_load fallback: %r (%r)\n" % (pid, e))
        return ("unknown", repr(pid), 0)

    def find_class(self, module, name):
        if module == "torch._utils":
            if name == "_rebuild_tensor_v2":
                return rebuild_tensor_v2
            if name in ("_rebuild_parameter", "_rebuild_parameter_with_state"):
                return rebuild_parameter
        if module == "collections" and name == "OrderedDict":
            return OrderedDict
        root = module.split(".")[0]
        if root in ("builtins", "__builtin__", "collections", "numpy", "_codecs"):
            try:
                return super(CkptUnpickler, self).find_class(module, name)
            except Exception:
                pass
        # torch / omegaconf / pytorch_lightning / 其它未知类 → 宽容 stub
        return make_stub(module, name)


# ---------------------------------------------------------------------------
# 遍历 / JSON 化工具（带环检测：omegaconf 有 _parent 反向引用）
# ---------------------------------------------------------------------------
def is_tensor(obj):
    return isinstance(obj, np.ndarray)


def tensor_dtype_name(t):
    td = getattr(t, "torch_dtype", None)
    return td if td else str(t.dtype)


def walk_tensors(obj, path, out, seen):
    """收集 (path, tensor)。"""
    if is_tensor(obj):
        out.append((path, obj))
        return
    oid = id(obj)
    if isinstance(obj, dict):
        if oid in seen:
            return
        seen.add(oid)
        for k, v in obj.items():
            walk_tensors(v, (path + "." + str(k)) if path else str(k), out, seen)
    elif isinstance(obj, (list, tuple)):
        if oid in seen:
            return
        seen.add(oid)
        for i, v in enumerate(obj):
            walk_tensors(v, "%s[%d]" % (path, i), out, seen)
    elif isinstance(obj, StubBase):
        if oid in seen:
            return
        seen.add(oid)
        st = obj._state
        if isinstance(st, dict):
            for k, v in st.items():
                if k == "_parent":
                    continue
                walk_tensors(v, path + ".<stub>." + str(k), out, seen)
        if obj._dict:
            for k, v in obj._dict.items():
                walk_tensors(v, path + ".<stub>{%s}" % k, out, seen)
        if obj._items:
            for i, v in enumerate(obj._items):
                walk_tensors(v, path + ".<stub>[%d]" % i, out, seen)


def simplify(obj, seen=None, depth=0):
    """把任意对象（含 stub）转成 JSON 可序列化结构，尽量还原 omegaconf 内容。"""
    if seen is None:
        seen = set()
    if depth > 80:
        return "<max-depth>"
    if obj is None or isinstance(obj, (bool, int, float, str)):
        return obj
    if isinstance(obj, bytes):
        return {"__bytes__": len(obj)}
    if isinstance(obj, np.integer):
        return int(obj)
    if isinstance(obj, np.floating):
        return float(obj)
    if isinstance(obj, np.bool_):
        return bool(obj)
    if is_tensor(obj):
        d = {"__tensor__": tensor_dtype_name(obj), "shape": list(obj.shape)}
        if obj.size <= 64:
            d["data"] = np.asarray(obj).tolist()
        return d
    oid = id(obj)
    if oid in seen:
        return "<cycle>"
    try:
        if isinstance(obj, dict):
            seen.add(oid)
            r = {str(k): simplify(v, seen, depth + 1) for k, v in obj.items()}
            seen.discard(oid)
            return r
        if isinstance(obj, (list, tuple, set)):
            seen.add(oid)
            r = [simplify(v, seen, depth + 1) for v in obj]
            seen.discard(oid)
            return r
        if isinstance(obj, StubBase):
            seen.add(oid)
            name = obj._pickle_module + "." + obj._pickle_name
            st = obj._state
            r = None
            if isinstance(st, dict):
                # omegaconf ValueNode: {'_val': x, ...} → 直接取值
                if "_val" in st:
                    r = simplify(st["_val"], seen, depth + 1)
                # omegaconf Container: {'_content': {...}, '_metadata': ..} → 取内容
                elif "_content" in st:
                    r = simplify(st["_content"], seen, depth + 1)
                else:
                    r = {"__class__": name,
                         "state": {k: simplify(v, seen, depth + 1)
                                   for k, v in st.items() if k != "_parent"}}
                    if obj._args:
                        r["args"] = simplify(list(obj._args), seen, depth + 1)
            else:
                r = {"__class__": name}
                if obj._args:
                    r["args"] = simplify(list(obj._args), seen, depth + 1)
                if st is not None:
                    r["state"] = simplify(st, seen, depth + 1)
                if obj._dict:
                    r["dict_items"] = simplify(obj._dict, seen, depth + 1)
                if obj._items:
                    r["list_items"] = simplify(obj._items, seen, depth + 1)
            seen.discard(oid)
            return r
    except Exception as e:
        return {"__unserializable__": type(obj).__name__, "repr": repr(obj)[:300],
                "error": repr(e)}
    return {"__repr__": repr(obj)[:300]}


def describe_top(v):
    if is_tensor(v):
        return "Tensor(%s, shape=%s)" % (tensor_dtype_name(v), list(v.shape))
    if isinstance(v, dict):
        return "dict(len=%d)" % len(v)
    if isinstance(v, (list, tuple)):
        return "%s(len=%d)" % (type(v).__name__, len(v))
    if isinstance(v, StubBase):
        return "stub<%s.%s>" % (v._pickle_module, v._pickle_name)
    if isinstance(v, (bool, int, float, str)) or v is None:
        return "%s = %r" % (type(v).__name__, v)
    return type(v).__name__


def main():
    global _ZF, _PREFIX
    os.makedirs(SPEC_DIR, exist_ok=True)
    os.makedirs(NPZ_DIR, exist_ok=True)

    _ZF = zipfile.ZipFile(CKPT_PATH, "r")
    names = _ZF.namelist()
    pkl_names = [n for n in names if n.endswith("/data.pkl") or n == "data.pkl"]
    if not pkl_names:
        raise RuntimeError("no data.pkl in zip; entries: %r" % names[:20])
    pkl_name = pkl_names[0]
    _PREFIX = pkl_name[: -len("data.pkl")]
    print("zip entries: %d, pickle: %s, prefix: %r" % (len(names), pkl_name, _PREFIX))

    with _ZF.open(pkl_name) as f:
        ckpt = CkptUnpickler(io.BytesIO(f.read())).load()

    # ---------------- 顶层结构 ----------------
    top_level = {}
    if isinstance(ckpt, dict):
        for k, v in ckpt.items():
            top_level[str(k)] = describe_top(v)
    else:
        top_level["<root>"] = describe_top(ckpt)
    print("top-level keys:")
    for k, v in top_level.items():
        print("  %-28s %s" % (k, v))

    # ---------------- state_dict 清单 ----------------
    state_dict = None
    if isinstance(ckpt, dict):
        for cand in ("state_dict", "model_state_dict", "model"):
            if cand in ckpt and isinstance(ckpt[cand], dict):
                state_dict = ckpt[cand]
                break
    if state_dict is None and isinstance(ckpt, dict) \
            and all(is_tensor(v) for v in ckpt.values()):
        state_dict = ckpt
    if state_dict is None:
        state_dict = {}

    inventory = OrderedDict()
    dtype_counter = Counter()
    total_params = 0
    total_elems = 0
    for k, v in state_dict.items():
        if is_tensor(v):
            dt = tensor_dtype_name(v)
            inventory[str(k)] = {"dtype": dt, "shape": list(v.shape),
                                 "numel": int(v.size)}
            dtype_counter[dt] += 1
            total_elems += int(v.size)
            if dt in ("float32", "float16", "float64", "bfloat16"):
                total_params += int(v.size)
        else:
            inventory[str(k)] = {"dtype": "<non-tensor:%s>" % type(v).__name__,
                                 "shape": None, "numel": 0}

    # 前缀分组统计（按前两级 '.' 组件）
    groups = OrderedDict()
    for k, meta in inventory.items():
        parts = k.split(".")
        g = ".".join(parts[:2]) if len(parts) > 2 else parts[0]
        e = groups.setdefault(g, {"count": 0, "numel": 0, "example_keys": []})
        e["count"] += 1
        e["numel"] += meta["numel"]
        if len(e["example_keys"]) < 4:
            e["example_keys"].append("%s %s %s" % (k, meta["dtype"], meta["shape"]))

    inv_doc = OrderedDict()
    inv_doc["checkpoint"] = CKPT_PATH
    inv_doc["zip_pickle_prefix"] = _PREFIX
    inv_doc["top_level_structure"] = top_level
    inv_doc["num_state_dict_keys"] = len(inventory)
    inv_doc["total_float_params"] = total_params
    inv_doc["total_elements"] = total_elems
    inv_doc["dtype_counts"] = dict(dtype_counter)
    inv_doc["prefix_groups"] = groups
    inv_doc["state_dict"] = inventory

    with open(INVENTORY_JSON, "w", encoding="utf-8") as f:
        json.dump(inv_doc, f, ensure_ascii=False, indent=2)
    print("wrote %s (%d keys, %d float params)"
          % (INVENTORY_JSON, len(inventory), total_params))

    # ---------------- hyper_parameters ----------------
    hp_key = None
    if isinstance(ckpt, dict):
        for cand in ("hyper_parameters", "hparams", "hyperparameters"):
            if cand in ckpt:
                hp_key = cand
                break
    if hp_key is not None:
        hp = simplify(ckpt[hp_key])
        with open(HPARAMS_JSON, "w", encoding="utf-8") as f:
            json.dump({"source_key": hp_key, "hyper_parameters": hp},
                      f, ensure_ascii=False, indent=2)
        print("wrote %s (from key %r)" % (HPARAMS_JSON, hp_key))
    else:
        print("no hyper_parameters key found")

    # ---------------- npz：全部权重数组 ----------------
    all_tensors = []
    walk_tensors(ckpt, "", all_tensors, set())
    npz_dict = OrderedDict()
    for path, t in all_tensors:
        key = path
        if key.startswith("state_dict."):
            key = key[len("state_dict."):]  # state_dict 内 key 原样
        base = key
        idx = 1
        while key in npz_dict:
            idx += 1
            key = "%s__dup%d" % (base, idx)
        npz_dict[key] = np.ascontiguousarray(np.asarray(t))
    np.savez(NPZ_PATH, **npz_dict)
    print("wrote %s (%d arrays)" % (NPZ_PATH, len(npz_dict)))

    # ---------------- stdout 摘要 ----------------
    print("\n=== prefix groups ===")
    for g, e in groups.items():
        print("%-40s count=%-3d numel=%d" % (g, e["count"], e["numel"]))
    print("\n=== dtype counts ===", dict(dtype_counter))
    stubs_used = sorted(_STUB_CACHE.keys())
    print("\n=== stub classes encountered (%d) ===" % len(stubs_used))
    for s in stubs_used:
        print("  " + s)


if __name__ == "__main__":
    main()
