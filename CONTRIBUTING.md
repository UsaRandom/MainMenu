# Contributing

This is a fork of [N64FlashcartMenu](https://github.com/Polprzewodnikowy/N64FlashcartMenu). The
boot and flashcart plumbing is still theirs. The grid, library, players, and the rest of the
presentation layer are not — send those changes here, not upstream.

House style is in [CLAUDE.md](CLAUDE.md): measure rather than assert, check that a test can fail
before trusting a green one, and keep ruled-out hypotheses in [docs/AUDIT.md](docs/AUDIT.md).

Commits are imperative, plain English, describing the effect. No conventional-commits prefixes,
no ticket IDs, no emoji.

Do not run `tools/regress.sh` or `tools/suite.sh` unless you were asked. The host tests
(`tools/hosttest/run.sh`) are a few seconds and need no emulator.
