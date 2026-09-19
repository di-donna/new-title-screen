# 1AU-fix: verified candidate repair

This branch packages the tested entrance repair for **Dawn 0.1.3's 1AU candidate**.
It fixes the Ghost console, bridge presentation, exit door, and the single early
pipe-crossing enemy found on the undeployed bridge.

**This is a candidate repair package, not a completed source integration.** The
published Dawn source does not contain 1AU. The downloaded candidate came from an
unpublished `codex/integrate-1au` integration based on `c18b39945abe733b6c274aa42a710b8088d6972f`.
That source must be supplied to finish the normal C++ integration. Building Dawn
from this branch today does **not** produce the complete 1AU repair.

## Make a tester package

Use Windows PowerShell 5.1 or newer and an extracted, unchanged
`Dawn-0.1.3-1au-candidate` installer package:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\one_au_fix\New-1AUFixCandidate.ps1 `
  -CandidateRoot 'C:\path\to\Dawn-0.1.3-1au-candidate'
```

The output is `build/releases/Dawn-0.1.3-1au-fix.zip`. An optional
`-OutputDirectory` selects another new output folder. Existing folders/ZIPs are
never overwritten. The script validates every original payload file, patches a
copy of the DLL in memory, verifies the full final hash, and regenerates the
release manifest. It does not modify an installation or running process.
Publishers can pass `-SourceCommit` with the full Git commit ID; the package records
it in `repair-source/build.json` alongside the recipe and DLL hashes.

The original candidate DLL must have SHA-256:

`403dc4bbddd6235273f3e5bd15268054b89852104111ca96e4109e85d7cfec1c`

The resulting DLL must have SHA-256:

`be08e2a8b73f661fe291998f6ac6b3f36838dfa4bcab9f28f24d5e0d43779f0f`

The recipe rejects rebuilt or otherwise different DLLs. Do not weaken that check
or reuse the patch offsets against a new build. Follow [SOURCE-PORT.md](SOURCE-PORT.md)
to finish the source integration so subsequent DLL builds retain the fixes.

Share the complete generated ZIP and [TESTING.md](TESTING.md). The DLL itself is
an output artifact, not a committed file. No game packages, player settings,
process dumps, account data, or runtime logs are included in the Git changes.

The root [GPL license](../../LICENSE) applies to the repair tools and reference
code. The input candidate retains its existing third-party license files.

## Package validation

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\one_au_fix\Test-1AUFixCandidate.ps1 `
  -CandidateRoot 'C:\path\to\Dawn-0.1.3-1au-candidate'
```

This verifies all 45 payload files, the DLL inside the ZIP, preservation of the
input and unrelated payload, and rejection of existing outputs, nested output
paths, and other DLL builds. It writes test artifacts under ignored `build/unit`.
