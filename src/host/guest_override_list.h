/*
 * Every lifted function the host may replace, sorted by guest address. tools/run_lift.py names each one's lifted body
 * lifted_sub_<va>; guest_overrides.c defines sub_<va>, the stable entry every direct call, alias entry and dispatch
 * reaches, so an override never depends on how the linker lays out or reaches the lifted code.
 * REQUIRED: the port's behaviour depends on it. OPTIONAL: without it the game runs as retail (a host kernel the lifted
 * body replaces exactly, or a presentation layer).
 */
GUEST_OVERRIDE(00403520, REQUIRED) /* CUIButton hit test (ui_canvas.c) */
GUEST_OVERRIDE(0040C780, REQUIRED) /* CGameWnd::DrawBackground (ui_canvas.c) */
GUEST_OVERRIDE(0040DA00, REQUIRED) /* legacy 100 MB disk-space startup check, bypassed (ce_gameplay.c) */
GUEST_OVERRIDE(00410540, REQUIRED) /* save list, Quick Save kept out of the Save dialog (ce_gameplay.c) */
GUEST_OVERRIDE(00411EA0, REQUIRED) /* window centring (ui_canvas.c) */
GUEST_OVERRIDE(004128A0, REQUIRED) /* RasterBlt (ui_canvas.c) */
GUEST_OVERRIDE(00413040, OPTIONAL) /* screen message (loading_hd.c) */
GUEST_OVERRIDE(004142C0, OPTIONAL) /* level load (loading_hd.c) */
GUEST_OVERRIDE(00415AD0, OPTIONAL) /* CLoaderWnd draw and present (loading_hd.c) */
GUEST_OVERRIDE(00417300, REQUIRED) /* CUIWnd frame (ui_canvas.c) */
GUEST_OVERRIDE(00417380, REQUIRED) /* UI windows (ui_canvas.c) */
GUEST_OVERRIDE(00417410, REQUIRED) /* UI cursor (ui_canvas.c) */
GUEST_OVERRIDE(00418AE0, OPTIONAL) /* CVideoWnd::Play (video_hd.c) */
GUEST_OVERRIDE(00419330, REQUIRED) /* CAISystem constructor, Active AI (active_ai.c) */
GUEST_OVERRIDE(004195A0, REQUIRED) /* CAISystem destructor, Active AI (active_ai.c) */
GUEST_OVERRIDE(00419800, REQUIRED) /* CAISystem::Activate, Active AI (active_ai.c) */
GUEST_OVERRIDE(004199D0, REQUIRED) /* CAISystem::Deactivate, Active AI (active_ai.c) */
GUEST_OVERRIDE(00419C60, REQUIRED) /* deactivate within a radius, Active AI (active_ai.c) */
GUEST_OVERRIDE(00419CE0, REQUIRED) /* nearest active animal sees an object, Active AI (active_ai.c) */
GUEST_OVERRIDE(00419DB0, REQUIRED) /* active animals near a point hear it, Active AI (active_ai.c) */
GUEST_OVERRIDE(00419E30, REQUIRED) /* AI think scheduler, Active AI (active_ai.c) */
GUEST_OVERRIDE(0041A200, REQUIRED) /* CAISystem::Process, Active AI (active_ai.c) */
GUEST_OVERRIDE(0041AE50, REQUIRED) /* CAISystem save, Active AI (active_ai.c) */
GUEST_OVERRIDE(0041AF00, REQUIRED) /* CAISystem load, Active AI (active_ai.c) */
GUEST_OVERRIDE(004213F0, REQUIRED) /* brain influences, Active AI (active_ai.c) */
GUEST_OVERRIDE(004218C0, REQUIRED) /* brain influences tail, Active AI (active_ai.c) */
GUEST_OVERRIDE(004395A0, OPTIONAL) /* voice captions to the text overlay (l10n.c) */
GUEST_OVERRIDE(004610D0, OPTIONAL) /* CPlayer stow handler, Dual stow (ce_gameplay.c) */
GUEST_OVERRIDE(00472B60, OPTIONAL) /* level TEXT action (l10n.c) */
GUEST_OVERRIDE(00475F60, REQUIRED) /* CPartition::bAddChild, official Patch 1.1 fix (patch117.c) */
GUEST_OVERRIDE(00477A40, REQUIRED) /* CPartition::bInRange (engine_limits.c) */
GUEST_OVERRIDE(00477E00, REQUIRED) /* combined culling (engine_limits.c) */
GUEST_OVERRIDE(00478A30, REQUIRED) /* CPartition::bFlagCacheable (engine_limits.c) */
GUEST_OVERRIDE(00492200, REQUIRED) /* CTextureNode::SetTextureMemSize (engine_limits.c) */
GUEST_OVERRIDE(0049E530, REQUIRED) /* CQuadRootTIN::Update (terrain_quality.c) */
GUEST_OVERRIDE(004EDFA0, REQUIRED) /* physics box set-up, official Patch 1.1 fix (patch117_physics.c) */
GUEST_OVERRIDE(004F0960, REQUIRED) /* jump hand-off, official Patch 1.1 fix (patch117_player.c) */
GUEST_OVERRIDE(004F3FB0, REQUIRED) /* Pelvic_IDOF (physics_fixes.c) */
GUEST_OVERRIDE(00502970, REQUIRED) /* mode table (widescreen.c) */
GUEST_OVERRIDE(00503400, REQUIRED) /* CRasterVid::Lock (cpu_polygons.c) */
GUEST_OVERRIDE(00506270, OPTIONAL) /* CDirect3D::AllocateTextureMemory, logged (engine_limits.c) */
GUEST_OVERRIDE(00506C00, REQUIRED) /* managed texture split (engine_limits.c) */
GUEST_OVERRIDE(00506EB0, REQUIRED) /* CDirect3D::bAllocateNonLocalMemory (engine_limits.c) */
GUEST_OVERRIDE(005072D0, REQUIRED) /* CDirect3D::iLargeMemAllocMB (engine_limits.c) */
GUEST_OVERRIDE(0050E750, REQUIRED) /* text font (game_text.c) */
GUEST_OVERRIDE(0050E830, REQUIRED) /* text line draw (game_text.c) */
GUEST_OVERRIDE(0050E8C0, OPTIONAL) /* text layout, translated text (game_text.c) */
GUEST_OVERRIDE(0050EAF0, REQUIRED) /* text draw (game_text.c) */
GUEST_OVERRIDE(00514FE0, REQUIRED) /* CAGPTextureMemManager::AllocateMemory (engine_limits.c) */
GUEST_OVERRIDE(0051FD90, REQUIRED) /* CCamera::SetProperties (widescreen.c) */
GUEST_OVERRIDE(005242C0, OPTIONAL) /* CSkyRender::DrawD3D, Enhanced sky (ce_graphics.c) */
GUEST_OVERRIDE(00543830, REQUIRED) /* bShouldCache (engine_limits.c) */
GUEST_OVERRIDE(00547710, REQUIRED) /* CRenderCache::SetTextureMemSize (engine_limits.c) */
GUEST_OVERRIDE(005477E0, REQUIRED) /* CRenderCache::SetParameters (engine_limits.c) */
GUEST_OVERRIDE(00559240, OPTIONAL) /* Direct3D polygon submission (polygon_submit.c) */
GUEST_OVERRIDE(0055A4E0, REQUIRED) /* UploadPolygonArray (texture_streaming.c) */
GUEST_OVERRIDE(0055F540, REQUIRED) /* CScreenRenderDWI::DrawPolygon (cpu_polygons.c) */
GUEST_OVERRIDE(00563890, REQUIRED) /* flat primitive (cpu_polygons.c) */
GUEST_OVERRIDE(005668C0, REQUIRED) /* texture primitive, linear (cpu_polygons.c) */
GUEST_OVERRIDE(00568EF0, REQUIRED) /* texture primitive, perspective (cpu_polygons.c) */
GUEST_OVERRIDE(0057B7B0, REQUIRED) /* bump primitive, linear (cpu_polygons.c) */
GUEST_OVERRIDE(0057DB50, REQUIRED) /* bump primitive, perspective (cpu_polygons.c) */
GUEST_OVERRIDE(005F80B0, OPTIONAL) /* CInput::tinReadDefaultControls (gamepad.c) */
GUEST_OVERRIDE(005F8A20, REQUIRED) /* pHeapAlloc (engine_heaps.c) */
GUEST_OVERRIDE(005F8B10, REQUIRED) /* pvCommitMem (engine_heaps.c) */
GUEST_OVERRIDE(0060E3F0, OPTIONAL) /* water texture rows (water_kernels.c) */
GUEST_OVERRIDE(0060FC10, REQUIRED) /* CEntityWater::CPriv::Integrate (physics_fixes.c) */
GUEST_OVERRIDE(00630D90, OPTIONAL) /* object occlusion test (occlusion_kernel.c) */
GUEST_OVERRIDE(00638670, OPTIONAL) /* CSimWaveVar2D step (water_kernels.c) */
