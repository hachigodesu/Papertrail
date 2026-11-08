# Security

Papertrail opens documents from anywhere, so a file that makes it misbehave matters. Every document is parsed in a separate worker process, so a bad file should cost at most that file, but please report anything that gets further.

## Reporting a problem

Report security problems privately, through **Report a vulnerability** on this repository's **Security** tab, rather than in a public issue. Say which version and platform you used, and what happens. If a document is needed to show the problem, make one that does rather than sending a real paper, and don't send anything you don't have the right to share.

You should hear back within two weeks. Fixes go into the next release, and the report is credited in its notes if you'd like that.

## What's in scope

- A document that makes Papertrail itself, rather than its worker process, crash, hang or run code
- A document, export or online reply that makes Papertrail change, delete or send files it shouldn't
- Anything that sends your Claude API key, files, notes or tags anywhere other than as Papertrail describes
- A `.papertrail` export that does any of these when imported

## What isn't

- A document that only crashes the worker, which is shown as unreadable and doesn't affect anything else, unless it happens with many ordinary files
- Problems in MuPDF, DjVuLibre or Qt themselves, which are best reported to those projects, though it helps to hear of them too
