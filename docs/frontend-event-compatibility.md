# Frontend Event Compatibility

This document records the OBS Studio 32.2.1 compatibility audit for the
plugin-owned Recording and Replay lifecycle introduced by Phase 7.

## Conclusion

OBS frontend lifecycle events are not emitted by the visible button itself. The
stock buttons and hotkeys enter `OBSBasic` controller methods, which start and stop
stock `obs_output_t` instances. The controller and output-signal callbacks then
dispatch the public frontend events.

Phase 7 replaces both the visible controls and the stock Recording/Replay backend.
The plugin-owned controls and hotkeys therefore do not reach those stock event
dispatch points.

OBS 32.2.1 has a public `obs_frontend_add_event_callback` subscription API, but no
public event-emission API. The internal `OBSStudioAPI::on_event` method and
`obs-frontend-internal.hpp` callback interface are not supported plugin APIs. This
follow-up deliberately does not call them, patch OBS, or claim event compatibility
through unrelated custom callbacks.

The resulting compatibility level is partial:

- stock OBS workflows retain their normal events and public state/query behavior;
- plugin-owned workflows do not produce standard frontend lifecycle events;
- plugin-owned UI and hotkeys remain consistent with each other because both enter
  the same `PluginCaptureRuntime` control path;
- no duplicate standard events are generated.

## Stock event ownership

The audit used the pinned source at `.deps/sources/obs-studio-32.2.1`.

| Event | Stock source and trigger | Ownership after Phase 7 |
|---|---|---|
| `RECORDING_STARTING` | `frontend/widgets/OBSBasic_Recording.cpp`, `OBSBasic::StartRecording`, after validation and before `outputHandler->StartRecording()` | OBS stock path only |
| `RECORDING_STARTED` | `OBSBasic::RecordingStart`, reached by the stock recording output `start` signal through `OBSStartRecording` | OBS stock path only |
| `RECORDING_STOPPING` | `OBSBasic::RecordStopping`, reached by the stock recording output `stopping` signal through `OBSRecordStopping` | OBS stock path only |
| `RECORDING_STOPPED` | `OBSBasic::RecordingStop`, reached after the stock output stops; emitted for success and failure completion codes | OBS stock path only |
| `REPLAY_BUFFER_STARTING` | `frontend/widgets/OBSBasic_ReplayBuffer.cpp`, `OBSBasic::StartReplayBuffer`, before `outputHandler->StartReplayBuffer()` | OBS stock path only |
| `REPLAY_BUFFER_STARTED` | `OBSBasic::ReplayBufferStart`, reached by the stock replay output `start` signal through `OBSStartReplayBuffer` | OBS stock path only |
| `REPLAY_BUFFER_STOPPING` | `OBSBasic::ReplayBufferStopping`, reached by the stock replay output `stopping` signal through `OBSReplayBufferStopping` | OBS stock path only |
| `REPLAY_BUFFER_STOPPED` | `OBSBasic::ReplayBufferStop`, reached by the stock replay output `stop` signal through `OBSStopReplayBuffer` | OBS stock path only |
| `REPLAY_BUFFER_SAVED` | `OBSBasic::ReplayBufferSaved`, reached by the stock replay output `saved` signal through `OBSReplayBufferSaved`; it checks the stock buffer is active and obtains the stock last-replay path before dispatch | OBS stock path only |

The stock recording and replay controllers also emit internal Qt signals such as
`RecordingStarted`, `RecordingStopping`, `RecordingStopped`, `ReplayBufStarted`,
`ReplayBufStopping`, and `ReplayBufStopped`. These are `OBSBasic` implementation
signals used by native widgets and are not a public plugin event-emission surface.

The relevant backend bridge is in
`frontend/utility/BasicOutputHandler.cpp`: stock output signals are converted to
queued calls such as `RecordingStart`, `RecordStopping`, `RecordingStop`,
`ReplayBufferStart`, `ReplayBufferStopping`, `ReplayBufferStop`, and
`ReplayBufferSaved`. Replacing only a button would not remove this behavior;
bypassing the stock output lifecycle does.

## Public API and state-query audit

`frontend/api/obs-frontend-api.h` publicly provides:

- `obs_frontend_add_event_callback` and `obs_frontend_remove_event_callback`;
- commands such as `obs_frontend_recording_start()` and
  `obs_frontend_replay_buffer_start()`;
- `obs_frontend_recording_active()` and `obs_frontend_replay_buffer_active()`;
- stock output references from `obs_frontend_get_recording_output()` and
  `obs_frontend_get_replay_buffer_output()`;
- stock path getters including `obs_frontend_get_current_record_output_path()`,
  `obs_frontend_get_last_recording()`, and `obs_frontend_get_last_replay()`.

The corresponding `OBSStudioAPI` implementation reads stock frontend state:

- `obs_frontend_recording_active()` reads the `recording_active` atomic set by the
  stock recording output callbacks;
- `obs_frontend_replay_buffer_active()` reads the `replaybuf_active` atomic set by
  the stock replay output callbacks;
- output getters return `fileOutput` and `replayBuffer` from
  `BasicOutputHandler`;
- last-path getters return `BasicOutputHandler::lastRecordingPath` and
  `OBSBasic::lastReplay`.

Plugin-owned capture does not mutate those stock objects. Consequently, during a
plugin-owned Recording or Replay session, third-party code querying the public
stock APIs will observe the stock backend as inactive and will not receive the
plugin's output references or paths. The plugin must not claim state-query or
output/path compatibility that it cannot provide.

## Script and plugin observers

OBS's frontend-tools scripts plugin registers the same public callback API used by
native plugins. Lua scripts can use `obs.obs_frontend_add_event_callback`, and the
Python/script bindings receive the same frontend event stream. Because the public
API only subscribes to OBS's dispatch stream, scripts naturally receive stock
events but there is no supported way for this plugin to inject standard events for
its private lifecycle.

Plugin-owned UI and plugin-owned hotkeys both call the same runtime methods. They
therefore have identical plugin state transitions, but neither path can produce
the standard OBS callback sequence without a supported frontend emitter.

## Event ordering and failure semantics

The stock semantic sequences audited in source are:

```text
Recording:
  RECORDING_STARTING -> stock output start -> RECORDING_STARTED
  RECORDING_STOPPING -> stock output stop  -> RECORDING_STOPPED

Replay:
  REPLAY_BUFFER_STARTING -> stock output start -> REPLAY_BUFFER_STARTED
  REPLAY_BUFFER_STOPPING -> stock output stop  -> REPLAY_BUFFER_STOPPED
  save request -> stock output saved callback -> REPLAY_BUFFER_SAVED
```

The stock saved event is not emitted when the replay buffer is inactive, and a
failed or absent save completion callback does not produce `REPLAY_BUFFER_SAVED`.
The stock stopped event is the completion notification even when the output reports
a failure code; consumers must inspect their other error surfaces for success.

The plugin control state follows analogous Starting/Running/Stopping/Saving states
internally, but it intentionally does not translate those states into standard
frontend events. This prevents false success events and avoids inconsistent public
state queries. Shutdown stops the plugin runtime and restores the native controls;
it cannot emit standard stock stop events for a backend that is not active.

## Runtime observer result

The existing plugin frontend callback registration was used as a public callback
observer during clean portable OBS startup and graceful shutdown. The run showed
the expected frontend loading/exit lifecycle, plugin-owned idle initialization, and
no stock Recording/Replay output activity. The adapter then restored all three
native controls on shutdown. Because the current callback only consumes the
frontend loading/cleanup/exit boundaries, it is not treated as a full event
recorder.

An end-to-end observer sequence for plugin-owned Start/Stop/Save actions was not
claimed: the environment's Windows UI automation bridge was unavailable, and the
public API audit proves there is no supported event-emission path to validate.
Stock event sequences remain source-grounded in the controller/output handlers
listed above. No private API or temporary compatibility emitter was added.

## Compatibility matrix

| Surface | Recording | Replay | Result |
|---|---|---|---|
| Frontend START/STOP events | Stock lifecycle only; plugin lifecycle unavailable | Stock lifecycle only; plugin lifecycle unavailable | Partial |
| `REPLAY_BUFFER_SAVED` | n/a | Stock save completion only; plugin save unavailable | Partial |
| `*_active()` state query | Reports stock `fileOutput`, not plugin Recording | Reports stock `replayBuffer`, not plugin Replay | Incompatible for plugin-owned lifecycle |
| Stock output getter | Stock recording output only | Stock replay output only | Incompatible for plugin-owned files |
| Last/current output path | Stock path fields only | Stock last-replay field only | Incompatible for plugin-owned files |
| Script frontend callbacks | Receives stock events | Receives stock events | No plugin-event propagation |
| Plugin frontend callbacks | Can observe stock events | Can observe stock events | No public injection |
| Native UI | Replaced by plugin-owned control | Replaced by plugin-owned controls | Intentionally plugin-owned |
| Hotkeys | Plugin-owned path | Plugin-owned path | Compatible with plugin UI behavior, not stock event APIs |

Any future compatibility implementation must wait for a supported OBS event-emission
extension or a deliberately approved OBS API change. Until then, standard events
remain owned by stock OBS and plugin-owned lifecycle notifications remain private to
the plugin control/logging surface.
