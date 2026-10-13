# CLAUDE.md

@AGENTS.md

## Claude Code specifics
- Shells: the PowerShell tool is Windows PowerShell 5.1; the Bash tool is Git Bash. Use `/c/itc-work/...` paths in Bash and `C:\itc-work\...` in PowerShell,
  never the real `C:\Users\日南\...` path for builds (GCC breaks on it).
- Run `run_test.ps1` from the PowerShell tool with a timeout (100-200 s runs) or in the background; read `<tag>.err` afterwards with Grep/Read.
- Look at frame dumps with the Read tool (`C:\itc-work\dumps\...png`); build a contact sheet first (`python sheet.py <tag>`) instead of reading dozens of PNGs.
- The user likes short progress notes during long work and plain-language summaries at the end (what changed, what was verified, what is still open).
- Persistent memory for this project lives in the auto-memory directory; keep it in sync with `work/NOTES.md` and this file when the status changes.
