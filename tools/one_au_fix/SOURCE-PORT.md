# Source integration required for future DLL builds

## Base and status

The reference 1AU implementation is `isinternets/evil-ass-repo-of-doom-and-despair`
at `04d37b229031c663b0117398ba12b27b9499fb0d`, branch `1AU-UnEx`.
The downloaded Dawn candidate adapted it to production base
`c18b39945abe733b6c274aa42a710b8088d6972f` on an unpublished integration branch.
The exact integration source is not present in the published Dawn repository or
the downloaded installer. This `1AU-fix` branch is based on published Dawn commit
`41b493221a08ee3d1a4db9b03342d1b77802437f`; it does not add the missing mission.

**Do not claim the source port is complete.** Obtain that integration checkout,
then apply the following changes on `1AU-fix`. Keep the three changesets separate
for review; publish only the test branch, with no production merge.

## 1. Retain region legs and accept partial roster deltas

`source-port/region-membership.patch` changes the current/pending native-leg
condition in `Dawn/src/server/bap/encrypted/activity_message/membership/activity_membership_route.h`
to accept exactly `mission_ember` as well as `mission_launchpad`. The held-region
policy remains unchanged. Test the real `make_authoritative` mapping, subsequent
state merge, and outgoing membership publication with nonempty legs and an
unrelated destination control.

`source-port/partial-roster.patch` ports the verified parser correction in
`Dawn/src/middleware/bap/activity_message/activity_sense_update_parser_other_missions.cpp`.
Top-level bodies, bubble identity, keys, masks, and state lists may independently
be absent. Consume valid deltas, publish acknowledgement entries only with a
complete identity/body, and retain all count/capacity/truncation checks. Do not
invent a bubble identity when the delta omits it.

Run the complete activity-sense parser suite and add generated partial-field,
unknown-bubble, capacity, state-count mismatch, and truncation cases. Preserve
complete Towerfall and other-mission captures. Keep warnings-as-errors enabled;
the reference checkout's temporary test-project warning relaxation is excluded.

## 2. Port console ownership and entrance device handling

Use named 1AU request/frame fields and existing native-reference helpers. **Do not
copy Dawn-DLL RVAs or Frame byte offsets into C++**: the candidate ABI differs from
the public reference. Native game RVAs below are for game build 86657 and require
the project's existing supported-build/signature gates.

The `reference/` assembly files record the exact tested conditions and native
operations. They are evidence for the C++ implementation, not a post-link step.

### Console

`reference/owner-fix.asm` runs through the existing gated E4A590 sensor observer.
Scope it to the enabled, unfinished bridge interaction and its current command
generation. Validate sensor `80B3C98F/80804D32/+258`, authored link `811C9DC5`,
the salted controller reference, and the current compaction-adjusted controller
`80C3D38C/80804D3A/+358`. Validate the controller revision, full entity handle,
row flags, and the request again immediately before granting local authority.
Set only that entity's authority bit. Preserve native Ghost timing, receipt
delivery, callback arguments/return values, and other missions.

### Door and duplicate bridge

`reference/entrance-v5-main.asm` and `entrance-v4-tail.asm` record the combined
observer. Run from an already-active native device callback because the two
automatic doors can be asleep and never invoke their own callback.

- Enabled, unfinished, nonfaulted landing/bridge/processing frames admit bridge
  cleanup. The **restricted/no-respawn flag must not disable the observer**.
- During bridge/processing, grant authority only to placement table `80C32CEE`,
  record 0 / identifier `30DB525724EDDB9F`, or record 1 / identifier
  `AB48F4FA0B44B151`. These are two distinct doors, not duplicates. Let native
  power, locks, proximity, and animation decide whether they open.
- For bridge table `80F0C055`, records `91,92,93,102,108,124`, retire an unowned
  entity only when a distinct live owned entity has the same table, record, and
  full authored identifier. Validate salted handles against their row indices.
- Skip invalid, retiring or uninitialized rows (`flags & 5`) and invalid component
  bundles (`FFFFFFFF`). Continue scanning after an incomplete candidate. Never
  retire the entity whose callback is executing; recheck ownership immediately
  before retirement and retire at most one duplicate per callback.
- Native entity retirement is game RVA `56A8F0`. Let compiled C++ generate the
  call frame/unwind metadata. The earlier unsafe version called retirement with
  an invalid bundle and crashed in native game RVA `5979C4`; keep that regression.

Add isolated tests using synthetic native rows: all row indices, stale handles,
wrong placement identities, absent owners, reverse duplicate order, invalid
bundles, callback-self exclusion, and every enabled/finished/restricted/fault
combination. Verify forwarding and other-state preservation. Integrate through
the existing hook lifecycle and quiescence checks rather than a detached timer.

## 3. Remove the identified early pipe enemy

`source-port/pipe-enemy.patch` changes `one_au/mission.h`:

```cpp
inline constexpr auto kPipes = cohort_members<kBridge>({59});
```

Remove source 58 (`80B3C989`, `pipe_crossing_support_a_squad`, count 1) from the
shared spawn/clearance roster. Keep source 59. Keep the Mercury roster
`{40,51,52,53,54,55}` unchanged. Live actor source resolution and the admission
ledger independently identified source 58; removing Mercury source 53 was an
earlier incorrect trial and is not part of the final repair.

Test the actual cohort spawn and clearance functions so an omitted enemy cannot
become an unmet kill requirement. Check the adjacent encounters are unchanged.

## Build and acceptance

Build the complete integration in Release x64, run the affected parser,
membership, native-reference, hook-lifecycle and 1AU regression suites, and
generate an installer with a new manifest and commit ID. Use the reconstructed
candidate only as comparison evidence. Run [TESTING.md](TESTING.md) with a cold
process start and repeated mission loads before promoting the rebuilt artifact.
The runtime patch's successful playtest is not a playtest of newly compiled C++.
