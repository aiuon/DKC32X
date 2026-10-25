# Contributing to DKC32X

Thank you for wanting to help! Bug reports, fixes, support for more ROM revisions, performance
work and documentation improvements are all welcome.

## The two rules

1. **Never attach ROM data.** No ROMs, no dumps or parts of dumps, no `.32x` files, no save
   states, no captured files (snapshots, surveys, `.npz`), no hex dumps of game data, and no
   links to any of these - not in issues, pull requests, comments or commits. Anything like that
   will be deleted. **Screenshots, videos and logs are fine** (for example
   `last conversion log.txt` from the Windows builder).
2. **Same licence.** By contributing you agree that your contribution is licensed under the same
   terms as the project: the PolyForm Noncommercial License 1.0.0 with the additional terms in
   [LICENSE](LICENSE) (or, for `tools/meta/layout.json.gz`, the GPL-3.0). Only contribute work
   you have the right to contribute.

## Good bug reports

- Where you played (emulator and version, MiSTer, or real 32X and flash cart).
- The level / screen, and what you did just before it happened.
- What you expected and what you saw (a screenshot or short video helps a lot).
- Your ROM's md5 as the builder prints it (the md5 is fine to share; the ROM is not).

## Code changes

- Read [docs/HANDBOOK.md](docs/HANDBOOK.md) first: it explains the architecture, the rules (the
  game logic must stay byte-exact with the SNES) and the pitfalls that already caused bugs.
- Changes to anything that runs the game's logic must be verified as described in HANDBOOK
  section 5.2.
- Before publishing anything you built, run `python3 tools/scanbundle.py YOUR_ROM DIR` on it: it
  must report `CLEAN`.

The maintainer does not monitor issues regularly, so please be patient.
