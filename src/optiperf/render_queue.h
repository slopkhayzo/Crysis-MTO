#pragma once
// The main thread's renderer calls that wait for nothing while the flush runs beside it (FlushThread=overlap; the
// render thread's groundwork, research/docs/render-thread.md §11): taken (recorded, and answered at once), then made in
// their order when the main thread joins the flush, after optiperf's own work after it; or answered at once and
// checked at the join. Taken: the two the 3D engine makes right after the flush, EnableFog (whose result none of its
// calls reads) and DeleteRenderMesh, whose meshes the flush may still be drawing (resource lifetime); the HUD's 2D
// state, image draws and the font's state, the text, the system's RenderDebug, the post effects' parameters set, the
// frame's end (EndFrame, the present) and the next one's start (BeginFrame), with the 3D engine's next RenderWorld as
// the barrier; the aux geometry's render flags, the render meshes', shaders' and textures' AddRef and a shader's
// Release (its destruction made at the join with it), the data uploaded
// into an existing texture, the viewport and the default materials. Answered (made again at the join to check the
// answers): the screen's size and scale, the frame id, the features, the aux geometry's object, a texture's id,
// settings, ProjectToScreen and UnProjectFromScreen, the camera and its matrices from the copy made at the hand-off
// (after a taken SetCamera, the matrices SetCamera itself makes on that copy's block), the render meshes' and shaders'
// getters that only read a field, the meshes' positions and indices read, the ocean's grid (the update the flush makes
// at its start when the traversal has added the ocean, from a copy until the flush's first ocean draw puts it in; at
// once when it hasn't, the grid staying as it was), the post effects' float and Vec4 parameters that don't blend, and
// the shader resources' color and float references (through bytes of the queue's own, their writes made at the join).
// While the flush may still run a scene traversal (a reflection the dry run didn't take), the main thread's next update
// waits for it at BeginFrame. x64.

namespace optiperf {

// From the flush thread's install (overlap, after the renderer's gate): CD3D9Renderer's vtable by RTTI, checked by its
// EF_EndEf3D slot against the shadow pre-pass's (`endEf3D`), and the taken methods hooked behind their slots.
bool RenderQueue_Install(const wchar_t* moduleName, char* rendererBase, char* endEf3D);
// Whether the gate lets a call through its thunk without joining the flush (the queue takes it).
bool RenderQueue_Passes(unsigned gateIndex);
// Before each outer flush, on the main thread: the camera ProjectToScreen reads (its viewport and matrices), copied.
void RenderQueue_BeforeFlush(char* renderer);
// On the flush's thread, at the main pass's shadow maps: the flush's scene passes are done (the recursive frame id,
// which they count, final).
void RenderQueue_OnScenePassesDone();
// On the flush's thread, when it has ended the flush (the main thread's waits for a mark in it end too).
void RenderQueue_OnFlushEnd();
// On the flush's thread, at the outer flush's start: the ocean's grid updated there when the flush is to draw it.
void RenderQueue_OnFlushStart();
// From the reflection part, on the flush's thread: the flush runs no scene traversal from here on (its last capture is
// replayed, and nothing was left to it), which the main thread's next update waits for at BeginFrame where the hand-off
// couldn't tell; and one runs now (counted).
void RenderQueue_OnScenesFree();
void RenderQueue_OnSceneRun();
// EF_AddEf, from a hook of its slot other than the queue's own (the traversal's recorder, traversal_record.cpp): the
// ocean's element noted when the traversal adds it.
void RenderQueue_NoteAddEf(void* re);
// At the join, on the main thread: the calls taken since the hand-off, made in their order (those the worker has made
// as the frame's end apart).
void RenderQueue_Replay();
// FlushThreadPresent=on, on the flush's thread right after the flush: what the join compares the flush's end with (the
// camera, the viewport, the matrices, the frame's statistics), copied before the frame's end changes it.
void RenderQueue_AfterFlush();
// FlushThreadPresent=on, on the flush's thread after the flush: the frame's end the main thread sealed at its EndFrame
// (the calls taken up to it), made in their order.
void RenderQueue_MakeSealed();
// At each hand-off of a flush to run beside the main thread: the last window's text buffer is free again.
void RenderQueue_OnHandOff();
void RenderQueue_Report(const char* when);
// What the queue is doing now, for a stall's log (another thread may ask): making the taken calls at a join, or not.
void RenderQueue_Where(char* buffer, unsigned size);

}  // namespace optiperf
