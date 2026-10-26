# Minimal Direct3D 12

A rotating coloured triangle on deep navy, in one readable C++17 source file.
This is a Windows desktop Win32 app, **not an Xbox app** or an engine.
No external libraries, assets, downloads, NuGet packages, or helper frameworks.
WRL `ComPtr`, the HLSL compiler, and Direct3D headers come with the Windows SDK.

## Build and run

Use Windows 10/11 x64 with Visual Studio 2022 and the **Desktop development
with C++** workload, including MSVC v143 x64/x86 tools and a current Windows
10 or Windows 11 SDK. Hardware rendering needs a Direct3D 12 driver supporting
feature level 11_0. `--warp` selects Windows' software adapter, useful in VMs
and on machines without suitable GPU support.

Open `minimal-directx12.vcxproj` in Visual Studio, select **Debug | x64**,
then press **F5**. There is no separate solution file to generate.

From a normal Command Prompt in this directory:

```bat
build.cmd
build\x64\Debug\minimal-directx12.exe
build.cmd Release
build\x64\Release\minimal-directx12.exe --warp --smoke-test
echo %errorlevel%
```

`build.cmd` locates MSBuild with the installed `vswhere`, then tries standard
Visual Studio 2022/2019 install paths. It propagates build failures. It does
not install anything. The default toolset is v143; select an installed
alternative with `build.cmd Release v142`, or invoke MSBuild directly:

```bat
MSBuild minimal-directx12.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
```

**Space** pauses/resumes rotation; **Escape** or the close button exits.
The client area is fixed at 960x640 physical pixels; resizing, maximizing,
and Alt+Enter fullscreen are disabled. Minimized windows wait for messages;
occluded presentation is throttled.

`--smoke-test` renders eight deterministic frames to an offscreen GPU texture,
using the same shaders, pipeline, and draw commands as the windowed renderer.
It creates no window or swap chain, so it can run in a noninteractive Windows
SSH/service session. After a GPU fence, it reads back the final frame and checks
for a navy corner and a visibly coloured centre, respecting the texture's row
pitch. Success is reported on stderr and exits 0; initialization, shader,
rendering, wait, pixel verification errors, or unknown arguments exit nonzero.
Combine it with `--warp` as above.
There are no error dialogs, including in smoke mode. Failures include the
operation and HRESULT, with compiler diagnostics where available, on stderr
and `OutputDebugString` (visible in Visual Studio's Output window).
Redirect stderr to a file when launching from a shell to capture it.
Debug builds enable the debug layer if it is already available; installing
Windows Graphics Tools is optional.

## From Java Canvas to Direct3D 12

The Win32 window and message loop play the role of the Canvas and UI event
dispatch. `WindowProc` handles keys and closing; `steady_clock` supplies the
elapsed time for animation. Instead of calling a CPU-side `Graphics` object,
the program records commands for a GPU:

1. The device represents the GPU; its queue executes command lists. The
   swap chain owns two images used in turn for presentation.
2. A render-target descriptor heap gives the command list handles to those
   images. Resource barriers explicitly switch each image between presentation
   and drawing use.
3. Inline HLSL is compiled with the Windows SDK's `D3DCompile`. The vertex
   shader uses **`SV_VertexID`** to generate all three positions and colours:
   there is no vertex buffer, input layout, or asset loading.
4. The root signature describes two root constants, angle and aspect ratio.
   The pipeline combines shaders, blending, rasterization, and target format.
   Aspect correction keeps the rotating triangle proportional, and culling is
   disabled so winding cannot hide it.
5. Each frame clears navy, supplies the constants, draws three vertices,
   transitions back, and presents with vsync. The pixel shader interpolates
   the three vertex colours.
6. A fence and event deliberately **wait for the GPU every frame** before
   reusing the one allocator or releasing resources. This sacrifices CPU/GPU
   overlap to keep ownership and synchronization easy to follow. It is not a
   performance template for a larger renderer.

## Verification status

This implementation was authored on Linux. Initial Windows Debug and Release
builds passed, but swap-chain creation failed in the SSH/service session.
Final Windows builds and offscreen GPU pixel verification are pending
parent-owned checks. GUI rendering and interaction remain unverified because
the GUI session is disconnected; headless smoke does not verify presentation
or controls. Source inspection is not runtime verification.

## License

MIT, copyright Anais Gisele Lacroix-Beauvillier. See [LICENSE](LICENSE).
