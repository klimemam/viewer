# Prebuilt binaries (branch: binaries)

Updated automatically from every push to main. To stay current on a
machine with no build environment:

    git clone -b binaries --single-branch <this repo> viewer-bin

and from then on just run `update.cmd` (Windows) / `./update.sh`.

To try a build that is not main's:

    update.cmd --fetch binaries-pr64      ./update.sh --fetch binaries-pr64

That one needs **nothing but git**. CI publishes every branch it builds as
`binaries-<branch>` - so a branch called `dblclick-probe` arrives as
`binaries-dblclick-probe` - and `--fetch` takes it in place, exactly the way
the plain form takes main's. Use this on a machine with no GitHub CLI and no
token, which is most of the machines that need a prebuilt binary at all.

The other two ask GitHub directly, and need the CLI (`gh auth login`):

    update.cmd --pr 64            ./update.sh --pr 64
    update.cmd --commit a1b2c3    ./update.sh --commit a1b2c3

Those unpack into `try/pr64/` and `try/a1b2c3/` and leave the published
binaries alone, because this branch's history is replaced on every publish
and anything written over `win64/` would be destroyed by the next update
without saying so. Run them from `try/`, and delete it when you are done.
They need the GitHub CLI (`gh auth login`), take only builds that SUCCEEDED,
and artifacts expire after 90 days.
(Plain `git pull` will NOT work: this branch's history is replaced on
every publish so the repository never grows with old binaries.)

- win64/viewer.exe            - the GUI (Windows)
- win64/viewer-serve.exe      - the headless peer (Windows)
- win64/plugins/              - analyzers (keep next to the exe)
- win64/install_shortcut.cmd  - put it on the desktop / Start menu
- linux-x64/viewer            - the GUI (Linux)
- linux-x64/viewer-serve      - the headless peer for a compute server
- linux-x64/install_shortcut.sh - the same, as a .desktop entry
- macos-arm64/                - the same set for macOS (.app bundle)

Starting it from an icon instead of a shell:

    win64\install_shortcut.cmd                 (Windows)
    ./linux-x64/install_shortcut.sh            (Linux / macOS)

Add `-RemoteHost user@server` / `--host user@server` for a second
shortcut that connects to that machine on startup. The shortcuts point
at the binary in place, so `update.cmd` keeps them current.

See docs/startup.md on main for how ssh:// viewing fits together.
