// The part of the Xiph.org vorbisfile API (vorbisfile.dll, libvorbis 1.0, __cdecl) that IShow
// uses.
#pragma once

#include <cstddef>
#include <cstdint>

typedef int64_t ogg_int64_t;

// Decoder state (0x2D0 bytes in this libvorbis build), only allocated and passed around here.
struct OggVorbis_File {
    unsigned char opaque[0x2d0];
};

struct vorbis_info {
    int version;
    int channels;                       // +0x04
    long rate;                          // +0x08
    long bitrate_upper;
    long bitrate_nominal;
    long bitrate_lower;
    long bitrate_window;
    void *codec_setup;
};

struct ov_callbacks {
    size_t(__cdecl *read_func)(void *ptr, size_t size, size_t nmemb, void *datasource);
    int(__cdecl *seek_func)(void *datasource, ogg_int64_t offset, int whence);
    int(__cdecl *close_func)(void *datasource);
    long(__cdecl *tell_func)(void *datasource);
};

extern "C" {
__declspec(dllimport) int __cdecl ov_open_callbacks(void *datasource, OggVorbis_File *vf, char *initial,
                                                    long ibytes, ov_callbacks callbacks);
__declspec(dllimport) int __cdecl ov_clear(OggVorbis_File *vf);
__declspec(dllimport) vorbis_info *__cdecl ov_info(OggVorbis_File *vf, int link);
__declspec(dllimport) long __cdecl ov_read(OggVorbis_File *vf, char *buffer, int length, int bigendianp,
                                           int word, int sgned, int *bitstream);
__declspec(dllimport) int __cdecl ov_time_seek(OggVorbis_File *vf, double s);
__declspec(dllimport) double __cdecl ov_time_tell(OggVorbis_File *vf);
}
