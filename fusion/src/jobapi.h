// The interface between the add-in stub (NCX.dll) and the implementation (NcxFusionImpl.dll).
//
// Every request is a JSON object {"cmd": ..., ...}; the reply is a JSON object allocated by the
// implementation and released with ncxf_free. Calls are made on Fusion's main thread only.
#pragma once

extern "C" {
// Run one request; returns a JSON reply (never null). Never throws.
typedef char* (*ncxf_job_fn)(const char* request_json);
// Release a reply.
typedef void (*ncxf_free_fn)(char* p);
// Called once before the implementation is unloaded (release handlers, Ptr<> globals).
typedef void (*ncxf_unload_fn)();
}
