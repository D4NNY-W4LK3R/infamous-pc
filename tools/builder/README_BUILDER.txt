SECOND SON ADAPTIVE - ONE-CLICK BUILDER (Windows x64)
====================================================

You normally do NOT need this: every push to the fork's second-son-adaptive branch is built on
GitHub's servers, and the finished zip is published under Releases. Use this builder to rebuild
the same thing on your own PC.

  1. Extract this folder to E:\SecondSonAdaptive\Builder
  2. Double-click CHECK_ONLY.cmd: it lists what is installed and what is missing (no changes).
  3. Double-click BUILD_ON_THIS_PC.cmd.
     - Missing tools are listed and installed ONLY if you answer y (Windows asks for admin).
     - Visual Studio always needs some space on C: for its shared files and the Windows SDK;
       the builder refuses to start its installer when C: has less than 10 GB free.
     - It downloads infamous-pc at the pinned v1.1.0 commit with all pinned submodules,
       checks each patch's SHA-256, applies them and verifies the final source tree hash.
     - It builds with clang-cl (2 jobs by default - laptop friendly; BUILD_ON_THIS_PC.cmd
       -Jobs 4 for more), runs the optimizer tests and packages the result.
  4. Result: E:\SecondSonAdaptive\Output\SecondSonAdaptive-<date>\PLAY_SECOND_SON.cmd and a zip.

Everything stays in E:\SecondSonAdaptive (Source, Build, Deps, Temp, Logs, Output); temporary
files of the build are redirected there too. A later run reuses the downloads and is faster.
Options: -Clean (fresh source and build), -SkipTests, -Jobs N.
If a step fails, the last lines say which one; the full log is in E:\SecondSonAdaptive\Logs.
