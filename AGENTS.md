# Rules for agents and contributors (Team 5)

- **Clean room first.** Read [CLEAN-ROOM.md](CLEAN-ROOM.md) before you touch
  anything. Never read vasm source code. Use the vasm manual and a vasm binary
  (black box) only. Add every new reference to CLEAN-ROOM.md.
- **Commits:** author with
  `git -c user.name=SacredTrees -c user.email=121399723+SacredTrees@users.noreply.github.com commit ...`.
  The last line of the message is `Team 5`. Do not add Co-Authored-By,
  Claude, Anthropic or "Generated with" lines.
- **Git hygiene:** do not force-push, rebase or delete branches. Push sparingly.
- **Workflow:** after the initial scaffold, every change goes through a pull
  request. Agents do not merge PRs; Team 5 merges.
- **Secrets:** never print tokens or `~/.config/amigachrome/mcp-token`.
- **Code:** portable C11 with no dependencies. It must build warning-free with
  `-Wall -Wextra` on Linux x86-64 and ARM64. Keep AmigaOS (our gcc) in mind:
  no POSIX-only calls in the core.
- **Tests:** `make test` must pass before you open a PR. Treat every
  difference from the oracle as a bug, or list it as an intentional difference
  in `docs/DIFFERENCES.md`.
