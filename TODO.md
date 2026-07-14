# TODO

- [ ] **Rewrite README.md** — it still describes the old interception-tools
  plugin. Should cover: what the daemon does (keep the existing "why"
  section, it's good), dependencies, build, install, the systemd user
  service + `input` group setup, config walkthrough
  (`~/.config/osu-intercept/config.yaml`), and the optional
  realtime-privileges note.
- [x] Add a "Snappy Tappy" mode ie "last input wins" SOCD cleaning without
  latching. "Press and Hold Z, Press X, Release X" should result in Z pressed 
  and held again, but "Press and Hold Z, Press and Hold X, Release Z" should
  not result in a toggle. *Done: top-level `mode: toggle|snappy` config field.*
- [ ] Come up with a new name for the project. osu-intercept es no bueno.
- [ ] Tag a release and submit the PKGBUILD to the AUR (switch it from the
  `-git` variant to a tagged source tarball at that point).
- [ ] Consider rtkit (D-Bus) as a portable way to get RT priority without
  limits.d configuration; current SCHED_FIFO attempt is best-effort.
