# OBS Frontend Capture Compatibility Research

This document records the Phase 7 follow-up audit against the pinned OBS Studio
32.2.1 source at .deps/sources/obs-studio-32.2.1. The same source is available
from the official [OBS Studio 32.2.1 tag](https://github.com/obsproject/obs-studio/tree/32.2.1).

The question is whether a plugin-owned synchronized Recording/Replay backend can
remain bidirectionally synchronized with OBS frontend/global state without an OBS
patch, private ABI use, or duplicate stock capture.

## Conclusion

**E. OBS API EXTENSION REQUIRED.**

There is no clean plugin-only boundary that satisfies all of these requirements at
once:

- plugin-owned Recording and Replay are the actual backends;
- stock and external OBS commands are redirected before stock output work starts;
- active-state queries, output getters, paths, native status/timer, and standard
  frontend events describe the plugin-owned sessions;
- no stock Recording or Replay encoder/muxer runs in parallel; and
- no private frontend ABI or OBS source patch is used.

Plugin-owned controls can provide a coherent private lifecycle. Public OBS APIs can
observe stock lifecycle events and request stock lifecycle transitions. They cannot
replace the frontend-owned output references, publish externally-managed lifecycle
state, emit standard events, or intercept every native/internal intent before the
stock controller starts its output.

Production implementation is therefore stopped at this boundary. No fake standard
events, query overrides, hidden dummy outputs, Qt monkey-patching, or partial
reconciler is appropriate.

## Layers: intent, backend state, frontend state, notification

These are separate in stock OBS and must not be collapsed into one callback:

| Layer | Meaning | OBS 32.2.1 owner |
|---|---|---|
| Intent | A user, script, plugin, hotkey, or automatic policy requested start/stop/save | OBSBasic action/slot, frontend API method, or streaming policy |
| Backend state | A concrete obs_output_t and its encoders are starting, active, stopping, or stopped | BasicOutputHandler plus output signal callbacks |
| Frontend state | OBS global Recording/Replay active flags and controller state | recording_active, replaybuf_active, recordingStarted, and replayBufferActive |
| Notification | Events delivered to scripts, plugins, timers, widgets, and status UI | OBSBasic::OnEvent, Qt signals, output callbacks, and public event subscribers |

The plugin runtime currently owns its own consumer states and packet consumers. That
is valid for synchronized capture, but it does not make those states OBS frontend
state. A bridge must explicitly acknowledge each transition and preserve the origin
of the command so an acknowledgement cannot recursively issue the same command.

## Stock command-entry architecture

The relevant source files are linked to the official tag below; line numbers are
also recorded from the pinned checkout because the research runtime is built from
that checkout.

### Recording

    button/tray action
      -> OBSBasic::RecordActionTriggered()
      -> OBSBasic::StartRecording()/StopRecording()
      -> BasicOutputHandler::StartRecording()/StopRecording()
      -> obs_output_start(fileOutput)/obs_output_stop(fileOutput)
      -> fileOutput signal callbacks
      -> OBSBasic::RecordingStart/RecordStopping/RecordingStop()
      -> frontend events, Qt control signals, status bar, timer state, queries

The stock button and tray action are wired to RecordActionTriggered in
OBSBasic.cpp:308 and OBSBasic_SysTray.cpp:81. StartRecording validates the path
and disk space, emits RECORDING_STARTING at
OBSBasic_Recording.cpp:113-137, then calls the output handler. The output handler
starts the fixed fileOutput object; the output start, stopping, and stop signals
are connected in AdvancedOutput.cpp:202-204.

The public obs_frontend_recording_start() API does not supply an alternate backend:
OBSStudioAPI.cpp:239-247 queues the private OBSBasic::StartRecording or
StopRecording method. A caller from another plugin or script therefore enters the
same stock path.

### Replay Buffer

    button/tray action
      -> OBSBasic::ReplayBufferActionTriggered()
      -> OBSBasic::StartReplayBuffer()/StopReplayBuffer()/ReplayBufferSave()
      -> BasicOutputHandler::StartReplayBuffer()/StopReplayBuffer()
      -> obs_output_start(replayBuffer)/obs_output_stop(replayBuffer)
      -> replayBuffer signal callbacks
      -> OBSBasic::ReplayBufferStart/Stopping/Stop/Saved()
      -> frontend events, Qt control signals, status/tray state, queries

The stock controls are wired in OBSBasic.cpp:311-312; the tray action is wired in
OBSBasic_SysTray.cpp:82. StartReplayBuffer validates availability and path,
emits REPLAY_BUFFER_STARTING, and calls the output handler at
OBSBasic_ReplayBuffer.cpp:68-101. Save calls the stock output's "save" proc at
OBSBasic_ReplayBuffer.cpp:154-167; REPLAY_BUFFER_SAVED is emitted only after
the output's "saved" signal and a successful last-replay path lookup at
OBSBasic_ReplayBuffer.cpp:169-190.

The public obs_frontend_replay_buffer_start/save/stop() functions queue those same
private OBSBasic methods in OBSStudioAPI.cpp:295-308.

### Automatic Recording/Replay around streaming

After a successful stream start, OBSBasic_Streaming.cpp:109-119 reads
RecordWhenStreaming and ReplayBufferWhileStreaming, then calls the same
StartRecording and StartReplayBuffer controller methods. Stream stop and force
stop apply the corresponding keep-running settings and call StopRecording and
StopReplayBuffer at lines 156-210. There is no separate public plugin callback
before these calls.

### Native hotkeys

OBSBasic_Hotkeys.cpp:163-171 registers the stock Recording hotkey pair. Its
callbacks directly call StartRecording and StopRecording when the stock
RecordingActive and recordingStarted predicates allow them. The Replay pair is
registered at OBSBasic_Hotkeys.cpp:203-211 and directly calls the Replay controller
methods. HotkeyTriggered only routes the already-identified OBS hotkey through
OBS's hotkey machinery; it is not a plugin-facing pre-command interception hook.

The plugin can register separate frontend hotkeys, and the current product controls
replace the visible widgets, but neither action suppresses stock hotkey pairs,
frontend API callers, stream automation, or other built-in callers. Registering a
global hotkey-routing function would also affect unrelated hotkeys and still would
not intercept frontend API or stream-policy calls; it is not a safe compatibility
boundary.

## Stock events and state transitions

The stock sequences are:

    Recording start: RECORDING_STARTING
                     -> output start signal
                     -> RecordingStart
                     -> RECORDING_STARTED

    Recording stop:  output stopping signal
                     -> RecordStopping
                     -> RECORDING_STOPPING
                     -> output stop signal
                     -> RecordingStop
                     -> RECORDING_STOPPED

    Replay start:    REPLAY_BUFFER_STARTING
                     -> output start signal
                     -> ReplayBufferStart
                     -> REPLAY_BUFFER_STARTED

    Replay stop:     output stopping signal
                     -> ReplayBufferStopping
                     -> REPLAY_BUFFER_STOPPING
                     -> output stop signal
                     -> ReplayBufferStop
                     -> REPLAY_BUFFER_STOPPED

    Replay save:     ReplayBufferSave calls output "save"
                     -> output "saved" signal
                     -> ReplayBufferSaved
                     -> REPLAY_BUFFER_SAVED

BasicOutputHandler.cpp:74-143 sets the global active flags and queues the
controller callbacks from output signals. OBSBasic::OnEvent at
OBSBasic.cpp:2195-2200 calls the internal OBSStudioAPI::on_event method, which
iterates public subscribers at OBSStudioAPI.cpp:745-756.

The public header exposes obs_frontend_add_event_callback and
obs_frontend_remove_event_callback at obs-frontend-api.h:162-165, but no public
function to emit an event. OBSStudioAPI::on_event and obs-frontend-internal.hpp
are internal frontend implementation surfaces, not a supported plugin lifecycle
emitter.

## Plugin -> OBS findings

| Candidate | Finding | Full bridge result |
|---|---|---|
| Public frontend command/state API | Start/stop APIs invoke the stock OBSBasic path; active queries read stock atomics | Cannot select the plugin backend or publish plugin state |
| Register/replace frontend output | obs_output_create is public, but the frontend exposes only getters; no public setter/registration function exists | Cannot make a plugin output the frontend Recording/Replay output |
| Output signals | A plugin output can have ordinary libobs output signals and packet callbacks | Signals are not connected to private BasicOutputHandler callbacks or OBSBasic::OnEvent |
| Public controller/action path | Public API has no backend argument or controller injection point | Always selects stock output handler |
| Qt action triggering | Can trigger an action, but that follows the stock controller and stock output | Produces duplicate/stock capture, not substitution |
| Frontend-tools/other exported interfaces | They consume public events and queries or call public commands | No public state/event injection boundary |

The plugin can show a plugin-owned indicator and keep its own UI/hotkeys internally
consistent. It cannot truthfully cause native state queries, frontend callbacks,
stock timer plugins, and the native status bar to describe that private lifecycle.

## OBS -> plugin findings

The earliest supported observation differs by origin:

| OBS-originated command | Earliest usable public observation | Can prevent stock work? |
|---|---|---|
| Stock Recording hotkey | A plugin may observe resulting frontend events, or register a separate hotkey | No; the stock pair already dispatches its controller callback, and there is no public pre-start hook |
| Stock Replay/Save hotkey | Resulting events only; Save has no pre-save public event | No |
| obs_frontend_recording_start/stop | The API queues the private controller method | No |
| obs_frontend_replay_buffer_start/save/stop | The API queues the private controller method | No |
| Start-recording-when-streaming | StartStreaming calls StartRecording after stream setup | No public interception point exists before the call |
| Tray/menu/button actions | Private Qt/controller path | No public action replacement contract |

A public event observer is therefore too late for redirection: for a start event it
is downstream of validation and the stock start request, and for save there is only
a post-save completion signal. Allowing stock startup and stopping it after an event
would violate the no-duplicate-work requirement and could race the first encoded
packets.

## Output-substitution experiment

The focused source-level feasibility check used the public output model already
used by the repository's stock encoder probe:

1. obs_output_create can create an independent output object.
2. Public APIs can attach encoders, media, packet callbacks, and output signal
   handlers to that object.
3. obs_frontend_get_recording_output() and
   obs_frontend_get_replay_buffer_output() return references to
   BasicOutputHandler::fileOutput and BasicOutputHandler::replayBuffer, as shown
   by OBSStudioAPI.cpp:412-422.
4. No public call changes either reference. BasicOutputHandler.hpp declares those
   fields as frontend-owned state, and the concrete output classes create and wire
   them internally.
5. Starting an independently-created output can exercise libobs output callbacks,
   but it does not set recording_active/replaybuf_active, invoke
   OBSBasic::RecordingStart/ReplayBufferStart, update the native status bar, or
   emit standard frontend events.

Consequently, a runtime experiment that starts a dummy output would prove only an
independent output lifecycle, not frontend substitution. It would also be an
unacceptable production workaround: starting the stock output to obtain native UI
state would create the duplicate encoder/mux path the product explicitly forbids.
No dummy output was shipped or left running.

The repository's existing clean portable smoke run is the compatibility probe
available without UI automation: it observes plugin-owned idle initialization,
stock Recording/Replay inactivity, native control replacement, and restoration on
graceful shutdown. It does not claim a complete Start/Stop/Save event trace. The
source audit above supplies the missing command-flow evidence; a full action trace
cannot change the absence of a public substitution or pre-start hook.

## Native UI, status, timer, and scripts

The red Recording indicator and native status timer are not generic output signals:

- OBSBasic::RecordingStart calls OBSBasicStatusBar::RecordingStarted with the
  specific stock fileOutput pointer (OBSBasic_Recording.cpp:162-178).
- OBSBasicStatusBar::RecordingStarted stores a weak reference and activates a
  one-second refresh timer (OBSBasicStatusBar.cpp:52-87, 541-545).
- OBSBasicStatusBar::RecordingStopped clears that weak reference and deactivates
  the status (OBSBasicStatusBar.cpp:547-551).
- Native controls receive private Qt signals from OBSBasic; their state changes are
  implemented in OBSBasicControls.cpp:180-245.
- frontend-tools/output-timer.cpp:292-309 starts and stops its timer only from
  public frontend events and calls the public active/query APIs for commands.
- OBSBasicStats.cpp:208-212 initializes its recording timer from the public active
  query and subscribes to the public event stream.

Calling a plugin callback, changing a replacement button, or creating a separate
output cannot update all of these surfaces. Triggering the stock action would update
them, but it necessarily starts the stock backend.

## Public query behavior

The public header exposes the commands, active queries, output getters, current
record path, last recording path, and last replay path at
frontend/api/obs-frontend-api.h:182-206 and the path getters later in that file.
Their behavior for a plugin-owned session is:

| Public surface | Plugin-owned Recording | Plugin-owned Replay |
|---|---|---|
| obs_frontend_recording_active() | false unless stock Recording is also active | n/a |
| obs_frontend_replay_buffer_active() | n/a | false unless stock Replay is also active |
| obs_frontend_get_recording_output() | stock fileOutput, not plugin session | n/a |
| obs_frontend_get_replay_buffer_output() | n/a | stock replayBuffer, not plugin session |
| current recording output path | stock handler path field | n/a |
| obs_frontend_get_last_recording() | last stock recording path | n/a |
| obs_frontend_get_last_replay() | n/a | last stock replay path; plugin save is absent |
| obs_frontend_recording_paused/split_file/add_chapter | stock output semantics or false/unsupported for the private session | n/a |
| standard frontend event callbacks | no plugin lifecycle events | no plugin lifecycle or saved event |

This is an incompatible public state model, not a cosmetic discrepancy. Claiming
active events while these queries remain false would violate the requirement that
events, queries, paths, and native UI agree.

## Duplicate-work and loop findings

The only plugin-side loop prevention that can be implemented today is for the
plugin's private commands:

    plugin command -> plugin runtime transition -> plugin acknowledgement
                 -> no second plugin command

It cannot reconcile an OBS command that the plugin never receives before stock
startup. Calling a public OBS command from the plugin creates this unsafe shape:

    plugin UI -> plugin backend
              -> obs_frontend_recording_start()
              -> stock fileOutput starts too

Calling the public command only to obtain native events or state therefore creates a
second Recording encoder/muxer or Replay ring. Observing the resulting event and
stopping stock afterward is both too late and a duplicate-work race. Replacing a Qt
button does not address native hotkeys, scripts, tray actions, or stream-start
automation.

## Minimal OBS API extension

The smallest generic supported boundary is an externally-managed frontend capture
session, covering one Recording session and one Replay Buffer session. It should be
an OBS frontend API addition, not a project-specific synchronized-capture concept.

Conceptually:

    intent source
      -> frontend capture-session request/override
      -> selected external session owns backend
      -> frontend tracks lifecycle and associated obs_output_t/path
      -> native UI, timer, events, queries, and scripts use that session

The API needs these properties:

1. Selection before backend start. A registered external provider, or a per-request
   provider callback, must be consulted by stock Recording/Replay controller methods
   before StartRecording/StartReplayBuffer calls the stock output. The same boundary
   must cover frontend API calls, native hotkeys, tray and menu actions, and
   stream-start automation.
2. Bidirectional lifecycle operations. The frontend must accept provider
   notifications for starting, started, stopping, stopped, and Replay saved, with
   explicit success/failure and last-error/path data. The provider must be able to
   request the same state transitions for plugin-originated UI and hotkeys.
3. Frontend-owned state record. The frontend stores the active flag, pause state
   where applicable, provider/session identity, associated output reference, current
   path, last completed path, and transition state. Active queries and output/path
   getters read that record.
4. Event ownership and ordering. OBS emits the existing standard frontend events
   exactly once from the shared state transition, with the current ordering and
   failure semantics. REPLAY_BUFFER_SAVED is emitted only after the provider confirms
   a successful save and supplies the resulting path.
5. Output association without forced encoding. The provider may expose an
   externally-managed obs_output_t for query/diagnostic association, or explicitly
   report no output object. The frontend must not start or attach stock encoders just
   to make the getter non-null.
6. Threading and ownership. Provider callbacks may request transitions from worker
   threads, but frontend state mutation and event dispatch occur on OBS's frontend
   thread. The API retains/references provider/session/output data until the terminal
   event and defines shutdown cancellation and in-flight save behavior.
7. Failure semantics. A rejected provider start must produce a failed transition,
   leave frontend state inactive, emit no started event, and make the failure
   observable. A failed stop still reaches a defined stopped/error terminal state.
   A failed Replay save must not emit REPLAY_BUFFER_SAVED.
8. Compatibility. Existing plugins and scripts keep the stock provider as the
   default. Existing public commands remain valid; provider selection is additive,
   explicit, and discoverable rather than a hidden global replacement.

With that extension, a future reconciler can use a CommandOrigin such as Plugin,
OBS, ExternalFrontendCaller, or Shutdown, and a single frontend state machine can
acknowledge each transition without reissuing it. Until then, implementing that
reconciler in this plugin would only synchronize private state.

## Compatibility matrix

| Requirement | Plugin-only OBS 32.2.1 result |
|---|---|
| Plugin UI starts/stops plugin backend | Yes, privately |
| Plugin UI changes OBS global active queries | No |
| Native Recording/Replay hotkeys redirect before stock backend | No |
| Other frontend API callers redirect before stock backend | No |
| Start-recording-during-stream redirects | No |
| Standard start/stop events for plugin backend | No public emitter |
| REPLAY_BUFFER_SAVED for plugin save | No public emitter/path handoff |
| Native status indicator and timer describe plugin backend | No |
| Frontend output getters return plugin output | No public setter |
| No duplicate stock capture | Yes only if OBS state is left incompatible; no if stock is started for compatibility |
| Full bidirectional bridge | No |

The exact missing boundary is a generic OBS-managed external capture-session API as
specified above. Do not implement that OBS patch in this PR.
