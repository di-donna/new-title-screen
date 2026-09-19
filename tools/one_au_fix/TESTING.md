# 1AU-fix tester notes

Experimental repair of the entrance sequence in Dawn `0.1.3-1au-candidate`,
for Destiny 2 build `86657.20.08.23.1800.d2_rc`.

## Install and verify

1. Fully exit Destiny 2. Extract the complete tester ZIP.
2. Use its `Update-Dawn.cmd` to preserve an existing Dawn save and settings.
   The bundled installer verifies the release manifest and installs both runtime
   DLL locations. Keep its backup receipt.
3. Start the game again and launch 1AU from the beginning.
4. Before using the console, verify that the bridge is retracted/hidden and the
   early enemy is absent from its future position.
5. Clear the console encounter, hold E, and verify the Ghost scans the console.
6. Verify that the bridge unfolds without a second static bridge appearing and
   that the restricted respawn zone activates.
7. Cross the bridge and verify the exit door opens normally.
8. Return to orbit and repeat a fresh mission load. Then test another mission.

The patch removes only registry `F6FFB59E`, source **58**
(`pipe_crossing_support_a_squad`). It preserves source 59 and the full Mercury
console encounter, including source 53.

Expected installed `steam_api64.dll` SHA-256 in both the game root and `bin/x64`:

`be08e2a8b73f661fe291998f6ac6b3f36838dfa4bcab9f28f24d5e0d43779f0f`

## What has been verified

The original tester confirmed the Ghost scan, unfolding bridge without a static
duplicate, opening door, absence of the early enemy, and fresh mission reload.
Stepping onto the bridge's future location before activation caused the player
to fall through at the tested spot.

That gameplay verification used a live-updated client. A **full process restart
with the complete final repair remains a tester check**, as does the rest of the
mission. The DLL passed isolated machine-code and Windows loader/unwind checks.
Those automated checks do not establish an uninterrupted full playthrough.

One failed reload was reported. The client process remained alive and no retained
fatal-process record established the cause. Report any recurrence with the error
code, whether the game froze/closed/returned to orbit, and whether it happened
before or after using the console. Do not label this patch a general crash fix.

## Reporting results

Include the tester package name, DLL hash, game build, whether it was a cold
startup, and the numbered step that failed. A short clip of the bridge/door is
useful. Remove account details from any logs before sharing them.

This is an exact-candidate patch. A new DLL from another build must receive the
source port; applying these offsets to it is unsupported.
