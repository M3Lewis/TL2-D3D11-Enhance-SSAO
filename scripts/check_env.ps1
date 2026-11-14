$ErrorActionPreference = "Stop"

$GameDir = "H:\Steam\steamapps\Common\Torchlight II"

Write-Host "=== [Phase 0 Audit] Process & Binary Baseline ===" -ForegroundColor Cyan
$proc = Get-Process Torchlight2 -ErrorAction SilentlyContinue
if ($proc) {
    Write-Warning "Torchlight2.exe is currently running (PID $($proc.Id))."
} else {
    Write-Host "[OK] Torchlight2.exe is not running." -ForegroundColor Green
}

@'
import os, hashlib
gd = r"H:\Steam\steamapps\Common\Torchlight II"
prog = os.path.join(gd, "programs")
van = os.path.join(gd, "programs_vanilla_reference")

print("\n--- Shader Reference Status ---")
dirty = []
clean = []
for dp, _, fns in os.walk(van):
    for fn in sorted(fns):
        vf = os.path.join(dp, fn)
        rel = os.path.relpath(vf, van)
        pf = os.path.join(prog, rel)
        if not os.path.exists(pf):
            dirty.append((rel, "MISSING"))
            continue
        hv = hashlib.sha256(open(vf, "rb").read()).hexdigest()
        hp = hashlib.sha256(open(pf, "rb").read()).hexdigest()
        if hv == hp:
            clean.append(rel)
        else:
            dirty.append((rel, "MODIFIED"))

print(f"Vanilla matched: {len(clean)}")
for r in clean:
    print(f"  [CLEAN] {r}")
print(f"Non-vanilla (needs Phase 0 revert): {len(dirty)}")
for r, st in dirty:
    print(f"  [{st}] {r}")
'@ | python -

Write-Host "`n--- Plugins_D3D11.cfg ---" -ForegroundColor Cyan
Get-Content -LiteralPath (Join-Path $GameDir "Plugins_D3D11.cfg")
