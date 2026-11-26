/*
 * Every lifted function the host may replace, sorted by 1.0.116 address (the other lifts' lists are these through their
 * guest map). tools/run_lift.py names each one's lifted body <lift>_lifted_sub_<va>; guest_overrides.c defines
 * <lift>_sub_<va>, the stable entry every direct call, alias entry and dispatch reaches, so an override never depends on
 * how the linker lays out or reaches the lifted code.
 * REQUIRED: the port's behaviour depends on it. OPTIONAL: without it the game runs as retail (a host kernel the lifted
 * body replaces exactly, or a presentation layer).
 */
GUEST_OVERRIDE(00403530, REQUIRED) /* CUIButton hit test (ui_canvas.c) */
GUEST_OVERRIDE(0040CAC0, REQUIRED) /* CGameWnd::DrawBackground (ui_canvas.c) */
GUEST_OVERRIDE(0040DD40, REQUIRED) /* legacy 100 MB disk-space startup check, bypassed (ce_gameplay.c) */
GUEST_OVERRIDE(00410860, REQUIRED) /* save list, Quick Save kept out of the Save dialog (ce_gameplay.c) */
GUEST_OVERRIDE(004121A0, REQUIRED) /* window centring (ui_canvas.c) */
GUEST_OVERRIDE(00412BA0, REQUIRED) /* RasterBlt (ui_canvas.c) */
GUEST_OVERRIDE(00413340, OPTIONAL) /* screen message (loading_hd.c) */
GUEST_OVERRIDE(00414590, OPTIONAL) /* level load (loading_hd.c) */
GUEST_OVERRIDE(00414760, OPTIONAL) /* Restart Level (level_actions.c) */
GUEST_OVERRIDE(00415030, OPTIONAL) /* menu selection sound loader, menu-only volume (menu_audio.c) */
GUEST_OVERRIDE(00415DB0, OPTIONAL) /* CLoaderWnd draw and present (loading_hd.c) */
GUEST_OVERRIDE(00417600, REQUIRED) /* CUIWnd frame (ui_canvas.c) */
GUEST_OVERRIDE(00417680, REQUIRED) /* UI windows (ui_canvas.c) */
GUEST_OVERRIDE(00417710, REQUIRED) /* UI cursor (ui_canvas.c) */
GUEST_OVERRIDE(00418D30, OPTIONAL) /* CVideoWnd::Play (video_hd.c) */
GUEST_OVERRIDE(0041D6B0, OPTIONAL) /* voice captions to the text overlay (l10n.c) */
GUEST_OVERRIDE(00445D60, OPTIONAL) /* CPlayer stow handler, Dual stow (ce_gameplay.c) */
GUEST_OVERRIDE(0044D430, OPTIONAL) /* CSetHintAction, actions of ATX and Trespasser CE (level_actions.c) */
GUEST_OVERRIDE(0044D440, OPTIONAL) /* CSetHintAction load (level_actions.c) */
GUEST_OVERRIDE(0044D460, OPTIONAL) /* CSetHintAction save (level_actions.c) */
GUEST_OVERRIDE(004516D0, OPTIONAL) /* trigger action factory (level_actions.c) */
GUEST_OVERRIDE(00458380, OPTIONAL) /* level TEXT action (l10n.c) */
GUEST_OVERRIDE(0045D490, REQUIRED) /* CPartition::bInRange (engine_limits.c) */
GUEST_OVERRIDE(0045D840, REQUIRED) /* combined culling (engine_limits.c) */
GUEST_OVERRIDE(0045E470, REQUIRED) /* CPartition::bFlagCacheable (engine_limits.c) */
GUEST_OVERRIDE(00470EA0, REQUIRED) /* CTextureNode::SetTextureMemSize (engine_limits.c) */
GUEST_OVERRIDE(0047D760, REQUIRED) /* CQuadRootTIN::Update (terrain_quality.c) */
GUEST_OVERRIDE(004DB0B0, REQUIRED) /* Pelvic_IDOF (physics_fixes.c) */
GUEST_OVERRIDE(004DE2C0, OPTIONAL) /* the player's jump vocal, OnJump of CE scripts (script_engine.c) */
GUEST_OVERRIDE(004E9C60, REQUIRED) /* mode table (widescreen.c) */
GUEST_OVERRIDE(004EA6F0, REQUIRED) /* CRasterVid::Lock (cpu_polygons.c) */
GUEST_OVERRIDE(004ED580, OPTIONAL) /* CDirect3D::AllocateTextureMemory, logged (engine_limits.c) */
GUEST_OVERRIDE(004EDF10, REQUIRED) /* managed texture split (engine_limits.c) */
GUEST_OVERRIDE(004EE1C0, REQUIRED) /* CDirect3D::bAllocateNonLocalMemory (engine_limits.c) */
GUEST_OVERRIDE(004EE5E0, REQUIRED) /* CDirect3D::iLargeMemAllocMB (engine_limits.c) */
GUEST_OVERRIDE(004F5B30, REQUIRED) /* text font (game_text.c) */
GUEST_OVERRIDE(004F5C00, REQUIRED) /* text line draw (game_text.c) */
GUEST_OVERRIDE(004F5C90, OPTIONAL) /* text layout, translated text (game_text.c) */
GUEST_OVERRIDE(004F5ED0, REQUIRED) /* text draw (game_text.c) */
GUEST_OVERRIDE(004FC3F0, REQUIRED) /* CAGPTextureMemManager::AllocateMemory (engine_limits.c) */
GUEST_OVERRIDE(005073A0, REQUIRED) /* CCamera::SetProperties (widescreen.c) */
GUEST_OVERRIDE(0050B920, OPTIONAL) /* CSkyRender::DrawD3D, Enhanced sky (ce_graphics.c) */
GUEST_OVERRIDE(0052B740, REQUIRED) /* bShouldCache (engine_limits.c) */
GUEST_OVERRIDE(0052F6E0, REQUIRED) /* CRenderCache::SetTextureMemSize (engine_limits.c) */
GUEST_OVERRIDE(0052F7B0, REQUIRED) /* CRenderCache::SetParameters (engine_limits.c) */
GUEST_OVERRIDE(00541210, OPTIONAL) /* Direct3D polygon submission (polygon_submit.c) */
GUEST_OVERRIDE(005424B0, REQUIRED) /* UploadPolygonArray (texture_streaming.c) */
GUEST_OVERRIDE(005474F0, REQUIRED) /* CScreenRenderDWI::DrawPolygon (cpu_polygons.c) */
GUEST_OVERRIDE(0054B840, REQUIRED) /* flat primitive (cpu_polygons.c) */
GUEST_OVERRIDE(0054E870, REQUIRED) /* texture primitive, linear (cpu_polygons.c) */
GUEST_OVERRIDE(00550EA0, REQUIRED) /* texture primitive, perspective (cpu_polygons.c) */
GUEST_OVERRIDE(00563760, REQUIRED) /* bump primitive, linear (cpu_polygons.c) */
GUEST_OVERRIDE(00565B00, REQUIRED) /* bump primitive, perspective (cpu_polygons.c) */
GUEST_OVERRIDE(005E00E0, OPTIONAL) /* CInput::tinReadDefaultControls (gamepad.c) */
GUEST_OVERRIDE(005E0E60, REQUIRED) /* pHeapAlloc (engine_heaps.c) */
GUEST_OVERRIDE(005E0F50, REQUIRED) /* pvCommitMem (engine_heaps.c) */
GUEST_OVERRIDE(005EECA0, OPTIONAL) /* CWorld::bLoadScene (level_actions.c) */
GUEST_OVERRIDE(005F6F70, OPTIONAL) /* water texture rows (water_kernels.c) */
GUEST_OVERRIDE(005F8750, REQUIRED) /* CEntityWater::CPriv::Integrate (physics_fixes.c) */
GUEST_OVERRIDE(005FDAA0, REQUIRED) /* CInstance::pcLoad, scenes of Trespasser CE (scene_scripts.c) */
GUEST_OVERRIDE(0060FD90, REQUIRED) /* CAISystem constructor, Active AI (active_ai.c) */
GUEST_OVERRIDE(0060FFD0, REQUIRED) /* CAISystem destructor, Active AI (active_ai.c) */
GUEST_OVERRIDE(00610230, REQUIRED) /* CAISystem::Activate, Active AI (active_ai.c) */
GUEST_OVERRIDE(00610400, REQUIRED) /* CAISystem::Deactivate, Active AI (active_ai.c) */
GUEST_OVERRIDE(00610690, REQUIRED) /* deactivate within a radius, Active AI (active_ai.c) */
GUEST_OVERRIDE(00610710, REQUIRED) /* nearest active animal sees an object, Active AI (active_ai.c) */
GUEST_OVERRIDE(006107E0, REQUIRED) /* active animals near a point hear it, Active AI (active_ai.c) */
GUEST_OVERRIDE(00610860, REQUIRED) /* AI think scheduler, Active AI (active_ai.c) */
GUEST_OVERRIDE(00610C30, REQUIRED) /* CAISystem::Process, Active AI (active_ai.c) */
GUEST_OVERRIDE(00611780, REQUIRED) /* CAISystem vocal table slot, bounds (ai_vocals.c) */
GUEST_OVERRIDE(006117E0, REQUIRED) /* CAISystem save, Active AI (active_ai.c) */
GUEST_OVERRIDE(00611890, REQUIRED) /* CAISystem load, Active AI (active_ai.c) */
GUEST_OVERRIDE(00615460, REQUIRED) /* brain influences, Active AI (active_ai.c) */
GUEST_OVERRIDE(006158B0, REQUIRED) /* brain influences tail, Active AI (active_ai.c) */
GUEST_OVERRIDE(00621C50, OPTIONAL) /* CActivityBite::Act, OnActBite of CE scripts (script_engine.c) */
GUEST_OVERRIDE(00621DC0, OPTIONAL) /* CActivityFeint::Act, OnActBite of CE scripts (script_engine.c) */
GUEST_OVERRIDE(00634620, OPTIONAL) /* object occlusion test (occlusion_kernel.c) */
GUEST_OVERRIDE(0063BEF0, OPTIONAL) /* CSimWaveVar2D step (water_kernels.c) */
