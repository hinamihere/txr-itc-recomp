# patches

`xenonrecomp-local.patch`: local changes to XenonRecomp (`tools/XenonRecomp`, a clone of https://github.com/hedge-dev/XenonRecomp at commit
`ddd128bcca99fe8bfbb99bea583c972351fa6ace`) that this project needs. To recreate the tool: clone XenonRecomp, check out that commit,
`git apply xenonrecomp-local.patch`, build it (`tools/XenonRecomp/build_ascii`, through the `C:\itc-xr` junction), and copy
XenonRecomp.exe / XenonAnalyse.exe to `work/bin/`. Refresh the patch after changing the tool: `git -C tools/XenonRecomp diff > patches/xenonrecomp-local.patch`.
