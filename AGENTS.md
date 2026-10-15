# E2SM-KPM Branch Instructions

## Scope

These rules apply to work on the `E2SM-KPM` branch in this repository and its
paired Falcon repository at `/home/nyu/Desktop/Falcon_repo/Falcon`.

Check the current branch and working-tree changes in each repository before
editing it. Preserve unrelated user changes.

## Source Code and Existing Documentation

- Source code, tests, scripts, build files, and runtime configuration may be
  changed as needed for the requested implementation.
- Preserve all explanatory documents that exist when a task begins, in both
  repositories. Do not edit, delete, rename, move, or reformat README files,
  technical documentation, runbooks, plans, or changelogs, including ocudu's
  existing `docs/` contents and `CHANGELOG`.
- Explain implementation changes and corrections in new Falcon documents.
  Do not update an existing document or its index to link them.
- Falcon development log reports may be created and updated as described below.
  Agent instruction files may be changed when the user explicitly requests
  instruction changes. These exceptions do not authorize
  changes to other explanatory documents or logging conventions.

## Documentation Destination

- Store all new technical documentation for this work in Falcon's
  `/home/nyu/Desktop/Falcon_repo/Falcon/docs/e2sm-kpm/` directory. Do not create
  technical documents or a separate development log in ocudu.
- Use a new descriptive filename such as `YYYY-MM-DD_<topic>.md`. If that path
  already exists, choose a distinct suffix instead of overwriting it. A document
  created during the current task may be refined before the task is finished.
- Write documentation in US English and identify ocudu's branch, affected source
  paths, implementation changes, usage, validation, and known limitations.
- Read and follow Falcon's root `AGENTS.md` before writing files there.

## Development Logs

- Record implementation changes after validation in Falcon's `dev_logs/`,
  including links to new technical documents and actual validation results.
  Identify Falcon and ocudu changes separately when both repositories are involved.
- Follow Falcon's root `AGENTS.md` and `dev_logs/PROMPT_CONVENTION.md` for
  naming, English content, developer-name confirmation for each new report,
  existing-report updates, New York timestamps, and save history.
- Existing Falcon development reports may be updated while preserving their
  identity and history. Keep ocudu's existing `CHANGELOG` unchanged.
