# Provenance policy for custom level work

This policy applies to every analysis, test and feature that concerns community levels, mods and fan missions, whether the
work is done by hand or with AI coding assistance.

## Rules

1. **Respect the author's terms.** Material whose author forbids its use with generative AI tools is never opened, read,
   summarised, extracted or quoted in an AI-assisted session, and never sent to any AI service. This covers scripts,
   assets, level files, readmes and any text of the package.
2. **No clear permission, no AI use.** Third-party material is not given to an AI tool unless its author or licence clearly
   allows it. Being publicly downloadable is not permission.
3. **Sources in order of preference:**
   1. official or author-authorised documentation (for example Trespasser CE's scripting documentation);
   2. the port's own code and interfaces;
   3. conventional local tools (the custom level scanner, hashes, file listings) whose output to an AI session is limited to
      metadata: paths, sizes, counts, hashes, status codes;
   4. fixtures written for the project from scratch.
4. **Independent fixtures only.** Tests use minimal synthetic examples, one engine function or format feature at a time.
   They never reproduce a community level's scripts, HUD, inventory, mechanics, names, paths, constants or structure.
5. **Generic support only.** Compatibility is implemented from documented formats and APIs. No code, default or behaviour
   is tailored to a specific community level, and no level or author is singled out in code, tests or public notes.
6. **Corpus runs stay local.** A compatibility corpus is downloaded and scanned on the maintainer's machine. Its files are
   never committed, packaged or uploaded, and its reports keep metadata only.
7. **Honest claims.** Public notes say what was verified and how (scanner, synthetic tests, manual play) and never describe a
   level as working because the scanner classified it as READY.
8. **When in doubt, stop.** If an analysis would need the original material of an author who has not allowed AI use, it
   stops at that point and is left to a human review outside AI tools.
9. **Uncertain authorship counts as excluded.** Co-authored packages of such an author, and packages whose authorship
   cannot be confirmed without opening them, stay out of AI-assisted work until a person has reviewed them.

## Development corpus exclusions

The corpus tools read a local exclusions file that is never versioned: `corpus_exclusions.json` in the corpus folder, or the
file named by `JPT_CORPUS_EXCLUSIONS` or `--exclusions`. A file inside the repository is refused.

```json
{ "ids": ["catalog-id"], "sha256": ["64 hexadecimal digits"] }
```

- `tools/download-community-levels.py` never fetches the page or the files of an excluded entry, drops a download whose
  hash is excluded before anything reads it, and keeps the entry excluded after a catalog refresh.
- `tools/community_level_compatibility.py` never extracts, scans or combines an excluded package. A download folder that no
  catalog entry owns is not read.
- Exclusion spreads to patches and parts of an excluded entry and to every package with the same content.
- `tools/analyze-custom-level.ps1` and `tools/custom-level-compatibility.ps1` refuse a folder of the development corpus.
- A file that cannot be read or that has unknown keys stops the tools; nothing is downloaded or scanned.
- The tools write `COMMUNITY_LEVELS_CATALOG.json` and `.md` and `COMMUNITY_LEVEL_COMPATIBILITY.md`. A corpus catalogued under the
  earlier name `TRESCOM_LEVELS_CATALOG.json` is still read, with the same exclusions; nothing in it is renamed or rewritten.
- Nothing names an author or a level in the repository. The player's Custom Levels import is not affected.

An entry that is not excluded is still not permitted for AI use: rules 1 and 2 apply to it. Without clear permission only
conventional local checks run, and only their metadata reaches an AI session.

## When a concern is raised

- Stop using the material at once and list where it was used, by metadata only.
- Replace any test, fixture or text derived from it with independent material.
- Say publicly and plainly what was done, and what cannot be undone (Git history, published archives, data already processed
  by external services).
