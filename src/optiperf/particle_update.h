#pragma once
// The particle emitters' update on several threads (Phase 4, option 2: research/docs/main-thread.md §6). x64.
// CPartManager::Update (the 3D engine's, in its scene traversal on the main thread) runs each emitter's update in turn
// (CParticleEmitter vtable slot 12), then asks the emitter whether it is still alive and registers it in the octree or
// removes it. The updates are independent of each other and of that per-emitter code (the particles' census, §6): at
// the manager's first update call, every emitter's update is made at once, by worker threads and the main thread; the
// manager's own update calls then skip them, and its liveness checks and registrations go on in its order. An emitter
// the list gains meanwhile is updated where the manager calls it, as in the game. Each thread has a range of the list
// (the same emitters from frame to frame), then helps with the others'.
//
// What an update reaches that isn't made for several threads at once is made under one lock while a batch runs (the
// serial calls, on the batch's threads only): an emitter's first vis-area lookup (SVisEnviron::Update), the sound
// system (every CSoundSystem and CSound method), the physical world's entity creation, removal, box queries and ray
// tests, the physical entities' (particles', rigid bodies', static ones') parameters, actions and geometry, the static
// meshes' and characters' AddRef and random positions, and the frame profiler's registration of a profiler label. Their
// Release waits for the main thread, after the batch (not atomic, and the last one deletes the object, with renderer
// calls). The vis areas' search an emitter in a vis area makes each frame (CVisArea::FindVisArea, whose list is
// function-static) gets a list per thread instead. GetCamera, the one renderer call of the update itself, answers a
// worker with what the main thread got at the batch's start (with the render thread on, the camera the render queue
// answers beside the flush). Reads the game already makes from its own particle thread (the physical areas, entities'
// status, the timer) are made freely;
// while the workers run, the vtable counters (vtable_counter.h) count nothing, so the timer's getters don't contend.
// A batch isn't made while the frame profiler runs, or with fewer than 8 emitters.
//
// [ParticleUpdate] Mode = off (default) | on | serial | time  (env OPTIPERF_PARTICLE_UPDATE): serial makes each batch
//                  on the main thread alone, through the same skips and serial calls; time only times the manager's
//                  update, the game's own (both for comparisons)
//                  Workers = auto (default) | 1-8         (env OPTIPERF_PARTICLE_UPDATE_WORKERS): auto takes the
//                  processors the game's other threads have (FlushThreadPlacement=apart's) less one, else half the
//                  process's less one; at most 4
//                  Priority = normal (default) | above  (env OPTIPERF_PARTICLE_UPDATE_PRIORITY): the workers' (a test)
// Needs [ShadowPrepass] Mode other than off (its flush hook sets it up, at the second flush), and the traversal census
// off.

namespace optiperf {

void ParticleUpdate_Install(const wchar_t* moduleName);
void ParticleUpdate_FlushEntry(void* renderer);  // from the shadow pre-pass's outer EF_EndEf3D, on the main thread
void ParticleUpdate_Report(const char* when);

}  // namespace optiperf
