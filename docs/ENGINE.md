# The Crew 2 engine notes

Reverse-engineering notes for `TheCrew2.exe` (Steam build, image base
`0x140000000`). All addresses are virtual addresses in that image; RVA =
address minus `0x140000000`. The executable is packed: code lives in the
`.xcode2` section and only exists decrypted at runtime, so analysis was done
on a memory dump.

## 1. Objects, types and components

### Entity hierarchy

Game objects are not GameObject/MonoBehaviour style. Entities are C++
classes in a single inheritance chain:

```
CNomadObject
  CITBaseEntity
    CITEntity
      CIT3DEntity          (has a world transform)
        CITLODEntity
          CITEntityVehicleBase   (cars, bikes, boats, planes)
```

### Runtime type information

The engine has its own RTTI, separate from MSVC's:

* Each class has a type record `{ const char* name; u32 depth @+0x08;
  u32 ancestorIds[] @+0x10 }`.
* An "is a" check compares `ancestorIds[T.depth - 1]` with T's id.
* Every class has a `StaticType()` getter starting with
  `sub rsp,28h; cmp [rip+record],0`. Records are filled lazily on first use.
* Type ids are hashed names (they are not plain CRC32 of the class name).
* Some vtables carry a class-name pointer at `vtable[-1]`.

### Components

Entities own a list of components:

| Entity offset | Meaning |
|---|---|
| `+0x288` | component pointer array |
| `+0x290` | component count |
| `+0x298`, `+0x2b8`, `+0x2c8` | partitioned sub-lists `{ptr, u32 count, u32 cap}` |

A component has its vtable at `+0x00` and a back pointer to its owning entity
at `+0x18`. `GetComponent<T>` (for example `0x14012bb20`) walks the array and
calls `vtbl[1]` to do the RTTI "is a" check. Components are created through a
factory hash map registered at `0x1404eb7c0`, keyed by `typeId ^ 0xdeadbeef`.

The global game object `g_Game` (`*(0x143480bb8)`) is itself a component
host: components at `+0x1dd8`, an index table at `+0x1de8`
(`component = components[index[slot]]`). Known slots: `0x1c` camera owner,
`0x1a` render-target manager.

## 2. Transform

The transform is **not a component**. It is stored inline in `CIT3DEntity`:

| Offset | Field |
|---|---|
| `+0x3a0` | integer world cell (large-world coordinates) |
| `+0x3b0` | fractional position inside the cell |
| `+0x3c0`, `+0x3d0`, `+0x3e0` | rotation matrix rows |
| `+0x3f0` | world position (cell + fraction) |
| `+0x410`, `+0x420` | local bounding box |
| `+0x450` | rotation quaternion |
| `+0x460` | embedded oriented bounding box object |

The world is **Z up**; a car's forward axis is **+Y**. Positions are split
into a cell and a fraction to keep float precision far from the origin; any
offset applied to a camera or object should go into the fraction.

## 3. Parenting

Parenting is done by a component, **`CITEntityComponentBinding`**
(reflection registered at `0x1403d7adc`):

| Offset | Field |
|---|---|
| `BindingType` | `EntityLogical`, `EntityPhysical`, `CarPuzzleLink`, `EntityPhysicalLockAngle` |
| `+0x38` | `ParentEntityID` (a `CITEntityHandle`) |
| `+0x98` | `BoneName` |
| `+0x9c` | `PhysicsLinkType` |
| `+0xa0` | `CarPuzzleLinkType` (front right seat, front left seat, parachute) |
| `+0xa8` | `Locator` |
| `+0xc0` | `LocalTransform` (4x4 matrix) |
| flags | `InheritParent*`, lock flags, `UseParentAnimation` |

So "attach X to the car" means giving X a binding component whose parent is
the car entity, optionally to a bone or locator, with a local transform.

## 4. Cameras

### Ownership

There is no camera entity. Cameras are owned by a **camera owner component**
on `g_Game` (index slot `0x1c`, type id hash `0xf0c5ede2`, vtable
`0x142d96318`, constructor `0x141ddb8a0`, size `0x1550`, update
`0x141ddc0a0`):

| Offset | Meaning |
|---|---|
| `+0x48 + 8*i` | camera controllers, 6 slots |
| `+0x78 + 8*i` | CameraData pointers for those slots |
| `+0x2f8` | active CameraData |
| `+0x1500` | override flag (then `+0xf00` is used) |
| `+0x1508`, `+0x1510` | per-view-bit CameraData array and count |

`0x141ddbf70` returns the CameraData for a given **view bit**; gameplay
systems (LOD, streaming, visibility checks) use it with an entity's view bit.

### Camera controllers ("camera scripts")

Camera behaviour lives in `CameraController` subclasses. The driving camera
is `CameraControllerVehicleEntity` (vtable `0x142b32b68`, size `0xa60`,
`StaticType` `0x1406215d0`); there are 16 other controllers. Vtable slots:

| Slot | Function |
|---|---|
| 0 | destructor |
| 1 | `StaticType` |
| 2 | `Update(self, CameraData*, dt)` (`0x140a05310`) |
| 3 | `Activate` (`0x1409eacb0`) |
| 4 | `Deactivate` (`0x1409eb790`) |

Controller fields: `+0x10` target vehicle, `+0x210` state, `+0x230` camera
state array, `+0x240` current mode, `+0x250` its CameraData.

The vehicle controller is a state machine; each mode is a state class:

| Mode | State |
|---|---|
| 1, 2, 3 | Follow |
| 4 | Plane |
| 5 | Hood |
| 6 | Bumper |
| 7 | Show |
| 8 | CameraInterior (vtable `0x142b86630`, update `0x140db8bc0`) |
| 9 | Snap |
| 10, 11 | CarConfigInterior |
| 12 | FastFav |
| 13 | Pipe |
| 14 | Looping |

The interior camera's eye position comes from a car-model locator found
through the car's CarPuzzle component (`0x140916ca0`; the car entity caches it
at `+0x6f8`).

### CameraData (0x600 bytes)

| Offset | Field |
|---|---|
| `0x00`, `0x10` | position cell, fraction |
| `0x20` | rotation quaternion |
| `0x30`, `0x34` | scales |
| `0x38` | horizontal field of view (radians) |
| `0x3c` | aspect ratio |
| `0x40`, `0x44` | near, far |
| `0x48` | perspective flag |
| `0x50`..`0x8f` | extra matrix, applied when byte `0x90` is set |
| `0x94`, `0x98` | projection centre offset (TAA jitter is written here) |
| `0xa0`.. | rotation and world matrices |
| `0x120` | projection |
| `0x160` | culling projection |
| `0x1a0`.. | view-projection |
| `0x260`.. | frustum planes |

Projection is **reversed Z with an infinite far plane**. Constructor
`0x141ff6f20`; `0x141ff7010` finalises (rebuilds matrices).

Camera space: **+X right, +Y up, looking down -Z**.

### From controller to screen

1. The owner updates the active controller, which fills its CameraData.
2. The main view object (`g_Game+0x1e30`, constructor `0x140a1e7f0`) copies
   the active CameraData into its **scene camera** every frame
   (`0x140a4db50`). Free look and camera shake are applied on the way, so the
   scene camera is the final camera.
3. The renderer builds per-view CameraData from the scene camera.

## 5. Scene objects and the render side

### Handles and double buffering

The game thread and render thread share scene objects (viewports, cameras,
lights, meshes...) through **handle tables**, not pointers. Each table keeps
two copies of every object:

* `table+0x10`: the copy the game thread writes.
* `table+0x18`: the copy the render thread reads (`0x142200480`).

`ResolveForWrite(table, &handle, copyPrevious)` returns the writable copy.
The first call after a buffer swap allocates a new object; with
`copyPrevious = false` it starts from **defaults** (all handles `-1`), with
`true` it copies the previous frame's fields. Always pass `true` when updating
an object every frame.

| Table | Address | ResolveForWrite |
|---|---|---|
| Scene camera | `0x1433ddc30` | `0x1421df8c0` |
| On-screen viewport (`CSceneViewport`) | `0x1433db7e0` | `0x1421e1090` |
| Render-to-texture viewport (`CSceneViewportEx`) | `0x1433ddff0` | `0x1421e1190` |

Create functions: camera `0x1421da370`, viewport `0x1421dc970`, RTT viewport
`0x1421dcb10`.

### Scene camera

`+0x10` position, `+0x20` fraction, `+0x30` quaternion, `+0x44` ortho size,
`+0x48` field of view, `+0x50` near, `+0x54` far, `+0x58` perspective flag.

### Viewport

| Offset | Field |
|---|---|
| `+0x0c` | scene camera handle |
| `+0x14` | pipeline type (2 = full scene) |
| `+0x18`, `+0x20` | normalised rectangle origin and size |
| `+0x40` | view bit |
| `+0x44`..`+0x4f7` | settings (exposure, white balance, fog...) |
| `+0x4d0` | exposure compensation (EV) |
| `+0x4e4` | white balance (K) |
| `+0x4f8` | keep history |
| `+0x4f9` | enabled |
| `+0x510` | output (texture for RTT views) |

The game rewrites the main viewport's settings every frame, so extra views
must copy them every frame to match exposure and colour.

### View bits

Every entity belongs to view bits (`entity+0x60`). Game systems pick the
camera for an entity through its view bit, and world entities spawn on bit
`0x1` (the main view). A view with any other bit only sees global systems
(sky, water, particles, HUD), so extra views of the world must share bit
`0x1`.

## 6. Render frame

### Scheduling

`RenderScheduler_Frame` (`0x142090300`) runs per frame on the render
thread:

1. Gather views (`0x14208fa80`): render-to-texture views first, then on-screen
   views.
2. For each view, **build and execute back to back**
   (`0x1420906b0` for RTT, `0x1420903d0` for on-screen): set the per-view
   resource index, call `pipelines[type].build`, then
   `pipelines[type].execute`, then reset per-view allocators and transient
   resources.

The pipeline table is at `frameCtx+0x50`, entries `{build, execute}`:

| Type | Build | Execute |
|---|---|---|
| 0 | `0x1420b8440` (simple forward) | `0x1420b8330` |
| 1 | `0x1420b6d70` | `0x1420b6930` |
| 2 | `0x1420b3170` (full scene) | `0x1420b23c0` |
| 3 | `0x1420b8ae0` | `0x1420b8ad0` |
| 4 | `0x1420b9380` | `0x1420b91e0` |
| 5 | none | `0x1420ba300` |

Renderer singleton: `*(0x1439d10d0)`; its frame context is at `+0x20`.

### Full-scene build (`0x1420b3170`)

Per view it:

* builds CameraData from the scene camera (`0x14208f2d0`),
* culls the world in several job rounds (main frustum plus sun shadow
  cascades, local light shadow slices, two more systems),
* prepares instanced geometry (`0x14211b210`, `0x14211df10`), world clusters
  (`0x14212d250`, `0x142130790`), cables (`0x142191820`) and more,
* records draw lists for every pass.

Execute (`0x1420b23c0`) replays the recorded jobs into D3D11.

### Culling

The world culler (`*(0x1439d5d70)`) runs a job (`VisNodeCulling`, kernel
`0x14213ab20`) over all world nodes per pass, then gathers results by type
into the visible set at `frameCtx+0x98`. On the **main pass** it advances a
per-object fade and frustum state machine. The global at `0x1439d1398`
marks a view as secondary; with it set the culler does a stateless test
instead. Pipelines 0 and 1 set it; pipeline 2 assumes it is the only view.

### Per-frame state that extra views must reset

The instanced renderer (`renderers+0x28`) distributes work to job threads
with atomic cursors at `+0x88` (5 view types x 8 slices). The game resets
them **once per frame**, so a second full-scene view in the same frame finds
them exhausted and draws no instanced geometry (buildings, trees, props).
Zero them before every full-scene build.

### GPU ring allocator

Per-frame GPU data (instances, draw constants) comes from ring buffers via
`0x1420d33e0`. Its buffer-map path detours into copy-protection VM code,
which cost about 80% of each view's build time. A native replacement
without the detour brings a full-scene view from about 4.5 ms to about
0.8 ms of render-thread time.

### Textures

Render target (`0x1420543b0`, 0x28 bytes) `+0x10` points to an engine
texture (`+0x10` HAL texture, `+0x18`/`+0x1c` size, `+0x28` format, `+0x80`
render counter). The HAL texture (`vtbl 0x142dc3900`) holds the
`ID3D11Texture2D*` at `+0x20`. The device is `*(0x1439cf4a0) + 0x50`.

The final pass writes gamma-encoded colour, so render targets for extra
views must be UNORM, not sRGB.
