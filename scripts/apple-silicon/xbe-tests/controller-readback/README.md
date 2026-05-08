# controller-readback — Tier-2 input preflight

This XBE records what the title-facing controller stack sees on a real Xbox
after chainload. It uses nxdk SDL's Xbox controller backend, which rides the
USB/XID path rather than the oracle agent's shared synthetic-input buffer.

Build:

```sh
eval "$(/Users/jbbrack03/XEMU_MacOS/nxdk/bin/activate -s)" && make
```

Artifacts written on Xbox:

- `D:\controller-readback.txt`
- `D:\controller-readback-done.txt`

Mac-side preflight:

```sh
python3 ../../controller-readback-validate.py \
  --xbe 'E:\Apps\controller-readback\default.xbe' \
  --ftp-collect /E/Apps/controller-readback
```

Default mode is record-only. For the next Tier-2 session, run the read-only
`tier2-shim-preflight.py` gate first, then use this XBE to prove the
NKPatcher-style no-op/counter hook before any input mutation. Once a safe
retail-input hook exists, pass `--expect key=value` checks to make this a hard
gate.
