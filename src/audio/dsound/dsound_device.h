// The DirectSound backend's own declarations: the handles' conversions. Only the backend (and the
// tests, which put fake DirectSound objects in) include this.
#pragma once

#include "audio/audio.h"

namespace ls3d::audio::dsound {

// The backend's handles are the DirectSound interfaces. (A buffer handle may hold either buffer
// interface: IDirectSoundBuffer8 extends IDirectSoundBuffer, and the engine calls only the latter's
// methods.)
inline IDirectSound8 *DS(System *s) { return reinterpret_cast<IDirectSound8 *>(s); }
inline IDirectSoundBuffer *DS(Buffer *b) { return reinterpret_cast<IDirectSoundBuffer *>(b); }
inline IDirectSound3DBuffer *DS(Buffer3D *b) { return reinterpret_cast<IDirectSound3DBuffer *>(b); }
inline IDirectSound3DListener *DS(Listener *l) { return reinterpret_cast<IDirectSound3DListener *>(l); }
inline IKsPropertySet *DS(PropertySet *p) { return reinterpret_cast<IKsPropertySet *>(p); }
inline IDirectSoundNotify *DS(Notify *n) { return reinterpret_cast<IDirectSoundNotify *>(n); }
inline System *Handle(IDirectSound8 *s) { return reinterpret_cast<System *>(s); }
inline Buffer *Handle(IDirectSoundBuffer *b) { return reinterpret_cast<Buffer *>(b); }
inline Buffer *Handle(IDirectSoundBuffer8 *b) { return reinterpret_cast<Buffer *>(b); }
inline Buffer3D *Handle(IDirectSound3DBuffer *b) { return reinterpret_cast<Buffer3D *>(b); }
inline Listener *Handle(IDirectSound3DListener *l) { return reinterpret_cast<Listener *>(l); }
inline PropertySet *Handle(IKsPropertySet *p) { return reinterpret_cast<PropertySet *>(p); }
inline Notify *Handle(IDirectSoundNotify *n) { return reinterpret_cast<Notify *>(n); }

}  // namespace ls3d::audio::dsound
