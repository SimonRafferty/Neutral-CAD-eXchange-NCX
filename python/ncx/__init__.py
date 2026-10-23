"""Python side of the neutral CAD translator.

- core:      ctypes binding to the C++ ncx_core library (the one implementation of the format logic)
- specread:  a small independent container reader/writer written from FORMAT-SPEC.md, used by the
             conformance tests as the second implementation
- occhost:   an OpenCASCADE "host": computes descriptors, candidate distances and fingerprints the way
             a CAD add-in does through its own API
"""
