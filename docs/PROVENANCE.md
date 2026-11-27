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

## When a concern is raised

- Stop using the material at once and list where it was used, by metadata only.
- Replace any test, fixture or text derived from it with independent material.
- Say publicly and plainly what was done, and what cannot be undone (Git history, published archives, data already processed
  by external services).
