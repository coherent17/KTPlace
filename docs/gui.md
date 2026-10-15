# How the GUI embeds the engine

`ktplace-gui` is the Qt window: the layout view on top, the Log and Tcl Console
docked underneath, with Properties, Metrics and Runs around them. This
note explains how it drives the same placement code as the command line, which
threads exist, and how to add to it. For starting and viewing it, see
[docker.md](docker.md#5-the-desktop-gui).

## One session, two front ends

```
            ktplace (CLI)                         ktplace-gui
                 |                                     |
          FlowMgr::run()                    menu, toolbar, typed command
     (a fixed sequence of calls)                       |
                 |                          TclShell (Tcl commands)
                 |                                     |
                 +-------------> ktSession <-----------+
                                     |
            readDesign  globalPlace  legalize  detailPlace  writeDesign ...
                                     |
                 SimPL / NTUPlace1, Abacus / multi-row, FastDP
```

`ktSession` (`src/session/`) owns one design and exposes each stage as a method
that can be called on its own. Nothing in it knows about Qt or Tcl.

- **The CLI** is `FlowMgr::run()`: the fixed sequence read, place, legalize,
  check, detail place, check, draw, write -- a dozen lines calling the session.
- **The GUI** calls the same methods, but one at a time and in whatever order
  the user asks, through Tcl commands (`read_design`, `global_place`,
  `legalize`, ...). `scripts/flow.tcl` is `FlowMgr::run()` written as those
  commands, and CI checks that both write the same `placed.pl`.

So the stages run the same code either way. The GUI adds no placement logic; it
only decides which session method runs next.

## Threads

The GUI process has two threads of its own, plus the placers' worker pool:

```
 Qt (GUI) thread                     engine thread                TBB workers
 ---------------                     -------------                -----------
 event loop, widgets,                owns the ktSession and       SimPL's parallel
 layout view, Log panel              the Tcl interpreter;         loops, as in the
       |                             runs one script at a time    CLI
       |  submit("legalize") ------>  shell.eval("legalize")
       |   (queued call)                   |
       |                              session.legalize() --------> parallel work
       |                                   |
       |  <------ log records ------- ktlog (any thread) via a log sink
       |  <------ progress ---------- per outer iteration (live view, metrics)
       |  <------ result ------------ the command's result or error
       |  <------ snapshot ---------- the placement, copied, after the command
       |
  Stop button: sets an atomic flag the placer checks between iterations
```

- **There is one engine thread and one session for the life of the window.**
  It is not a thread per run, and nothing is joined per command: the thread is
  started with the window and joined when the window closes. Scripts are queued
  to it and run in order, one at a time, like commands typed into a shell.
- **Why a thread of its own:** a placement takes seconds to minutes. Run on the
  Qt thread it would freeze the window; on its own thread the window keeps
  drawing and stays clickable, and can show the placement as it moves.
- **Why always that one thread:** a Tcl interpreter may only be used from the
  thread that created it, and one owner for the session means no locking around
  the design.
- **Nothing on the Qt thread touches the live design.** After each command, and
  during a stage once per outer iteration, the engine thread copies the
  placement into an immutable `LayoutSnapshot` and hands it over. The layout view
  draws and picks from that copy while the engine carries on.
- **Every crossing is a queued call** (`QMetaObject::invokeMethod` with a
  context object), in both directions. Results arriving for a widget that has
  been closed are dropped by Qt. No `Q_OBJECT`, so the Make build needs no moc.

The only shared state touched from both sides is deliberately small:

| What | Written by | Read by | How it is kept safe |
| --- | --- | --- | --- |
| log records | any thread | Log panel | the logger's lock; each record is copied and queued to the Qt thread |
| the list of log sinks | Qt thread (panel opens or closes) | the logging thread | `sinkMutex`; `removeSink` waits for a call in flight |
| progress samples | engine thread, inside a stage | Metrics, layout view | copied and queued |
| the stop flag | Qt thread (Stop) | the placer, between iterations | an atomic; a stopped stage returns the best placement it has |

The CLI uses none of this: it installs no log sink and no progress observer, so
`ktlog` and the placers skip that work with one atomic check.

## Where the code is

| File | Role |
| --- | --- |
| `src/session/kt_session.*` | the design and its stages; no Qt, no Tcl |
| `src/kt_flowMgr.*` | the CLI's fixed sequence over a session |
| `src/tcl/kt_tclShell.*` | the Tcl commands over a session |
| `src/util/kt_progress.*` | per-iteration progress and the stop flag |
| `src/gui/kt_engine.*` | the engine thread: session + shell, queued in and out |
| `src/gui/kt_mainWindow.*` | menus, docks; every action is a Tcl command |
| `src/gui/kt_tclConsole.*`, `kt_logView.*` | the console and the Log panel |
| `src/gui/kt_layoutSnapshot.*`, `kt_layoutView.*` | the copied placement and the canvas |

## Adding to the GUI

1. **Give the engine the capability**: a method on `ktSession` if it is a stage
   or a query, an option (`add_option`) if it is a setting.
2. **Give it a Tcl command** in `kt_tclShell.cc`, so a script and the batch mode
   (`ktplace --script`) can use it too, and a Tcl test covers it without a display.
3. **Give it a menu action** that runs that command through the console
   (`MainWindow::engineAction`). It is then echoed in the console and the log,
   disabled while the engine is busy, and replayable as a script.

A read-only view of the design that a user did not type -- the details of a
clicked cell, say -- uses `Engine::query()` instead: it runs on the engine
thread like a command, but stays out of the console and the log.
