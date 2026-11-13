# T session format (contract between C++ and Python)

Write this before either side. Decisions to make:

## Session directory layout
- `session.json`  : metadata
- black level image, floodlit image
- T data (see below)

## Metadata fields (suggested)
projector resolution, camera resolution, pattern type + parameters,
block size, exposure / gain settings, camera backend, timestamps, format version.

## T storage
- [ ] Layout: COO triplets / CSC columns / blocks per pattern?
- [ ] dtype (float32?), endianness, per-channel (RGB) handling
- [ ] Hierarchical levels (T_1 ... T_k): one file per level?
- [ ] Index conventions (row-major pixel order, 0- or 1-based)

## Versioning
Include a format version in `session.json`; bump on any breaking change.
