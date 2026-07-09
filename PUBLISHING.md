# Publishing to pub.dev

Package **flutter_pty_new** publishes to [pub.dev](https://pub.dev/packages/flutter_pty_new)
via GitHub Actions + pub.dev OIDC (no long-lived tokens in the repo).

## One-time OIDC setup

Do this once on [pub.dev](https://pub.dev) after the package exists (or when claiming
the name for the first publish):

1. Sign in as a package uploader → **Admin** tab.
2. **Automated publishing** → enable **Publishing from GitHub Actions**.
3. Repository: `hhoao/flutter_pty_new`
4. Tag pattern: `v{{version}}` (tag `v1.0.0` publishes version `1.0.0`).
5. *(Optional)* Require GitHub environment `pub.dev` with reviewers on both
   pub.dev and the publish job in `.github/workflows/publish.yml`.

## Release flow

1. Bump `version` in `pubspec.yaml` and update `CHANGELOG.md` on `main`.
2. **auto-tag.yml** detects the version change, creates annotated tag `v{version}`,
   and pushes it.
3. Because tag pushes with `GITHUB_TOKEN` do not trigger other workflows,
   auto-tag **dispatches** `publish.yml` for that tag.
4. Manual `git push origin v*` also runs `publish.yml` via `on.push.tags`.
5. `publish.yml` checks out the tag, runs `dart pub publish --dry-run`, then
   `dart pub publish -f` using OIDC.

| Trigger | Workflow |
|---------|----------|
| `pubspec.yaml` version bump on `main` | `auto-tag.yml` → creates `v*` → dispatches `publish.yml` |
| Push tag `v*` (manual) | `publish.yml` |
| Manual Actions run | `publish.yml` (`workflow_dispatch`, optional tag input) |

## Publish before flutter_alacritty

Consumers such as [flutter_alacritty](https://github.com/hhoao/flutter_alacritty)
depend on a published `flutter_pty_new` version on pub.dev. **Publish this package
first**, wait until [pub.dev](https://pub.dev/packages/flutter_pty_new) shows the
new version, then bump and publish `flutter_alacritty`.

Use `PUB_HOSTED_URL=https://pub.dev` if your shell points at a mirror.

## Publish an already-tagged version

OIDC runs on tag push or `workflow_dispatch`. If the tag existed before
`publish.yml` was on `main`, re-push the tag after merging the workflow, or
dispatch the workflow manually:

```bash
git tag -d v1.0.0
git push origin :refs/tags/v1.0.0
git tag -a v1.0.0 -m "Release v1.0.0" <commit>
git push origin v1.0.0

# Or: Actions → Publish to pub.dev → Run workflow → tag v1.0.0
```

## Manual fallback

```bash
dart pub login   # once per machine
dart pub get && dart pub publish --dry-run && dart pub publish
```

## Pre-flight checklist

- [ ] OIDC configured on pub.dev for `hhoao/flutter_pty_new` (or `dart pub login`)
- [ ] `flutter test` and `flutter analyze` pass
- [ ] `version` + `CHANGELOG.md` updated
- [ ] Tag pattern matches `v{{version}}`
- [ ] Published on pub.dev **before** flutter_alacritty consumes the new version
