#pragma once
// The characters' forward kinematics on several threads (Phase 4, option 3: research/docs/main-thread.md §7). x64.
// Each character's update, in the entity system's update loop on the main thread, runs CCharInstance::SkeletonPreProcess
// (the animation's update, then forward kinematics: the pose) and, soon after, SkeletonPostProcess (IK, the physics'
// parts, the attachments). Here, in that loop, a character's forward kinematics and post-process wait: they are made
// after the loop, the forward kinematics of all the characters at once by worker threads and the main thread, then the
// post-processes on the main thread in the order the game asked for them. So what the game reads of a pose in the loop
// is a step old (the user's choice: the game's own order can't batch); the traversal and the renderer see this frame's
// pose, as in the game. The root motion, which forward kinematics makes and the animation's update resets, is kept for
// the game's readers: it too is a step old. The animation's update stays where it is (the queue, the events, the times).
//
// Waits only for a caller in CryAction (the characters) or CryEntitySystem (the render proxies), not for the local
// player's character, and only inside the entity system's update. The rest as in the game: the weapons' first-person
// characters (CItem::Update, CryGame), a character's update from anywhere else, CCharInstance::Render's. An entity's
// deletion first makes what waits (its character's callbacks point into it), and each waiting character is kept alive.
//
// Forward kinematics on several threads, by what its reach (research/profiling/re) shares between characters: the last
// result the animation's tracks cache in themselves (the key-time searches and CControllerPQLog::DecodeKey: on a batch
// thread they work through a copy of the track's header, so nothing shared is read back or written), the samplers' static
// scratch for the animation-driven characters' root motion, the TCB splines' lazily computed tangents, the name lookup's
// static buffer, the warnings and the profiler labels' registration (on a batch thread under one lock). A batch isn't made
// while the frame profiler runs, with fewer than 6 characters, or in the first flushes of a run (the statics' one-time
// setups).
//
// [AnimUpdate] Mode = off (default) | on | serial | time | verify  (env OPTIPERF_ANIM_UPDATE): serial makes the waiting
//              work on the main thread alone (the step's delay without the threads); time only times the game's own (both
//              for comparisons); verify is on, each forward kinematics made again on the main thread and compared (a test)
//              Workers = auto (default) | 1-8           (env OPTIPERF_ANIM_UPDATE_WORKERS): as [ParticleUpdate] Workers
// Needs [ShadowPrepass] Mode other than off (its flush hook sets it up, at the second flush).

namespace optiperf {

void AnimUpdate_Install(const wchar_t* moduleName);
void AnimUpdate_FlushEntry();  // from the shadow pre-pass's outer EF_EndEf3D, on the main thread
void AnimUpdate_Report(const char* when);

}  // namespace optiperf
