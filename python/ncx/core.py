"""ctypes binding to ncx_core (core/include/ncx_core.h).

The DLL is located via $NCX_CORE_DLL, else core/build/Release/ncx_core.dll in this repository.
All functions return (status_name, report_dict[, payload]) and never raise for format errors;
they raise CoreError only for a missing library or a crash-level condition.
"""
import ctypes
import json
import os

_HERE = os.path.dirname(os.path.abspath(__file__))
_DEFAULT = os.path.join(_HERE, "..", "..", "core", "build", "Release", "ncx_core.dll")

STATUS = {0: "OK", 1: "NO_PAYLOAD", 2: "HEADER_UNREADABLE", 3: "CHUNK_MISSING", 4: "DIGEST_MISMATCH",
          5: "SCHEMA_TOO_NEW", 6: "INVALID_JSON", 7: "INVALID_MODEL", 8: "STEP_UNPARSEABLE",
          9: "BAD_ARGUMENT", 99: "INTERNAL"}


class CoreError(RuntimeError):
    pass


class Core:
    def __init__(self, path=None):
        path = os.path.abspath(path or os.environ.get("NCX_CORE_DLL") or _DEFAULT)
        if not os.path.exists(path):
            raise CoreError(f"ncx_core not built: {path} (run core/build.ps1)")
        self.path = path
        lib = ctypes.CDLL(path)
        c_char_pp = ctypes.POINTER(ctypes.c_void_p)
        size_p = ctypes.POINTER(ctypes.c_size_t)
        lib.ncx_version.restype = ctypes.c_char_p
        lib.ncx_schema_version.restype = ctypes.c_char_p
        lib.ncx_payload_write.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t,
                                          ctypes.c_char_p, c_char_pp, size_p, c_char_pp]
        lib.ncx_payload_read.argtypes = [ctypes.c_char_p, ctypes.c_size_t, c_char_pp, size_p, c_char_pp]
        lib.ncx_model_validate.argtypes = [ctypes.c_char_p, ctypes.c_size_t, c_char_pp]
        lib.ncx_fingerprint_compare.argtypes = [ctypes.c_char_p, ctypes.c_char_p, c_char_pp]
        lib.ncx_fingerprint_compare_ex.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, c_char_pp]
        lib.ncx_fingerprint_hash.argtypes = [ctypes.c_char_p, c_char_pp]
        lib.ncx_reference_match.argtypes = [ctypes.c_char_p, c_char_pp]
        lib.ncx_face_compare.argtypes = [ctypes.c_char_p, c_char_pp]
        lib.ncx_free.argtypes = [ctypes.c_void_p]
        for f in ("ncx_payload_write", "ncx_payload_read", "ncx_model_validate", "ncx_fingerprint_compare",
                  "ncx_fingerprint_compare_ex", "ncx_fingerprint_hash", "ncx_reference_match"):
            getattr(lib, f).restype = ctypes.c_int
        self.lib = lib

    # -------------------------------------------------------------------------- helpers
    def _take(self, ptr, length=None):
        """Copy a core-allocated buffer to bytes and free it."""
        if not ptr.value:
            return None
        data = ctypes.string_at(ptr.value, length) if length is not None else ctypes.string_at(ptr.value)
        self.lib.ncx_free(ptr)
        return data

    def _report(self, ptr):
        raw = self._take(ptr)
        return json.loads(raw.decode("utf-8")) if raw else {}

    # ------------------------------------------------------------------------------ API
    @property
    def version(self):
        return self.lib.ncx_version().decode()

    @property
    def schema_version(self):
        return self.lib.ncx_schema_version().decode()

    def payload_write(self, step_bytes, model_json, generator=None, chunk_chars=None, notice=None):
        """step_bytes: STEP file bytes (latin-1/UTF-8 as on disk); model_json: str or bytes.
        notice: None = the standard notice body, False = none, a string = its text."""
        if isinstance(model_json, str):
            model_json = model_json.encode("utf-8")
        opts = {}
        if generator:
            opts["generator"] = generator
        if chunk_chars:
            opts["chunk_chars"] = chunk_chars
        if notice is not None:
            opts["notice"] = notice
        out, out_len, rep = ctypes.c_void_p(), ctypes.c_size_t(), ctypes.c_void_p()
        st = self.lib.ncx_payload_write(step_bytes, len(step_bytes), model_json, len(model_json),
                                        json.dumps(opts).encode() if opts else None,
                                        ctypes.byref(out), ctypes.byref(out_len), ctypes.byref(rep))
        return STATUS.get(st, str(st)), self._report(rep), self._take(out, out_len.value)

    def payload_read(self, step_bytes):
        out, out_len, rep = ctypes.c_void_p(), ctypes.c_size_t(), ctypes.c_void_p()
        st = self.lib.ncx_payload_read(step_bytes, len(step_bytes), ctypes.byref(out), ctypes.byref(out_len),
                                       ctypes.byref(rep))
        return STATUS.get(st, str(st)), self._report(rep), self._take(out, out_len.value)

    def validate(self, model):
        raw = model if isinstance(model, (bytes, str)) else json.dumps(model)
        raw = raw.encode("utf-8") if isinstance(raw, str) else raw
        rep = ctypes.c_void_p()
        st = self.lib.ncx_model_validate(raw, len(raw), ctypes.byref(rep))
        return STATUS.get(st, str(st)), self._report(rep)

    def fingerprint_compare(self, expected, actual, options=None):
        """options: tolerance profile/overrides, e.g. {"profile": "exchange"}; None = native."""
        rep = ctypes.c_void_p()
        self.lib.ncx_fingerprint_compare_ex(json.dumps(expected).encode(), json.dumps(actual).encode(),
                                            None if options is None else json.dumps(options).encode(),
                                            ctypes.byref(rep))
        return self._report(rep)

    def fingerprint_hash(self, fingerprint):
        out = ctypes.c_void_p()
        self.lib.ncx_fingerprint_hash(json.dumps(fingerprint).encode(), ctypes.byref(out))
        return (self._take(out) or b"").decode()

    def match(self, request):
        out = ctypes.c_void_p()
        self.lib.ncx_reference_match(json.dumps(request).encode(), ctypes.byref(out))
        return self._report(out)

    def face_compare(self, request):
        out = ctypes.c_void_p()
        self.lib.ncx_face_compare(json.dumps(request).encode(), ctypes.byref(out))
        return self._report(out)


_core = None


def core():
    """Process-wide Core instance."""
    global _core
    if _core is None:
        _core = Core()
    return _core
