# KartPad maintained runtime source

This is the **ios** runtime source for [KartPad](https://github.com/chrissotraidis/kartpad), derived from [WiiCompiled by patchzyy and contributors](https://github.com/patchzyy/wiicompiled).

Upstream base: `1912292c804ff9b1b79938de89369ec4496f9fff`. KartPad migration baseline: `dd79c936e5f32dde2d5a003798163cf615935c0d`.

The `kartpad-macos`, `kartpad-ios` (also iPadOS), `kartpad-android`, and `kartpad-tvos` branches preserve each platform's existing source behavior. Edit and commit source on the relevant branch; KartPad pins the reviewed commit as a Git submodule. Runtime source is in `runtime/`; Aurora retains its upstream location and notices in `aurora-main/`. Existing upstream licenses and authorship apply.

These branches are initially materialized from KartPad's ordered platform patches, without an upstream version update. Product profile headers, sse2neon dependency headers and KartPad's Android trace header remain explicit generated/copied build inputs in KartPad. No game image, private generated translation, signing material or saves belongs in this repository.

The translator in this branch is the untouched upstream baseline. KartPad's maintained translator lives in KartPad's `vendor/wiicompiled/translator` subtree; its unmodified native-registration baseline must remain separate from these platform runtime sources.

Merge reviewed upstream changes into an isolated platform branch, test in KartPad, then update its submodule pin. Shared fixes can be reviewed and cherry-picked between affected platform branches. Do not silently update all platforms or submit the whole platform delta as one upstream bug fix.
