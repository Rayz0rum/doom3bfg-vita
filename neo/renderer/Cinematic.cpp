/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013-2014 Robert Beckebans
Copyright (C) 2014 Carl Kenner

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#pragma hdrstop
#include "precompiled.h"

extern idCVar s_noSound;

#define JPEG_INTERNALS
// extern "C" {
#include "../libs/jpeg-6/jpeglib.h"
//}

#include "tr_local.h"

#if defined(__vita__)
#include "../sys/vita/vita_public.h"
class idCinematicLocal;
void Vita_CinematicRegister(idCinematicLocal* cin, bool add);
bool Vita_CinematicEvict(idCinematicLocal* requester, int now);
bool Vita_ReadMp4Header(idFile* f, int& width, int& height, int& lengthMs);
#endif

#define CIN_system 1
#define CIN_loop 2
#define CIN_hold 4
#define CIN_silent 8
#define CIN_shader 16

#if defined(USE_FFMPEG)
// Carl: ffmpg for bink video files
extern "C" {

// #ifdef WIN32
#ifndef INT64_C
#define INT64_C(c) (c##LL)
#define UINT64_C(c) (c##ULL)
#endif
#include <inttypes.h>
// #endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
#endif

class idCinematicLocal : public idCinematic {
public:
    idCinematicLocal();
    virtual ~idCinematicLocal();

    virtual bool InitFromFile(const char* qpath, bool looping);
    virtual cinData_t ImageForTime(int milliseconds);
    virtual int AnimationLength();
    // RB begin
    bool IsPlaying() const;
    // RB end
    virtual void Close();
    virtual void ResetTime(int time);

private:
#if defined(USE_FFMPEG)
    int video_stream_index;
    AVFormatContext* fmt_ctx;
    AVFrame* frame;
    AVFrame* frame2;
    AVCodec* dec;
    AVCodecContext* dec_ctx;
    SwsContext* img_convert_ctx;
    bool hasFrame;
    long framePos;

    cinData_t ImageForTimeFFMPEG(int milliseconds);
    bool InitFromFFMPEGFile(const char* qpath, bool looping);
    void FFMPEGReset();
#endif
    idImage* img;
    bool isRoQ;

#if defined(__vita__)
    // The Vita plays every video through the hardware H.264 / AAC player
    // (sceAvPlayer, vita_avplayer.cpp) from the .mp4 shipped beside it,
    // opened when the video is first drawn rather than when its material is
    // parsed. See "Video" in VITA_PORT.md.
    vitaVideo_t* vitaVideo;    // the hardware player, NULL when none
    idStr vitaMp4Path;         // OS path of the .mp4 ("" when there is none)
    int vitaStreamW, vitaStreamH, vitaLengthMs; // from the .mp4's header
    bool vitaLooping;
    volatile bool vitaRestart; // ResetTime asked for the start (game thread)
    int vitaLastDrawMs;
    int vitaAppearMs;          // when it last came on screen (not drawn for a second before)
    int vitaNextTryMs;         // when to ask for a hardware player again
    int vitaFails;             // players in a row that gave no frame (backs the retries off)
    int vitaUploadedQuad;      // the RoQ frame last uploaded (skip unchanged ones)
    // The software stream beside the hardware one: the Vita's H.264 decoder
    // serves one player at a time (vita_avplayer.cpp), so a video that cannot
    // have it - another, more important one is playing - plays its .roq
    // instead (tools/vita/make_roq.py: the in-world screens, small, 15 fps)
    // with the engine's own RoQ decoder, as DOOM 3 shipped them. It stays in
    // software while it is on screen; back on screen later, it asks for the
    // hardware player first again.
    idStr vitaRoqName;         // the .roq beside the .mp4 ("" when there is none)
    bool vitaRoqActive;        // playing in software now
    bool VitaStartRoQ();
    void VitaStopRoQ(const char* why);
    cinData_t ImageForTimeVita(int thisTime);
    cinData_t ImageForTimeVitaHw();
    cinData_t ImageForTimeRoQ(int thisTime);
    void VitaReleasePlayer();
    friend bool Vita_CinematicEvict(idCinematicLocal* requester, int now);
#endif

    // RB: 64 bit fixes, changed long to int
    size_t mcomp[256];
    // RB end
    byte** qStatus[2];
    idStr fileName;
    int CIN_WIDTH, CIN_HEIGHT;
    idFile* iFile;
    cinStatus_t status;
    // RB: 64 bit fixes, changed long to int
    int tfps;
    int RoQPlayed;
    int ROQSize;
    unsigned int RoQFrameSize;
    int onQuad;
    int numQuads;
    int samplesPerLine;
    unsigned int roq_id;
    int screenDelta;
    byte* buf;
    int samplesPerPixel; // defaults to 2
    unsigned int xsize, ysize, maxsize, minsize;
    int normalBuffer0;
    int roq_flags;
    int roqF0;
    int roqF1;
    int t[2];
    int roqFPS;
    int drawX, drawY;
    // RB end

    int animationLength;
    int startTime;
    float frameRate;

    byte* image;

    bool looping;
    bool dirty;
    bool half;
    bool smootheddouble;
    bool inMemory;

    void RoQ_init();
    void blitVQQuad32fs(byte** status, unsigned char* data);
    void RoQShutdown();
    void RoQInterrupt();

    void move8_32(byte* src, byte* dst, int spl);
    void move4_32(byte* src, byte* dst, int spl);
    void blit8_32(byte* src, byte* dst, int spl);
    void blit4_32(byte* src, byte* dst, int spl);
    void blit2_32(byte* src, byte* dst, int spl);

    // RB: 64 bit fixes, changed long to int
    unsigned short yuv_to_rgb(int y, int u, int v);
    unsigned int yuv_to_rgb24(int y, int u, int v);

    void decodeCodeBook(byte* input, unsigned short roq_flags);
    void recurseQuad(int startX, int startY, int quadSize, int xOff, int yOff);
    void setupQuad(int xOff, int yOff);
    void readQuadInfo(byte* qData);
    void RoQPrepMcomp(int xoff, int yoff);
    void RoQReset();
    // RB end
};

// Carl: ROQ files from original Doom 3
const int DEFAULT_CIN_WIDTH = 512;
const int DEFAULT_CIN_HEIGHT = 512;
const int MAXSIZE = 8;
const int MINSIZE = 4;

const int ROQ_FILE = 0x1084;
const int ROQ_QUAD = 0x1000;
const int ROQ_QUAD_INFO = 0x1001;
const int ROQ_CODEBOOK = 0x1002;
const int ROQ_QUAD_VQ = 0x1011;
const int ROQ_QUAD_JPEG = 0x1012;
const int ROQ_QUAD_HANG = 0x1013;
const int ROQ_PACKET = 0x1030;
const int ZA_SOUND_MONO = 0x1020;
const int ZA_SOUND_STEREO = 0x1021;

// temporary buffers used by all cinematics
// RB: 64 bit fixes, changed long to int
static int ROQ_YY_tab[256];
static int ROQ_UB_tab[256];
static int ROQ_UG_tab[256];
static int ROQ_VG_tab[256];
static int ROQ_VR_tab[256];
// RB end
static byte* file = NULL;
static unsigned short* vq2 = NULL;
static unsigned short* vq4 = NULL;
static unsigned short* vq8 = NULL;

//===========================================

/*
==============
idCinematicLocal::InitCinematic
==============
*/
// RB: 64 bit fixes, changed long to int
#if defined(__vita__)
// Every video the game has (the 51 in the retail data: tools/vita/convert_video.py
// makes their .mp4 under base/video and base/sound/VO/video). Checked at
// startup so a missing or misplaced file is named in console.txt at once.
static const char* vitaExpectedVideos[] = {
    "sound/VO/video/deployment.mp4",
    "sound/VO/video/epd.mp4",
    "sound/VO/video/generator.mp4",
    "sound/VO/video/grabber.mp4",
    "sound/VO/video/hydrocon.mp4",
    "sound/VO/video/mfs.mp4",
    "sound/VO/video/video_alphalabs_intro.mp4",
    "sound/VO/video/video_ancient_civ.mp4",
    "sound/VO/video/video_bfg.mp4",
    "sound/VO/video/video_chaingun.mp4",
    "sound/VO/video/video_demon_museum.mp4",
    "sound/VO/video/video_ian_report.mp4",
    "sound/VO/video/video_ipn_news.mp4",
    "sound/VO/video/video_marine_sops.mp4",
    "sound/VO/video/video_meyers_report.mp4",
    "sound/VO/video/video_plasmagun.mp4",
    "sound/VO/video/video_recycling.mp4",
    "sound/VO/video/video_soulcube.mp4",
    "sound/VO/video/video_tablets.mp4",
    "sound/VO/video/video_uac_welcome.mp4",
    "video/admin/betruger.mp4",
    "video/comm1/swann_vidphone1.mp4",
    "video/comm1/swann_vidphone2.mp4",
    "video/comm1/swann_vidphone3.mp4",
    "video/data.mp4",
    "video/delta1/sarge_video1.mp4",
    "video/enpro1/lm_meyers_enpro.mp4",
    "video/erebusteam.mp4",
    "video/fireball.mp4",
    "video/hellhole/rescue.mp4",
    "video/intro/introloop.mp4",
    "video/loadvideo.mp4",
    "video/mars1.mp4",
    "video/mars_rotation.mp4",
    "video/marscity/decom.mp4",
    "video/marscity/dropship.mp4",
    "video/marscity/monoflyby.mp4",
    "video/marscity/sarge_mission_brief.mp4",
    "video/marscity/sci_team_request.mp4",
    "video/marscity/ship.mp4",
    "video/mc_underground/inv1sm.mp4",
    "video/mc_underground/inv2sm.mp4",
    "video/mc_underground/inv3.mp4",
    "video/mc_underground/inv4sm.mp4",
    "video/mc_underground/inv5sm.mp4",
    "video/mc_underground/inv6sm.mp4",
    "video/mc_underground/inv7sm.mp4",
    "video/mcneil.mp4",
    "video/radar.mp4",
    "video/recycling2/cs_bert01_composite.mp4",
    "video/recycling2/cs_bert02_composite.mp4",
    NULL
};

static void Vita_AuditVideos()
{
    int found = 0, missing = 0;
    idStr missingList;
    for (int i = 0; vitaExpectedVideos[i] != NULL; i++) {
        idFile* f = fileSystem->OpenFileRead(vitaExpectedVideos[i]);
        if (f != NULL) {
            if (f->Length() < 1024) {
                common->Printf("video audit: %s is only %d bytes - a broken copy?\n", vitaExpectedVideos[i], f->Length());
            }
            fileSystem->CloseFile(f);
            found++;
        } else {
            missing++;
            missingList += va("\n  %s", vitaExpectedVideos[i]);
        }
    }
    if (missing > 0) {
        common->Warning("video audit: %d of the game's %d videos are missing on the memory card (copy them into ux0:data/doom3bfg/base/, keeping the folders):%s",
            missing, found + missing, missingList.c_str());
    } else {
        common->Printf("video audit: all %d videos of the game are on the memory card\n", found);
    }
}
#endif

void idCinematic::InitCinematic()
{
#if defined(__vita__)
    // (the video arena is claimed once GXM is up: idRenderSystemLocal::InitOpenGL)
    Vita_AuditVideos();
#endif
#if defined(USE_FFMPEG)
    // Carl: ffmpeg for Bink and regular video files
    common->Warning("Loading FFMPEG...\n");
    avcodec_register_all();
    av_register_all();
#endif

    // Carl: Doom 3 ROQ:
    float t_ub, t_vr, t_ug, t_vg;
    int i;

    // generate YUV tables
    t_ub = (1.77200f / 2.0f) * (float)(1 << 6) + 0.5f;
    t_vr = (1.40200f / 2.0f) * (float)(1 << 6) + 0.5f;
    t_ug = (0.34414f / 2.0f) * (float)(1 << 6) + 0.5f;
    t_vg = (0.71414f / 2.0f) * (float)(1 << 6) + 0.5f;
    for (i = 0; i < 256; i++) {
        float x = (float)(2 * i - 255);

        ROQ_UB_tab[i] = (int)((t_ub * x) + (1 << 5));
        ROQ_VR_tab[i] = (int)((t_vr * x) + (1 << 5));
        ROQ_UG_tab[i] = (int)((-t_ug * x));
        ROQ_VG_tab[i] = (int)((-t_vg * x) + (1 << 5));
        ROQ_YY_tab[i] = (int)((i << 6) | (i >> 2));
    }

    file = (byte*)Mem_Alloc(65536, TAG_CINEMATIC);
    vq2 = (word*)Mem_Alloc(256 * 16 * 4 * sizeof(word), TAG_CINEMATIC);
    vq4 = (word*)Mem_Alloc(256 * 64 * 4 * sizeof(word), TAG_CINEMATIC);
    vq8 = (word*)Mem_Alloc(256 * 256 * 4 * sizeof(word), TAG_CINEMATIC);
}

/*
==============
idCinematicLocal::ShutdownCinematic
==============
*/
void idCinematic::ShutdownCinematic()
{
    // Carl: Original Doom 3 RoQ files:
    Mem_Free(file);
    file = NULL;
    Mem_Free(vq2);
    vq2 = NULL;
    Mem_Free(vq4);
    vq4 = NULL;
    Mem_Free(vq8);
    vq8 = NULL;
}

/*
==============
idCinematic::Alloc
==============
*/
idCinematic* idCinematic::Alloc()
{
    return new idCinematicLocal; // Carl: Use the proper class like in Doom 3, not just the unimplemented abstract one.
}

/*
==============
idCinematic::~idCinematic
==============
*/
idCinematic::~idCinematic()
{
    Close();
}

/*
==============
idCinematic::InitFromFile
==============
*/
bool idCinematic::InitFromFile(const char* qpath, bool looping)
{
    return false; // Carl: this is just the abstract virtual method
}

/*
==============
idCinematic::AnimationLength
==============
*/
int idCinematic::AnimationLength()
{
    return 0;
}

/*
==============
idCinematic::GetStartTime
==============
*/
int idCinematic::GetStartTime()
{
    return -1;
}

/*
==============
idCinematic::ResetTime
==============
*/
void idCinematic::ResetTime(int milliseconds)
{
}

/*
==============
idCinematic::ImageForTime
==============
*/
cinData_t idCinematic::ImageForTime(int milliseconds)
{
    cinData_t c;
    memset(&c, 0, sizeof(c));
    declManager->FindMaterial("doom64.tga");
    c.image = globalImages->GetImage("doom64.tga");
    return c;
}

/*
==============
idCinematic::ExportToTGA
==============
*/
void idCinematic::ExportToTGA(bool skipExisting)
{
}

/*
==============
idCinematic::GetFrameRate
==============
*/
float idCinematic::GetFrameRate() const
{
    return 30.0f;
}

/*
==============
idCinematic::Close
==============
*/
void idCinematic::Close()
{
}

// RB begin
bool idCinematic::IsPlaying() const
{
    return false;
}
// RB end

//===========================================

/*
==============
idCinematicLocal::idCinematicLocal
==============
*/
idCinematicLocal::idCinematicLocal()
{
    qStatus[0] = (byte**)Mem_Alloc(32768 * sizeof(byte*), TAG_CINEMATIC);
    qStatus[1] = (byte**)Mem_Alloc(32768 * sizeof(byte*), TAG_CINEMATIC);

#if defined(__vita__)
    vitaVideo = NULL;
    vitaStreamW = vitaStreamH = vitaLengthMs = 0;
    vitaLooping = false;
    vitaRestart = false;
    vitaLastDrawMs = 0;
    vitaAppearMs = 0;
    vitaNextTryMs = 0;
    vitaFails = 0;
    vitaUploadedQuad = -1;
    vitaRoqActive = false;
    Vita_CinematicRegister(this, true);
#endif

#if defined(USE_FFMPEG)
    // Carl: ffmpeg stuff, for bink and normal video files:
    isRoQ = false;
    //	fmt_ctx = avformat_alloc_context();
    frame = avcodec_alloc_frame();
    frame2 = avcodec_alloc_frame();
    dec_ctx = NULL;
    fmt_ctx = NULL;
    video_stream_index = -1;
    img_convert_ctx = NULL;
    hasFrame = false;
#endif

    // Carl: Original Doom 3 RoQ files:
    image = NULL;
    status = FMV_EOF;
    buf = NULL;
    iFile = NULL;
    img = globalImages->AllocStandaloneImage("_cinematic");
#if defined(__vita__)
    // A cinematic is created when its material is parsed, which during play
    // happens on the game/draw thread; vitaGL only works on the GL thread.
    // The texture is then made at the first frame the back end asks for.
    if (img != NULL && Vita_IsGLThread()) {
#else
    if (img != NULL) {
#endif
        idImageOpts opts;
        opts.format = FMT_RGBA8;
        opts.colorFormat = CFM_DEFAULT;
        opts.width = 32;
        opts.height = 32;
        opts.numLevels = 1;
        img->AllocImage(opts, TF_LINEAR, TR_REPEAT);
    }
}

/*
==============
idCinematicLocal::~idCinematicLocal
==============
*/
idCinematicLocal::~idCinematicLocal()
{
#if defined(__vita__)
    Vita_CinematicRegister(this, false);
#endif
    Close();

    // Carl: Original Doom 3 RoQ files:
    Mem_Free(qStatus[0]);
    qStatus[0] = NULL;
    Mem_Free(qStatus[1]);
    qStatus[1] = NULL;

#if defined(USE_FFMPEG)
    // Carl: ffmpeg for bink and other video files:

    // RB: TODO double check this. It seems we have different versions of ffmpeg on Kubuntu 13.10 and the win32 development files
#if defined(_WIN32) || defined(_WIN64)
    avcodec_free_frame(&frame);
    avcodec_free_frame(&frame2);
#else
    av_freep(&frame);
    av_freep(&frame2);
#endif

    if (fmt_ctx) {
        avformat_free_context(fmt_ctx);
    }

    if (img_convert_ctx) {
        sws_freeContext(img_convert_ctx);
    }
#endif

    delete img;
    img = NULL;
}

/*
==============
idCinematicLocal::InitFromFFMPEGFile
==============
*/
#if defined(USE_FFMPEG)
bool idCinematicLocal::InitFromFFMPEGFile(const char* qpath, bool amilooping)
{
    int ret;
    looping = amilooping;
    startTime = 0;
    isRoQ = false;
    CIN_HEIGHT = DEFAULT_CIN_HEIGHT;
    CIN_WIDTH = DEFAULT_CIN_WIDTH;
    idStr fullpath = fileSystem->RelativePathToOSPath(qpath, "fs_basepath");

    if ((ret = avformat_open_input(&fmt_ctx, fullpath, NULL, NULL)) < 0) {
        common->Warning("idCinematic: Cannot open FFMPEG video file: '%s', %d\n", qpath, looping);
        return false;
    }
    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
        common->Warning("idCinematic: Cannot find stream info: '%s', %d\n", qpath, looping);
        return false;
    }
    /* select the video stream */
    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (ret < 0) {
        common->Warning("idCinematic: Cannot find a video stream in: '%s', %d\n", qpath, looping);
        return false;
    }
    video_stream_index = ret;
    dec_ctx = fmt_ctx->streams[video_stream_index]->codec;
    /* init the video decoder */
    if ((ret = avcodec_open2(dec_ctx, dec, NULL)) < 0) {
        common->Warning("idCinematic: Cannot open video decoder for: '%s', %d\n", qpath, looping);
        return false;
    }

    common->Warning("Loaded FFMPEG file: '%s', looping=%d\n", qpath, looping);
    CIN_WIDTH = dec_ctx->width;
    CIN_HEIGHT = dec_ctx->height;
    /** Calculate Duration in seconds
     * This is the fundamental unit of time (in seconds) in terms
     * of which frame timestamps are represented. For fixed-fps content,
     * timebase should be 1/framerate and timestamp increments should be
     * identically 1.
     * - encoding: MUST be set by user.
     * - decoding: Set by libavcodec.
     */
    AVRational avr = dec_ctx->time_base;
    /**
     * For some codecs, the time base is closer to the field rate than the frame rate.
     * Most notably, H.264 and MPEG-2 specify time_base as half of frame duration
     * if no telecine is used ...
     *
     * Set to time_base ticks per frame. Default 1, e.g., H.264/MPEG-2 set it to 2.
     */
    int ticksPerFrame = dec_ctx->ticks_per_frame;
    float durationSec = static_cast<double>(fmt_ctx->streams[video_stream_index]->duration) * static_cast<double>(ticksPerFrame) / static_cast<double>(avr.den);
    animationLength = durationSec * 1000;
    frameRate = av_q2d(fmt_ctx->streams[video_stream_index]->r_frame_rate);
    buf = NULL;
    hasFrame = false;
    framePos = -1;
    common->Warning("%dx%d, %f FPS, %f sec", CIN_WIDTH, CIN_HEIGHT, frameRate, durationSec);
    image = (byte*)Mem_Alloc(CIN_WIDTH * CIN_HEIGHT * 4 * 2, TAG_CINEMATIC);
    avpicture_fill((AVPicture*)frame2, image, PIX_FMT_BGR32, CIN_WIDTH, CIN_HEIGHT);
    if (img_convert_ctx) {
        sws_freeContext(img_convert_ctx);
    }
    img_convert_ctx = sws_getContext(dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt, CIN_WIDTH, CIN_HEIGHT, PIX_FMT_BGR32, SWS_BICUBIC, NULL, NULL, NULL);
    status = FMV_PLAY;

    startTime = 0;
    ImageForTime(0);
    status = (looping) ? FMV_PLAY : FMV_IDLE;

    // startTime = Sys_Milliseconds();

    return true;
}
#endif

/*
==============
idCinematicLocal::FFMPEGReset
==============
*/
#if defined(USE_FFMPEG)
void idCinematicLocal::FFMPEGReset()
{
    // RB: don't reset startTime here because that breaks video replays in the PDAs
    // startTime = 0;

    framePos = -1;

    if (av_seek_frame(fmt_ctx, video_stream_index, 0, 0) >= 0) {
        status = FMV_LOOPED;
    } else {
        status = FMV_EOF;
    }
}
#endif

/*
==============
idCinematicLocal::InitFromFile
==============
*/
bool idCinematicLocal::InitFromFile(const char* qpath, bool amilooping)
{
    unsigned short RoQID;

    Close();

    inMemory = 0;
    animationLength = 100000;

    // Carl: if no folder is specified, look in the video folder
    if (strstr(qpath, "/") == NULL && strstr(qpath, "\\") == NULL) {
        sprintf(fileName, "video/%s", qpath);
    } else {
        sprintf(fileName, "%s", qpath);
    }
    // Carl: Look for original Doom 3 RoQ files first:
    idStr ext;
    fileName.ExtractFileExtension(ext);
    fileName = fileName.StripFileExtension();

#if defined(__vita__)
    // The .mp4 shipped beside the video. Nothing is opened here - this runs
    // when the material is parsed, often on the game/draw thread; the
    // hardware player is asked for when the video is first drawn
    // (ImageForTimeVita).
    {
        vitaMp4Path.Clear();
        vitaRestart = false;
        vitaStreamW = vitaStreamH = vitaLengthMs = 0;
        vitaNextTryMs = 0;
        vitaFails = 0;
        vitaUploadedQuad = -1;

        idStr mp4Name = fileName + ".mp4";
        idFile* probe = fileSystem->OpenFileRead(mp4Name);
        // Five invasion screens of mc_underground ask for full-size videos
        // the retail data never had (inv1, inv2, inv5, inv6, inv7); their
        // small versions (inv1sm...) exist - and the reverse (inv3sm, inv4)
        if (probe == NULL) {
            idStr alt = fileName;
            if (alt.Right(2).Icmp("sm") == 0) {
                alt = alt.Left(alt.Length() - 2);
            } else {
                alt += "sm";
            }
            alt += ".mp4";
            probe = fileSystem->OpenFileRead(alt);
            if (probe != NULL) {
                common->Printf("video '%s' does not exist - playing '%s' in its place\n", mp4Name.c_str(), alt.c_str());
                mp4Name = alt;
            }
        }
        if (probe != NULL) {
            Vita_ReadMp4Header(probe, vitaStreamW, vitaStreamH, vitaLengthMs);
            fileSystem->CloseFile(probe);
            // sceAvPlayer opens the file itself, so it needs a real console
            // path rather than an engine-relative one.
            vitaMp4Path = fileSystem->RelativePathToOSPath(mp4Name, "fs_basepath");
        }
        if (vitaMp4Path.Length() > 0) {
            // and its software twin, for when the hardware player is taken
            vitaRoqName = mp4Name;
            vitaRoqName.SetFileExtension("roq");
            idFile* roq = fileSystem->OpenFileRead(vitaRoqName);
            if (roq != NULL) {
                fileSystem->CloseFile(roq);
            } else {
                vitaRoqName.Clear();
            }
            fileName = mp4Name;
            looping = amilooping;
            vitaLooping = amilooping;
            startTime = 0;
            status = FMV_PLAY;
            animationLength = (vitaLengthMs > 0) ? vitaLengthMs : 100000;
            CIN_WIDTH = vitaStreamW;
            CIN_HEIGHT = vitaStreamH;
            common->Printf("video '%s': %dx%d, %d ms, %s%s\n", mp4Name.c_str(), vitaStreamW, vitaStreamH, vitaLengthMs, amilooping ? "looping" : "once",
                vitaRoqName.Length() > 0 ? " (and a .roq for when the hardware player is taken)" : "");
            return true;
        }
        common->Warning("video '%s': no .mp4 beside it on the Vita - it will show black (convert it with tools/vita/convert_video.py)", fileName.c_str());
    }
#endif

    fileName = fileName + ".roq";
    // if (fileName == "video\\loadvideo.roq") {
    //	fileName = "video\\idlogo.roq";
    // }

    iFile = fileSystem->OpenFileRead(fileName);

    // Carl: If the RoQ file doesn't exist, try using ffmpeg instead:
    if (!iFile) {
#if defined(USE_FFMPEG)
        idLib::Warning("Original Doom 3 RoQ Cinematic not found: '%s'\n", fileName.c_str());
        idStr temp = fileName.StripFileExtension() + ".bik";
        animationLength = 0;
        hasFrame = false;
        RoQShutdown();
        fileName = temp;
        idLib::Warning("New filename: '%s'\n", fileName.c_str());
        return InitFromFFMPEGFile(fileName.c_str(), amilooping);
#else
        animationLength = 0;
        return false;
#endif
    }
    // Carl: The rest of this function is for original Doom 3 RoQ files:
    isRoQ = true;
    ROQSize = iFile->Length();

    looping = amilooping;

    CIN_HEIGHT = DEFAULT_CIN_HEIGHT;
    CIN_WIDTH = DEFAULT_CIN_WIDTH;
    samplesPerPixel = 4;
    startTime = 0; // Sys_Milliseconds();
    buf = NULL;

    iFile->Read(file, 16);

    RoQID = (unsigned short)(file[0]) + (unsigned short)(file[1]) * 256;

    frameRate = file[6];
    if (frameRate == 32.0f) {
        frameRate = 1000.0f / 32.0f;
    }

    if (RoQID == ROQ_FILE) {
        RoQ_init();
        status = FMV_PLAY;
        ImageForTime(0);
        status = (looping) ? FMV_PLAY : FMV_IDLE;
        return true;
    }

    RoQShutdown();
    return false;
}

/*
==============
idCinematicLocal::Close
==============
*/
void idCinematicLocal::Close()
{
#if defined(__vita__)
    if (vitaVideo != NULL) {
        VitaVideo_Close(vitaVideo);
        vitaVideo = NULL;
        status = FMV_EOF;
    }
    if (vitaRoqActive) {
        VitaStopRoQ("closed");
    }
#endif

    if (image) {
        Mem_Free((void*)image);
        image = NULL;
        buf = NULL;
        status = FMV_EOF;
    }

    RoQShutdown();

#if defined(USE_FFMPEG)
    hasFrame = false;

    if (!isRoQ) {
        if (img_convert_ctx) {
            sws_freeContext(img_convert_ctx);
        }

        img_convert_ctx = NULL;

        if (dec_ctx) {
            avcodec_close(dec_ctx);
        }

        if (fmt_ctx) {
            avformat_close_input(&fmt_ctx);
        }
        status = FMV_EOF;
    }
#endif
}

/*
==============
idCinematicLocal::AnimationLength
==============
*/
int idCinematicLocal::AnimationLength()
{
    return animationLength;
}

// RB begin
bool idCinematicLocal::IsPlaying() const
{
#if defined(__vita__)
    if (vitaVideo != NULL) {
        return VitaVideo_IsPlaying(vitaVideo);
    }
#endif

    return (status == FMV_PLAY);
}
// RB end

/*
==============
idCinematicLocal::ResetTime
==============
*/
void idCinematicLocal::ResetTime(int time)
{
#if defined(__vita__)
    // From the start, at the next draw (on the GL thread: a player that has
    // ended is closed and a new one opened; one still playing is rewound).
    // The PDA calls this from the game thread when a video disc is played.
    vitaRestart = true;
#endif

    startTime = time; // originally this was: ( backEnd.viewDef ) ? 1000 * backEnd.viewDef->floatTime : -1;
    status = FMV_PLAY;
}

#if defined(__vita__)
/*
==============
idCinematicLocal::ImageForTimeVita

The hardware decoder paces itself, so the engine time this is nominally
called with is not used: sceAvPlayer is asked whether a newer frame exists
and hands one back when it does.

Only cinData_t::image is filled in, never the Y/Cr/Cb triple. GXM converts
YUV to RGB inside the texture unit, so as far as the renderer is concerned
this is an ordinary colour texture and the existing single-image branch of
RB_BindVariableStageImage draws it with the standard textured shader.
==============
*/
cinData_t idCinematicLocal::ImageForTimeVitaHw()
{
    cinData_t cinData;
    memset(&cinData, 0, sizeof(cinData));

    int width = 0;
    int height = 0;

    if (img != NULL && !img->IsLoaded() && Vita_IsGLThread()) {
        // created off the GL thread (see the constructor): make it now
        idImageOpts opts;
        opts.format = FMT_RGBA8;
        opts.colorFormat = CFM_DEFAULT;
        opts.width = 32;
        opts.height = 32;
        opts.numLevels = 1;
        img->AllocImage(opts, TF_LINEAR, TR_REPEAT);
    }

    if (img != NULL && img->IsLoaded() && Vita_IsGLThread()
        && VitaVideo_UpdateFrame(vitaVideo, img->GetTexNum(), &width, &height)) {
        CIN_WIDTH = width;
        CIN_HEIGHT = height;
        vitaFails = 0;

        cinData.imageWidth = width;
        cinData.imageHeight = height;
        cinData.image = img;
    }

    // Before the first frame arrives cinData.image stays NULL and the caller
    // draws black, which is what the other decoders do while starting up.
    status = VitaVideo_IsPlaying(vitaVideo) ? FMV_PLAY : FMV_EOF;
    cinData.status = status;

    return cinData;
}
#endif

/*
==============
idCinematicLocal::ImageForTime
==============
*/
#if defined(__vita__)
/*
==============
The Vita's cinematics: who has a hardware player

Every cinematic is listed so that one needing a player can take the one of
another nobody is looking at. There is no time-out: a player stays with its
video until another needs the room, so a screen looked at again keeps playing
instead of starting over from black.
==============
*/
static idList<idCinematicLocal*> vitaCinematics;
// The video a player was just taken for (one player at a time): until it has
// opened its own, or has not been drawn for half a second, nobody else opens
// one - the looping screen that gave its player up would otherwise grab it
// back the moment the old player closes. (Set and read on the GL thread.)
static idCinematicLocal* vitaPlayerReservedFor = NULL;
// when a video played once (a cutscene, a PDA disc, a screen's message) was
// last drawn: a looping screen (the menu's background) does not open a player
// for 2 s after - in the fourth verification run it took the player back
// between every two videos, and every open is a chance of a player that
// never starts
static int vitaLastOnceDrawMs = -100000;
static int vitaPlayerReservedMs = 0;
static idSysMutex vitaCinematicsLock;

void Vita_CinematicRegister(idCinematicLocal* cin, bool add)
{
    idScopedCriticalSection lock(vitaCinematicsLock);
    if (add) {
        vitaCinematics.Append(cin);
    } else {
        vitaCinematics.Remove(cin);
        if (vitaPlayerReservedFor == cin) {
            vitaPlayerReservedFor = NULL;
        }
    }
}

// The player of the cinematic drawn longest ago, given up for the requester:
// one not drawn for 200 ms, or - for a video played once (a PDA video, a
// cutscene) - any looping one (world screens, menu backgrounds). True if one
// was closed (its memory comes back when the worker has closed it).
bool Vita_CinematicEvict(idCinematicLocal* requester, int now)
{
    idScopedCriticalSection lock(vitaCinematicsLock);
    idCinematicLocal* victim = NULL;
    for (int i = 0; i < vitaCinematics.Num(); i++) {
        idCinematicLocal* c = vitaCinematics[i];
        if (c == requester || c->vitaVideo == NULL) {
            continue;
        }
        // Who gives up a player: one nobody has drawn for 200 ms; a looping
        // screen when a video played once wants it; and otherwise the video
        // that came on screen before the one asking - the newest thing on
        // screen is what the player is looking at (the intro over the menu's
        // background). The one that lost cannot take it back while the newer
        // one stays on screen, so two videos never trade a player to and fro.
        // not drawn for a second: a hitch (a screenshot written, a level's
        // first frames) is not leaving the screen - 200 ms let the menu's
        // background take the player from a video still on screen
        const bool idle = (now - c->vitaLastDrawMs) > 1000;
        // a video played once that has reached its end holds its last frame
        // on screen and nothing more: its player is anyone's (one player at a
        // time, vita_avplayer.cpp)
        const bool finished = !VitaVideo_IsPlaying(c->vitaVideo);
        const bool outranked = !requester->vitaLooping && c->vitaLooping;
        const bool older = c->vitaAppearMs < requester->vitaAppearMs;
        if (!idle && !outranked && !older && !finished) {
            continue;
        }
        if (victim == NULL || c->vitaLastDrawMs < victim->vitaLastDrawMs) {
            victim = c;
        }
    }
    if (victim == NULL) {
        static int lastLog = 0;
        if (now - lastLog > 2000) {
            lastLog = now;
            common->Printf("video '%s' waits: every player belongs to a video that came on screen after it\n", requester->fileName.c_str());
        }
        return false;
    }
    common->Printf("video '%s' gives its hardware player to '%s' (%s)\n", victim->fileName.c_str(), requester->fileName.c_str(),
        (now - victim->vitaLastDrawMs) > 1000 ? "not drawn any more" : !VitaVideo_IsPlaying(victim->vitaVideo) ? "it has finished" : (!requester->vitaLooping && victim->vitaLooping) ? "a looping screen gives way to a video played once" : "the newer video on screen wins");
    victim->VitaReleasePlayer();
    vitaPlayerReservedFor = requester;
    vitaPlayerReservedMs = now;
    return true;
}

// the .mp4's picture size and length, from its header (the converter writes
// the index at the front: -movflags +faststart)
bool Vita_ReadMp4Header(idFile* f, int& width, int& height, int& lengthMs)
{
    width = height = lengthMs = 0;
    const int fileLen = f->Length();
    int pos = 0;
    while (pos + 8 <= fileLen) {
        byte hdr[16];
        f->Seek(pos, FS_SEEK_SET);
        if (f->Read(hdr, 8) != 8) {
            return false;
        }
        int64 size = ((int64)hdr[0] << 24) | (hdr[1] << 16) | (hdr[2] << 8) | hdr[3];
        int headerLen = 8;
        if (size == 1) {
            if (f->Read(hdr + 8, 8) != 8) {
                return false;
            }
            size = 0;
            for (int i = 8; i < 16; i++) {
                size = (size << 8) | hdr[i];
            }
            headerLen = 16;
        } else if (size == 0) {
            size = fileLen - pos;
        }
        if (size < headerLen) {
            return false;
        }
        if (memcmp(hdr + 4, "moov", 4) == 0 && size < 4 * 1024 * 1024) {
            const int moovLen = (int)size - headerLen;
            byte* moov = (byte*)Mem_Alloc(moovLen, TAG_CINEMATIC);
            if (f->Read(moov, moovLen) != moovLen) {
                Mem_Free(moov);
                return false;
            }
            // walk the boxes inside: mvhd (length), trak > tkhd (size)
            struct walk_t {
                static int U32(const byte* p)
                {
                    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
                }
            };
            int timescale = 0;
            int64 duration = 0;
            for (int o = 0; o + 8 <= moovLen;) {
                int bsize = walk_t::U32(moov + o);
                if (bsize < 8 || o + bsize > moovLen) {
                    break;
                }
                const byte* b = moov + o;
                if (memcmp(b + 4, "mvhd", 4) == 0) {
                    const int version = b[8];
                    if (version == 1 && bsize >= 40) {
                        timescale = walk_t::U32(b + 28);
                        duration = ((int64)walk_t::U32(b + 32) << 32) | (uint32)walk_t::U32(b + 36);
                    } else if (bsize >= 28) {
                        timescale = walk_t::U32(b + 20);
                        duration = (uint32)walk_t::U32(b + 24);
                    }
                } else if (memcmp(b + 4, "trak", 4) == 0) {
                    for (int t = 8; t + 8 <= bsize;) {
                        const int tsize = walk_t::U32(b + t);
                        if (tsize < 8 || t + tsize > bsize) {
                            break;
                        }
                        const byte* tb = b + t;
                        if (memcmp(tb + 4, "tkhd", 4) == 0) {
                            const int version = tb[8];
                            const int off = (version == 1) ? 96 : 84; // width, height: 16.16 at the end
                            if (tsize >= off + 8) {
                                const int w = walk_t::U32(tb + off) >> 16;
                                const int h = walk_t::U32(tb + off + 4) >> 16;
                                if (w > 0 && h > 0 && width == 0) {
                                    width = w;
                                    height = h;
                                }
                            }
                        }
                        t += tsize;
                    }
                }
                o += bsize;
            }
            Mem_Free(moov);
            if (timescale > 0) {
                lengthMs = (int)(duration * 1000 / timescale);
            }
            return width > 0;
        }
        pos += (int)size;
    }
    return false;
}

/*
==============
idCinematicLocal::VitaStartRoQ / VitaStopRoQ

The software stream (see vitaRoqName): the RoQ opened as InitFromFile opens
one, played from its start. At most VITA_MAX_ROQ_STREAMS at once - each
costs the render thread its decoding and a texture upload a frame.
==============
*/
#define VITA_MAX_ROQ_STREAMS 4
static int vitaRoqStreams = 0;

bool idCinematicLocal::VitaStartRoQ()
{
    if (vitaRoqStreams >= VITA_MAX_ROQ_STREAMS) {
        return false;
    }
    const idStr keepName = fileName;
    iFile = fileSystem->OpenFileRead(vitaRoqName);
    if (iFile == NULL) {
        vitaRoqName.Clear();
        return false;
    }
    isRoQ = true;
    ROQSize = iFile->Length();
    looping = vitaLooping;
    CIN_HEIGHT = DEFAULT_CIN_HEIGHT;
    CIN_WIDTH = DEFAULT_CIN_WIDTH;
    samplesPerPixel = 4;
    startTime = 0;
    buf = NULL;
    iFile->Read(file, 16);
    const unsigned short id = (unsigned short)(file[0]) + (unsigned short)(file[1]) * 256;
    frameRate = file[6];
    if (frameRate == 32.0f) {
        frameRate = 1000.0f / 32.0f;
    }
    if (id != ROQ_FILE) {
        common->Printf("video '%s': '%s' is not a RoQ file - no software stream\n", keepName.c_str(), vitaRoqName.c_str());
        fileSystem->CloseFile(iFile);
        iFile = NULL;
        vitaRoqName.Clear();
        return false;
    }
    RoQ_init();
    status = FMV_PLAY;
    vitaRoqActive = true;
    vitaRoqStreams++;
    vitaUploadedQuad = -1;
    fileName = keepName;
    common->Printf("video '%s' plays in software ('%s', %.0f fps) - the hardware player is another video's (%d software stream(s))\n",
        fileName.c_str(), vitaRoqName.c_str(), frameRate, vitaRoqStreams);
    return true;
}

void idCinematicLocal::VitaStopRoQ(const char* why)
{
    if (!vitaRoqActive) {
        return;
    }
    const idStr keepName = fileName;
    RoQShutdown(); // (it clears fileName)
    fileName = keepName;
    vitaRoqActive = false;
    vitaRoqStreams--;
    status = FMV_PLAY;
    common->Printf("video '%s': software stream stopped (%s)\n", fileName.c_str(), why);
}

void idCinematicLocal::VitaReleasePlayer()
{
    if (vitaVideo != NULL) {
        VitaVideo_Close(vitaVideo);
        vitaVideo = NULL;
    }
    vitaNextTryMs = 0;
}

/*
==============
idCinematicLocal::ImageForTimeVita

On the GL thread, at each draw: the hardware player, asked for here the first
time the video is drawn (taking the player of one nobody is looking at when
there is no room). Until its first frame the caller draws black, as with any
decoder starting up.
==============
*/
cinData_t idCinematicLocal::ImageForTimeVita(int thisTime)
{
    const int now = Sys_Milliseconds();
    if (vitaLastDrawMs == 0 || now - vitaLastDrawMs > 1000) {
        vitaAppearMs = now; // (back) on screen
        if (vitaRoqActive) {
            VitaStopRoQ("it left the screen"); // the hardware player first, again
        }
        common->Printf("video '%s' on screen (%dx%d, %s, %s)\n", fileName.c_str(), vitaStreamW, vitaStreamH,
            vitaLooping ? "looping" : "once", vitaVideo != NULL ? "has a player" : "needs a player");
    }
    vitaLastDrawMs = now;

    if (!vitaLooping) {
        vitaLastOnceDrawMs = now;
    }

    if (vitaRoqActive) {
        // in software while it stays on screen (a restart is ResetTime's,
        // which the RoQ decoder follows by itself)
        vitaRestart = false;
        return ImageForTimeRoQ(thisTime);
    }
    bool waits = false; // the hardware player is not coming for this video soon
    const bool loopHolds = vitaLooping && now - vitaLastOnceDrawMs < 2000;
    if (loopHolds && vitaVideo == NULL) {
        waits = true;
    }
    // A player that has not started 1.5 s after its open (they start in
    // 150-300 ms) is given up when the video has its .roq: the intro's
    // tracking screen (monoflyby, 2.7 s long) stalled and the 6 s wait was
    // longer than the video. The player is closed once it gets going (or
    // left alone), vita_avplayer.cpp.
    if (vitaVideo != NULL && vitaRoqName.Length() > 0 && VitaVideo_StartupMs(vitaVideo) > 1500) {
        vitaFails++;
        common->Printf("video '%s': no frame from the hardware player %d ms after its open - its .roq instead\n", fileName.c_str(), VitaVideo_StartupMs(vitaVideo));
        VitaReleasePlayer();
        vitaNextTryMs = now + (2000 << (vitaFails < 4 ? vitaFails : 4));
    }

    if (vitaRestart) {
        vitaRestart = false;
        if (vitaVideo != NULL) {
            if (VitaVideo_IsPlaying(vitaVideo) && !VitaVideo_Failed(vitaVideo)) {
                VitaVideo_Rewind(vitaVideo);
            } else {
                VitaReleasePlayer(); // ended: a new player from the start
            }
        }
    }

    if (vitaVideo != NULL && VitaVideo_Failed(vitaVideo)) {
        // backed off (2, 4, 8, 16 s): opening and closing a player that cannot
        // run, over and over, is churn inside sceAvPlayer for nothing
        vitaFails++;
        const int wait = 1000 << (vitaFails < 4 ? vitaFails : 4);
        common->Printf("video '%s': the hardware player gave no frame (%d in a row) - trying again in %d ms\n", fileName.c_str(), vitaFails, wait);
        VitaReleasePlayer();
        vitaNextTryMs = now + wait;
    }
    if (vitaPlayerReservedFor != NULL && vitaPlayerReservedFor != this) {
        // the player was taken for another video: it opens first, unless it
        // has left the screen
        if (now - vitaPlayerReservedFor->vitaLastDrawMs > 1000 || now - vitaPlayerReservedMs > 20000) {
            vitaPlayerReservedFor = NULL;
        } else {
            waits = true;
        }
    }
    if (vitaVideo == NULL && now < vitaNextTryMs && vitaFails > 0) {
        waits = true; // backing off after players that gave no frame
    }
    if (vitaVideo == NULL && now >= vitaNextTryMs && !loopHolds && (vitaPlayerReservedFor == NULL || vitaPlayerReservedFor == this)) {
        vitaVideo = VitaVideo_Open(vitaMp4Path.c_str(), vitaLooping, vitaStreamW, vitaStreamH);
        if (vitaVideo != NULL && vitaPlayerReservedFor == this) {
            vitaPlayerReservedFor = NULL;
        }
        if (vitaVideo == NULL && Vita_CinematicEvict(this, now)) {
            // its memory comes back once the worker has closed it
            vitaNextTryMs = now + 100;
        } else if (vitaVideo == NULL) {
            vitaNextTryMs = now + 250;
            waits = true; // a more important video has it
        }
    }
    if (vitaVideo != NULL) {
        return ImageForTimeVitaHw();
    }
    // (not in its first 600 ms on screen: a player closing or the gap after
    // one is not a reason to go to software)
    if (waits && vitaRoqName.Length() > 0 && now - vitaAppearMs > 600 && VitaStartRoQ()) {
        return ImageForTimeRoQ(thisTime);
    }
    cinData_t none;
    memset(&none, 0, sizeof(none));
    none.status = FMV_PLAY; // waiting for a player: still "playing"
    return none;
}
#endif

cinData_t idCinematicLocal::ImageForTime(int thisTime)
{
#if defined(__vita__)
    if (vitaMp4Path.Length() > 0) {
        return ImageForTimeVita(thisTime);
    }
    return ImageForTimeRoQ(thisTime);
}

cinData_t idCinematicLocal::ImageForTimeRoQ(int thisTime)
{
#endif

#if defined(USE_FFMPEG)
    // Carl: Handle BFG format BINK videos separately
    if (!isRoQ)
        return ImageForTimeFFMPEG(thisTime);
#endif

    // Carl: Handle original Doom 3 RoQ video files
    cinData_t cinData;

    if (thisTime == 0) {
        thisTime = Sys_Milliseconds();
    }

    if (thisTime < 0) {
        thisTime = 0;
    }

    memset(&cinData, 0, sizeof(cinData));

    //	if ( r_skipROQ.GetBool() ) {
    if (r_skipDynamicTextures.GetBool()) {
        return cinData;
    }

    if (!iFile) {
        // RB: neither .bik or .roq found
        return cinData;
    }

    if (status == FMV_EOF || status == FMV_IDLE) {
        return cinData;
    }

    if (buf == NULL || startTime == -1) {
        if (startTime == -1) {
            RoQReset();
        }
        startTime = thisTime;
    }

    tfps = ((thisTime - startTime) * frameRate) / 1000;

    if (tfps < 0) {
        tfps = 0;
    }

    if (tfps < numQuads) {
        RoQReset();
        buf = NULL;
        status = FMV_PLAY;
    }

    if (buf == NULL) {
        while (buf == NULL) {
            RoQInterrupt();
        }
    } else {
        while ((tfps != numQuads && status == FMV_PLAY)) {
            RoQInterrupt();
        }
    }

    if (status == FMV_LOOPED) {
        status = FMV_PLAY;
        while (buf == NULL && status == FMV_PLAY) {
            RoQInterrupt();
        }
        startTime = thisTime;
    }

    if (status == FMV_EOF) {
        if (looping) {
            RoQReset();
            buf = NULL;
            if (status == FMV_LOOPED) {
                status = FMV_PLAY;
            }
            while (buf == NULL && status == FMV_PLAY) {
                RoQInterrupt();
            }
            startTime = thisTime;
        } else {
            status = FMV_IDLE;
            RoQShutdown();
        }
    }

    cinData.imageWidth = CIN_WIDTH;
    cinData.imageHeight = CIN_HEIGHT;
    cinData.status = status;
#if defined(__vita__)
    // the first frame is decoded when the material is parsed, possibly on the
    // game/draw thread: leave the upload to the back end's next call; and a
    // frame already uploaded is not uploaded again (a screen is bound several
    // times a frame, the video changes 30 times a second at most)
    if (Vita_IsGLThread() && (numQuads != vitaUploadedQuad || !img->IsLoaded())) {
        img->UploadScratch(image, CIN_WIDTH, CIN_HEIGHT);
        vitaUploadedQuad = numQuads;
    }
#else
    img->UploadScratch(image, CIN_WIDTH, CIN_HEIGHT);
#endif
    cinData.image = img;

    return cinData;
}

/*
==============
idCinematicLocal::ImageForTimeFFMPEG
==============
*/
#if defined(USE_FFMPEG)
cinData_t idCinematicLocal::ImageForTimeFFMPEG(int thisTime)
{
    cinData_t cinData;

    if (thisTime <= 0) {
        thisTime = Sys_Milliseconds();
    }

    memset(&cinData, 0, sizeof(cinData));
    if (r_skipDynamicTextures.GetBool() || status == FMV_EOF || status == FMV_IDLE) {
        return cinData;
    }

    if (!fmt_ctx) {
        // RB: .bik requested but not found
        return cinData;
    }

    if ((!hasFrame) || startTime == -1) {
        if (startTime == -1) {
            FFMPEGReset();
        }
        startTime = thisTime;
    }

    long desiredFrame = ((thisTime - startTime) * frameRate) / 1000;
    if (desiredFrame < 0) {
        desiredFrame = 0;
    }

    if (desiredFrame < framePos) {
        FFMPEGReset();
    }

    if (hasFrame && desiredFrame == framePos) {
        cinData.imageWidth = CIN_WIDTH;
        cinData.imageHeight = CIN_HEIGHT;
        cinData.status = status;
        cinData.image = img;
        return cinData;
    }

    AVPacket packet;
    while (framePos < desiredFrame) {
        int frameFinished = 0;

        // Do a single frame by getting packets until we have a full frame
        while (!frameFinished) {
            // if we got to the end or failed
            if (av_read_frame(fmt_ctx, &packet) < 0) {
                // can't read any more, set to EOF
                status = FMV_EOF;
                if (looping) {
                    desiredFrame = 0;
                    FFMPEGReset();
                    framePos = -1;
                    startTime = thisTime;
                    if (av_read_frame(fmt_ctx, &packet) < 0) {
                        status = FMV_IDLE;
                        return cinData;
                    }
                    status = FMV_PLAY;
                } else {
                    status = FMV_IDLE;
                    return cinData;
                }
            }
            // Is this a packet from the video stream?
            if (packet.stream_index == video_stream_index) {
                // Decode video frame
                avcodec_decode_video2(dec_ctx, frame, &frameFinished, &packet);
            }
            // Free the packet that was allocated by av_read_frame
            av_free_packet(&packet);
        }

        framePos++;
    }

    // We have reached the desired frame
    // Convert the image from its native format to RGB
    sws_scale(img_convert_ctx, frame->data, frame->linesize, 0, dec_ctx->height, frame2->data, frame2->linesize);
    cinData.imageWidth = CIN_WIDTH;
    cinData.imageHeight = CIN_HEIGHT;
    cinData.status = status;
    img->UploadScratch(image, CIN_WIDTH, CIN_HEIGHT);
    hasFrame = true;
    cinData.image = img;

    return cinData;
}
#endif

/*
==============
idCinematicLocal::move8_32
==============
*/
void idCinematicLocal::move8_32(byte* src, byte* dst, int spl)
{
#if 1
    int *dsrc, *ddst;
    int dspl;

    dsrc = (int*)src;
    ddst = (int*)dst;
    dspl = spl >> 2;

    ddst[0 * dspl + 0] = dsrc[0 * dspl + 0];
    ddst[0 * dspl + 1] = dsrc[0 * dspl + 1];
    ddst[0 * dspl + 2] = dsrc[0 * dspl + 2];
    ddst[0 * dspl + 3] = dsrc[0 * dspl + 3];
    ddst[0 * dspl + 4] = dsrc[0 * dspl + 4];
    ddst[0 * dspl + 5] = dsrc[0 * dspl + 5];
    ddst[0 * dspl + 6] = dsrc[0 * dspl + 6];
    ddst[0 * dspl + 7] = dsrc[0 * dspl + 7];

    ddst[1 * dspl + 0] = dsrc[1 * dspl + 0];
    ddst[1 * dspl + 1] = dsrc[1 * dspl + 1];
    ddst[1 * dspl + 2] = dsrc[1 * dspl + 2];
    ddst[1 * dspl + 3] = dsrc[1 * dspl + 3];
    ddst[1 * dspl + 4] = dsrc[1 * dspl + 4];
    ddst[1 * dspl + 5] = dsrc[1 * dspl + 5];
    ddst[1 * dspl + 6] = dsrc[1 * dspl + 6];
    ddst[1 * dspl + 7] = dsrc[1 * dspl + 7];

    ddst[2 * dspl + 0] = dsrc[2 * dspl + 0];
    ddst[2 * dspl + 1] = dsrc[2 * dspl + 1];
    ddst[2 * dspl + 2] = dsrc[2 * dspl + 2];
    ddst[2 * dspl + 3] = dsrc[2 * dspl + 3];
    ddst[2 * dspl + 4] = dsrc[2 * dspl + 4];
    ddst[2 * dspl + 5] = dsrc[2 * dspl + 5];
    ddst[2 * dspl + 6] = dsrc[2 * dspl + 6];
    ddst[2 * dspl + 7] = dsrc[2 * dspl + 7];

    ddst[3 * dspl + 0] = dsrc[3 * dspl + 0];
    ddst[3 * dspl + 1] = dsrc[3 * dspl + 1];
    ddst[3 * dspl + 2] = dsrc[3 * dspl + 2];
    ddst[3 * dspl + 3] = dsrc[3 * dspl + 3];
    ddst[3 * dspl + 4] = dsrc[3 * dspl + 4];
    ddst[3 * dspl + 5] = dsrc[3 * dspl + 5];
    ddst[3 * dspl + 6] = dsrc[3 * dspl + 6];
    ddst[3 * dspl + 7] = dsrc[3 * dspl + 7];

    ddst[4 * dspl + 0] = dsrc[4 * dspl + 0];
    ddst[4 * dspl + 1] = dsrc[4 * dspl + 1];
    ddst[4 * dspl + 2] = dsrc[4 * dspl + 2];
    ddst[4 * dspl + 3] = dsrc[4 * dspl + 3];
    ddst[4 * dspl + 4] = dsrc[4 * dspl + 4];
    ddst[4 * dspl + 5] = dsrc[4 * dspl + 5];
    ddst[4 * dspl + 6] = dsrc[4 * dspl + 6];
    ddst[4 * dspl + 7] = dsrc[4 * dspl + 7];

    ddst[5 * dspl + 0] = dsrc[5 * dspl + 0];
    ddst[5 * dspl + 1] = dsrc[5 * dspl + 1];
    ddst[5 * dspl + 2] = dsrc[5 * dspl + 2];
    ddst[5 * dspl + 3] = dsrc[5 * dspl + 3];
    ddst[5 * dspl + 4] = dsrc[5 * dspl + 4];
    ddst[5 * dspl + 5] = dsrc[5 * dspl + 5];
    ddst[5 * dspl + 6] = dsrc[5 * dspl + 6];
    ddst[5 * dspl + 7] = dsrc[5 * dspl + 7];

    ddst[6 * dspl + 0] = dsrc[6 * dspl + 0];
    ddst[6 * dspl + 1] = dsrc[6 * dspl + 1];
    ddst[6 * dspl + 2] = dsrc[6 * dspl + 2];
    ddst[6 * dspl + 3] = dsrc[6 * dspl + 3];
    ddst[6 * dspl + 4] = dsrc[6 * dspl + 4];
    ddst[6 * dspl + 5] = dsrc[6 * dspl + 5];
    ddst[6 * dspl + 6] = dsrc[6 * dspl + 6];
    ddst[6 * dspl + 7] = dsrc[6 * dspl + 7];

    ddst[7 * dspl + 0] = dsrc[7 * dspl + 0];
    ddst[7 * dspl + 1] = dsrc[7 * dspl + 1];
    ddst[7 * dspl + 2] = dsrc[7 * dspl + 2];
    ddst[7 * dspl + 3] = dsrc[7 * dspl + 3];
    ddst[7 * dspl + 4] = dsrc[7 * dspl + 4];
    ddst[7 * dspl + 5] = dsrc[7 * dspl + 5];
    ddst[7 * dspl + 6] = dsrc[7 * dspl + 6];
    ddst[7 * dspl + 7] = dsrc[7 * dspl + 7];
#else
    double *dsrc, *ddst;
    int dspl;

    dsrc = (double*)src;
    ddst = (double*)dst;
    dspl = spl >> 3;

    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    ddst[2] = dsrc[2];
    ddst[3] = dsrc[3];
    dsrc += dspl;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    ddst[2] = dsrc[2];
    ddst[3] = dsrc[3];
    dsrc += dspl;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    ddst[2] = dsrc[2];
    ddst[3] = dsrc[3];
    dsrc += dspl;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    ddst[2] = dsrc[2];
    ddst[3] = dsrc[3];
    dsrc += dspl;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    ddst[2] = dsrc[2];
    ddst[3] = dsrc[3];
    dsrc += dspl;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    ddst[2] = dsrc[2];
    ddst[3] = dsrc[3];
    dsrc += dspl;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    ddst[2] = dsrc[2];
    ddst[3] = dsrc[3];
    dsrc += dspl;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    ddst[2] = dsrc[2];
    ddst[3] = dsrc[3];
#endif
}

/*
==============
idCinematicLocal::move4_32
==============
*/
void idCinematicLocal::move4_32(byte* src, byte* dst, int spl)
{
#if 1
    int *dsrc, *ddst;
    int dspl;

    dsrc = (int*)src;
    ddst = (int*)dst;
    dspl = spl >> 2;

    ddst[0 * dspl + 0] = dsrc[0 * dspl + 0];
    ddst[0 * dspl + 1] = dsrc[0 * dspl + 1];
    ddst[0 * dspl + 2] = dsrc[0 * dspl + 2];
    ddst[0 * dspl + 3] = dsrc[0 * dspl + 3];

    ddst[1 * dspl + 0] = dsrc[1 * dspl + 0];
    ddst[1 * dspl + 1] = dsrc[1 * dspl + 1];
    ddst[1 * dspl + 2] = dsrc[1 * dspl + 2];
    ddst[1 * dspl + 3] = dsrc[1 * dspl + 3];

    ddst[2 * dspl + 0] = dsrc[2 * dspl + 0];
    ddst[2 * dspl + 1] = dsrc[2 * dspl + 1];
    ddst[2 * dspl + 2] = dsrc[2 * dspl + 2];
    ddst[2 * dspl + 3] = dsrc[2 * dspl + 3];

    ddst[3 * dspl + 0] = dsrc[3 * dspl + 0];
    ddst[3 * dspl + 1] = dsrc[3 * dspl + 1];
    ddst[3 * dspl + 2] = dsrc[3 * dspl + 2];
    ddst[3 * dspl + 3] = dsrc[3 * dspl + 3];
#else
    double *dsrc, *ddst;
    int dspl;

    dsrc = (double*)src;
    ddst = (double*)dst;
    dspl = spl >> 3;

    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    dsrc += dspl;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    dsrc += dspl;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    dsrc += dspl;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
#endif
}

/*
==============
idCinematicLocal::blit8_32
==============
*/
void idCinematicLocal::blit8_32(byte* src, byte* dst, int spl)
{
#if 1
    int *dsrc, *ddst;
    int dspl;

    dsrc = (int*)src;
    ddst = (int*)dst;
    dspl = spl >> 2;

    ddst[0 * dspl + 0] = dsrc[0];
    ddst[0 * dspl + 1] = dsrc[1];
    ddst[0 * dspl + 2] = dsrc[2];
    ddst[0 * dspl + 3] = dsrc[3];
    ddst[0 * dspl + 4] = dsrc[4];
    ddst[0 * dspl + 5] = dsrc[5];
    ddst[0 * dspl + 6] = dsrc[6];
    ddst[0 * dspl + 7] = dsrc[7];

    ddst[1 * dspl + 0] = dsrc[8];
    ddst[1 * dspl + 1] = dsrc[9];
    ddst[1 * dspl + 2] = dsrc[10];
    ddst[1 * dspl + 3] = dsrc[11];
    ddst[1 * dspl + 4] = dsrc[12];
    ddst[1 * dspl + 5] = dsrc[13];
    ddst[1 * dspl + 6] = dsrc[14];
    ddst[1 * dspl + 7] = dsrc[15];

    ddst[2 * dspl + 0] = dsrc[16];
    ddst[2 * dspl + 1] = dsrc[17];
    ddst[2 * dspl + 2] = dsrc[18];
    ddst[2 * dspl + 3] = dsrc[19];
    ddst[2 * dspl + 4] = dsrc[20];
    ddst[2 * dspl + 5] = dsrc[21];
    ddst[2 * dspl + 6] = dsrc[22];
    ddst[2 * dspl + 7] = dsrc[23];

    ddst[3 * dspl + 0] = dsrc[24];
    ddst[3 * dspl + 1] = dsrc[25];
    ddst[3 * dspl + 2] = dsrc[26];
    ddst[3 * dspl + 3] = dsrc[27];
    ddst[3 * dspl + 4] = dsrc[28];
    ddst[3 * dspl + 5] = dsrc[29];
    ddst[3 * dspl + 6] = dsrc[30];
    ddst[3 * dspl + 7] = dsrc[31];

    ddst[4 * dspl + 0] = dsrc[32];
    ddst[4 * dspl + 1] = dsrc[33];
    ddst[4 * dspl + 2] = dsrc[34];
    ddst[4 * dspl + 3] = dsrc[35];
    ddst[4 * dspl + 4] = dsrc[36];
    ddst[4 * dspl + 5] = dsrc[37];
    ddst[4 * dspl + 6] = dsrc[38];
    ddst[4 * dspl + 7] = dsrc[39];

    ddst[5 * dspl + 0] = dsrc[40];
    ddst[5 * dspl + 1] = dsrc[41];
    ddst[5 * dspl + 2] = dsrc[42];
    ddst[5 * dspl + 3] = dsrc[43];
    ddst[5 * dspl + 4] = dsrc[44];
    ddst[5 * dspl + 5] = dsrc[45];
    ddst[5 * dspl + 6] = dsrc[46];
    ddst[5 * dspl + 7] = dsrc[47];

    ddst[6 * dspl + 0] = dsrc[48];
    ddst[6 * dspl + 1] = dsrc[49];
    ddst[6 * dspl + 2] = dsrc[50];
    ddst[6 * dspl + 3] = dsrc[51];
    ddst[6 * dspl + 4] = dsrc[52];
    ddst[6 * dspl + 5] = dsrc[53];
    ddst[6 * dspl + 6] = dsrc[54];
    ddst[6 * dspl + 7] = dsrc[55];

    ddst[7 * dspl + 0] = dsrc[56];
    ddst[7 * dspl + 1] = dsrc[57];
    ddst[7 * dspl + 2] = dsrc[58];
    ddst[7 * dspl + 3] = dsrc[59];
    ddst[7 * dspl + 4] = dsrc[60];
    ddst[7 * dspl + 5] = dsrc[61];
    ddst[7 * dspl + 6] = dsrc[62];
    ddst[7 * dspl + 7] = dsrc[63];
#else
    double *dsrc, *ddst;
    int dspl;

    dsrc = (double*)src;
    ddst = (double*)dst;
    dspl = spl >> 3;

    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    ddst[2] = dsrc[2];
    ddst[3] = dsrc[3];
    dsrc += 4;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    ddst[2] = dsrc[2];
    ddst[3] = dsrc[3];
    dsrc += 4;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    ddst[2] = dsrc[2];
    ddst[3] = dsrc[3];
    dsrc += 4;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    ddst[2] = dsrc[2];
    ddst[3] = dsrc[3];
    dsrc += 4;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    ddst[2] = dsrc[2];
    ddst[3] = dsrc[3];
    dsrc += 4;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    ddst[2] = dsrc[2];
    ddst[3] = dsrc[3];
    dsrc += 4;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    ddst[2] = dsrc[2];
    ddst[3] = dsrc[3];
    dsrc += 4;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    ddst[2] = dsrc[2];
    ddst[3] = dsrc[3];
#endif
}

/*
==============
idCinematicLocal::blit4_32
==============
*/
void idCinematicLocal::blit4_32(byte* src, byte* dst, int spl)
{
#if 1
    int *dsrc, *ddst;
    int dspl;

    dsrc = (int*)src;
    ddst = (int*)dst;
    dspl = spl >> 2;

    ddst[0 * dspl + 0] = dsrc[0];
    ddst[0 * dspl + 1] = dsrc[1];
    ddst[0 * dspl + 2] = dsrc[2];
    ddst[0 * dspl + 3] = dsrc[3];
    ddst[1 * dspl + 0] = dsrc[4];
    ddst[1 * dspl + 1] = dsrc[5];
    ddst[1 * dspl + 2] = dsrc[6];
    ddst[1 * dspl + 3] = dsrc[7];
    ddst[2 * dspl + 0] = dsrc[8];
    ddst[2 * dspl + 1] = dsrc[9];
    ddst[2 * dspl + 2] = dsrc[10];
    ddst[2 * dspl + 3] = dsrc[11];
    ddst[3 * dspl + 0] = dsrc[12];
    ddst[3 * dspl + 1] = dsrc[13];
    ddst[3 * dspl + 2] = dsrc[14];
    ddst[3 * dspl + 3] = dsrc[15];
#else
    double *dsrc, *ddst;
    int dspl;

    dsrc = (double*)src;
    ddst = (double*)dst;
    dspl = spl >> 3;

    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    dsrc += 2;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    dsrc += 2;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
    dsrc += 2;
    ddst += dspl;
    ddst[0] = dsrc[0];
    ddst[1] = dsrc[1];
#endif
}

/*
==============
idCinematicLocal::blit2_32
==============
*/
void idCinematicLocal::blit2_32(byte* src, byte* dst, int spl)
{
#if 1
    int *dsrc, *ddst;
    int dspl;

    dsrc = (int*)src;
    ddst = (int*)dst;
    dspl = spl >> 2;

    ddst[0 * dspl + 0] = dsrc[0];
    ddst[0 * dspl + 1] = dsrc[1];
    ddst[1 * dspl + 0] = dsrc[2];
    ddst[1 * dspl + 1] = dsrc[3];
#else
    double *dsrc, *ddst;
    int dspl;

    dsrc = (double*)src;
    ddst = (double*)dst;
    dspl = spl >> 3;

    ddst[0] = dsrc[0];
    ddst[dspl] = dsrc[1];
#endif
}

/*
==============
idCinematicLocal::blitVQQuad32fs
==============
*/
void idCinematicLocal::blitVQQuad32fs(byte** status, unsigned char* data)
{
    unsigned short newd, celdata, code;
    unsigned int index, i;

    newd = 0;
    celdata = 0;
    index = 0;

    do {
        if (!newd) {
            newd = 7;
            celdata = data[0] + data[1] * 256;
            data += 2;
        } else {
            newd--;
        }

        code = (unsigned short)(celdata & 0xc000);
        celdata <<= 2;

        switch (code) {
        case 0x8000: // vq code
            blit8_32((byte*)&vq8[(*data) * 128], status[index], samplesPerLine);
            data++;
            index += 5;
            break;
        case 0xc000: // drop
            index++; // skip 8x8
            for (i = 0; i < 4; i++) {
                if (!newd) {
                    newd = 7;
                    celdata = data[0] + data[1] * 256;
                    data += 2;
                } else {
                    newd--;
                }

                code = (unsigned short)(celdata & 0xc000);
                celdata <<= 2;

                switch (code) // code in top two bits of code
                {
                case 0x8000: // 4x4 vq code
                    blit4_32((byte*)&vq4[(*data) * 32], status[index], samplesPerLine);
                    data++;
                    break;
                case 0xc000: // 2x2 vq code
                    blit2_32((byte*)&vq2[(*data) * 8], status[index], samplesPerLine);
                    data++;
                    blit2_32((byte*)&vq2[(*data) * 8], status[index] + 8, samplesPerLine);
                    data++;
                    blit2_32((byte*)&vq2[(*data) * 8], status[index] + samplesPerLine * 2, samplesPerLine);
                    data++;
                    blit2_32((byte*)&vq2[(*data) * 8], status[index] + samplesPerLine * 2 + 8, samplesPerLine);
                    data++;
                    break;
                case 0x4000: // motion compensation
                    move4_32(status[index] + mcomp[(*data)], status[index], samplesPerLine);
                    data++;
                    break;
                }
                index++;
            }
            break;
        case 0x4000: // motion compensation
            move8_32(status[index] + mcomp[(*data)], status[index], samplesPerLine);
            data++;
            index += 5;
            break;
        case 0x0000:
            index += 5;
            break;
        }
    } while (status[index] != NULL);
}

#define VQ2TO4(a, b, c, d) \
    {                      \
        *c++ = a[0];       \
        *d++ = a[0];       \
        *d++ = a[0];       \
        *c++ = a[1];       \
        *d++ = a[1];       \
        *d++ = a[1];       \
        *c++ = b[0];       \
        *d++ = b[0];       \
        *d++ = b[0];       \
        *c++ = b[1];       \
        *d++ = b[1];       \
        *d++ = b[1];       \
        *d++ = a[0];       \
        *d++ = a[0];       \
        *d++ = a[1];       \
        *d++ = a[1];       \
        *d++ = b[0];       \
        *d++ = b[0];       \
        *d++ = b[1];       \
        *d++ = b[1];       \
        a += 2;            \
        b += 2;            \
    }

#define VQ2TO2(a, b, c, d) \
    {                      \
        *c++ = *a;         \
        *d++ = *a;         \
        *d++ = *a;         \
        *c++ = *b;         \
        *d++ = *b;         \
        *d++ = *b;         \
        *d++ = *a;         \
        *d++ = *a;         \
        *d++ = *b;         \
        *d++ = *b;         \
        a++;               \
        b++;               \
    }

/*
==============
idCinematicLocal::yuv_to_rgb
==============
*/
// RB: 64 bit fixes, changed long to int
unsigned short idCinematicLocal::yuv_to_rgb(int y, int u, int v)
{
    int r, g, b, YY = (int)(ROQ_YY_tab[(y)]);

    r = (YY + ROQ_VR_tab[v]) >> 9;
    g = (YY + ROQ_UG_tab[u] + ROQ_VG_tab[v]) >> 8;
    b = (YY + ROQ_UB_tab[u]) >> 9;

    if (r < 0)
        r = 0;
    if (g < 0)
        g = 0;
    if (b < 0)
        b = 0;
    if (r > 31)
        r = 31;
    if (g > 63)
        g = 63;
    if (b > 31)
        b = 31;

    return (unsigned short)((r << 11) + (g << 5) + (b));
}
// RB end

/*
==============
idCinematicLocal::yuv_to_rgb24
==============
*/
// RB: 64 bit fixes, changed long to int
unsigned int idCinematicLocal::yuv_to_rgb24(int y, int u, int v)
{
    int r, g, b, YY = (int)(ROQ_YY_tab[(y)]);

    r = (YY + ROQ_VR_tab[v]) >> 6;
    g = (YY + ROQ_UG_tab[u] + ROQ_VG_tab[v]) >> 6;
    b = (YY + ROQ_UB_tab[u]) >> 6;

    if (r < 0)
        r = 0;
    if (g < 0)
        g = 0;
    if (b < 0)
        b = 0;
    if (r > 255)
        r = 255;
    if (g > 255)
        g = 255;
    if (b > 255)
        b = 255;

    return LittleLong((r) + (g << 8) + (b << 16));
}
// RB end

/*
==============
idCinematicLocal::decodeCodeBook
==============
*/
// RB: 64 bit fixes, changed long to int
void idCinematicLocal::decodeCodeBook(byte* input, unsigned short roq_flags)
{
    int i, j, two, four;
    unsigned short *aptr, *bptr, *cptr, *dptr;
    int y0, y1, y2, y3, cr, cb;
    unsigned int *iaptr, *ibptr, *icptr, *idptr;

    if (!roq_flags) {
        two = four = 256;
    } else {
        two = roq_flags >> 8;
        if (!two)
            two = 256;
        four = roq_flags & 0xff;
    }

    four *= 2;

    bptr = (unsigned short*)vq2;

    if (!half) {
        if (!smootheddouble) {
            //
            // normal height
            //
            if (samplesPerPixel == 2) {
                for (i = 0; i < two; i++) {
                    y0 = (int)*input++;
                    y1 = (int)*input++;
                    y2 = (int)*input++;
                    y3 = (int)*input++;
                    cr = (int)*input++;
                    cb = (int)*input++;
                    *bptr++ = yuv_to_rgb(y0, cr, cb);
                    *bptr++ = yuv_to_rgb(y1, cr, cb);
                    *bptr++ = yuv_to_rgb(y2, cr, cb);
                    *bptr++ = yuv_to_rgb(y3, cr, cb);
                }

                cptr = (unsigned short*)vq4;
                dptr = (unsigned short*)vq8;

                for (i = 0; i < four; i++) {
                    aptr = (unsigned short*)vq2 + (*input++) * 4;
                    bptr = (unsigned short*)vq2 + (*input++) * 4;
                    for (j = 0; j < 2; j++)
                        VQ2TO4(aptr, bptr, cptr, dptr);
                }
            } else if (samplesPerPixel == 4) {
                ibptr = (unsigned int*)bptr;
                for (i = 0; i < two; i++) {
                    y0 = (int)*input++;
                    y1 = (int)*input++;
                    y2 = (int)*input++;
                    y3 = (int)*input++;
                    cr = (int)*input++;
                    cb = (int)*input++;
                    *ibptr++ = yuv_to_rgb24(y0, cr, cb);
                    *ibptr++ = yuv_to_rgb24(y1, cr, cb);
                    *ibptr++ = yuv_to_rgb24(y2, cr, cb);
                    *ibptr++ = yuv_to_rgb24(y3, cr, cb);
                }

                icptr = (unsigned int*)vq4;
                idptr = (unsigned int*)vq8;

                for (i = 0; i < four; i++) {
                    iaptr = (unsigned int*)vq2 + (*input++) * 4;
                    ibptr = (unsigned int*)vq2 + (*input++) * 4;
                    for (j = 0; j < 2; j++)
                        VQ2TO4(iaptr, ibptr, icptr, idptr);
                }
            }
        } else {
            //
            // double height, smoothed
            //
            if (samplesPerPixel == 2) {
                for (i = 0; i < two; i++) {
                    y0 = (int)*input++;
                    y1 = (int)*input++;
                    y2 = (int)*input++;
                    y3 = (int)*input++;
                    cr = (int)*input++;
                    cb = (int)*input++;
                    *bptr++ = yuv_to_rgb(y0, cr, cb);
                    *bptr++ = yuv_to_rgb(y1, cr, cb);
                    *bptr++ = yuv_to_rgb(((y0 * 3) + y2) / 4, cr, cb);
                    *bptr++ = yuv_to_rgb(((y1 * 3) + y3) / 4, cr, cb);
                    *bptr++ = yuv_to_rgb((y0 + (y2 * 3)) / 4, cr, cb);
                    *bptr++ = yuv_to_rgb((y1 + (y3 * 3)) / 4, cr, cb);
                    *bptr++ = yuv_to_rgb(y2, cr, cb);
                    *bptr++ = yuv_to_rgb(y3, cr, cb);
                }

                cptr = (unsigned short*)vq4;
                dptr = (unsigned short*)vq8;

                for (i = 0; i < four; i++) {
                    aptr = (unsigned short*)vq2 + (*input++) * 8;
                    bptr = (unsigned short*)vq2 + (*input++) * 8;
                    for (j = 0; j < 2; j++) {
                        VQ2TO4(aptr, bptr, cptr, dptr);
                        VQ2TO4(aptr, bptr, cptr, dptr);
                    }
                }
            } else if (samplesPerPixel == 4) {
                ibptr = (unsigned int*)bptr;
                for (i = 0; i < two; i++) {
                    y0 = (int)*input++;
                    y1 = (int)*input++;
                    y2 = (int)*input++;
                    y3 = (int)*input++;
                    cr = (int)*input++;
                    cb = (int)*input++;
                    *ibptr++ = yuv_to_rgb24(y0, cr, cb);
                    *ibptr++ = yuv_to_rgb24(y1, cr, cb);
                    *ibptr++ = yuv_to_rgb24(((y0 * 3) + y2) / 4, cr, cb);
                    *ibptr++ = yuv_to_rgb24(((y1 * 3) + y3) / 4, cr, cb);
                    *ibptr++ = yuv_to_rgb24((y0 + (y2 * 3)) / 4, cr, cb);
                    *ibptr++ = yuv_to_rgb24((y1 + (y3 * 3)) / 4, cr, cb);
                    *ibptr++ = yuv_to_rgb24(y2, cr, cb);
                    *ibptr++ = yuv_to_rgb24(y3, cr, cb);
                }

                icptr = (unsigned int*)vq4;
                idptr = (unsigned int*)vq8;

                for (i = 0; i < four; i++) {
                    iaptr = (unsigned int*)vq2 + (*input++) * 8;
                    ibptr = (unsigned int*)vq2 + (*input++) * 8;
                    for (j = 0; j < 2; j++) {
                        VQ2TO4(iaptr, ibptr, icptr, idptr);
                        VQ2TO4(iaptr, ibptr, icptr, idptr);
                    }
                }
            }
        }
    } else {
        //
        // 1/4 screen
        //
        if (samplesPerPixel == 2) {
            for (i = 0; i < two; i++) {
                y0 = (int)*input;
                input += 2;
                y2 = (int)*input;
                input += 2;
                cr = (int)*input++;
                cb = (int)*input++;
                *bptr++ = yuv_to_rgb(y0, cr, cb);
                *bptr++ = yuv_to_rgb(y2, cr, cb);
            }

            cptr = (unsigned short*)vq4;
            dptr = (unsigned short*)vq8;

            for (i = 0; i < four; i++) {
                aptr = (unsigned short*)vq2 + (*input++) * 2;
                bptr = (unsigned short*)vq2 + (*input++) * 2;
                for (j = 0; j < 2; j++) {
                    VQ2TO2(aptr, bptr, cptr, dptr);
                }
            }
        } else if (samplesPerPixel == 4) {
            ibptr = (unsigned int*)bptr;
            for (i = 0; i < two; i++) {
                y0 = (int)*input;
                input += 2;
                y2 = (int)*input;
                input += 2;
                cr = (int)*input++;
                cb = (int)*input++;
                *ibptr++ = yuv_to_rgb24(y0, cr, cb);
                *ibptr++ = yuv_to_rgb24(y2, cr, cb);
            }

            icptr = (unsigned int*)vq4;
            idptr = (unsigned int*)vq8;

            for (i = 0; i < four; i++) {
                iaptr = (unsigned int*)vq2 + (*input++) * 2;
                ibptr = (unsigned int*)vq2 + (*input++) * 2;
                for (j = 0; j < 2; j++) {
                    VQ2TO2(iaptr, ibptr, icptr, idptr);
                }
            }
        }
    }
}
// RB end

/*
==============
idCinematicLocal::recurseQuad
==============
*/
// RB: 64 bit fixes, changed long to int
void idCinematicLocal::recurseQuad(int startX, int startY, int quadSize, int xOff, int yOff)
{
    byte* scroff;
    int bigx, bigy, lowx, lowy, useY;
    int offset;

    offset = screenDelta;

    lowx = lowy = 0;
    bigx = xsize;
    bigy = ysize;

    if (bigx > CIN_WIDTH)
        bigx = CIN_WIDTH;
    if (bigy > CIN_HEIGHT)
        bigy = CIN_HEIGHT;

    if ((startX >= lowx) && (startX + quadSize) <= (bigx) && (startY + quadSize) <= (bigy) && (startY >= lowy) && quadSize <= MAXSIZE) {
        useY = startY;
        scroff = image + (useY + ((CIN_HEIGHT - bigy) >> 1) + yOff) * (samplesPerLine) + (((startX + xOff)) * samplesPerPixel);

        qStatus[0][onQuad] = scroff;
        qStatus[1][onQuad++] = scroff + offset;
    }

    if (quadSize != MINSIZE) {
        quadSize >>= 1;
        recurseQuad(startX, startY, quadSize, xOff, yOff);
        recurseQuad(startX + quadSize, startY, quadSize, xOff, yOff);
        recurseQuad(startX, startY + quadSize, quadSize, xOff, yOff);
        recurseQuad(startX + quadSize, startY + quadSize, quadSize, xOff, yOff);
    }
}
// RB end

/*
==============
idCinematicLocal::setupQuad
==============
*/
// RB: 64 bit fixes, changed long to int
void idCinematicLocal::setupQuad(int xOff, int yOff)
{
    int numQuadCels, i, x, y;
    byte* temp;

    numQuadCels = (CIN_WIDTH * CIN_HEIGHT) / (16);
    numQuadCels += numQuadCels / 4 + numQuadCels / 16;
    numQuadCels += 64; // for overflow

    numQuadCels = (xsize * ysize) / (16);
    numQuadCels += numQuadCels / 4;
    numQuadCels += 64; // for overflow

    onQuad = 0;

    for (y = 0; y < (int)ysize; y += 16)
        for (x = 0; x < (int)xsize; x += 16)
            recurseQuad(x, y, 16, xOff, yOff);

    temp = NULL;

    for (i = (numQuadCels - 64); i < numQuadCels; i++) {
        qStatus[0][i] = temp; // eoq
        qStatus[1][i] = temp; // eoq
    }
}
// RB end

/*
==============
idCinematicLocal::readQuadInfo
==============
*/
void idCinematicLocal::readQuadInfo(byte* qData)
{
    xsize = qData[0] + qData[1] * 256;
    ysize = qData[2] + qData[3] * 256;
    maxsize = qData[4] + qData[5] * 256;
    minsize = qData[6] + qData[7] * 256;

    CIN_HEIGHT = ysize;
    CIN_WIDTH = xsize;

    samplesPerLine = CIN_WIDTH * samplesPerPixel;
    screenDelta = CIN_HEIGHT * samplesPerLine;

    if (!image) {
        image = (byte*)Mem_Alloc(CIN_WIDTH * CIN_HEIGHT * samplesPerPixel * 2, TAG_CINEMATIC);
    }

    half = false;
    smootheddouble = false;

    // RB: 64 bit fixes, changed unsigned int to ptrdiff_t
    t[0] = (0 - (ptrdiff_t)image) + (ptrdiff_t)image + screenDelta;
    t[1] = (0 - ((ptrdiff_t)image + screenDelta)) + (ptrdiff_t)image;
    // RB end

    drawX = CIN_WIDTH;
    drawY = CIN_HEIGHT;
}

/*
==============
idCinematicLocal::RoQPrepMcomp
==============
*/
// RB: 64 bit fixes, changed long to int
void idCinematicLocal::RoQPrepMcomp(int xoff, int yoff)
{
    int i, j, x, y, temp, temp2;

    i = samplesPerLine;
    j = samplesPerPixel;
    if (xsize == (ysize * 4) && !half) {
        j = j + j;
        i = i + i;
    }

    for (y = 0; y < 16; y++) {
        temp2 = (y + yoff - 8) * i;
        for (x = 0; x < 16; x++) {
            temp = (x + xoff - 8) * j;
            mcomp[(x * 16) + y] = normalBuffer0 - (temp2 + temp);
        }
    }
}
// RB end

/*
==============
idCinematicLocal::RoQReset
==============
*/
void idCinematicLocal::RoQReset()
{

    iFile->Seek(0, FS_SEEK_SET);
    iFile->Read(file, 16);
    RoQ_init();
    status = FMV_LOOPED;
}

typedef struct
{
    struct jpeg_source_mgr pub; /* public fields */

    byte* infile;          /* source stream */
    JOCTET* buffer;        /* start of buffer */
    boolean start_of_file; /* have we gotten any data yet? */
    int memsize;
} my_source_mgr;

typedef my_source_mgr* my_src_ptr;

#define INPUT_BUF_SIZE 32768 /* choose an efficiently fread'able size */

/* jpeg error handling */
struct jpeg_error_mgr jerr;

/*
 * Fill the input buffer --- called whenever buffer is emptied.
 *
 * In typical applications, this should read fresh data into the buffer
 * (ignoring the current state of next_input_byte & bytes_in_buffer),
 * reset the pointer & count to the start of the buffer, and return TRUE
 * indicating that the buffer has been reloaded.  It is not necessary to
 * fill the buffer entirely, only to obtain at least one more byte.
 *
 * There is no such thing as an EOF return.  If the end of the file has been
 * reached, the routine has a choice of ERREXIT() or inserting fake data into
 * the buffer.  In most cases, generating a warning message and inserting a
 * fake EOI marker is the best course of action --- this will allow the
 * decompressor to output however much of the image is there.  However,
 * the resulting error message is misleading if the real problem is an empty
 * input file, so we handle that case specially.
 *
 * In applications that need to be able to suspend compression due to input
 * not being available yet, a FALSE return indicates that no more data can be
 * obtained right now, but more may be forthcoming later.  In this situation,
 * the decompressor will return to its caller (with an indication of the
 * number of scanlines it has read, if any).  The application should resume
 * decompression after it has loaded more data into the input buffer.  Note
 * that there are substantial restrictions on the use of suspension --- see
 * the documentation.
 *
 * When suspending, the decompressor will back up to a convenient restart point
 * (typically the start of the current MCU). next_input_byte & bytes_in_buffer
 * indicate where the restart point will be if the current call returns FALSE.
 * Data beyond this point must be rescanned after resumption, so move it to
 * the front of the buffer rather than discarding it.
 */

METHODDEF boolean fill_input_buffer(j_decompress_ptr cinfo)
{
    my_src_ptr src = (my_src_ptr)cinfo->src;
    int nbytes;

    nbytes = INPUT_BUF_SIZE;
    if (nbytes > src->memsize)
        nbytes = src->memsize;
    if (nbytes == 0) {
        /* Insert a fake EOI marker */
        src->buffer[0] = (JOCTET)0xFF;
        src->buffer[1] = (JOCTET)JPEG_EOI;
        nbytes = 2;
    } else {
        memcpy(src->buffer, src->infile, INPUT_BUF_SIZE);
        src->infile = src->infile + nbytes;
        src->memsize = src->memsize - INPUT_BUF_SIZE;
    }
    src->pub.next_input_byte = src->buffer;
    src->pub.bytes_in_buffer = nbytes;
    src->start_of_file = FALSE;

    return TRUE;
}
/*
 * Initialize source --- called by jpeg_read_header
 * before any data is actually read.
 */

METHODDEF void init_source(j_decompress_ptr cinfo)
{
    my_src_ptr src = (my_src_ptr)cinfo->src;

    /* We reset the empty-input-file flag for each image,
     * but we don't clear the input buffer.
     * This is correct behavior for reading a series of images from one source.
     */
    src->start_of_file = TRUE;
}

/*
 * Skip data --- used to skip over a potentially large amount of
 * uninteresting data (such as an APPn marker).
 *
 * Writers of suspendable-input applications must note that skip_input_data
 * is not granted the right to give a suspension return.  If the skip extends
 * beyond the data currently in the buffer, the buffer can be marked empty so
 * that the next read will cause a fill_input_buffer call that can suspend.
 * Arranging for additional bytes to be discarded before reloading the input
 * buffer is the application writer's problem.
 */

METHODDEF void
skip_input_data(j_decompress_ptr cinfo, long num_bytes)
{
    my_src_ptr src = (my_src_ptr)cinfo->src;

    /* Just a dumb implementation for now.  Could use fseek() except
     * it doesn't work on pipes.  Not clear that being smart is worth
     * any trouble anyway --- large skips are infrequent.
     */
    if (num_bytes > 0) {
        src->infile = src->infile + num_bytes;
        src->pub.next_input_byte += (size_t)num_bytes;
        src->pub.bytes_in_buffer -= (size_t)num_bytes;
    }
}

/*
 * An additional method that can be provided by data source modules is the
 * resync_to_restart method for error recovery in the presence of RST markers.
 * For the moment, this source module just uses the default resync method
 * provided by the JPEG library.  That method assumes that no backtracking
 * is possible.
 */

/*
 * Terminate source --- called by jpeg_finish_decompress
 * after all data has been read.  Often a no-op.
 *
 * NB: *not* called by jpeg_abort or jpeg_destroy; surrounding
 * application must deal with any cleanup that should happen even
 * for error exit.
 */

METHODDEF void
term_source(j_decompress_ptr cinfo)
{
    cinfo = cinfo;
    /* no work necessary here */
}

GLOBAL void
jpeg_memory_src(j_decompress_ptr cinfo, byte* infile, int size)
{
    my_src_ptr src;

    /* The source object and input buffer are made permanent so that a series
     * of JPEG images can be read from the same file by calling jpeg_stdio_src
     * only before the first one.  (If we discarded the buffer at the end of
     * one image, we'd likely lose the start of the next one.)
     * This makes it unsafe to use this manager and a different source
     * manager serially with the same JPEG object.  Caveat programmer.
     */
    if (cinfo->src == NULL) /* first time for this JPEG object? */
    {
        cinfo->src = (struct jpeg_source_mgr*)(*cinfo->mem->alloc_small)((j_common_ptr)cinfo, JPOOL_PERMANENT,
            sizeof(my_source_mgr));
        src = (my_src_ptr)cinfo->src;
        src->buffer = (JOCTET*)(*cinfo->mem->alloc_small)((j_common_ptr)cinfo, JPOOL_PERMANENT,
            INPUT_BUF_SIZE * sizeof(JOCTET));
    }

    src = (my_src_ptr)cinfo->src;
    src->pub.init_source = init_source;
    src->pub.fill_input_buffer = fill_input_buffer;
    src->pub.skip_input_data = skip_input_data;
    src->pub.resync_to_restart = jpeg_resync_to_restart; /* use default method */
    src->pub.term_source = term_source;
    src->infile = infile;
    src->memsize = size;
    src->pub.bytes_in_buffer = 0;    /* forces fill_input_buffer on first read */
    src->pub.next_input_byte = NULL; /* until buffer loaded */
}

int JPEGBlit(byte* wStatus, byte* data, int datasize)
{
    /* This struct contains the JPEG decompression parameters and pointers to
     * working space (which is allocated as needed by the JPEG library).
     */
    struct jpeg_decompress_struct cinfo;
    /* We use our private extension JPEG error handler.
     * Note that this struct must live as long as the main JPEG parameter
     * struct, to avoid dangling-pointer problems.
     */
    /* More stuff */
    JSAMPARRAY buffer; /* Output row buffer */
    int row_stride;    /* physical row width in output buffer */

    /* Step 1: allocate and initialize JPEG decompression object */

    /* We set up the normal JPEG error routines, then override error_exit. */
    cinfo.err = jpeg_std_error(&jerr);

    /* Now we can initialize the JPEG decompression object. */
    jpeg_create_decompress(&cinfo);

    /* Step 2: specify data source (eg, a file) */

    jpeg_memory_src(&cinfo, data, datasize);

    /* Step 3: read file parameters with jpeg_read_header() */

    jpeg_read_header(&cinfo, TRUE);
    /* We can ignore the return value from jpeg_read_header since
     *   (a) suspension is not possible with the stdio data source, and
     *   (b) we passed TRUE to reject a tables-only JPEG file as an error.
     * See libjpeg.doc for more info.
     */

    /* Step 4: set parameters for decompression */

    /* In this example, we don't need to change any of the defaults set by
     * jpeg_read_header(), so we do nothing here.
     */

    /* Step 5: Start decompressor */

    cinfo.dct_method = JDCT_IFAST;
    cinfo.dct_method = JDCT_FASTEST;
    cinfo.dither_mode = JDITHER_NONE;
    cinfo.do_fancy_upsampling = FALSE;
    //	cinfo.out_color_space = JCS_GRAYSCALE;

    jpeg_start_decompress(&cinfo);
    /* We can ignore the return value since suspension is not possible
     * with the stdio data source.
     */

    /* We may need to do some setup of our own at this point before reading
     * the data.  After jpeg_start_decompress() we have the correct scaled
     * output image dimensions available, as well as the output colormap
     * if we asked for color quantization.
     * In this example, we need to make an output work buffer of the right size.
     */
    /* JSAMPLEs per row in output buffer */
    row_stride = cinfo.output_width * cinfo.output_components;

    /* Make a one-row-high sample array that will go away when done with image */
    buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, 1);

    /* Step 6: while (scan lines remain to be read) */
    /*           jpeg_read_scanlines(...); */

    /* Here we use the library's state variable cinfo.output_scanline as the
     * loop counter, so that we don't have to keep track ourselves.
     */

    wStatus += (cinfo.output_height - 1) * row_stride;
    while (cinfo.output_scanline < cinfo.output_height) {
        /* jpeg_read_scanlines expects an array of pointers to scanlines.
         * Here the array is only one element long, but you could ask for
         * more than one scanline at a time if that's more convenient.
         */
        jpeg_read_scanlines(&cinfo, &buffer[0], 1);

        /* Assume put_scanline_someplace wants a pointer and sample count. */
        memcpy(wStatus, &buffer[0][0], row_stride);
        /*
        int x;
        unsigned int *buf = (unsigned int *)&buffer[0][0];
        unsigned int *out = (unsigned int *)wStatus;
        for(x=0;x<cinfo.output_width;x++) {
                unsigned int pixel = buf[x];
                byte *roof = (byte *)&pixel;
                byte temp = roof[0];
                roof[0] = roof[2];
                roof[2] = temp;
                out[x] = pixel;
        }
        */
        wStatus -= row_stride;
    }

    /* Step 7: Finish decompression */

    jpeg_finish_decompress(&cinfo);
    /* We can ignore the return value since suspension is not possible
     * with the stdio data source.
     */

    /* Step 8: Release JPEG decompression object */

    /* This is an important step since it will release a good deal of memory. */
    jpeg_destroy_decompress(&cinfo);

    /* At this point you may want to check to see whether any corrupt-data
     * warnings occurred (test whether jerr.pub.num_warnings is nonzero).
     */

    /* And we're done! */
    return 1;
}

/*
==============
idCinematicLocal::RoQInterrupt
==============
*/
void idCinematicLocal::RoQInterrupt()
{
    byte* framedata;

    iFile->Read(file, RoQFrameSize + 8);
    if (RoQPlayed >= ROQSize) {
        if (looping) {
            RoQReset();
        } else {
            status = FMV_EOF;
        }
        return;
    }

    framedata = file;
//
// new frame is ready
//
redump:
    switch (roq_id) {
    case ROQ_QUAD_VQ:
        if ((numQuads & 1)) {
            normalBuffer0 = t[1];
            RoQPrepMcomp(roqF0, roqF1);
            blitVQQuad32fs(qStatus[1], framedata);
            buf = image + screenDelta;
        } else {
            normalBuffer0 = t[0];
            RoQPrepMcomp(roqF0, roqF1);
            blitVQQuad32fs(qStatus[0], framedata);
            buf = image;
        }
        if (numQuads == 0) // first frame
        {
            memcpy(image + screenDelta, image, samplesPerLine * ysize);
        }
        numQuads++;
        dirty = true;
        break;
    case ROQ_CODEBOOK:
        decodeCodeBook(framedata, (unsigned short)roq_flags);
        break;
    case ZA_SOUND_MONO:
        break;
    case ZA_SOUND_STEREO:
        break;
    case ROQ_QUAD_INFO:
        if (numQuads == -1) {
            readQuadInfo(framedata);
            setupQuad(0, 0);
        }
        if (numQuads != 1)
            numQuads = 0;
        break;
    case ROQ_PACKET:
        inMemory = (roq_flags != 0);
        RoQFrameSize = 0; // for header
        break;
    case ROQ_QUAD_HANG:
        RoQFrameSize = 0;
        break;
    case ROQ_QUAD_JPEG:
        if (!numQuads) {
            normalBuffer0 = t[0];
            JPEGBlit(image, framedata, RoQFrameSize);
            memcpy(image + screenDelta, image, samplesPerLine * ysize);
            numQuads++;
        }
        break;
    default:
        status = FMV_EOF;
        break;
    }
    //
    // read in next frame data
    //
    if (RoQPlayed >= ROQSize) {
        if (looping) {
            RoQReset();
        } else {
            status = FMV_EOF;
        }
        return;
    }

    framedata += RoQFrameSize;
    roq_id = framedata[0] + framedata[1] * 256;
    RoQFrameSize = framedata[2] + framedata[3] * 256 + framedata[4] * 65536;
    roq_flags = framedata[6] + framedata[7] * 256;
    roqF0 = (char)framedata[7];
    roqF1 = (char)framedata[6];

    if (RoQFrameSize > 65536 || roq_id == 0x1084) {
        common->DPrintf("roq_size>65536||roq_id==0x1084\n");
        status = FMV_EOF;
        if (looping) {
            RoQReset();
        }
        return;
    }
    if (inMemory && (status != FMV_EOF)) {
        inMemory = false;
        framedata += 8;
        goto redump;
    }
    //
    // one more frame hits the dust
    //
    //	assert(RoQFrameSize <= 65536);
    //	r = Sys_StreamedRead( file, RoQFrameSize+8, 1, iFile );
    RoQPlayed += RoQFrameSize + 8;
}

/*
==============
idCinematicLocal::RoQ_init
==============
*/
void idCinematicLocal::RoQ_init()
{

    RoQPlayed = 24;

    /*	get frame rate */
    roqFPS = file[6] + file[7] * 256;

    if (!roqFPS)
        roqFPS = 30;

    numQuads = -1;

    roq_id = file[8] + file[9] * 256;
    RoQFrameSize = file[10] + file[11] * 256 + file[12] * 65536;
    roq_flags = file[14] + file[15] * 256;
}

/*
==============
idCinematicLocal::RoQShutdown
==============
*/
void idCinematicLocal::RoQShutdown()
{
    if (status == FMV_IDLE) {
        return;
    }
    status = FMV_IDLE;

    if (iFile) {
        fileSystem->CloseFile(iFile);
        iFile = NULL;
    }

    fileName = "";
}

//===========================================

/*
==============
idSndWindow::InitFromFile
==============
*/
bool idSndWindow::InitFromFile(const char* qpath, bool looping)
{
    idStr fname = qpath;

    fname.ToLower();
    if (!fname.Icmp("waveform")) {
        showWaveform = true;
    } else {
        showWaveform = false;
    }
    return true;
}

/*
==============
idSndWindow::ImageForTime
==============
*/
cinData_t idSndWindow::ImageForTime(int milliseconds)
{
    return soundSystem->ImageForTime(milliseconds, showWaveform);
}

/*
==============
idSndWindow::AnimationLength
==============
*/
int idSndWindow::AnimationLength()
{
    return -1;
}
