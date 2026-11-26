/*
 * Every lifted function the host may replace, sorted by 1.0.116 address (the other lifts' lists are these through their
 * guest map). tools/run_lift.py names each one's lifted body <lift>_lifted_sub_<va>; guest_overrides.c defines
 * <lift>_sub_<va>, the stable entry every direct call, alias entry and dispatch reaches, so an override never depends on
 * how the linker lays out or reaches the lifted code.
 * REQUIRED: the port's behaviour depends on it. OPTIONAL: without it the game runs as retail (a host kernel the lifted
 * body replaces exactly, or a presentation layer).
 */
GUEST_OVERRIDE(00403560, REQUIRED) /* CUIButton hit test (ui_canvas.c) */
GUEST_OVERRIDE(0040C8A0, REQUIRED) /* CGameWnd::DrawBackground (ui_canvas.c) */
GUEST_OVERRIDE(0040DB20, REQUIRED) /* legacy 100 MB disk-space startup check, bypassed (ce_gameplay.c) */
GUEST_OVERRIDE(00410650, REQUIRED) /* save list, Quick Save kept out of the Save dialog (ce_gameplay.c) */
GUEST_OVERRIDE(00411FC0, REQUIRED) /* window centring (ui_canvas.c) */
GUEST_OVERRIDE(004129C0, REQUIRED) /* RasterBlt (ui_canvas.c) */
GUEST_OVERRIDE(00413160, OPTIONAL) /* screen message (loading_hd.c) */
GUEST_OVERRIDE(004143B0, OPTIONAL) /* level load (loading_hd.c) */
GUEST_OVERRIDE(00414580, OPTIONAL) /* Restart Level (level_actions.c) */
GUEST_OVERRIDE(00414E50, OPTIONAL) /* menu selection sound loader, menu-only volume (menu_audio.c) */
GUEST_OVERRIDE(00415BD0, OPTIONAL) /* CLoaderWnd draw and present (loading_hd.c) */
GUEST_OVERRIDE(00417420, REQUIRED) /* CUIWnd frame (ui_canvas.c) */
GUEST_OVERRIDE(004174A0, REQUIRED) /* UI windows (ui_canvas.c) */
GUEST_OVERRIDE(00417530, REQUIRED) /* UI cursor (ui_canvas.c) */
GUEST_OVERRIDE(00418B40, OPTIONAL) /* CVideoWnd::Play (video_hd.c) */
GUEST_OVERRIDE(0041D4A0, OPTIONAL) /* voice captions to the text overlay (l10n.c) */
GUEST_OVERRIDE(00445B20, OPTIONAL) /* CPlayer stow handler, Dual stow (ce_gameplay.c) */
GUEST_OVERRIDE(0044D270, OPTIONAL) /* CSetHintAction, actions of ATX and Trespasser CE (level_actions.c) */
GUEST_OVERRIDE(0044D280, OPTIONAL) /* CSetHintAction load (level_actions.c) */
GUEST_OVERRIDE(0044D2A0, OPTIONAL) /* CSetHintAction save (level_actions.c) */
GUEST_OVERRIDE(00451510, OPTIONAL) /* trigger action factory (level_actions.c) */
GUEST_OVERRIDE(004581A0, OPTIONAL) /* level TEXT action (l10n.c) */
GUEST_OVERRIDE(0045B6F0, REQUIRED) /* CPartition::bAddChild, official Patch 1.1 fix (patch117.c) */
GUEST_OVERRIDE(0045D1A0, REQUIRED) /* CPartition::bInRange (engine_limits.c) */
GUEST_OVERRIDE(0045D550, REQUIRED) /* combined culling (engine_limits.c) */
GUEST_OVERRIDE(0045E180, REQUIRED) /* CPartition::bFlagCacheable (engine_limits.c) */
GUEST_OVERRIDE(00470B70, REQUIRED) /* CTextureNode::SetTextureMemSize (engine_limits.c) */
GUEST_OVERRIDE(0047D180, REQUIRED) /* CQuadRootTIN::Update (terrain_quality.c) */
GUEST_OVERRIDE(004BCFD0, REQUIRED) /* physics box set-up, official Patch 1.1 fix (patch117_physics.c) */
GUEST_OVERRIDE(004D9CF0, REQUIRED) /* Pelvic_IDOF (physics_fixes.c) */
GUEST_OVERRIDE(004E8820, REQUIRED) /* mode table (widescreen.c) */
GUEST_OVERRIDE(004E92B0, REQUIRED) /* CRasterVid::Lock (cpu_polygons.c) */
GUEST_OVERRIDE(004EC150, OPTIONAL) /* CDirect3D::AllocateTextureMemory, logged (engine_limits.c) */
GUEST_OVERRIDE(004ECAE0, REQUIRED) /* managed texture split (engine_limits.c) */
GUEST_OVERRIDE(004ECD90, REQUIRED) /* CDirect3D::bAllocateNonLocalMemory (engine_limits.c) */
GUEST_OVERRIDE(004ED1B0, REQUIRED) /* CDirect3D::iLargeMemAllocMB (engine_limits.c) */
GUEST_OVERRIDE(004F46E0, REQUIRED) /* text font (game_text.c) */
GUEST_OVERRIDE(004F47B0, REQUIRED) /* text line draw (game_text.c) */
GUEST_OVERRIDE(004F4840, OPTIONAL) /* text layout, translated text (game_text.c) */
GUEST_OVERRIDE(004F4A80, REQUIRED) /* text draw (game_text.c) */
GUEST_OVERRIDE(004FAFA0, REQUIRED) /* CAGPTextureMemManager::AllocateMemory (engine_limits.c) */
GUEST_OVERRIDE(00505D60, REQUIRED) /* CCamera::SetProperties (widescreen.c) */
GUEST_OVERRIDE(0050A2B0, OPTIONAL) /* CSkyRender::DrawD3D, Enhanced sky (ce_graphics.c) */
GUEST_OVERRIDE(00529810, REQUIRED) /* bShouldCache (engine_limits.c) */
GUEST_OVERRIDE(0052D6F0, REQUIRED) /* CRenderCache::SetTextureMemSize (engine_limits.c) */
GUEST_OVERRIDE(0052D7C0, REQUIRED) /* CRenderCache::SetParameters (engine_limits.c) */
GUEST_OVERRIDE(0053F300, OPTIONAL) /* Direct3D polygon submission (polygon_submit.c) */
GUEST_OVERRIDE(005405A0, REQUIRED) /* UploadPolygonArray (texture_streaming.c) */
GUEST_OVERRIDE(00545610, REQUIRED) /* CScreenRenderDWI::DrawPolygon (cpu_polygons.c) */
GUEST_OVERRIDE(00549960, REQUIRED) /* flat primitive (cpu_polygons.c) */
GUEST_OVERRIDE(0054C990, REQUIRED) /* texture primitive, linear (cpu_polygons.c) */
GUEST_OVERRIDE(0054EFC0, REQUIRED) /* texture primitive, perspective (cpu_polygons.c) */
GUEST_OVERRIDE(00561880, REQUIRED) /* bump primitive, linear (cpu_polygons.c) */
GUEST_OVERRIDE(00563C20, REQUIRED) /* bump primitive, perspective (cpu_polygons.c) */
GUEST_OVERRIDE(005DE210, OPTIONAL) /* CInput::tinReadDefaultControls (gamepad.c) */
GUEST_OVERRIDE(005DEFA0, REQUIRED) /* pHeapAlloc (engine_heaps.c) */
GUEST_OVERRIDE(005DF090, REQUIRED) /* pvCommitMem (engine_heaps.c) */
GUEST_OVERRIDE(005EC660, OPTIONAL) /* CWorld::bLoadScene (level_actions.c) */
GUEST_OVERRIDE(005F4970, OPTIONAL) /* water texture rows (water_kernels.c) */
GUEST_OVERRIDE(005F6190, REQUIRED) /* CEntityWater::CPriv::Integrate (physics_fixes.c) */
GUEST_OVERRIDE(005FB4D0, REQUIRED) /* CInstance::pcLoad, scenes of Trespasser CE (scene_scripts.c) */
GUEST_OVERRIDE(0060D880, REQUIRED) /* CAISystem constructor, Active AI (active_ai.c) */
GUEST_OVERRIDE(0060DAC0, REQUIRED) /* CAISystem destructor, Active AI (active_ai.c) */
GUEST_OVERRIDE(0060DD20, REQUIRED) /* CAISystem::Activate, Active AI (active_ai.c) */
GUEST_OVERRIDE(0060DEF0, REQUIRED) /* CAISystem::Deactivate, Active AI (active_ai.c) */
GUEST_OVERRIDE(0060E180, REQUIRED) /* deactivate within a radius, Active AI (active_ai.c) */
GUEST_OVERRIDE(0060E200, REQUIRED) /* nearest active animal sees an object, Active AI (active_ai.c) */
GUEST_OVERRIDE(0060E2D0, REQUIRED) /* active animals near a point hear it, Active AI (active_ai.c) */
GUEST_OVERRIDE(0060E350, REQUIRED) /* AI think scheduler, Active AI (active_ai.c) */
GUEST_OVERRIDE(0060E720, REQUIRED) /* CAISystem::Process, Active AI (active_ai.c) */
GUEST_OVERRIDE(0060F270, REQUIRED) /* CAISystem vocal table slot, bounds (ai_vocals.c) */
GUEST_OVERRIDE(0060F2D0, REQUIRED) /* CAISystem save, Active AI (active_ai.c) */
GUEST_OVERRIDE(0060F380, REQUIRED) /* CAISystem load, Active AI (active_ai.c) */
GUEST_OVERRIDE(00612FB0, REQUIRED) /* brain influences, Active AI (active_ai.c) */
GUEST_OVERRIDE(00613400, REQUIRED) /* brain influences tail, Active AI (active_ai.c) */
GUEST_OVERRIDE(00631FA0, OPTIONAL) /* object occlusion test (occlusion_kernel.c) */
GUEST_OVERRIDE(00639970, OPTIONAL) /* CSimWaveVar2D step (water_kernels.c) */
