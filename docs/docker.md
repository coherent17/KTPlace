# Using KTPlace with Docker

A task-oriented guide to the Docker environment: start it, build the engine,
and -- most of all -- run commands in it. The
[README](../README.md#docker-any-os) has the three-line version.

All you need is Docker (Docker Desktop on macOS and Windows). Every command
below runs from the repository root on the host unless it says "inside".

## The pieces

| Container     | Image target | What it is for                                     | Reach it at                     |
|---------------|--------------|----------------------------------------------------|---------------------------------|
| `ktplace-dev` | `dev`        | Build, test and run; the repository is live at `/workspace` | `ssh -p 2222 dev@127.0.0.1` |
| `ktplace-web` | `web`        | Browser console: start runs, watch them, download results   | http://127.0.0.1:8080        |

The dev container also serves the desktop GUI's display over VNC on
`vnc://127.0.0.1:5901` (see [5. The desktop GUI](#5-the-desktop-gui)).

Both containers share one build volume, so the `build/bin/ktplace` the dev
container compiles is the binary the console runs. Every port is bound to
`127.0.0.1` only -- nothing is exposed to the network.

## Start, stop, rebuild

With a bash shell (macOS, Linux, WSL, Git Bash), use `scripts/devenv.sh`:

```sh
scripts/devenv.sh up              # start both containers, wait for SSH, compile ktplace
scripts/devenv.sh up --no-build   # start without compiling
scripts/devenv.sh build           # recompile later (args go to make: build -j8 test)
scripts/devenv.sh status          # what is running, on which ports
scripts/devenv.sh stop            # stop; containers and volumes kept
scripts/devenv.sh down            # remove containers; volumes (builds, runs) kept
```

Without bash -- PowerShell included -- use compose directly, then compile once
inside:

```sh
docker compose up -d --build                           # both containers
docker exec -u dev ktplace-dev bash -lc 'make -j"$(nproc)"'   # build the engine
docker compose stop                                    # or: docker compose down
```

A rebuild is picked up by the console on its next run; it needs no restart.

## Running commands

There are four ways in, from most to least interactive.

### 1. A shell in the dev container

```sh
scripts/devenv.sh ssh                          # or: ssh -p 2222 dev@127.0.0.1
scripts/devenv.sh exec                         # same, through docker exec (no SSH key needed)
docker exec -it -u dev ktplace-dev bash -l     # what `exec` runs, for PowerShell
```

You land in `/workspace` (the repository) with `ktplace` on `PATH`:

```sh
make -j"$(nproc)" && make test
ktplace benchmark/ISPD_2005/adaptec1 -w output/adaptec1
ktplace benchmark/ISPD_2005/adaptec1 -w output/adaptec1-ntu -a ntuplace1 -v
```

Anything written under `output/` appears on the host straight away.

### 2. One command, without a session

Pass the command as a string; it runs in `/workspace`, the same as a login:

```sh
scripts/devenv.sh ssh 'make test'
scripts/devenv.sh exec 'ktplace benchmark/ISPD_2005/adaptec1 -w output/adaptec1'
docker exec -u dev ktplace-dev bash -lc 'make -j"$(nproc)" && make test'
```

With the `~/.ssh/config` entry from `scripts/devenv.sh ssh-config`, plain
`ssh ktplace-dev 'make test'`, `scp`, `rsync` and VS Code *Remote-SSH* all work
too.

### 3. The engine's options

```
ktplace <input_dir> [options]
ktplace -t <script.tcl> [input_dir] [options]
  -a, --algorithm <name>   simpl (default) or ntuplace1
  -t, --tcl <script>       run a Tcl script instead of the fixed flow
                           (scripts/flow.tcl is the fixed flow as a script)
      --gui                open the design in the Qt window instead (see 5.)
  -w, --work-dir <dir>     where placed.pl, plots/ and ktplace.log go
      --no-plots           draw nothing -- no frames, gallery or GIF (much less disk)
  -v, --verbose            also print trace diagnostics (ktplace_trace.log is always written)
  -h, --help / -V, --version
```

Tuning is through options, not flags or environment variables: a script of
`add_option <component> <option> <value>` lines, given with `-c`:

```sh
printf 'add_option simpl iters 25\nadd_option simpl seed 1\n' > output/tuning.tcl
ktplace benchmark/ISPD_2005/adaptec1 -c output/tuning.tcl -w output/adaptec1-25it
```

Common ones: `simpl iters`, `simpl seed`, `simpl grid`, `simpl density`,
`fastdp window`, `visualizer anim` (animated GIF), `visualizer final_zoom`. The
README's [Options](../README.md#options) section lists where each component
declares its options; every stage logs its full table when it runs.

### 4. From the browser: the web console

Open http://127.0.0.1:8080.

- **Start a run**: pick a design (every suite under `benchmark/` is mounted),
  an algorithm, optionally *verbose*, and `component option value` lines in
  *Options* (written into the run's `config.tcl` and given to `ktplace -c`).
  The transcript streams live with a phase bar and a heartbeat (frames, CPU,
  memory), so a long solve is visibly alive.
- **Review**: the *Run* tab has the stats, links and logs; the *Animated
  placement* tab plays the run's GIF (loaded only when opened). Click the open
  tab to fold it. Download the transcript, trace, `placed.pl`, `request.json`,
  or *everything (.zip)*.
- **How it works** (top right, or http://127.0.0.1:8080/guide): flow charts of
  an experiment, the placement pipeline and the SimPL loop, plus what each
  output file is and the main tuning knobs.
- **History**: every run with status, time, HPWL and verdict; label or delete
  runs there. Runs live in the `web-runs` volume and survive `down`/`up`.
- **Benchmarks** (http://127.0.0.1:8080/bench): one row per design, a column per
  algorithm, each its best finished run ranked by HPWL (runtime breaks ties).

**The Terminal panel** runs a one-shot command in the *console* container and
streams its output. Use it for quick checks next to your runs:

```sh
ls runs/                                            # every run directory
tail -n 30 runs/<id>/ktplace.log
build/bin/ktplace benchmark/ISPD_2005/adaptec1 -w runs/manual-1 -a simpl
build/bin/ktplace benchmark/ISPD_2005/adaptec1 -c runs/<id>/config.tcl -w runs/again
```

Know its limits:

- It starts in `/ktplace`. The engine is `build/bin/ktplace` (the shared build
  volume), not on `PATH` as it is in the dev container.
- No terminal: interactive programs (`vim`, `top`, `less`) will not work; pipe
  through `head` or `tail` instead.
- No compiler: build in the dev container (way 1 or 2).
- At most three commands at once. Output is capped at 64 MB and a command at
  one hour; past either it is stopped with a note at the end of its output.
  *Stop* sends SIGTERM, then SIGKILL five seconds later.
- Runs started here are not in the console's history -- only runs started from
  the form are. Their files are still under `runs/`.

The panel is enabled by `KTPLACE_WEB_ALLOW_EXEC=1`, which `compose.yaml` sets.
Remove that line to switch it off.

### 5. The desktop GUI

`ktplace-gui` is the native Qt window ([how it embeds the engine](gui.md)): the layout view on top, a Properties
panel on the right, and a tabbed dock underneath with the live log and a Tcl
console (the commands of `ktplace -t`; type `help`). Every menu action runs as a
Tcl command in that console, so a session reads back as a script, and the
layout view shows the placement as each command leaves it.

In the layout view: wheel to zoom about the pointer, drag to pan, `F` to fit,
`+`/`-` to zoom, click a cell to select it (Esc clears). The selected cell's nets
are drawn from it to the cells they reach, and its name, position, size and nets
are in Properties. Pads are orange, macros gray, standard cells blue; cells too
small to see one by one are drawn as a density map, so nothing is left out at
any zoom.

Open it from the host, or from any shell inside the dev container:

```sh
scripts/devenv.sh gui benchmark/ICCAD04/ibm01          # host: build, open, start the viewer
scripts/devenv.sh gui benchmark/ICCAD04/ibm01 -t scripts/flow.tcl

ktplace --gui benchmark/ICCAD04/ibm01 -w output/gui    # inside: like any ktplace run
ktplace --gui -t scripts/flow.tcl benchmark/ICCAD04/ibm01
```

`ktplace --gui` takes ktplace's own arguments (`input_dir`, `-w`, `-t`, `-a`,
`-v`) and opens the window instead of running the fixed flow; `make gui` builds
it (`scripts/devenv.sh gui` does that for you). From the host, `gui` defaults the
work directory to `output/gui` and replaces a window that is already open.

The window appears on the dev container's own display, which the container
starts when it boots and serves over VNC on `vnc://127.0.0.1:5901` (loopback
only). Connect a viewer to that address:

- **macOS Screen Sharing** (built in): `open vnc://127.0.0.1:5901`, password
  `ktplace`. `scripts/devenv.sh gui` opens it for you on macOS. Use *View >
  Scale to Fit* if the window does not fit.
- **TigerVNC**: works, but on a Retina Mac turn on *Open in Low Resolution* for
  TigerVNC Viewer (Finder > Get Info) first. Without it TigerVNC draws the
  remote screen at half size and its clicks land somewhere else.
- **Shortcuts**: the viewer or macOS may keep F-keys and Cmd shortcuts for
  themselves; the menus and the toolbar do the same things.

**As a native window (X11).** Qt draws the window through X11 either way;
instead of the container's virtual display it can use your machine's own X
server, so the window opens on your desktop like any other, with no VNC viewer.
On macOS that server is XQuartz (`brew install --cask xquartz`, then log out and
back in); Linux desktops have one already; on Windows, WSLg does.

```sh
scripts/devenv.sh x11 benchmark/ICCAD04/ibm01          # host: native window via ssh -Y
ssh -Y -p 2222 dev@127.0.0.1                           # or by hand, then inside:
ktplace --gui benchmark/ICCAD04/ibm01
```

`x11` builds the GUI and runs `ktplace --gui` over `ssh -Y`, which forwards the
window to your X server; it returns when the window is closed. Inside an
`ssh -Y` session `DISPLAY` is the forwarded one, so `ktplace --gui` goes to your
desktop; in any other shell it is `:1`, the VNC display. XQuartz draws at
standard density, so on a Retina screen the window is sharp-edged but not
high-resolution; over VNC with Screen Sharing it is scaled instead.

The display's size (`KTPLACE_GUI_SIZE`, default `1440x900`) and password
(`KTPLACE_VNC_PASSWORD`, default `ktplace`; empty for none) are set when the
dev container is created: `KTPLACE_VNC_PASSWORD=secret scripts/devenv.sh up`.
A dev container created before the display existed needs that `up` once.

## Calling the console's API

The page is a thin client over a JSON API, so scripts can drive it too:

```sh
H='Content-Type: application/json'
curl -s http://127.0.0.1:8080/api/benchmarks
curl -s -X POST -H "$H" http://127.0.0.1:8080/api/runs \
     -d '{"benchmark":"ISPD_2005/adaptec1","algorithm":"simpl","options":[["simpl","seed","1"]]}'
curl -s "http://127.0.0.1:8080/api/runs/<id>/log?offset=0"
curl -s -o run.zip "http://127.0.0.1:8080/api/runs/<id>/download?what=all"
curl -s -X POST -H "$H" http://127.0.0.1:8080/api/exec -d '{"cmd":"ls runs"}'
curl -s "http://127.0.0.1:8080/api/exec/<id>?offset=0"
```

The full endpoint list is at the top of [`webui/server.py`](../webui/server.py).
Log and command output come back at most 1 MB per call: keep requesting with
the returned `offset` until `done` is true.

Because the console can run commands, it only accepts requests that look like
they came from this machine:

- The `Host` header must be `localhost`, `127.0.0.1`, `[::1]` or `ktplace-web`.
  To reach it under another name (a LAN IP, a tunnel), list the names in
  `KTPLACE_WEB_ALLOWED_HOSTS=name1,name2`.
- POST and DELETE requests from a page on another origin are refused, and
  every POST must be `Content-Type: application/json` (otherwise HTTP 415).

## Without compose

```sh
# CI-style image: builds and tests a copy of the tree
docker build -t ktplace .
docker run -v "$PWD/output:/ktplace/output" ktplace \
    ktplace benchmark/ISPD_2005/adaptec1 -w output/adaptec1

# Small run-only image (~112 MB); ktplace is the entrypoint
docker build --target run -t ktplace-run .
docker run --rm -v "$PWD/output:/ktplace/output" ktplace-run \
    benchmark/ISPD_2005/adaptec1 -w output/adaptec1

# CI's GUI job: build ktplace-gui and run its tests headless
docker build --target gui-ci .

# Console by hand, against the compose build volume
docker build --target web -t ktplace-web .
docker run --rm -p 127.0.0.1:8080:8080 -e KTPLACE_WEB_ALLOW_EXEC=1 \
    -v ktplace_build:/ktplace/build -v ktplace_web-runs:/ktplace/runs ktplace-web
```

## Settings

| Variable                     | Default   | Effect                                         |
|------------------------------|-----------|------------------------------------------------|
| `KTPLACE_SSH_PORT`           | `2222`    | Host port for the dev container's SSH          |
| `KTPLACE_SSH_PUBKEY`         | `~/.ssh/id_ed25519.pub` | Key installed for `dev`          |
| `KTPLACE_WEB_PORT`           | `8080`    | Host port for the console                      |
| `KTPLACE_VNC_PORT`           | `5901`    | Host port for `ktplace-gui`'s VNC display      |
| `KTPLACE_GUI_SIZE`           | `1440x900` | Size of the GUI's display, in points          |
| `KTPLACE_VNC_PASSWORD`       | `ktplace` | Password of the GUI's VNC display (`""`: none) |
| `KTPLACE_WEB_ALLOW_EXEC`     | off (`1` in compose) | Enables the Terminal panel          |
| `KTPLACE_WEB_ALLOWED_HOSTS`  | --        | Extra host names the console answers to        |
| `KTPLACE_WEB_MAX_RUNS`       | `1`       | Placements allowed at once                     |
| `KTPLACE_WEB_EXEC_MAX_BYTES` | 64 MB     | Output cap per Terminal command                |
| `KTPLACE_WEB_EXEC_TIMEOUT`   | `3600`    | Seconds before a Terminal command is stopped   |
| `KTPLACE_BIN`                | --        | Force a specific engine binary for the console |

Set the ports on the host when starting (`KTPLACE_WEB_PORT=9090 scripts/devenv.sh up`);
the console settings go under `environment:` for `web` in `compose.yaml`. The
GUI's two take effect when the dev container is created (`up`).

## Troubleshooting

- **"ktplace is not built yet"** in the console: run `scripts/devenv.sh build`
  (or `make` inside the dev container), then start the run again.
- **SSH: "REMOTE HOST IDENTIFICATION HAS CHANGED"**: the host-key volume was
  recreated. `ssh-keygen -R ktplace-dev`, then retry.
- **SSH refuses your key**: after changing `KTPLACE_SSH_PUBKEY` or your key,
  run `scripts/devenv.sh down` and `up` -- the key is copied in at start.
- **Console returns 403 "unexpected Host header"**: you opened it under a name
  not in the allow-list; use `127.0.0.1`/`localhost` or set
  `KTPLACE_WEB_ALLOWED_HOSTS`.
- **GUI: the viewer cannot connect**: a dev container created before the
  display existed does not publish its port; run `scripts/devenv.sh up` once.
- **GUI: clicks land in the wrong place, or the window is half size**
  (TigerVNC on a Retina Mac): turn on *Open in Low Resolution* for TigerVNC
  Viewer, or use Screen Sharing.
- **GUI: `--gui: no display`**: the shell has no `DISPLAY`. Inside the dev
  container it is `:1`; elsewhere, use `scripts/devenv.sh gui` from the host.
- **GUI over X11: "cannot connect to display" or no window**: XQuartz must be
  running and the session started with `ssh -Y` (`scripts/devenv.sh x11` does
  that). A dev container from before X11 forwarding was enabled needs
  `scripts/devenv.sh up` once.
- **GUI: the window does not appear**: when started by `scripts/devenv.sh gui`
  its output is in `/tmp/ktplace-gui.log` inside the container
  (`scripts/devenv.sh exec 'cat /tmp/ktplace-gui.log'`).
- **Logs**: `docker logs ktplace-dev`, `docker logs ktplace-web`.
- **Windows**: keep the clone inside WSL 2 rather than on `C:`; bind mounts from
  the Windows filesystem are slow.
