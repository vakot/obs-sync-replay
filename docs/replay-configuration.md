# OBS Replay Buffer Configuration Integration

The plugin treats the active OBS profile's Replay Buffer settings as its
configuration source. It does not start, stop, or consume OBS's stock
`replay_buffer` output. The plugin-owned capture engine remains the only replay
backend, and the OBS values are translated into one shared `ReplayConfiguration`.

## Profile values

The adapter reads the public profile config returned by
`obs_frontend_get_profile_config()` after frontend loading and on profile
lifecycle/config refreshes. Values use OBS's profile keys and units:

| Output mode | Enabled | Duration | Memory | Defaults |
| --- | --- | --- | --- | --- |
| Simple | `SimpleOutput/RecRB` | `SimpleOutput/RecRBTime` seconds | `SimpleOutput/RecRBSize` MB | disabled, 20 seconds, 512 MB |
| Advanced | `AdvOut/RecRB` | `AdvOut/RecRBTime` seconds | `AdvOut/RecRBSize` MB | disabled, 20 seconds, 512 MB |

The adapter also applies the stock backend availability rules:

- Simple `RecQuality=Lossless` has no stock replay backend, so plugin replay is unavailable.
- Simple `RecQuality=Stream` passes an unlimited stock replay size; the plugin records that the stock memory limit does not apply.
- Advanced `RecType=FFmpeg` has no stock replay backend, so plugin replay is unavailable.
- Advanced `RecEncoder=none` selects `streamEncoder.json`; other encoders select `recordEncoder.json`. A `CBR`, `VBR`, or `ABR` rate control means the stock replay size is unlimited; other rate controls use `RecRBSize`.

OBS's MB value is converted with `1024 * 1024`. The converted value is one
global bound over the plugin's retained encoded packets for Master, Scene A,
and Scene B. This matches the stock replay output's single packet-queue budget.
Eviction removes a common temporal prefix and then removes partial leading GOPs
from every stream, so Save Replay always sees one common range.

When OBS's selected mode is unlimited, the plugin uses the explicit 30 MiB
emergency bound. This is a boundedness safeguard, not a claim that the stock
backend has a 30 MiB file-size limit; `memory_limit_configured` is false in this case.

## Availability and transitions

`replayAvailable` is separate from Recording state. If OBS configuration makes
replay unavailable, the plugin hides Replay and Save Replay controls, keeps
Recording visible and usable, and rejects replay start/save commands with the
observable reason `replay-unavailable-by-obs-config`. Plugin hotkeys are not
modified. Re-enabling the setting makes the controls available but never
auto-starts replay.

If replay is active when the setting becomes unavailable, the plugin waits for
any save, stops its replay consumer, disables retention, releases replay-only
encoders, and leaves Recording active. This does not invoke the stock OBS replay
backend.

The adapter refreshes at frontend finished-loading, profile changes, and before
hotkey commands. OBS 32.2.1 exposes no public settings-applied frontend event,
so the existing 250 ms plugin control refresh timer is the least invasive
fallback for settings changed in the Output settings dialog. Profile changing
stops plugin-owned resources before replacement; profile changed refreshes or
recreates the plugin-owned runtime.

The configured duration is one shared target for all participating replay
streams. Save Replay selects one `ReplaySnapshot` range and passes that same
range to both MKV outputs. No output-specific duration, offset, sleep, or
post-hoc drift correction is introduced.

## Synchronization impact

This integration changes configuration and retention policy, not temporal
identity. Master frame ID and PTS remain assigned by the existing shared timing
path. The affected guarantees are:

1. one configured duration is used for the paired save;
2. one global memory budget evicts a common temporal prefix;
3. disabling replay cannot leave a live replay consumer behind; and
4. unavailable or unsupported OBS configurations fail explicitly and are logged.

The focused configuration, control, UI, and capture-session tests cover these
transitions. Portable runtime validation checks the disabled-profile path,
control replacement/visibility, zero startup encoders, and clean restoration;
actual plugin-owned Start/Save actions still require the manual OBS acceptance
workflow described in `docs/testing.md`.
