# Panda3D DC parser + CConnectionRepository (vendored)

Source: [panda3d/panda3d](https://github.com/panda3d/panda3d) tag `v1.10.16`
(commit `e560eb8da370d86b096d6cb65eafd9ab40879b9a`), files
`direct/src/dcparser/*` and `direct/src/distributed/{cConnectionRepository,config_distributed}.*`.
License: Panda3D's modified BSD license, see `LICENSE`.

## Why it is here

Panda3D's distributed networking has a C++ core (`DCFile`, `DCClass`, `DCPacker`,
`CConnectionRepository`), but in the 1.10 SDKs it is compiled into the *Python*
extension module (`panda3d/direct.pyd` / `direct.so`), not into `libp3direct`.
`makepanda.py` says so:

> These are part of direct.pyd, not libp3direct.dll, because they rely on the Python
> libraries. If a C++ user needs these modules, we can move them back and filter out
> the Python-specific code.

That is exactly what this folder does for the C++ client.

## The one change

`tools/vendor_panda3d_direct.py` copies the files and renames `HAVE_PYTHON` to
`ARENA_DC_WITH_PYTHON` (never defined), which removes the Python bindings of these
classes. The bison/flex outputs are the `*.prebuilt` files shipped with Panda3D. Nothing
else is modified. The rest of Panda3D keeps the SDK's own `HAVE_PYTHON`, so the layouts of
SDK classes (e.g. `GraphicsWindow`) match the binaries exactly, and no Python headers are
needed to build the client.

To update to a newer Panda3D, run `python tools/vendor_panda3d_direct.py <panda3d-source>`.
